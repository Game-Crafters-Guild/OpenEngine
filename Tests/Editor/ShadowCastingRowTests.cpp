// The Mesh Renderer's Shadow Casting row: a choice in the dropdown sets the mode on each selected
// entity as one undo step. Built in process over a live World, as in RenderLayersRowTests.

#include <gtest/gtest.h>

#include "Inspectors/ShadowCastingRow.h"

#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/EnumField.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

using namespace GameEngine;

TEST(ShadowCastingRow, AChoiceSetsTheModeOnEachSelectedEntityAsOneUndoStep)
{
    ECS::World world{nullptr};
    Editor::UndoRedoService undo;
    UIElement parent{"div"};
    Components::MeshRenderer on{};
    Components::MeshRenderer off{};
    off.shadowCasting = Components::ShadowCasting::Off;
    const ECS::EntityHandle a = world.Create(on).GetHandle();
    const ECS::EntityHandle b = world.Create(off).GetHandle();

    EnumField<Components::ShadowCasting>* field = InspectorDrag::AddShadowCastingRow(&parent, &world, a, nullptr, &undo, {b});
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->GetDropdown()->GetSelectedIndex(), 1) << "the row shows the entity's mode, On";

    field->GetDropdown()->SetSelectedIndex(2);
    EXPECT_EQ(world.GetComponent<Components::MeshRenderer>(a)->shadowCasting, Components::ShadowCasting::ShadowsOnly);
    EXPECT_EQ(world.GetComponent<Components::MeshRenderer>(b)->shadowCasting, Components::ShadowCasting::ShadowsOnly);
    EXPECT_EQ(undo.GetUndoCount(), 1u) << "the selection is one undo step";

    undo.Undo();
    EXPECT_EQ(world.GetComponent<Components::MeshRenderer>(a)->shadowCasting, Components::ShadowCasting::On);
    EXPECT_EQ(world.GetComponent<Components::MeshRenderer>(b)->shadowCasting, Components::ShadowCasting::Off);
}
