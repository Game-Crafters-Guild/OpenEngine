#include "CreateSmartShapeCommand.h"
#include "SmartShape2D/SmartShape.h"
#include "SmartShape2D/SmartShapeEdge.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "UndoRedo/UndoRedoService.h"

namespace GameEngine::SmartShape2D
{
namespace
{
constexpr uint32 kCreateClosedShape = 0x6610u;
constexpr uint32 kCreateOpenShape = 0x6611u;
constexpr uint32 kCreateEdgeOverride = 0x6612u;

class SmartShapeEditorPlugin final : public Editor::IEditorPlugin
{
public:
    SmartShapeEditorPlugin()
    {
        Editor::EditorComponentTraits traits;
        traits.DisplayName = "Smart Shape 2D";
        traits.InspectorCategory = "2D";
        traits.InspectorTooltip = "Textured fill and edges generated from a spline in local XY.";
        traits.IsPickInstanceRoot = true;
        traits.GizmoPivotAtEntityOrigin = true;
        traits.EnableToggleTooltip = "Switch shape rendering on or off. Off removes the generated fill and edges.";
        Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::SmartShape>(), std::move(traits));
        Editor::EditorComponentTraits edgeTraits;
        edgeTraits.DisplayName = "Shape Edge Override";
        edgeTraits.InspectorCategory = "2D";
        edgeTraits.InspectorTooltip = "Overrides the outgoing segment of a point on the parent Smart Shape.";
        Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::SmartShapeEdge>(), std::move(edgeTraits));
        Editor::EditorPluginRegistry::Get().RegisterPlugin(*this);
    }
    const Plugins::PluginDescriptor& GetDescriptor() const override
    {
        static const Plugins::PluginDescriptor kDescriptor{"smartshape2d", "Smart Shape 2D", "1.0.0", true};
        return kDescriptor;
    }
    void BuildHierarchyContextMenu(ContextMenuBuilder& builder, const Editor::HierarchyContextMenuContext& context) override
    {
        builder.AddItem("Create/2D/Smart Shape", kCreateClosedShape, MenuItemFlag_None, 40);
        builder.AddItem("Create/2D/Smart Path", kCreateOpenShape, MenuItemFlag_None, 41);
        if (context.World && context.HasTargetEntity && context.World->HasComponent<Components::SmartShape>(context.TargetEntity))
            builder.AddItem("Create/2D/Shape Edge Override", kCreateEdgeOverride, MenuItemFlag_None, 42);
    }
    bool HandleHierarchyCommand(uint32 command, const Editor::HierarchyCommandContext& context) override
    {
        if (command != kCreateClosedShape && command != kCreateOpenShape && command != kCreateEdgeOverride)
            return false;
        if (command == kCreateEdgeOverride && (!context.World || !context.HasTargetEntity ||
            !context.World->HasComponent<Components::SmartShape>(context.TargetEntity)))
            return false;
        const auto kind = command == kCreateClosedShape ? ShapeCreationKind::Closed :
                          command == kCreateOpenShape ? ShapeCreationKind::Open : ShapeCreationKind::EdgeOverride;
        auto create = std::make_unique<CreateSmartShapeCommand>(context, kind);
        if (context.Undo)
            context.Undo->Execute(std::move(create));
        else
            create->Do();
        return true;
    }
};
SmartShapeEditorPlugin s_Plugin;
} // namespace
} // namespace GameEngine::SmartShape2D
