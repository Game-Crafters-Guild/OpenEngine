#include "SceneView/SceneViewport/SceneViewportEdits.h"

#include "EditorChangeNotifications.h"
#include "UndoRedo/CreateMeasureEntitiesCommand.h"
#include "UndoRedo/SceneViewDropUndoCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Hierarchy.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Name.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "Types/ColorUtils.h"

#include <memory>
#include <string>
#include <utility>

namespace GameEngine
{

namespace
{

Components::Transform MakeTransform(const Mathematics::Vector3& point)
{
    Components::Transform transform{};
    transform.Position = Mathematics::WorldPosition::FromVector3(point);
    return transform;
}

void AddMeasureEndpoint(ECS::World& world,
                        ECS::EntityHandle endpoint,
                        ECS::EntityHandle root,
                        std::string_view name,
                        const Mathematics::Vector3& localPoint,
                        std::uint32_t order)
{
    Components::Parent parent{};
    parent.Entity = root;
    Components::HierarchyOrder hierarchyOrder{};
    hierarchyOrder.order = order;
    world.AddComponentImmediate(endpoint, Components::MakeName(name));
    world.AddComponentImmediate(endpoint, MakeTransform(localPoint));
    world.AddComponentImmediate(endpoint, parent);
    world.AddComponentImmediate(endpoint, hierarchyOrder);
}

} // namespace

SceneViewportEdits::SceneViewportEdits(ECS::World& world, const SceneViewportBinding& binding)
    : m_World(world)
    , m_Binding(binding)
{
}

ECS::EntityHandle SceneViewportEdits::CreateMeasure(const Mathematics::Vector3& start,
                                                    const Mathematics::Vector3& end,
                                                    bool is2D,
                                                    std::uint32_t colorArgb)
{
    constexpr float kMinLengthSquared = 1.0e-8f;
    if ((end - start).LengthSquared() < kMinLengthSquared)
        return {};

    ECS::World& world = m_World;
    const ECS::EntityHandle root = world.CreateEntity();
    if (!root.IsValid())
        return {};

    const Mathematics::Vector3 center = (start + end) * 0.5f;
    world.AddComponentImmediate(root, Components::MakeName("Measure"));
    world.AddComponentImmediate(root, MakeTransform(center));

    const ECS::EntityHandle startEntity = world.CreateEntity();
    const ECS::EntityHandle endEntity = world.CreateEntity();
    if (!startEntity.IsValid() || !endEntity.IsValid())
        return root;
    AddMeasureEndpoint(world, startEntity, root, "Measure Start", start - center, 0);
    AddMeasureEndpoint(world, endEntity, root, "Measure End", end - center, 1);

    const ColorLinear color = ColorUtils::UnpackArgb(colorArgb);
    Components::MeasureComponent measure{};
    measure.Color[0] = color.r;
    measure.Color[1] = color.g;
    measure.Color[2] = color.b;
    measure.Color[3] = color.a;
    measure.Start[0] = start.x;
    measure.Start[1] = start.y;
    measure.Start[2] = start.z;
    measure.End[0] = end.x;
    measure.End[1] = end.y;
    measure.End[2] = end.z;
    measure.StartEntity = startEntity;
    measure.EndEntity = endEntity;
    measure.Is2D = is2D;
    world.AddComponentImmediate(root, measure);

    if (m_Binding.Undo)
    {
        m_Binding.Undo->CommitAlreadyApplied(std::make_unique<Editor::CreateMeasureEntitiesCommand>(
            &world, m_Binding.Notifications, std::vector<ECS::EntityHandle>{root, startEntity, endEntity}));
    }
    MarkStructureChanged();
    return root;
}

void SceneViewportEdits::RecordDrop(std::vector<ECS::EntityHandle> roots,
                                    const DropSelection& before,
                                    const DropSelection& after,
                                    ApplySelectionFn applySelection)
{
    if (!m_Binding.Undo || roots.empty())
        return;
    std::string name = Editor::SceneViewDropUndoCommand::NameFor(m_World, roots);
    m_Binding.Undo->CommitAlreadyApplied(std::make_unique<Editor::SceneViewDropUndoCommand>(std::move(name),
                                                                                          &m_World,
                                                                                          m_Binding.Notifications,
                                                                                          std::move(roots),
                                                                                          before.Items,
                                                                                          before.Anchor,
                                                                                          after.Items,
                                                                                          after.Anchor,
                                                                                          std::move(applySelection)));
}

void SceneViewportEdits::MarkStructureChanged()
{
    Editor::NotifyWorldStructure(m_Binding.Notifications, &m_World);
    if (m_Binding.OnDirty)
        m_Binding.OnDirty();
}

} // namespace GameEngine
