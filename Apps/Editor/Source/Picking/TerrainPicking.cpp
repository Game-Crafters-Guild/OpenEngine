#include "Picking/TerrainPicking.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "CBTTerrainECS/CBTRenderFeature.h"
#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/RenderServices.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/TerrainService.h"

namespace GameEngine::Editor::Picking
{

namespace
{

using Mathematics::Vector3;

// The normalized height range a terrain's baked field ACTUALLY spans.
//
// Modifier bakes accumulate straight into raw heightfield samples and never clamp
// (TerrainModifierSystem::ApplyHeightEffectSample), and TerrainRenderFeature uploads the field
// verbatim as R32_FLOAT — so a lowering volume draws ground below normalized 0, and a raise
// draws it above 1. Treating [0,1] as the bound is what silently clipped that ground out of
// the march: below the floor every coarse sample reads as "above the surface" and the cast
// misses outright; above the lid the first sample is already under the surface and the cast
// reports the lid instead of the ground.
//
// O(1) for a single terrain (the CDLOD root node's min/max, re-derived by every bake) and
// O(resident tiles) for a tiled one. The single-terrain scan is the documented fallback for a
// field whose quadtree is not built yet — a transient before the first extraction, when the
// field is still the zero fill.
void ResolveBakedHeightRange(const TerrainECS::TerrainData* data,
                             const TerrainECS::TiledTerrainData* tiled,
                             float32& outMinNorm, float32& outMaxNorm)
{
    // Nothing resolvable: keep the nominal band rather than collapsing the slab to nothing.
    outMinNorm = 0.0f;
    outMaxNorm = 1.0f;

    if (tiled)
    {
        bool any = false;
        for (const auto& entry : tiled->Tiles)
        {
            const auto& tile = entry.second;
            if (!tile || tile->LodState == TerrainECS::TileLodState::Empty ||
                tile->Heightfield.IsEmpty())
                continue;
            outMinNorm = any ? std::min(outMinNorm, tile->CachedMinH) : tile->CachedMinH;
            outMaxNorm = any ? std::max(outMaxNorm, tile->CachedMaxH) : tile->CachedMaxH;
            any = true;
        }
        return;
    }

    if (!data || data->Heightfield.IsEmpty())
        return;

    const uint32 w = data->Heightfield.GetWidth();
    const uint32 h = data->Heightfield.GetHeight();
    if (data->Quadtree.TryGetGlobalHeightRange(w, h, outMinNorm, outMaxNorm))
        return;
    data->Heightfield.GetMinMax(0, 0, static_cast<int32>(w), static_cast<int32>(h),
                                outMinNorm, outMaxNorm);
}

// One planar terrain's height as the pick reads it: its CPU height pages when it pages, its resident
// tiles when it is tiled, its single heightfield otherwise. World meters.
struct PlanarPickSurface
{
    const TerrainECS::TerrainData* Single = nullptr;
    const TerrainECS::TiledTerrainData* Tiled = nullptr;
    TerrainECS::SurfaceHeightQuery Pages;
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
    float32 OriginY = 0.0f;
    float32 SizeX = 0.0f;
    float32 SizeZ = 0.0f;
    float32 HeightScale = 0.0f;
};

// The nearest planar terrain hit so far.
struct PlanarPickBest
{
    float32 Distance = 0.0f;
    Vector3 Position{};
    Vector3 Normal{0.0f, 1.0f, 0.0f};
    ECS::EntityHandle Entity{};
    bool Hit = false;
};

// World height of `surface` at (worldX, worldZ). A paged terrain answers from its finest CPU page, then
// from its resident tile (exact where no CPU window reaches), then from a coarser pinned page, so a pick
// is never coarser than the data the editor holds there and still lands where no tile is resident.
// False outside the footprint, and on a tiled terrain that does not page where its tile is not resident.
bool SamplePickHeight(const PlanarPickSurface& surface, float32 worldX, float32 worldZ, float32& outY)
{
    const Vector3 point(worldX, 0.0f, worldZ);
    TerrainECS::SurfaceHeightSample sample;
    if (surface.Pages.IsValid() &&
        surface.Pages.SampleHeight(point, TerrainECS::SurfaceHeightPolicy::RequireFinest, sample))
    {
        outY = sample.Height;
        return true;
    }
    if (surface.Tiled)
    {
        float32 normalized = 0.0f;
        if (TerrainECS::SampleTiledHeightNormalized(*surface.Tiled, worldX, worldZ, normalized))
        {
            outY = surface.OriginY + normalized * surface.HeightScale;
            return true;
        }
        if (!surface.Pages.IsValid() ||
            !surface.Pages.SampleHeight(point, TerrainECS::SurfaceHeightPolicy::AcceptCoarse, sample))
            return false;
        outY = sample.Height;
        return true;
    }
    const float32 u = (worldX - surface.OriginX) / surface.SizeX;
    const float32 v = (worldZ - surface.OriginZ) / surface.SizeZ;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        return false;
    outY = surface.OriginY + surface.Single->Heightfield.SampleBilinear(u, v) * surface.HeightScale;
    return true;
}

// Central-difference normal of `surface` at (worldX, worldZ), stepped at the terrain's own sample spacing
// (`samplesPerMeter`). +Y wherever a tap has no height.
Vector3 PickSurfaceNormal(const PlanarPickSurface& surface, float32 samplesPerMeter, float32 worldX, float32 worldZ)
{
    const float32 step = 1.0f / std::clamp(samplesPerMeter, 0.05f, 20.0f);
    float32 heightPlusX = 0.0f, heightMinusX = 0.0f, heightPlusZ = 0.0f, heightMinusZ = 0.0f;
    if (!SamplePickHeight(surface, worldX + step, worldZ, heightPlusX) ||
        !SamplePickHeight(surface, worldX - step, worldZ, heightMinusX) ||
        !SamplePickHeight(surface, worldX, worldZ + step, heightPlusZ) ||
        !SamplePickHeight(surface, worldX, worldZ - step, heightMinusZ))
        return Vector3(0.0f, 1.0f, 0.0f);
    // Heightfield y = h(x, z) has normal along (-dh/dx, 1, -dh/dz).
    const float32 inverseSpan = 1.0f / (2.0f * step);
    const Vector3 normal((heightMinusX - heightPlusX) * inverseSpan, 1.0f, (heightMinusZ - heightPlusZ) * inverseSpan);
    return normal * (1.0f / std::sqrt(Vector3::Dot(normal, normal)));
}

// Marches `ray` against one planar terrain and keeps the hit in `best` when it is nearer.
void PickPlanarTerrain(TerrainECS::TerrainService& service, const Mathematics::Ray3D& ray, ECS::EntityHandle entity,
                       const Components::Terrain& terrain, const Components::WorldTransform& worldTransform,
                       PlanarPickBest& best)
{
    if (terrain.Domain != Components::TerrainDomain::Planar)
        return;

    // A tiled terrain has no single TerrainData (extraction zeroes its handle): it is read through
    // its pages and its resident tiles.
    PlanarPickSurface surface;
    surface.Single =
        service.GetTerrainData(TerrainECS::TerrainHandle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration});
    surface.Tiled = service.GetTiledTerrainData(
        TerrainECS::TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
    if (surface.Single && surface.Single->Heightfield.IsEmpty())
        surface.Single = nullptr;
    // Neither store resolved: the terrain has no height data yet (extraction has not run for it), so
    // there is no surface to hit and nothing drawn to click.
    if (!surface.Single && !surface.Tiled)
        return;
    if (surface.Tiled)
        surface.Pages = service.SurfaceQuery(terrain);

    const Vector3 center = worldTransform.GetPosition();
    surface.OriginX = center.x - terrain.SizeX * 0.5f;
    surface.OriginZ = center.z - terrain.SizeZ * 0.5f;
    surface.OriginY = center.y;
    surface.SizeX = terrain.SizeX;
    surface.SizeZ = terrain.SizeZ;
    surface.HeightScale = terrain.HeightScale;

    // Terrain world AABB: XZ is the footprint; Y spans the range the baked field actually holds, NOT the
    // nominal [0,1] band: modifier volumes drive samples outside that band and the renderer draws them
    // there (see ResolveBakedHeightRange); a paged terrain states its range with its pages. A small Y
    // margin keeps a ray grazing the exact peak or trough from being clipped out by rounding. The
    // marcher reach is this box regardless of size, so far-side hits on large terrains land.
    constexpr float32 kHeightMargin = 1.0f;
    float32 lowY = 0.0f;
    float32 highY = 0.0f;
    if (surface.Pages.IsValid())
    {
        lowY = surface.Pages.MinHeight();
        highY = surface.Pages.MaxHeight();
    }
    else
    {
        float32 minNorm = 0.0f;
        float32 maxNorm = 1.0f;
        ResolveBakedHeightRange(surface.Single, surface.Tiled, minNorm, maxNorm);
        // HeightScale orders the two products; take the extremes so a negative scale cannot invert the slab.
        const float32 heightA = surface.OriginY + minNorm * terrain.HeightScale;
        const float32 heightB = surface.OriginY + maxNorm * terrain.HeightScale;
        lowY = std::min(heightA, heightB);
        highY = std::max(heightA, heightB);
    }
    const Vector3 aabbMin(surface.OriginX, lowY - kHeightMargin, surface.OriginZ);
    const Vector3 aabbMax(surface.OriginX + terrain.SizeX, highY + kHeightMargin, surface.OriginZ + terrain.SizeZ);

    float32 hitT = 0.0f;
    Vector3 hitPos{};
    const auto sampleHeight = [&surface](float32 worldX, float32 worldZ, float32& outY) {
        return SamplePickHeight(surface, worldX, worldZ, outY);
    };
    if (!MarchTerrainSurface(ray, aabbMin, aabbMax, best.Distance, sampleHeight, hitT, hitPos) || hitT >= best.Distance)
        return;
    best.Distance = hitT;
    best.Position = hitPos;
    best.Normal = PickSurfaceNormal(surface, terrain.SamplesPerMeter, hitPos.x, hitPos.z);
    best.Entity = entity;
    best.Hit = true;
}

// Ray -> every planar terrain's heightfield footprint, nearest hit. The march walks geometry
// only the planar domain draws: a spherical terrain retains its heightfield but renders a
// displaced sphere instead, so marching it would offer a phantom flat target at the entity
// origin. Spherical terrains are handled by RaycastActivePlanet against what is drawn.
bool RaycastPlanarTerrains(const Mathematics::Ray3D& ray,
                           GameEngine::ECS::World& world,
                           float32 maxDistance,
                           TerrainPickHit& outHit)
{
    // The service owns the heightfields, so without it there is nothing planar to march.
    auto* terrainService = TerrainECS::TerrainService::TryGet();
    if (!terrainService)
        return false;

    PlanarPickBest best;
    best.Distance = maxDistance;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [terrainService, &ray, &best](ECS::EntityHandle entity, const Components::Terrain& terrain,
                                      const Components::WorldTransform& worldTransform) {
            PickPlanarTerrain(*terrainService, ray, entity, terrain, worldTransform, best);
        });
    if (!best.Hit)
        return false;

    outHit.Entity        = best.Entity;
    outHit.WorldPosition = best.Position;
    outHit.WorldNormal   = best.Normal;
    outHit.Distance      = best.Distance;
    return true;
}

// The CBT render feature when it is currently drawing a spherical planet. It is the
// authority for both halves of the planet's geometry: GetDomainConfig().PlanetRadius is the
// radius the GPU displaces FROM, and SampleSphereSurfaceHeight is the CPU mirror of the
// height it displaces BY (procedural relief from the active planet tuning + TerrainService's
// editable sculpt layer). Reading the renderer rather than the Terrain component is what
// makes a pick land on the ground the user can see, including mid-edit frames where the
// component has already moved and the drawn geometry has not.
//
// No feature, or a feature not on the spherical domain, means no planet is on screen: there
// is then nothing to pick, and inventing a radius would place hits on a sphere the renderer
// never drew.
const CBTTerrainECS::CBTRenderFeature* ActivePlanetFeature()
{
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    if (!renderServices)
        return nullptr;
    const auto* feature = renderServices->GetFeature<CBTTerrainECS::CBTRenderFeature>();
    if (!feature)
        return nullptr;
    const auto& domain = feature->GetDomainConfig();
    if (domain.DomainMode != CBTTerrain::kDomainSpherical || !(domain.PlanetRadius > 0.0f))
        return nullptr;
    return feature;
}

// Ray -> the one spherical terrain, resolved through the SAME active-terrain resolver the
// renderer tunes from and the sculpt brush routes by, so pick, sculpt and draw can never
// disagree about which terrain is the planet. CBT is single-active-terrain by design, so
// this runs once per cast rather than once per entity.
bool RaycastActivePlanet(const Mathematics::Ray3D& ray,
                         GameEngine::ECS::World& world,
                         float32 maxDistance,
                         TerrainPickHit& outHit)
{
    const CBTTerrainECS::CBTRenderFeature* feature = ActivePlanetFeature();
    if (!feature)
        return false;

    const ECS::EntityHandle entity = CBTTerrainECS::FindActiveTerrainEntity(world);
    if (!entity.IsValid())
        return false;
    const auto* terrain = world.GetComponent<Components::Terrain>(entity);
    if (!terrain || terrain->Domain != Components::TerrainDomain::Spherical)
        return false;

    float32 t = 0.0f;
    Vector3 pos{};
    Vector3 normal{};
    if (!SolvePlanetSurfaceHit(ray, feature->GetDomainConfig().PlanetRadius, maxDistance,
                               [feature](float32 dx, float32 dy, float32 dz) {
                                   return feature->SampleSphereSurfaceHeight(dx, dy, dz);
                               },
                               t, pos, normal))
        return false;

    outHit.Entity        = entity;
    outHit.WorldPosition = pos;
    outHit.WorldNormal   = normal;
    outHit.Distance      = t;
    return true;
}

} // namespace

bool RaycastTerrain(const Mathematics::Ray3D& ray,
                    GameEngine::ECS::World& world,
                    float32 maxDistance,
                    TerrainPickHit& outHit)
{
    bool    anyHit = false;
    float32 bestT  = std::min(maxDistance, std::numeric_limits<float32>::max());
    TerrainPickHit best{};
    TerrainPickHit hit{};

    if (RaycastPlanarTerrains(ray, world, bestT, hit))
    {
        best   = hit;
        bestT  = hit.Distance;
        anyHit = true;
    }
    // Capped at bestT, so a planar terrain (or, through RaycastScene, a mesh) in front of the
    // planet still wins.
    if (RaycastActivePlanet(ray, world, bestT, hit))
    {
        best   = hit;
        bestT  = hit.Distance;
        anyHit = true;
    }

    if (!anyHit)
        return false;

    outHit = best;
    return true;
}

}
