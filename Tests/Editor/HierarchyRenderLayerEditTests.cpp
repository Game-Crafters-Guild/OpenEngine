// The Hierarchy's render layer row: its text reads the project's layer names and follows a rename, and a
// click on a multi-selection changes one layer per entity and keeps each entity's other layers.

#include "Editor/Hierarchy/HierarchyRenderLayerEdit.h"

#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Engine/Rendering/RenderLayerNames.h"
#include "Inspectors/RenderLayerField.h"
#include "UI/Controls/MaskField.h"
#include "UI/Internal/MaskFieldAccess.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <vector>

using namespace GameEngine;
namespace gr = GameEngine::Rendering;

namespace
{

ECS::EntityHandle MakeRenderer(ECS::World& world, uint32 bits)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::MeshRenderer renderer{};
    renderer.RenderLayers.Bits = bits;
    world.AddComponentImmediate(entity, renderer);
    return entity;
}

uint32 LayersOf(ECS::World& world, ECS::EntityHandle entity)
{
    return world.GetComponent<Components::MeshRenderer>(entity)->RenderLayers.Bits;
}

} // namespace

TEST(HierarchyRenderLayerEditTest, TheRowReadsTheProjectsNameAndFollowsARename)
{
    gr::RenderLayerNames names{};
    ASSERT_EQ(names.Set(1, "Props"), "");
    gr::SetRenderLayerNames(names);

    MaskField row;
    Editor::FeedRenderLayerNames(row);
    row.SetValueWithoutNotify(gr::RenderLayerMask::FromLayer(1).Bits);
    EXPECT_EQ(MaskFieldAccess::Summary(row), "Props");

    ASSERT_EQ(names.Set(1, "Crates"), "");
    gr::SetRenderLayerNames(names);
    Editor::RefreshRenderLayerFields();
    EXPECT_EQ(MaskFieldAccess::Summary(row), "Crates") << "a rename on the settings page reaches the row";

    gr::SetRenderLayerNames({});
}

TEST(HierarchyRenderLayerEditTest, AClickOnASelectionChangesOneLayerPerEntity)
{
    ECS::World world;
    Editor::EditorChangeNotifications notifications;
    Editor::UndoRedoService undo;
    const ECS::EntityHandle onDefault = MakeRenderer(world, 0b0001u);
    const ECS::EntityHandle onTwoLayers = MakeRenderer(world, 0b0110u);
    const std::vector<ECS::EntityHandle> selection = {onDefault, onTwoLayers};

    Editor::ApplyRenderLayerBits(world, selection, gr::RenderLayerMask::FromLayer(3).Bits, true, &undo, &notifications);
    EXPECT_EQ(LayersOf(world, onDefault), 0b1001u);
    EXPECT_EQ(LayersOf(world, onTwoLayers), 0b1110u) << "each entity keeps its other layers";
    EXPECT_EQ(undo.GetUndoCount(), 1u) << "the whole selection is one undo step";

    Editor::ApplyRenderLayerBits(world, selection, gr::RenderLayerMask::FromLayer(1).Bits, false, &undo, &notifications);
    EXPECT_EQ(LayersOf(world, onDefault), 0b1001u);
    EXPECT_EQ(LayersOf(world, onTwoLayers), 0b1100u);

    undo.Undo();
    undo.Undo();
    EXPECT_EQ(LayersOf(world, onDefault), 0b0001u);
    EXPECT_EQ(LayersOf(world, onTwoLayers), 0b0110u);
}
