#include "DebugServer/GroundQueryDebugHandlers.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "Components/Name.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "SplineRecipes/PlacementGroundGap.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/TerrainGpuBake.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

// One request samples a route, not a scene: a few thousand stations is the whole
// of a long approach road at 0.5 m spacing, and the cap keeps a typo from asking
// the editor to bilinear-sample a million points on the main thread.
constexpr std::size_t kMaxGroundSamples = 8192u;

// A request fetches at most this many finest height pages on the main thread. Each fetch reads one
// page from the cooked height store on disk and decodes it, or evaluates it for a generated terrain.
constexpr uint32 kMaxFinestPageFetches = 64u;

// The composed-ground description shared by every handler in this file. Reported
// alongside the samples so a reading carries the terrain it was taken against:
// two runs that disagree are usually two different terrains, not two heights.
// What the query reads: a whole-map heightfield, a paged terrain's CPU pages, or a paged terrain
// whose pages have no source yet (a height store still cooking), which samples nothing.
const char* SurfaceKind(const TerrainECS::SurfaceHeightQuery& query)
{
    if (query.WholeMap())
        return "single";
    return query.Edits().Source != 0 ? "paged" : "pagedNoSourceYet";
}

json DescribeSurface(const TerrainECS::SurfaceHeightQuery& query)
{
    // Every footprint number comes from the query's own accessors, which report
    // the extent it SAMPLES over. The lattice origin is the footprint origin, so
    // it is reported once.
    return json{
        {"kind", SurfaceKind(query)},
        {"originX", query.FootprintOriginX()},
        {"originZ", query.FootprintOriginZ()},
        {"originY", query.OriginY()},
        {"sizeX", query.FootprintSizeX()},
        {"sizeZ", query.FootprintSizeZ()},
        {"heightScale", query.HeightScale()},
        {"latticeSpacingX", query.LatticeSpacingX()},
        {"latticeSpacingZ", query.LatticeSpacingZ()},
        // A tiled terrain whose modifiers are all GPU-bakeable leaves the CPU
        // heightfield stale between bake and settle readback, so the same XZ can
        // read differently from what is drawn. Corridors, splines, stamps and
        // paint layers are never GPU-bakeable and force the whole bake back to
        // the CPU, which is why a corridored route reads authoritatively here —
        // but the caller is told the gate is armed rather than left to assume.
        {"gpuHeightBake", TerrainECS::IsGpuHeightBakeEnabled()}};
}

// A piece with no gap numbers says which cause left it that way, because the
// fixes differ: wait out a streaming tile, move a piece off the terrain edge,
// and stop asking a wall how far above the ground its underside floats.
const char* DescribeGapStatus(SplineRecipes::GroundGapStatus status)
{
    switch (status)
    {
    case SplineRecipes::GroundGapStatus::Measured:
        return "measured";
    case SplineRecipes::GroundGapStatus::NoGroundSampled:
        return "noGroundSampled";
    case SplineRecipes::GroundGapStatus::UndersideNearVertical:
        return "undersideNearVertical";
    case SplineRecipes::GroundGapStatus::DegenerateFootprint:
        return "degenerateFootprint";
    }
    return "unknown";
}

// "No terrain here" is an answer, not a failure: the surface is absent before
// provisioning and in an empty scene, and a caller that cannot tell those from a
// malformed request will read zero as ground.
json UnresolvedSurface()
{
    return json{{"resolved", false},
                {"note", "no enabled terrain with height data at the first point: an unprovisioned "
                         "terrain, a planet that does not stream its pages yet, or an empty scene"}};
}

// A point of the request: [x, z] (y 0) or [x, y, z].
bool IsGroundPoint(const json& point)
{
    return point.is_array() && (point.size() == 2u || point.size() == 3u) &&
           std::ranges::all_of(point, [](const json& value) { return value.is_number(); });
}

Mathematics::Vector3 GroundPointOf(const json& point)
{
    if (point.size() == 2u)
        return Mathematics::Vector3(point[0].get<float32>(), 0.0f, point[1].get<float32>());
    return Mathematics::Vector3(point[0].get<float32>(), point[1].get<float32>(), point[2].get<float32>());
}

// The planar height under `point` at the finest level, the height a caller places on. A Coarse
// answer fetches the level-0 page under the point and samples again from `query`, resolved again
// at `first`. After kMaxFinestPageFetches fetches, a Coarse answer stays Coarse.
bool SampleFinestGround(ECS::World& world, const Mathematics::Vector3& first, TerrainECS::SurfaceHeightQuery& query,
                        const Mathematics::Vector3& point, uint32& fetches, TerrainECS::SurfaceHeightSample& out)
{
    constexpr TerrainECS::SurfaceHeightPolicy kAcceptCoarse = TerrainECS::SurfaceHeightPolicy::AcceptCoarse;
    if (!query.SampleHeight(point, kAcceptCoarse, out))
        return false;
    if (out.Detail == TerrainECS::SurfaceHeightDetail::Finest || fetches == kMaxFinestPageFetches)
        return true;
    ++fetches;
    if (!TerrainECS::FetchFinestSurfaceHeightPage(world, point))
        return true;
    query = TerrainECS::ResolveSurfaceHeightQuery(world, first);
    return query.SampleHeight(point, kAcceptCoarse, out);
}

json DescribePlanetSurface(const TerrainECS::SurfaceHeightQuery& query)
{
    const TerrainECS::SurfaceHeightSphere& sphere = query.Sphere();
    return json{{"kind", "planet"},
                {"center", {sphere.Center.x, sphere.Center.y, sphere.Center.z}},
                {"referenceRadius", sphere.ReferenceRadius},
                {"faceSamples", sphere.FaceSamples}};
}

// Each point's height above the reference radius on the line from the center through it, and the
// surface point there. Unsampled when no page under it is on the CPU, or at the center.
json SamplePlanetGround(const TerrainECS::SurfaceHeightQuery& query, const json& points)
{
    const TerrainECS::SurfaceHeightSphere& sphere = query.Sphere();
    json samples = json::array();
    for (const json& point : points)
    {
        const Mathematics::Vector3 position = GroundPointOf(point);
        TerrainECS::SurfaceHeightSample height;
        const bool sampled =
            query.SampleHeight(position, TerrainECS::SurfaceHeightPolicy::AcceptCoarse, height);
        json sample{{"x", position.x}, {"y", position.y}, {"z", position.z}, {"sampled", sampled}};
        if (sampled)
        {
            const Mathematics::Vector3 surface =
                sphere.Center + (position - sphere.Center).Normalize() *
                                    static_cast<float32>(sphere.ReferenceRadius + height.Height);
            sample["height"] = height.Height;
            sample["surface"] = {surface.x, surface.y, surface.z};
            sample["level"] = height.Level;
            sample["finest"] = height.Detail == TerrainECS::SurfaceHeightDetail::Finest;
        }
        samples.push_back(std::move(sample));
    }
    return samples;
}

} // namespace

void RegisterGroundQueryDebugHandlers(EditorDebugServer& server)
{
    // The composed ground the renderer and the conform placement actually stand
    // on: the CPU heightfield AFTER the modifier stack — flattens, corridors,
    // stamps and noise — not the base heightmap an offline model reads. That
    // difference is the whole point of the method: a scene-authored corridor
    // exists only here.
    //
    // Terrain only, by construction. This reads the heightfield directly rather
    // than tracing the scene, so it never reports a mesh, a prop or a bridge
    // deck as ground; a caller that wants "what would I land on" wants a conform
    // ray instead.
    server.RegisterHandler("sample_ground_height",
                           [](const EditorDebugServer::RequestContext& ctx) -> json
    {
        if (!ctx.params.contains("points") || !ctx.params["points"].is_array())
            return Editor::RefuseRequest("Missing points parameter — expected [[x, z], ...] or, on a planet, "
                                         "[[x, y, z], ...]");

        const auto& points = ctx.params["points"];
        if (points.size() > kMaxGroundSamples)
            return Editor::RefuseRequest("Too many points: " + std::to_string(points.size()) +
                                             " exceeds the " + std::to_string(kMaxGroundSamples) +
                                             " sample cap");

        // The whole request is validated BEFORE any scene state is read, so
        // "malformed" and "no surface here" cannot swap places with the scene: a
        // shape check inside the sampling loop answers [[1, 2, 3]] with the
        // terrain's absence in an empty scene and with the real complaint in a
        // terrain scene, which is the one thing a caller cannot debug.
        for (const auto& point : points)
        {
            if (!IsGroundPoint(point) || point.size() != points[0].size())
                return Editor::RefuseRequest("Each point must be [x, z] or [x, y, z], every point the same");
        }

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        // The terrain under the first point answers every point.
        const Mathematics::Vector3 first = points.empty() ? Mathematics::Vector3{} : GroundPointOf(points[0]);
        TerrainECS::SurfaceHeightQuery query = TerrainECS::ResolveSurfaceHeightQuery(*world, first);
        if (query.IsSpherical())
        {
            if (!points.empty() && points[0].size() != 3u)
                return Editor::RefuseRequest("On a planet each point must be [x, y, z]: the height is read on "
                                             "the line from the planet's center through the point");
            return json{{"resolved", true},
                        {"surface", DescribePlanetSurface(query)},
                        {"samples", SamplePlanetGround(query, points)}};
        }
        if (!query.IsValid())
            return UnresolvedSurface();

        json samples = json::array();
        uint32 fetches = 0;
        uint32 coarsePastCap = 0;
        uint32 coarseNoFinestPage = 0;
        for (const auto& point : points)
        {
            const Mathematics::Vector3 position = GroundPointOf(point);
            const float32 x = position.x;
            const float32 z = position.z;
            const bool pastCap = fetches == kMaxFinestPageFetches;

            // Outside the footprint there is no ground and never will be;
            // inside it but unsampled means no page is on the CPU there yet.
            // Collapsing those into one "false" is how a caller ends up bedding
            // geometry into a streaming hole, so they stay separate. A coarse
            // answer is reported with its level and `finest` false.
            const bool inside = query.ContainsXZ(x, z);
            TerrainECS::SurfaceHeightSample height;
            const bool sampled =
                SampleFinestGround(*world, first, query, Mathematics::Vector3(x, 0.0f, z), fetches, height);

            json sample{{"x", x}, {"z", z}, {"inside", inside}, {"sampled", sampled}};
            if (sampled)
            {
                const bool finest = height.Detail == TerrainECS::SurfaceHeightDetail::Finest;
                coarsePastCap += !finest && pastCap ? 1u : 0u;
                coarseNoFinestPage += !finest && !pastCap ? 1u : 0u;
                sample["y"] = height.Height;
                sample["level"] = height.Level;
                sample["finest"] = finest;
            }
            samples.push_back(std::move(sample));
        }

        json reply{{"resolved", true}, {"surface", DescribeSurface(query)}, {"samples", std::move(samples)}};
        if (coarsePastCap + coarseNoFinestPage > 0u)
            reply["note"] = std::to_string(coarsePastCap) + " samples answered from a coarser level after the request "
                            "used its " + std::to_string(kMaxFinestPageFetches) + " page fetches: ask again for "
                            "those points. " + std::to_string(coarseNoFinestPage) + " samples answered from a "
                            "coarser level because the height store gives no finest page there: asking again "
                            "gives the same answer until the store holds that page. Do not place on a sample with "
                            "finest false.";
        return reply;
    });

    // The signed ground-to-underside gap for every piece a spline route placed:
    // positive floating, negative buried. This is the quantity a conforming
    // placement actually fails on. Station tilt is not — flattening the ground
    // under a slab lowers its tilt and opens its gap at the same time, so tilt
    // improves as the defect worsens.
    server.RegisterHandler("get_placement_ground_gap",
                           [](const EditorDebugServer::RequestContext& ctx) -> json
    {
        // Validated before any state is read, for the reason the sampling handler
        // above states: a malformed request must not be answered with the scene's
        // condition.
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter — the spline route entity");
        if (!ctx.params["entityId"].is_number_unsigned())
            return Editor::RefuseRequest("entityId must be an unsigned number — the spline route entity");

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        const ECS::EntityHandle route(ctx.params["entityId"].get<uint32_t>());
        if (!world->IsValid(route))
            return Editor::RefuseRequest("Invalid entity");

        const auto* routeTransform = world->GetComponent<Components::WorldTransform>(route);
        const TerrainECS::SurfaceHeightQuery query = TerrainECS::ResolveSurfaceHeightQuery(
            *world, routeTransform ? routeTransform->GetPosition() - Mathematics::WorldPosition{} : Mathematics::Vector3{});
        if (!query.IsValid())
            return UnresolvedSurface();

        std::vector<SplineRecipes::PieceGroundGap> gaps;
        SplineRecipes::MeasureRouteGroundGaps(*world, route, query, gaps);

        json pieces = json::array();
        float32 worstFloating = std::numeric_limits<float32>::lowest();
        float32 worstBuried = std::numeric_limits<float32>::max();
        uint32_t worstFloatingPiece = 0u;
        uint32_t worstBuriedPiece = 0u;
        uint32 measured = 0u;
        for (const SplineRecipes::PieceGroundGap& gap : gaps)
        {
            json piece{
                {"entityId", gap.Piece.id},
                {"status", DescribeGapStatus(gap.Status)},
                {"undersideCenterY", gap.UndersideCenterY},
                {"probesSampled", gap.ProbesSampled},
                {"probesRequested", gap.ProbesRequested},
                {"fullySampled", gap.FullySampled()}};

            // A display label, renumbered whenever spacing changes — readable,
            // never an identity. Match pieces by entityId.
            if (const auto* name = world->GetComponent<Components::Name>(gap.Piece))
                piece["name"] = std::string(name->View());

            // Per-probe detail is the point of probing five places: a summary
            // cannot say WHICH corner is buried, and on a tilted piece the answer
            // is not the one a box around it would nominate.
            if (gap.ProbesRequested > 0u)
            {
                json probes = json::array();
                for (const SplineRecipes::GroundGapProbe& probe : gap.Probes)
                {
                    json entry{{"x", probe.X},
                               {"z", probe.Z},
                               {"undersideY", probe.UndersideY},
                               {"sampled", probe.Sampled}};
                    if (probe.Sampled)
                    {
                        entry["groundY"] = probe.GroundY;
                        entry["gap"] = probe.Gap;
                    }
                    probes.push_back(std::move(entry));
                }
                piece["probes"] = std::move(probes);
            }

            if (gap.Status == SplineRecipes::GroundGapStatus::Measured)
            {
                piece["minGap"] = gap.MinGap;
                piece["maxGap"] = gap.MaxGap;
                piece["centerGap"] = gap.CenterGap;
                if (gap.MaxGap > worstFloating)
                {
                    worstFloating = gap.MaxGap;
                    worstFloatingPiece = gap.Piece.id;
                }
                if (gap.MinGap < worstBuried)
                {
                    worstBuried = gap.MinGap;
                    worstBuriedPiece = gap.Piece.id;
                }
                ++measured;
            }
            pieces.push_back(std::move(piece));
        }

        json summary{{"pieceCount", static_cast<uint32_t>(gaps.size())},
                     {"measuredCount", measured}};
        if (measured > 0u)
        {
            summary["worstFloating"] = worstFloating;
            summary["worstFloatingEntityId"] = worstFloatingPiece;
            summary["worstBuried"] = worstBuried;
            summary["worstBuriedEntityId"] = worstBuriedPiece;
        }

        return json{{"resolved", true},
                    {"surface", DescribeSurface(query)},
                    {"summary", std::move(summary)},
                    {"pieces", std::move(pieces)}};
    });
}

} // namespace GameEngine
