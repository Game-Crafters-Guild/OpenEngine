#include "SmartShape2D/ShapeExplosion.h"

#include "SmartShape2D/SmartShape.h"
#include "SmartShape2D/SmartShapeCut.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "Mathematics/MatrixOps.h"
#include "PhysicsECS/PhysicsWorldService.h"

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace GameEngine::SmartShape2D
{
namespace
{
bool MakeLocalCut(const Components::WorldTransform& transform, Mathematics::Vector3 center,
                  float radius, Components::SmartShapeCut& cut)
{
    constexpr float kMinimumCutArea = 1.0e-8f;
    const auto inverse = Mathematics::Inverse(Mathematics::Matrix4x4::FromColumnMajor(transform.matrix));
    const auto local = inverse.TransformPoint(center);
    const auto x = inverse.TransformPoint(center + Mathematics::Vector3{radius, 0.0f, 0.0f}) - local;
    const auto y = inverse.TransformPoint(center + Mathematics::Vector3{0.0f, radius, 0.0f}) - local;
    const float area = x.x * y.y - x.y * y.x;
    if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(area) ||
        std::abs(area) < kMinimumCutArea)
        return false;
    cut = {{local.x, local.y}, {x.x, x.y}, {y.x, y.y}};
    return true;
}
}

uint32 ExplodeShapes(ECS::World& world, Mathematics::Vector3 center, float radius)
{
    if (!std::isfinite(radius) || radius <= 0.0f || !std::isfinite(center.x) ||
        !std::isfinite(center.y) || !std::isfinite(center.z))
        return 0;
    std::vector<std::pair<ECS::EntityHandle, Components::SmartShapeCut>> cuts;
    world.Query<ECS::Read<Components::SmartShape>, ECS::Read<Components::WorldTransform>>()
        .Each([&cuts, center, radius](ECS::EntityHandle entity, const Components::SmartShape& shape,
                                     const Components::WorldTransform& transform)
        {
            if (!shape.Destructible)
                return;
            Components::SmartShapeCut cut;
            if (MakeLocalCut(transform, center, radius, cut))
                cuts.emplace_back(entity, cut);
        });
    for (const auto& [owner, cut] : cuts)
    {
        const auto entity = world.CreateEntity();
        Components::Name name{};
        std::strncpy(name.value, "Explosion cut", sizeof(name.value) - 1);
        world.AddComponentImmediate(entity, name);
        world.AddComponentImmediate(entity, Components::Parent{owner});
        world.AddComponentImmediate(entity, Components::RuntimeOnlyEntity{});
        world.AddComponentImmediate(entity, cut);
    }
    if (!cuts.empty())
    {
        if (auto* physics = PhysicsECS::PhysicsWorldService::TryGet())
        {
            Physics::AABB bounds;
            bounds.min = {center.x - radius, center.y - radius, center.z - radius};
            bounds.max = {center.x + radius, center.y + radius, center.z + radius};
            physics->ActivateBodiesInAABB(bounds);
        }
    }
    return static_cast<uint32>(cuts.size());
}

void ResetShapeDestruction(ECS::World& world)
{
    std::vector<ECS::EntityHandle> cuts;
    world.Query<ECS::Read<Components::SmartShapeCut>>().IncludeDisabled()
        .Each([&cuts](ECS::EntityHandle entity, const Components::SmartShapeCut&) { cuts.push_back(entity); });
    for (const auto entity : cuts)
        world.DestroyEntityImmediate(entity);
}
}
