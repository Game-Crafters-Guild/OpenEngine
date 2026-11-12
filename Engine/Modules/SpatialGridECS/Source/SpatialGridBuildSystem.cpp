#include "SpatialGridECS/Systems/SpatialGridBuildSystem.h"

#include "Components/Hierarchy.h"
#include "Components/SpatialGridMover.h"
#include "Components/SpatialGridSettings.h"
#include "Components/Transform.h"
#include "SpatialGrid/Grid.h"
#include "SpatialGridIds.h"
#include "SpatialGridState.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SpatialGridECS
{

namespace
{

constexpr float64 kMinCellSize = 1.0;

bool IsValidCellSize(float32 setting)
{
    return std::isfinite(setting) && setting >= kMinCellSize;
}

// Meters: the largest power of two at or under `setting`. A setting that is not finite gives the
// default size, and a setting under 1 gives 1.
float64 CellSizeOf(float32 setting)
{
    if (!std::isfinite(setting))
        return Components::SpatialGridSettings{}.CellSize;
    return std::ldexp(1.0, std::ilogb(std::max(static_cast<float64>(setting), kMinCellSize)));
}

// A mover with a parent takes its WorldTransform; a mover with an empty Parent is a root.
const Mathematics::WorldPosition& PlaceOf(const Components::Transform& local,
                                          const Components::WorldTransform& transform,
                                          const Components::Parent& parent)
{
    return parent.Entity.IsValid() ? transform.Position : local.Position;
}

} // namespace

SpatialGridBuildSystem::SpatialGridBuildSystem() = default;
SpatialGridBuildSystem::~SpatialGridBuildSystem()
{
    ReleaseState();
}

void SpatialGridBuildSystem::ReleaseState()
{
    if (m_World == nullptr || !ECS::World::FindLiveEntityHandleEpoch(m_WorldId).has_value())
        return;
    const Components::SpatialGridState* state = m_World->GetSingleton<Components::SpatialGridState>();
    if (state != nullptr && state->Grid == m_Grid.get())
        m_World->SetSingleton(Components::SpatialGridState{});
}

void SpatialGridBuildSystem::Update(ECS::World& world, float32)
{
    if (m_World == nullptr || m_WorldId != world.GetWorldId())
    {
        ReleaseState();
        m_World = &world;
        m_WorldId = world.GetWorldId();
    }

    const Components::SpatialGridSettings* settings = world.GetSingleton<Components::SpatialGridSettings>();
    const float32 setting = settings ? settings->CellSize : Components::SpatialGridSettings{}.CellSize;
    const float64 cellSize = CellSizeOf(setting);
    const bool validCellSize = IsValidCellSize(setting);
    const bool rebuild = !m_Grid || cellSize != m_CellSize;
    if (!validCellSize && (rebuild || m_ValidCellSize))
    {
        Logger::Log::Warning("SpatialGridBuildSystem: SpatialGridSettings.CellSize is {}, so the grid uses cells of "
                             "{} meters. Set CellSize to a finite value of 1 or more.",
                             setting, cellSize);
    }
    m_ValidCellSize = validCellSize;
    if (rebuild)
    {
        m_Grid = std::make_unique<SpatialGrid::Grid>(cellSize);
        m_CellSize = cellSize;
    }

    // A root mover takes its own Transform, so a write in one step reaches the next step. A child mover
    // takes its WorldTransform, which the hierarchy update writes once per frame.
    m_Movers.clear();
    m_RefusedMovers = 0u;
    world.Query<ECS::Read<Components::SpatialGridMover>, ECS::Read<Components::Transform>>()
        .Without<Components::Parent>()
        .Each([this](ECS::EntityHandle entity, const Components::SpatialGridMover& mover,
                     const Components::Transform& local) { AddMover(entity, mover, local.Position); });
    world.Query<ECS::Read<Components::SpatialGridMover>, ECS::Read<Components::Transform>,
                ECS::Read<Components::WorldTransform>, ECS::Read<Components::Parent>>()
        .Each([this](ECS::EntityHandle entity, const Components::SpatialGridMover& mover,
                     const Components::Transform& local, const Components::WorldTransform& transform,
                     const Components::Parent& parent)
              { AddMover(entity, mover, PlaceOf(local, transform, parent)); });
    m_Grid->Build(m_Movers);
    if (m_RefusedMovers != m_PreviousRefusedMovers && m_RefusedMovers > 0u)
    {
        Logger::Log::Warning("SpatialGridBuildSystem: movers not in the grid: {}. A mover's Radius must be 0 or more "
                             "and finite.",
                             m_RefusedMovers);
    }
    m_PreviousRefusedMovers = m_RefusedMovers;

    // World::Clear removes singletons, so the build sets the state again when it is missing.
    const Components::SpatialGridState* state = world.GetSingleton<Components::SpatialGridState>();
    if (state == nullptr || state->Grid != m_Grid.get() || state->RefusedMovers != m_RefusedMovers)
        world.SetSingleton(Components::SpatialGridState{m_Grid.get(), m_RefusedMovers});
}

void SpatialGridBuildSystem::AddMover(ECS::EntityHandle entity, const Components::SpatialGridMover& mover,
                                      const Mathematics::WorldPosition& position)
{
    // A NaN radius fails the compare.
    if (!(mover.Radius >= 0.0f && std::isfinite(mover.Radius)))
    {
        ++m_RefusedMovers;
        return;
    }
    m_Movers.push_back({position, mover.Radius, mover.Mask, ToGridId(entity)});
}

void AddSpatialGridSystemsToSchedule(ECS::SystemScheduleBuilder& schedule)
{
    // Ahead of the AI systems (Early 10 to 16), which query the grid.
    schedule.Add<SpatialGridBuildSystem>("SpatialGridBuildSystem", ECS::SystemPhase::Early, 8, {});
}

} // namespace GameEngine::SpatialGridECS
