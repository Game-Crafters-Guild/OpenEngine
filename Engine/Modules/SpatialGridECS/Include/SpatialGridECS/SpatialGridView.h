#pragma once

#include "SpatialGridECS/SpatialGridFilter.h"
#include "SpatialGridECS/SpatialGridHit.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
#include "Types/Types.h"

#include <span>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::SpatialGrid
{
class Grid;
}

namespace GameEngine::SpatialGridECS
{

/// The queries of a world's spatial grid as the build left it at the start of this step; no build means no movers.
/// Take a view once per update for the system's jobs: the lookup takes the world's lock, a query takes none. A hit
/// can name an entity that a later system destroyed. Hits follow SpatialGrid::Grid's order, by entity index.
class SpatialGridView
{
public:
    explicit SpatialGridView(const ECS::World& world);

    /// The movers that the sphere overlaps.
    uint32 Sphere(const Mathematics::WorldPosition& center, float64 radius, const SpatialGridFilter& filter,
                  std::span<SpatialGridHit> out) const;
    /// The movers that the upright cylinder overlaps. The axis is +Y; an infinite `halfHeight` ignores
    /// height.
    uint32 Cylinder(const Mathematics::WorldPosition& center, float64 radius, float64 halfHeight,
                    const SpatialGridFilter& filter, std::span<SpatialGridHit> out) const;
    /// The movers that the axis-aligned box overlaps.
    uint32 Box(const Mathematics::WorldPosition& center, const Mathematics::Vector3& halfExtents,
               const SpatialGridFilter& filter, std::span<SpatialGridHit> out) const;
    /// The movers that overlap the solid cone: the points in `range` of the apex and in `halfAngle` of
    /// `direction`. `direction` is a unit vector; `halfAngle` is in radians, in (0, pi/2].
    uint32 Cone(const Mathematics::WorldPosition& apex, const Mathematics::Vector3& direction, float64 range,
                float32 halfAngle, const SpatialGridFilter& filter, std::span<SpatialGridHit> out) const;
    /// The movers that a sphere of `radius` swept from `start` to `end` overlaps, nearest the start first.
    uint32 Segment(const Mathematics::WorldPosition& start, const Mathematics::WorldPosition& end, float64 radius,
                   const SpatialGridFilter& filter, std::span<SpatialGridHit> out) const;
    /// The movers that overlap the sphere of `maxRadius`, nearest first; `out.size()` is the k.
    uint32 Nearest(const Mathematics::WorldPosition& center, float64 maxRadius, const SpatialGridFilter& filter,
                   std::span<SpatialGridHit> out) const;
    /// The number of movers that the sphere overlaps.
    uint32 Count(const Mathematics::WorldPosition& center, float64 radius, const SpatialGridFilter& filter) const;

private:
    const SpatialGrid::Grid* m_Grid = nullptr;
};

} // namespace GameEngine::SpatialGridECS
