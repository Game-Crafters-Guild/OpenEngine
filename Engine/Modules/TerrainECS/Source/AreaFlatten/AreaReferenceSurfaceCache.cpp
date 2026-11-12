#include "TerrainECS/AreaFlatten/AreaReferenceSurfaceCache.h"

#include "AreaFlatten/AreaReferenceSurface.h"
#include "Components/Hierarchy.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Spline/SplineData.h"
#include "TerrainECS/TerrainModifierSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace GameEngine::TerrainECS
{
namespace
{

const ResolvedEffect* FindFlatten(const ResolvedModifier& mod)
{
    const auto it = std::find_if(mod.Effects.begin(), mod.Effects.end(), [](const ResolvedEffect& fx) {
        return fx.EffectKind == ResolvedEffect::Kind::Flatten;
    });
    return it == mod.Effects.end() ? nullptr : &*it;
}

// A SplineArea volume on a closed spline whose flatten levels to the volume's own height: the one
// reference a surface replaces.
bool GradesToItsRim(const ResolvedModifier& mod)
{
    if (mod.ModType != ResolvedModifier::Type::Volume || mod.Shape != Components::TerrainModifierShape::Spline
        || !mod.SplineFillInterior || mod.GlobalScope || !mod.ResolvedSpline
        || !mod.ResolvedSpline->IsEffectivelyClosed())
        return false;
    const ResolvedEffect* flatten = FindFlatten(mod);
    return flatten && flatten->Flatten.UseVolumeHeight;
}

bool HasFiniteBounds(const ResolvedModifier& mod)
{
    return std::isfinite(mod.BoundsMinX) && std::isfinite(mod.BoundsMinZ) && std::isfinite(mod.BoundsMaxX)
           && std::isfinite(mod.BoundsMaxZ) && mod.BoundsMaxX > mod.BoundsMinX && mod.BoundsMaxZ > mod.BoundsMinZ;
}

// The runs of `route` whose band and fade reach the rectangle: each run is the consecutive stations
// of the segments that do, so a road that leaves and re-enters keeps its gaps.
std::vector<RoutePolyline> RunsNear(const RoutePolyline& route, float32 fade, float32 minX, float32 minZ,
                                    float32 maxX, float32 maxZ)
{
    std::vector<RoutePolyline> runs;
    std::vector<RouteStation> run;
    const std::vector<RouteStation>& stations = route.Stations;
    for (std::size_t segment = 0; segment + 1 < stations.size(); ++segment)
    {
        const RouteStation& a = stations[segment];
        const RouteStation& b = stations[segment + 1];
        const float32 reach = std::max(a.HalfWidth, b.HalfWidth) + fade;
        const bool near = std::min(a.X, b.X) - reach <= maxX && std::max(a.X, b.X) + reach >= minX
                          && std::min(a.Z, b.Z) - reach <= maxZ && std::max(a.Z, b.Z) + reach >= minZ;
        if (!near)
        {
            if (!run.empty())
                BuildRoutePolyline(std::exchange(run, {}), fade, runs.emplace_back());
            continue;
        }
        if (run.empty())
            run.push_back(a);
        run.push_back(b);
    }
    if (!run.empty())
        BuildRoutePolyline(std::move(run), fade, runs.emplace_back());
    return runs;
}

// True when the opening's band or fade covers any station of the rim.
bool ReachesRim(const AreaOpening& opening, const RoutePolyline& rim)
{
    for (const RouteStation& station : rim.Stations)
    {
        float32 distanceFromEdge = -std::numeric_limits<float32>::infinity();
        for (const RoutePolyline& run : opening.Runs)
            distanceFromEdge =
                std::max(distanceFromEdge, ClosestStationXZ(run, station.X, station.Z).DistanceFromEdge());
        if (ShapeFalloffWeight(distanceFromEdge, opening.Falloff, opening.FalloffInward) * opening.Weight > 0.0f)
            return true;
    }
    return false;
}

// The roads at `area`'s openings: the other route volumes with a flatten whose band reaches its rim.
// A volume parented to the area is the area's own piece, never one of its roads.
std::vector<AreaOpening> OpeningsOf(const ECS::World& world, const ResolvedModifier& area, const RoutePolyline& rim,
                                    std::span<const ResolvedModifier> modifiers)
{
    std::vector<AreaOpening> openings;
    for (const ResolvedModifier& road : modifiers)
    {
        if (&road == &area || road.ModType != ResolvedModifier::Type::Volume || !road.Route || !road.Route->IsValid())
            continue;
        const ResolvedEffect* flatten = FindFlatten(road);
        if (!flatten)
            continue;
        const auto* parent = world.GetComponent<Components::Parent>(road.Entity);
        if (parent && parent->Entity == area.Entity)
            continue;
        AreaOpening opening{};
        opening.Runs = RunsNear(*road.Route, std::max(road.Falloff, 0.0f), area.BoundsMinX, area.BoundsMinZ,
                                area.BoundsMaxX, area.BoundsMaxZ);
        if (opening.Runs.empty())
            continue;
        opening.TargetHeight = flatten->Flatten.TargetHeight;
        opening.UseVolumeHeight = flatten->Flatten.UseVolumeHeight;
        opening.Falloff = road.Falloff;
        opening.FalloffInward = road.FalloffInward;
        opening.Weight = road.Weight;
        if (ReachesRim(opening, rim))
            openings.push_back(std::move(opening));
    }
    return openings;
}

} // namespace

AreaReferenceSurfaceCache::AreaReferenceSurfaceCache() = default;
AreaReferenceSurfaceCache::~AreaReferenceSurfaceCache() = default;

void AreaReferenceSurfaceCache::Resolve(const ECS::World& world, std::vector<ResolvedModifier>& modifiers,
                                        JobSystem::WorkStealingThreadPool* jobs)
{
    struct PendingBuild
    {
        AreaReferenceSurface* Surface = nullptr;
        AreaSurfaceInputs Inputs;
    };
    std::vector<PendingBuild> builds;
    std::vector<ECS::EntityHandle> seen;

    for (ResolvedModifier& area : modifiers)
    {
        if (!GradesToItsRim(area) || !HasFiniteBounds(area))
            continue;
        AreaSurfaceInputs inputs{};
        BuildRoutePolyline(*area.ResolvedSpline, area.StationSpacing, area.Falloff, inputs.Rim);
        if (inputs.Rim.Stations.size() < 4)
            continue; // Fewer than three distinct rim points enclose nothing to fill.
        inputs.Openings = OpeningsOf(world, area, inputs.Rim, modifiers);
        inputs.MinX = area.BoundsMinX;
        inputs.MinZ = area.BoundsMinZ;
        inputs.MaxX = area.BoundsMaxX;
        inputs.MaxZ = area.BoundsMaxZ;
        inputs.Spacing = std::max(area.StationSpacing, kMinRouteStationSpacing);

        std::unique_ptr<AreaReferenceSurface>& surface = m_Surfaces.GetOrInsert(area.Entity);
        if (!surface || surface->GetInputKey() != AreaReferenceSurface::HashInputs(inputs))
        {
            surface = std::make_unique<AreaReferenceSurface>();
            builds.push_back({surface.get(), std::move(inputs)});
        }
        area.AreaSurface = surface.get();
        seen.push_back(area.Entity);
    }

    m_Surfaces.EraseIf([&seen](ECS::EntityHandle entity, const std::unique_ptr<AreaReferenceSurface>&) {
        return std::find(seen.begin(), seen.end(), entity) == seen.end();
    });

    if (builds.empty())
        return;
    JobSystem::ParallelFor(jobs, builds.size(),
                           [&builds](std::size_t i) { builds[i].Surface->Build(builds[i].Inputs); });
    m_BuildCount += static_cast<uint32>(builds.size());
}

} // namespace GameEngine::TerrainECS
