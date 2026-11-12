#pragma once

#include "ECS/Systems.h"
#include "SpatialGrid/GridMover.h"

#include <memory>
#include <vector>

namespace GameEngine::ECS
{
class SystemScheduleBuilder;
}

namespace GameEngine::SpatialGrid
{
class Grid;
}

namespace GameEngine::Components
{
struct SpatialGridMover;
}

namespace GameEngine::SpatialGridECS
{

/// Builds the world's spatial grid from every SpatialGridMover once per simulation step: a root at its
/// Transform position, a child at its WorldTransform position. It is the only writer of the grid, and
/// no query runs during the build.
class SpatialGridBuildSystem : public ECS::ISystem
{
public:
    static constexpr ECS::SystemCadence Cadence = ECS::SystemCadence::Simulation;

    SpatialGridBuildSystem();
    /// Clears the grid state of the last world that the build wrote, when that world is alive. A view of
    /// that world then has no movers.
    ~SpatialGridBuildSystem() override;
    SpatialGridBuildSystem(const SpatialGridBuildSystem&) = delete;
    SpatialGridBuildSystem& operator=(const SpatialGridBuildSystem&) = delete;

    void Update(ECS::World& world, float32 deltaTime) override;
    const char* GetName() const override { return "SpatialGridBuildSystem"; }
    // The build writes the grid that every query reads, and may add the world's grid singleton.
    bool RequiresExclusiveUpdate() const override { return true; }

private:
    void ReleaseState();
    // Adds the mover to the build, or counts it as refused when its radius is NaN, infinite or negative.
    void AddMover(ECS::EntityHandle entity, const Components::SpatialGridMover& mover,
                  const Mathematics::WorldPosition& position);

    std::unique_ptr<SpatialGrid::Grid> m_Grid;
    float64 m_CellSize = 0.0;
    // The validity of the previous cell size setting. A change to an invalid setting logs one warning.
    bool m_ValidCellSize = true;
    std::vector<SpatialGrid::GridMover> m_Movers;
    uint32 m_RefusedMovers = 0u;
    // The refused count of the previous build. A change to a count above 0 logs one warning.
    uint32 m_PreviousRefusedMovers = 0u;
    // The last world that the build wrote. A world id is never used again, so the id tells if it is alive.
    ECS::World* m_World = nullptr;
    uint64 m_WorldId = 0;
};

/// Adds SpatialGridBuildSystem to the schedule: Early phase, order 8, ahead of the AI systems.
void AddSpatialGridSystemsToSchedule(ECS::SystemScheduleBuilder& schedule);

} // namespace GameEngine::SpatialGridECS
