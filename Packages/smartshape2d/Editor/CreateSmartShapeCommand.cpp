#include "CreateSmartShapeCommand.h"

#include "SmartShape2D/SmartShape.h"
#include "SmartShape2D/SmartShapeEdge.h"
#include "SmartShape2D/SmartShapeAnchor.h"
#include "SmartShape2D/SmartShapeTexture.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "SplineECS/SplineService.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace GameEngine::SmartShape2D
{
CreateSmartShapeCommand::CreateSmartShapeCommand(const Editor::HierarchyCommandContext& context, ShapeCreationKind kind)
    : m_Context(context), m_Kind(kind)
{
}

void CreateSmartShapeCommand::Do()
{
    auto* world = m_Context.World;
    auto* service = SplineECS::SplineService::TryGet();
    if (!world || !service)
        return;
    if (!m_Entity.IsValid())
        m_Entity = world->CreateEntity();
    else if (m_Undone && !world->ReviveEntityImmediatePreserveHandle(m_Entity))
        return;
    if (m_Kind == ShapeCreationKind::EdgeOverride || m_Kind == ShapeCreationKind::Anchor || m_Kind == ShapeCreationKind::Texture)
    {
        Components::Name name{};
        std::strncpy(name.value, m_Kind == ShapeCreationKind::Anchor ? "Shape point anchor" :
            m_Kind == ShapeCreationKind::Texture ? "Shape texture entry" : "Shape edge override", sizeof(name.value) - 1);
        world->AddComponentImmediate(m_Entity, Components::Transform{});
        world->AddComponentImmediate(m_Entity, name);
        if (m_Kind == ShapeCreationKind::Anchor)
            world->AddComponentImmediate(m_Entity, Components::SmartShapeAnchor{});
        else if (m_Kind == ShapeCreationKind::Texture)
        {
            Components::SmartShapeTexture texture;
            world->Query<ECS::Read<Components::SmartShapeTexture>, ECS::Read<Components::Parent>>()
                .Each([this, &texture](const Components::SmartShapeTexture& entry, const Components::Parent& parent)
                {
                    if (parent.Entity == m_Context.TargetEntity && entry.Index < std::numeric_limits<uint32>::max())
                        texture.Index = std::max(texture.Index, entry.Index + 1);
                });
            world->AddComponentImmediate(m_Entity, texture);
        }
        else
            world->AddComponentImmediate(m_Entity, Components::SmartShapeEdge{});
        NotifyCreated();
        return;
    }
    const bool closed = m_Kind == ShapeCreationKind::Closed;
    const auto handle = service->CreateSpline(Spline::SplineType::Linear, closed);
    auto* spline = service->GetSplineData(handle);
    if (closed)
    {
        spline->AddPoint({-2.0f, -1.0f, 0.0f}, 1.0f);
        spline->AddPoint({2.0f, -1.0f, 0.0f}, 1.0f);
        spline->AddPoint({2.0f, 1.0f, 0.0f}, 1.0f);
        spline->AddPoint({-2.0f, 1.0f, 0.0f}, 1.0f);
    }
    else
    {
        spline->AddPoint({-2.0f, 0.0f, 0.0f}, 1.0f);
        spline->AddPoint({0.0f, 0.5f, 0.0f}, 1.0f);
        spline->AddPoint({2.0f, 0.0f, 0.0f}, 1.0f);
    }
    service->RebuildCache(handle);
    Components::Name name{};
    std::strncpy(name.value, closed ? "Smart Shape 2D" : "Smart Shape 2D Path", sizeof(name.value) - 1);
    Components::SplineComponent reference{};
    reference.SplineDataIndex = handle.Index();
    reference.SplineDataGeneration = handle.Generation();
    reference.DefaultRadius = 1.0f;
    reference.ShowWidthEnvelope = false;
    world->AddComponentImmediate(m_Entity, Components::Transform{});
    world->AddComponentImmediate(m_Entity, name);
    world->AddComponentImmediate(m_Entity, reference);
    world->AddComponentImmediate(m_Entity, Components::SmartShape{});
    NotifyCreated();
}

void CreateSmartShapeCommand::NotifyCreated()
{
    auto* world = m_Context.World;
    if (m_Context.HasTargetEntity && world->IsValid(m_Context.TargetEntity))
        world->AddComponentImmediate(m_Entity, Components::Parent{m_Context.TargetEntity});
    if (m_Context.NotifyEntityCreated)
        m_Context.NotifyEntityCreated(m_Entity);
    m_Undone = false;
    if (m_Context.NotifyWorldStructureChanged)
        m_Context.NotifyWorldStructureChanged();
}

void CreateSmartShapeCommand::Undo()
{
    auto* world = m_Context.World;
    if (!world || !world->IsValid(m_Entity))
        return;
    const auto* reference = world->GetComponent<Components::SplineComponent>(m_Entity);
    if (reference)
    {
        if (auto* service = SplineECS::SplineService::TryGet())
            service->DestroySpline({reference->SplineDataIndex, reference->SplineDataGeneration});
    }
    world->DestroyEntityImmediatePreserveHandle(m_Entity);
    m_Undone = true;
    if (m_Context.NotifyWorldStructureChanged)
        m_Context.NotifyWorldStructureChanged();
}
} // namespace GameEngine::SmartShape2D
