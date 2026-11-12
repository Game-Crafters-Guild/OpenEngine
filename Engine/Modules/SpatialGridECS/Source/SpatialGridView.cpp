#include "SpatialGridECS/SpatialGridView.h"

#include "SpatialGrid/Grid.h"
#include "SpatialGridIds.h"
#include "SpatialGridState.h"
#include "ECS/ECSTemplates.h"
#include "Memory/ScratchScope.h"

#include <algorithm>

namespace GameEngine::SpatialGridECS
{

namespace
{

SpatialGrid::GridFilter ToGridFilter(const SpatialGridFilter& filter)
{
    return {filter.Mask, filter.Ignore.IsValid() ? ToGridId(filter.Ignore) : SpatialGrid::GridFilter::kNoId};
}

// Runs `query` into the calling thread's scratch, then writes the hits to `out` with their entities.
template <typename Query>
uint32 Forward(const SpatialGrid::Grid* grid, std::span<SpatialGridHit> out, Query&& query)
{
    if (grid == nullptr)
        return 0u;
    Memory::ScratchScope scratch;
    const std::span<SpatialGrid::GridHit> hits = scratch.AllocateArrayForOverwrite<SpatialGrid::GridHit>(out.size());
    const uint32 matched = query(*grid, hits);
    const size_t kept = std::min<size_t>(matched, out.size());
    for (size_t i = 0; i < kept; ++i)
        out[i] = {ToEntity(hits[i].Id), hits[i].DistanceSquared};
    return matched;
}

} // namespace

SpatialGridView::SpatialGridView(const ECS::World& world)
{
    if (const Components::SpatialGridState* state = world.GetSingleton<Components::SpatialGridState>())
        m_Grid = state->Grid;
}

uint32 SpatialGridView::Sphere(const Mathematics::WorldPosition& center, float64 radius,
                               const SpatialGridFilter& filter, std::span<SpatialGridHit> out) const
{
    return Forward(m_Grid, out, [&](const SpatialGrid::Grid& grid, std::span<SpatialGrid::GridHit> hits)
                   { return grid.Sphere(center, radius, ToGridFilter(filter), hits); });
}

uint32 SpatialGridView::Cylinder(const Mathematics::WorldPosition& center, float64 radius, float64 halfHeight,
                                 const SpatialGridFilter& filter, std::span<SpatialGridHit> out) const
{
    return Forward(m_Grid, out, [&](const SpatialGrid::Grid& grid, std::span<SpatialGrid::GridHit> hits)
                   { return grid.Cylinder(center, radius, halfHeight, ToGridFilter(filter), hits); });
}

uint32 SpatialGridView::Box(const Mathematics::WorldPosition& center, const Mathematics::Vector3& halfExtents,
                            const SpatialGridFilter& filter, std::span<SpatialGridHit> out) const
{
    return Forward(m_Grid, out, [&](const SpatialGrid::Grid& grid, std::span<SpatialGrid::GridHit> hits)
                   { return grid.Box(center, halfExtents, ToGridFilter(filter), hits); });
}

uint32 SpatialGridView::Cone(const Mathematics::WorldPosition& apex, const Mathematics::Vector3& direction,
                             float64 range, float32 halfAngle, const SpatialGridFilter& filter,
                             std::span<SpatialGridHit> out) const
{
    return Forward(m_Grid, out, [&](const SpatialGrid::Grid& grid, std::span<SpatialGrid::GridHit> hits)
                   { return grid.Cone(apex, direction, range, halfAngle, ToGridFilter(filter), hits); });
}

uint32 SpatialGridView::Segment(const Mathematics::WorldPosition& start, const Mathematics::WorldPosition& end,
                                float64 radius, const SpatialGridFilter& filter,
                                std::span<SpatialGridHit> out) const
{
    return Forward(m_Grid, out, [&](const SpatialGrid::Grid& grid, std::span<SpatialGrid::GridHit> hits)
                   { return grid.Segment(start, end, radius, ToGridFilter(filter), hits); });
}

uint32 SpatialGridView::Nearest(const Mathematics::WorldPosition& center, float64 maxRadius,
                                const SpatialGridFilter& filter, std::span<SpatialGridHit> out) const
{
    return Forward(m_Grid, out, [&](const SpatialGrid::Grid& grid, std::span<SpatialGrid::GridHit> hits)
                   { return grid.Nearest(center, maxRadius, ToGridFilter(filter), hits); });
}

uint32 SpatialGridView::Count(const Mathematics::WorldPosition& center, float64 radius,
                              const SpatialGridFilter& filter) const
{
    return m_Grid ? m_Grid->Count(center, radius, ToGridFilter(filter)) : 0u;
}

} // namespace GameEngine::SpatialGridECS
