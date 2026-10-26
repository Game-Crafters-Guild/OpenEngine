#include "SmartShape2D/ShapeRuntime.h"

#include "SmartShape2D/ShapeGeometry.h"
#include "SmartShape2D/ShapeAnchor.h"
#include "SmartShape2D/ShapeCollision.h"
#include "SmartShape2D/ShapeCollisionOwner.h"
#include "SmartShape2D/ShapeDestruction.h"
#include "PhysicsECS/MeshColliderData.h"
#include "PhysicsECS/Components/MeshColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"

#include <algorithm>
#include <cstring>

namespace GameEngine::SmartShape2D
{
namespace
{
void UpdateShapeAnchor(ECS::World& world, SplineECS::SplineService& splines,
    const Components::SmartShapeAnchor& anchor, const Components::Parent& parent, Components::Transform& transform)
{
    if (!world.IsValid(parent.Entity) || !world.HasComponent<Components::SmartShape>(parent.Entity))
        return;
    const auto* reference = world.GetComponent<Components::SplineComponent>(parent.Entity);
    const auto* parentTransform = world.GetComponent<Components::WorldTransform>(parent.Entity);
    if (!reference || !parentTransform)
        return;
    const auto* spline = splines.GetSplineData({reference->SplineDataIndex, reference->SplineDataGeneration});
    if (!spline)
        return;
    const auto matrix = EvaluateAnchor(*spline, anchor, Mathematics::Matrix4x4::FromColumnMajor(parentTransform->matrix));
    std::copy(matrix.Data(), matrix.Data() + 16, transform.matrix);
}
}

ShapeRuntime& ShapeRuntime::Get()
{
    static ShapeRuntime runtime;
    return runtime;
}

void ShapeRuntime::Initialize(Engine::Renderer::RenderServices* services)
{
    m_Services = services;
}

void ShapeRuntime::ReleaseGeneratedResources(Components::GeneratedShapeResources& resources)
{
    auto* services = Get().m_Services;
    if (services && !resources.MeshKey.IsNull())
        services->GetMeshGPURegistry().UnregisterSubmesh({resources.MeshKey, resources.Part});
    PhysicsECS::MeshColliderData::Release(resources.CollisionData);
    resources = {};
}

void ShapeRuntime::Release(ECS::World* world, ShapeState& state)
{
    for (auto& child : state.Children)
    {
        if (world && world->IsValid(child))
            world->DestroyEntityImmediate(child);
        child = {};
    }
    if (world && world->IsValid(state.CollisionEntity))
        RemoveShapeCollision(*world, state.CollisionEntity);
    state.CollisionEntity = {};
    PhysicsECS::MeshColliderData::Release(state.CollisionData);
    state.CollisionData = {};
    if (m_Services && !state.MeshKey.IsNull())
        m_Services->GetMeshGPURegistry().UnregisterModel(state.MeshKey);
    state.Built = false;
}

void ShapeRuntime::Shutdown()
{
    for (auto& [worldId, world] : m_Worlds)
    {
        (void)worldId;
        for (auto& [entityId, shape] : world.Shapes)
        {
            (void)entityId;
            Release(nullptr, shape);
        }
    }
    m_Worlds.Clear();
    m_Services = nullptr;
}

void ShapeRuntime::Rebuild(ECS::World& world, ECS::EntityHandle owner, const Spline::SplineData& spline,
                           const Components::SmartShape& settings, ShapeState& state)
{
    // Catmull-Rom auto tangents can still be dirty before SplineExtraction's tick.
    auto source = spline;
    if (source.Dirty)
        Spline::RebuildSplineCache(source);
    auto geometry = BuildShape(source, settings, state.Overrides, state.Textures, state.WorldMatrix);
    if (!geometry.Error.empty())
    {
        Release(&world, state);
        Logger::Log::Warning("Smart Shape 2D entity {}: {}", owner.id, geometry.Error);
        state.Built = true; // Retry on the next authored edit, not on every frame.
        return;
    }
    constexpr std::array<const char*, 11> kNames{
        "Fill", "Top edge", "Right edge", "Bottom edge", "Left edge", "Inner corners", "Outer corners", "Start taper", "End taper", "Sharp start taper", "Sharp end taper"};
    const auto cuts = BuildShapeCuts(state.Cuts);
    for (auto& mesh : geometry.Parts)
        CutShapeMesh(mesh, cuts);
    if (m_Services)
    {
        auto& registry = m_Services->GetMeshGPURegistry();
        Rendering::MeshGPURegistry::TableScope scope(registry);
        for (size_t i = geometry.Parts.size(); i < state.Children.size(); ++i)
        {
            if (world.IsValid(state.Children[i]))
                world.DestroyEntityImmediate(state.Children[i]);
            registry.UnregisterSubmesh({state.MeshKey, static_cast<uint32>(i)});
        }
        state.Children.resize(geometry.Parts.size());
        for (size_t i = 0; i < geometry.Parts.size(); ++i)
        {
            const auto& mesh = geometry.Parts[i];
            auto& child = state.Children[i];
            if (mesh.Indices.empty())
            {
                if (world.IsValid(child))
                    world.DestroyEntityImmediate(child);
                child = {};
                registry.UnregisterSubmesh({state.MeshKey, static_cast<uint32>(i)});
                continue;
            }
            const auto handle = registry.RegisterSubmesh({state.MeshKey, static_cast<uint32>(i)}, mesh, true);
            GUID material = geometry.Materials[i].ToGuid();
            if (material.IsNull())
                material = Engine::Renderer::PrimitiveGenerator::DefaultMaterialGuid();
            else
                Engine::Renderer::RegisterOneStandaloneMaterial(*m_Services, material);
            Components::MeshRenderer renderer{};
            renderer.meshGpuHandleId = static_cast<uint64>(handle);
            renderer.materialAssetGuid.Set(material);
            renderer.shadowCasting = Components::ShadowCasting::Off;
            renderer.motionVectors = false;
            Components::LocalBounds bounds{};
            bounds.Box = Mathematics::BoundingBox::FromMinMax(
                {mesh.MinBounds[0], mesh.MinBounds[1], mesh.MinBounds[2]},
                {mesh.MaxBounds[0], mesh.MaxBounds[1], mesh.MaxBounds[2]});
            bounds.CastShadows = false;
            if (!world.IsValid(child))
            {
                child = world.CreateEntity();
                Components::Name name{};
                std::strncpy(name.value, i < kNames.size() ? kNames[i] : "Edge override", sizeof(name.value) - 1);
                world.AddComponentImmediate(child, name);
                world.AddComponentImmediate(child, Components::Transform{});
                world.AddComponentImmediate(child, Components::Parent{owner});
                world.AddComponentImmediate(child, Components::RuntimeOnlyEntity{});
                world.AddComponentImmediate(child, Components::MeshGPUData{});
            }
            world.AddComponentImmediate(child, renderer);
            world.AddComponentImmediate(child, bounds);
            Components::GeneratedShapeResources resources{};
            resources.Owner = owner;
            resources.MeshKey = state.MeshKey;
            resources.Part = static_cast<uint32>(i);
            world.AddComponentImmediate(child, resources);
        }
    }
    if (settings.CollisionEnabled)
    {
        auto collisionSettings = settings;
        collisionSettings.FillEnabled = true;
        collisionSettings.EdgesEnabled = true;
        auto collisionGeometry = BuildShape(source, collisionSettings, state.Overrides, state.Textures, state.WorldMatrix);
        auto collisionMesh = BuildCollision(collisionGeometry, source.IsEffectivelyClosed(), settings, cuts);
        const auto data = PhysicsECS::MeshColliderData::Register(std::move(collisionMesh));
        if (SetShapeCollision(world, owner, data))
        {
            state.CollisionEntity = owner;
            state.CollisionData = data;
        }
        else
        {
            PhysicsECS::MeshColliderData::Release(data);
            RemoveShapeCollision(world, owner);
            state.CollisionEntity = {};
            state.CollisionData = {};
        }
    }
    else
    {
        RemoveShapeCollision(world, owner);
        state.CollisionEntity = {};
        state.CollisionData = {};
    }
    state.Built = true;
}

void ShapeRuntime::AdoptGeneratedResource(WorldState& cache, ECS::EntityHandle entity,
    const Components::GeneratedShapeResources& resources)
{
    auto& state = cache.Shapes.GetOrInsert(resources.Owner.id);
    if (resources.Part != 0xFFFFFFFFu)
    {
        state.MeshKey = resources.MeshKey;
        if (state.Children.size() <= resources.Part)
            state.Children.resize(static_cast<size_t>(resources.Part) + 1);
        state.Children[resources.Part] = entity;
    }
    else
    {
        state.CollisionEntity = entity;
        state.CollisionData = resources.CollisionData;
    }
}

void ShapeRuntime::AdoptGeneratedResources(ECS::World& world, WorldState& cache)
{
    world.Query<ECS::Read<Components::GeneratedShapeResources>>().IncludeDisabled()
        .Each([&cache](ECS::EntityHandle entity, const Components::GeneratedShapeResources& resources)
        {
            AdoptGeneratedResource(cache, entity, resources);
        });
}

void ShapeRuntime::Update(ECS::World& world)
{
    auto* splines = SplineECS::SplineService::TryGet();
    if (!splines)
        return;
    auto& cache = m_Worlds.GetOrInsert(world.GetWorldId());
    if (!cache.HooksRegistered)
    {
        world.RegisterOnRemove<Components::GeneratedShapeResources>(&ReleaseGeneratedResources);
        // Reuse output left by a replaced package module. Resource keys and ownership are in
        // engine-owned ECS storage, so a module reload does not double-spawn geometry.
        AdoptGeneratedResources(world, cache);
        cache.HooksRegistered = true;
        cache.ResetGeneration = world.GetLifecycleResetGeneration();
    }
    const uint64 reset = world.GetLifecycleResetGeneration();
    const uint64 device = m_Services && m_Services->GetDevice() ? m_Services->GetDevice()->GetDeviceRebuildGeneration() : 0;
    if (cache.ResetGeneration != reset)
    {
        // Clear recycles entity generations. Never destroy old handles in the new world.
        for (auto& [id, state] : cache.Shapes)
        {
            (void)id;
            Release(nullptr, state);
        }
        cache.Shapes.Clear();
        cache.ResetGeneration = reset;
        // Play-mode snapshots restore runtime children too. Reuse their ownership tags
        // before rebuilding; otherwise the restored outputs become orphan duplicates.
        AdoptGeneratedResources(world, cache);
    }
    const bool deviceChanged = cache.DeviceGeneration != device;
    cache.DeviceGeneration = device;
    for (auto& [id, state] : cache.Shapes)
    {
        (void)id;
        state.Visited = false;
    }
    m_Candidates.clear();
    world.Query<ECS::Read<Components::SmartShape>, ECS::Read<Components::SplineComponent>>()
        .Each([this](ECS::EntityHandle entity, const Components::SmartShape&, const Components::SplineComponent&)
              { m_Candidates.push_back(entity); });
    m_Edges.clear();
    world.Query<ECS::Read<Components::SmartShapeEdge>, ECS::Read<Components::Parent>>()
        .Each([this](const Components::SmartShapeEdge& edge, const Components::Parent& parent)
              { m_Edges.emplace_back(parent.Entity, edge); });
    m_Textures.clear();
    world.Query<ECS::Read<Components::SmartShapeTexture>, ECS::Read<Components::Parent>>()
        .Each([this](const Components::SmartShapeTexture& texture, const Components::Parent& parent)
              { m_Textures.emplace_back(parent.Entity, texture); });
    m_Cuts.clear();
    world.Query<ECS::Read<Components::SmartShapeCut>, ECS::Read<Components::Parent>>()
        .Each([this](const Components::SmartShapeCut& cut, const Components::Parent& parent)
              { m_Cuts.emplace_back(parent.Entity, cut); });
    // Structural writes happen after the query has released its archetype views.
    for (const auto entity : m_Candidates)
    {
        const auto settings = *world.GetComponent<Components::SmartShape>(entity);
        const auto reference = *world.GetComponent<Components::SplineComponent>(entity);
        const auto* spline = splines->GetSplineData({reference.SplineDataIndex, reference.SplineDataGeneration});
        if (!spline)
            continue;
        auto& state = cache.Shapes.GetOrInsert(entity.id);
        state.Visited = true;
        m_Overrides.clear();
        for (const auto& [parent, edge] : m_Edges)
        {
            if (parent == entity)
                m_Overrides.push_back(edge);
        }
        std::sort(m_Overrides.begin(), m_Overrides.end(), [](const auto& left, const auto& right)
                  { return left.Priority != right.Priority ? left.Priority > right.Priority : left.PointIndex < right.PointIndex; });
        m_TextureSet.clear();
        for (const auto& [parent, texture] : m_Textures)
        {
            if (parent == entity)
                m_TextureSet.push_back(texture);
        }
        std::stable_sort(m_TextureSet.begin(), m_TextureSet.end(), [](const auto& left, const auto& right)
                        { return left.Index < right.Index; });
        Mathematics::Matrix4x4 matrix;
        if (const auto* transform = world.GetComponent<Components::WorldTransform>(entity))
            matrix = Mathematics::Matrix4x4::FromColumnMajor(transform->matrix);
        const bool transformChanged = !std::equal(matrix.Data(), matrix.Data() + 16, state.WorldMatrix.Data());
        m_ShapeCuts.clear();
        if (settings.Destructible)
        {
            for (const auto& [parent, cut] : m_Cuts)
            {
                if (parent == entity)
                    m_ShapeCuts.push_back(cut);
            }
        }
        const bool missing = (state.CollisionEntity.IsValid() && (!world.IsValid(state.CollisionEntity) ||
                              !world.HasComponent<Components::MeshColliderShape>(state.CollisionEntity))) || std::any_of(state.Children.begin(), state.Children.end(), [&world](ECS::EntityHandle child)
                                        { return child.IsValid() && !world.IsValid(child); });
        if (!state.Built || deviceChanged || missing || transformChanged || state.Cuts != m_ShapeCuts || state.Textures != m_TextureSet || state.Overrides != m_Overrides || state.Settings != settings || state.SplineVersion != spline->Version ||
            state.SplineIndex != reference.SplineDataIndex || state.SplineGeneration != reference.SplineDataGeneration)
        {
            if (state.MeshKey.IsNull())
                state.MeshKey = GUID::Derive(GUID::Null(), "smartshape2d/" + std::to_string(world.GetWorldId()) + "/" +
                                            std::to_string(reset) + "/" + std::to_string(entity.id));
            state.Overrides = m_Overrides;
            state.Textures = m_TextureSet;
            state.Cuts = m_ShapeCuts;
            state.WorldMatrix = matrix;
            state.Settings = settings;
            state.SplineVersion = spline->Version;
            state.SplineIndex = reference.SplineDataIndex;
            state.SplineGeneration = reference.SplineDataGeneration;
            Rebuild(world, entity, *spline, settings, state);
        }
    }
    world.Query<ECS::Read<Components::SmartShapeAnchor>, ECS::Read<Components::Parent>, ECS::Write<Components::Transform>>()
        .Each([&world, splines](const Components::SmartShapeAnchor& anchor, const Components::Parent& parent,
                              Components::Transform& transform)
        {
            UpdateShapeAnchor(world, *splines, anchor, parent, transform);
        });
    m_Retired.clear();
    for (auto& [id, state] : cache.Shapes)
    {
        if (!state.Visited)
        {
            Release(&world, state);
            m_Retired.push_back(id);
        }
    }
    for (const auto id : m_Retired)
        cache.Shapes.Erase(id);
}
} // namespace GameEngine::SmartShape2D
