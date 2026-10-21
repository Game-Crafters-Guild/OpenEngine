// The inspector's render layer rows: a click on one layer changes that layer on each selected entity and
// keeps the entity's other layers, as one undo step. A reflected row (the Reflection Probe's Captured
// Layers) over a two-entity selection, and the Terrain's typed row. Built in process over a live World;
// events go through UIElement::DispatchEvent, as in TerrainGrassRootColorRowTests.

#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Terrain/TerrainInspector.h"

#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Terrain/Terrain.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/MaskField.h"
#include "UI/Internal/MaskFieldAccess.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <string>

using namespace GameEngine;

namespace
{

MaskField* FindMask(UIElement& element)
{
    if (auto* field = dynamic_cast<MaskField*>(&element))
        return field;
    for (const auto& child : element.GetChildren())
        if (MaskField* found = FindMask(*child))
            return found;
    return nullptr;
}

// The MaskField in the row whose label reads `labelText`.
MaskField* RowMask(UIElement& element, const std::string& labelText)
{
    for (const auto& child : element.GetChildren())
    {
        if (child->HasClass("inspector-row"))
        {
            bool labelled = false;
            for (const auto& cell : child->GetChildren())
                for (const auto& part : cell->GetChildren())
                    if (auto* label = dynamic_cast<Label*>(part.get()); label && label->GetText() == labelText)
                        labelled = true;
            if (labelled)
                return FindMask(*child);
        }
        if (MaskField* found = RowMask(*child, labelText))
            return found;
    }
    return nullptr;
}

void ClickLayer(MaskField& field, uint32 layer)
{
    MaskFieldAccess::OpenPopup(field);
    ASSERT_GT(MaskFieldAccess::OptionRows(field).size(), layer);
    Checkbox& row = *MaskFieldAccess::OptionRows(field)[layer];
    UILayoutAccess::SetLastLayoutRect(row, 0.0f, 0.0f, 100.0f, 20.0f);
    for (const EventId id : {kEventMouseDown, kEventMouseUp})
    {
        UIEvent event{};
        event.Id = id;
        event.X = 5.0f;
        event.Y = 5.0f;
        event.Target = &row;
        event.CurrentTarget = &row;
        row.DispatchEvent(event);
    }
}

class RenderLayersRowTest : public ::testing::Test
{
  protected:
    // The terrain inspector's asset rows read the engine's asset registry.
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    ECS::World world{nullptr};
    Editor::UndoRedoService undo;
    UIElement parent{"div"};
};

} // namespace

TEST_F(RenderLayersRowTest, AReflectedRowChangesOneLayerOnEachSelectedEntity)
{
    Components::ReflectionProbe first{};
    first.CapturedLayers.Bits = 0b0001u;
    Components::ReflectionProbe second{};
    second.CapturedLayers.Bits = 0b0110u;
    const ECS::EntityHandle a = world.Create(first).GetHandle();
    const ECS::EntityHandle b = world.Create(second).GetHandle();

    InspectorContext ctx;
    ctx.Parent = &parent;
    ctx.World = &world;
    ctx.Entity = a;
    ctx.Entities = {a, b};
    ctx.Undo = &undo;
    ASSERT_TRUE(RenderDefaultComponentInspector(ctx, ECS::GetComponentTypeId<Components::ReflectionProbe>()));

    MaskField* layers = RowMask(parent, "Captured Layers");
    ASSERT_NE(layers, nullptr);
    ASSERT_NO_FATAL_FAILURE(ClickLayer(*layers, 3));

    EXPECT_EQ(world.GetComponent<Components::ReflectionProbe>(a)->CapturedLayers.Bits, 0b1001u);
    EXPECT_EQ(world.GetComponent<Components::ReflectionProbe>(b)->CapturedLayers.Bits, 0b1110u)
        << "each entity keeps its other layers";
    EXPECT_EQ(undo.GetUndoCount(), 1u) << "the selection is one undo step";
}

TEST_F(RenderLayersRowTest, TheTerrainRowChangesOneLayer)
{
    RegisterTerrainInspector();
    Components::Terrain terrain{};
    terrain.RenderLayers.Bits = 0b0110u;
    const ECS::EntityHandle entity = world.Create(terrain).GetHandle();
    InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Components::Terrain>();
    ASSERT_NE(inspector, nullptr);
    InspectorContext ctx;
    ctx.Parent = &parent;
    ctx.World = &world;
    ctx.Entity = entity;
    ctx.Undo = &undo;
    (*inspector)(ctx);

    MaskField* layers = RowMask(parent, "Render Layers");
    ASSERT_NE(layers, nullptr);
    ASSERT_NO_FATAL_FAILURE(ClickLayer(*layers, 0));
    EXPECT_EQ(world.GetComponent<Components::Terrain>(entity)->RenderLayers.Bits, 0b0111u);
    EXPECT_EQ(undo.GetUndoCount(), 1u);
}
