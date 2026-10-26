#include "CreateSmartShapeCommand.h"
#include "SmartShape2D/SmartShape.h"
#include "SmartShape2D/SmartShapeEdge.h"
#include "SmartShape2D/SmartShapeAnchor.h"
#include "SmartShape2D/SmartShapeTexture.h"
#include "SmartShape2D/GeneratedShapeResources.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/EditorIcons.h"

namespace GameEngine::SmartShape2D
{
namespace
{
constexpr uint32 kCreateClosedShape = 0x6610u;
constexpr uint32 kCreateOpenShape = 0x6611u;
constexpr uint32 kCreateEdgeOverride = 0x6612u;
constexpr uint32 kCreateAnchor = 0x6613u;
constexpr uint32 kCreateTexture = 0x6614u;

class SmartShapeEditorPlugin final : public Editor::IEditorPlugin
{
public:
    SmartShapeEditorPlugin()
    {
        Editor::EditorComponentTraits generatedTraits;
        generatedTraits.HideInInspector = true;
        Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::GeneratedShapeResources>(), std::move(generatedTraits));
        Editor::EditorComponentTraits traits;
        traits.DisplayName = "Smart Shape 2D";
        traits.InspectorCategory = "2D";
        traits.InspectorTooltip = "Textured fill and edges generated from a spline in local XY.";
        traits.IsPickInstanceRoot = true;
        traits.PreserveSceneToolOnSelection = true;
        traits.GizmoPivotAtEntityOrigin = true;
        traits.EnableToggleTooltip = "Switch the shape on or off. Off removes generated rendering and collision.";
        Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::SmartShape>(), std::move(traits));
        Editor::EditorComponentTraits edgeTraits;
        edgeTraits.DisplayName = "Shape Edge Override";
        edgeTraits.InspectorCategory = "2D";
        edgeTraits.InspectorTooltip = "Overrides the outgoing segment of a point on the parent Smart Shape.";
        Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::SmartShapeEdge>(), std::move(edgeTraits));
        Editor::EditorComponentTraits anchorTraits;
        anchorTraits.DisplayName = "Shape Point Anchor";
        anchorTraits.InspectorCategory = "2D";
        anchorTraits.InspectorTooltip = "Attach children to a point or an offset along the parent shape curve.";
        Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::SmartShapeAnchor>(), std::move(anchorTraits));
        Editor::EditorComponentTraits textureTraits;
        textureTraits.DisplayName = "Shape Texture Entry";
        textureTraits.InspectorCategory = "2D";
        textureTraits.InspectorTooltip = "One material variant with matching corners and tapers in the parent shape texture set.";
        Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::SmartShapeTexture>(), std::move(textureTraits));
        Editor::EditorPluginRegistry::Get().RegisterPlugin(*this);
    }
    const Plugins::PluginDescriptor& GetDescriptor() const override
    {
        static const Plugins::PluginDescriptor kDescriptor{"smartshape2d", "Smart Shape 2D", "1.0.0", true};
        return kDescriptor;
    }
    void BuildHierarchyContextMenu(ContextMenuBuilder& builder, const Editor::HierarchyContextMenuContext& context) override
    {
        builder.AddItem("Create/Shape/Smart Shape", kCreateClosedShape, MenuItemFlag_None, 40, EditorIcons::kMarkupRegion);
        builder.AddItem("Create/Shape/Smart Path", kCreateOpenShape, MenuItemFlag_None, 41, EditorIcons::kSpline);
        if (context.World && context.HasTargetEntity && context.World->HasComponent<Components::SmartShape>(context.TargetEntity))
        {
            builder.AddItem("Create/Shape/Shape Edge Override", kCreateEdgeOverride, MenuItemFlag_None, 42, EditorIcons::kLinearCurve);
            builder.AddItem("Create/Shape/Shape Point Anchor", kCreateAnchor, MenuItemFlag_None, 43, EditorIcons::kNode);
            builder.AddItem("Create/Shape/Shape Texture Entry", kCreateTexture, MenuItemFlag_None, 44, EditorIcons::kMaterial);
        }
    }
    bool HandleHierarchyCommand(uint32 command, const Editor::HierarchyCommandContext& context) override
    {
        if (command != kCreateClosedShape && command != kCreateOpenShape && command != kCreateEdgeOverride && command != kCreateAnchor && command != kCreateTexture)
            return false;
        if ((command == kCreateEdgeOverride || command == kCreateAnchor || command == kCreateTexture) && (!context.World || !context.HasTargetEntity ||
            !context.World->HasComponent<Components::SmartShape>(context.TargetEntity)))
            return false;
        const auto kind = command == kCreateClosedShape ? ShapeCreationKind::Closed :
                          command == kCreateOpenShape ? ShapeCreationKind::Open : command == kCreateAnchor ? ShapeCreationKind::Anchor :
                          command == kCreateTexture ? ShapeCreationKind::Texture : ShapeCreationKind::EdgeOverride;
        auto create = std::make_unique<CreateSmartShapeCommand>(context, kind);
        auto* commandState = create.get();
        if (context.Undo)
        {
            context.Undo->BeginCompound(create->GetName());
            context.Undo->Execute(std::move(create));
        }
        else
            create->Do();
        if (context.SelectEntity)
            context.SelectEntity(commandState->GetEntity());
        if (context.Undo)
            context.Undo->EndCompound();
        return true;
    }
};
SmartShapeEditorPlugin s_Plugin;
} // namespace
} // namespace GameEngine::SmartShape2D
