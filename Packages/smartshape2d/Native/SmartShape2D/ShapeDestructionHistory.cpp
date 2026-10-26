#include "SmartShape2D/ShapeDestructionHistory.h"

#include "SmartShape2D/ShapeExplosion.h"
#include "SmartShape2D/SmartShape.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "PhysicsECS/PhysicsWorldService.h"

#include <utility>
#include <cstring>

namespace GameEngine::SmartShape2D
{
ShapeDestructionHistory::Snapshot ShapeDestructionHistory::Capture(ECS::World& world)
{
    Snapshot result;
    world.Query<ECS::Read<Components::SmartShapeCut>, ECS::Read<Components::Parent>>()
        .Each([&result](const Components::SmartShapeCut& cut, const Components::Parent& parent)
              { result.push_back({parent.Entity, cut}); });
    return result;
}

void ShapeDestructionHistory::Apply(ECS::World& world, const Snapshot& snapshot)
{
    ResetShapeDestruction(world);
    for (const auto& footprint : snapshot)
    {
        if (!world.IsValid(footprint.Owner) || !world.HasComponent<Components::SmartShape>(footprint.Owner))
            continue;
        const auto entity = world.CreateEntity();
        Components::Name name{};
        std::strncpy(name.value, "Explosion cut", sizeof(name.value) - 1);
        world.AddComponentImmediate(entity, name);
        world.AddComponentImmediate(entity, Components::Parent{footprint.Owner});
        world.AddComponentImmediate(entity, Components::RuntimeOnlyEntity{});
        world.AddComponentImmediate(entity, footprint.Cut);
        // Redo can remove support below a sleeping body, just like a new blast.
        const auto* transform = world.GetComponent<Components::WorldTransform>(footprint.Owner);
        auto* physics = PhysicsECS::PhysicsWorldService::TryGet();
        if (!transform || !physics)
            continue;
        const auto matrix = Mathematics::Matrix4x4::FromColumnMajor(transform->matrix);
        const auto& cut = footprint.Cut;
        const auto center = matrix.TransformPoint({cut.Center.x, cut.Center.y, 0.0f});
        const auto x = matrix.TransformPoint({cut.Center.x + cut.AxisX.x, cut.Center.y + cut.AxisX.y, 0.0f}) - center;
        const auto y = matrix.TransformPoint({cut.Center.x + cut.AxisY.x, cut.Center.y + cut.AxisY.y, 0.0f}) - center;
        const float radius = x.Length() + y.Length();
        Physics::AABB bounds;
        bounds.min = {center.x - radius, center.y - radius, center.z - radius};
        bounds.max = {center.x + radius, center.y + radius, center.z + radius};
        physics->ActivateBodiesInAABB(bounds);
    }
}

void ShapeDestructionHistory::Synchronize(ECS::World& world)
{
    auto current = Capture(world);
    if (m_WorldId == world.GetWorldId() && m_ResetGeneration == world.GetLifecycleResetGeneration() &&
        !m_States.empty() && current == m_States[m_Position])
        return;
    m_WorldId = world.GetWorldId();
    m_ResetGeneration = world.GetLifecycleResetGeneration();
    m_States.clear();
    m_States.push_back(std::move(current));
    m_Position = 0;
}

void ShapeDestructionHistory::Commit(ECS::World& world)
{
    auto current = Capture(world);
    if (current == m_States[m_Position])
        return;
    m_States.resize(m_Position + 1);
    m_States.push_back(std::move(current));
    constexpr size_t kMaximumStates = 129; // Current state plus 128 undo steps.
    if (m_States.size() > kMaximumStates)
        m_States.erase(m_States.begin());
    m_Position = m_States.size() - 1;
}

uint32 ShapeDestructionHistory::Explode(ECS::World& world, Mathematics::Vector3 center, float radius)
{
    Synchronize(world);
    const auto count = ExplodeShapes(world, center, radius);
    Commit(world);
    return count;
}

void ShapeDestructionHistory::Reset(ECS::World& world)
{
    Synchronize(world);
    ResetShapeDestruction(world);
    Commit(world);
}

bool ShapeDestructionHistory::Undo(ECS::World& world)
{
    Synchronize(world);
    if (m_Position == 0)
        return false;
    Apply(world, m_States[--m_Position]);
    m_States[m_Position] = Capture(world);
    return true;
}

bool ShapeDestructionHistory::Redo(ECS::World& world)
{
    Synchronize(world);
    if (m_Position + 1 >= m_States.size())
        return false;
    Apply(world, m_States[++m_Position]);
    m_States[m_Position] = Capture(world);
    return true;
}
}
