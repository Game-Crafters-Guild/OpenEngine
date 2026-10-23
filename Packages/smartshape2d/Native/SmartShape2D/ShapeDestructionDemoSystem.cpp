#include "SmartShape2D/ShapeDestructionDemoSystem.h"

#include "SmartShape2D/ShapeDestructionDemo.h"
#include "SmartShape2D/ShapeExplosion.h"
#include "SmartShape2D/ShapeExplosionFlash.h"
#include "SmartShape2D/SmartShape.h"
#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "Engine/GameUI/GameplayUI.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace GameEngine::SmartShape2D
{
namespace
{
void ShowExplosion(ECS::World& world, Mathematics::Vector3 position,
                   const Components::ShapeDestructionDemo& demo)
{
    auto* splines = SplineECS::SplineService::TryGet();
    if (!splines)
        return;
    const auto handle = splines->CreateSpline(Spline::SplineType::Linear, true);
    auto* data = splines->GetSplineData(handle);
    constexpr uint32 kFlashSegments = 32;
    for (uint32 index = 0; index < kFlashSegments; ++index)
    {
        const float angle = static_cast<float>(index) * 2.0f * std::numbers::pi_v<float> / kFlashSegments;
        data->AddPoint({std::cos(angle) * demo.Radius, std::sin(angle) * demo.Radius, 0.0f}, 1.0f);
    }
    const auto entity = world.CreateEntity();
    position.z = -2.0f;
    const auto transform = Components::Transform::FromTRS(position, {}, {1.0f, 1.0f, 1.0f});
    Components::SplineComponent reference;
    reference.SplineDataIndex = handle.Index();
    reference.SplineDataGeneration = handle.Generation();
    reference.ShowWidthEnvelope = false;
    Components::SmartShape shape;
    shape.FillMaterial = demo.FlashMaterial;
    shape.EdgesEnabled = false;
    shape.CollisionEnabled = false;
    Components::Name name{};
    std::strncpy(name.value, "Explosion flash", sizeof(name.value) - 1);
    world.AddComponentImmediate(entity, name);
    world.AddComponentImmediate(entity, transform);
    world.AddComponentImmediate(entity, reference);
    world.AddComponentImmediate(entity, shape);
    world.AddComponentImmediate(entity, Components::ShapeExplosionFlash{});
    world.AddComponentImmediate(entity, Components::RuntimeOnlyEntity{});
}
}

void ShapeDestructionDemoSystem::Update(ECS::World& world, float32 deltaTime)
{
    if (!PhysicsECS::PhysicsWorldService::IsInitialized())
        return;
    std::vector<ECS::EntityHandle> expired;
    world.Query<ECS::Write<Components::ShapeExplosionFlash>>()
        .Each([&expired, deltaTime](ECS::EntityHandle entity, Components::ShapeExplosionFlash& flash)
        {
            flash.Remaining -= deltaTime;
            if (flash.Remaining <= 0.0f)
                expired.push_back(entity);
        });
    for (const auto entity : expired)
        world.DestroyEntityImmediate(entity);
    Components::ShapeDestructionDemo demo;
    ECS::EntityHandle controller;
    world.Query<ECS::Read<Components::ShapeDestructionDemo>>()
        .Each([&demo, &controller](ECS::EntityHandle entity, const Components::ShapeDestructionDemo& settings)
        {
            if (!controller.IsValid() || entity.id < controller.id)
            {
                controller = entity;
                demo = settings;
            }
        });
    if (!controller.IsValid())
        return;
    auto* input = EngineCore::GetInstance().GetRuntimeInput();
    auto* ui = GameUI::GetUIManager();
    auto* root = ui ? ui->GetRootElement() : nullptr;
    if (!input || !root)
        return;
    if (input->WasKeyPressed(Input::kKeyCode_R))
        ResetShapeDestruction(world);
    if (!input->WasMouseButtonPressed(Input::kMouseButton_Left))
        return;
    const float width = root->GetLayoutWidth();
    const float height = root->GetLayoutHeight();
    const auto pointer = input->GetMousePosition();
    if (width <= 0.0f || height <= 0.0f || pointer.x < 0.0f || pointer.y < 0.0f ||
        pointer.x >= width || pointer.y >= height)
        return;
    ECS::EntityHandle cameraEntity;
    Components::Camera camera;
    Mathematics::Matrix4x4 cameraMatrix;
    world.Query<ECS::Read<Components::Camera>, ECS::Read<Components::WorldTransform>>()
        .Each([&cameraEntity, &camera, &cameraMatrix](ECS::EntityHandle entity,
            const Components::Camera& candidate, const Components::WorldTransform& transform)
        {
            if (!candidate.Perspective && (!cameraEntity.IsValid() || entity.id < cameraEntity.id))
            {
                cameraEntity = entity;
                camera = candidate;
                cameraMatrix = Mathematics::Matrix4x4::FromColumnMajor(transform.matrix);
            }
        });
    if (!cameraEntity.IsValid())
        return;
    auto position = cameraMatrix.TransformPoint({(pointer.x / width - 0.5f) * camera.OrthographicSize * width / height,
                                               (0.5f - pointer.y / height) * camera.OrthographicSize, 0.0f});
    position.z = 0.0f;
    if (ExplodeShapes(world, position, demo.Radius) > 0)
        ShowExplosion(world, position, demo);
}
}
