// The grass inspector's soil rows: Darken Ground, a percentage of TerrainGrass::GroundDarkening, and
// Soil Color, the swatch it mixes the ground under the field toward.
//
// BOUNDARY: the inspector is built in process over a live World with a stub colour picker, and
// events go through UIElement::DispatchEvent, as in TerrainGrassRootColorRowTests. Hit-testing,
// UIManager routing and the native picker window are not covered.

#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Terrain/TerrainInspector.h"

#include "Components/Terrain/TerrainGrass.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <cstdint>
#include <optional>
#include <string>

using namespace GameEngine;
using Components::TerrainGrass;

namespace
{

Label* FindLabel(UIElement& element, const std::string& text)
{
    if (auto* label = dynamic_cast<Label*>(&element); label && label->GetText() == text)
        return label;
    for (const auto& child : element.GetChildren())
        if (Label* found = FindLabel(*child, text))
            return found;
    return nullptr;
}

FloatField* FindFloatField(UIElement& element)
{
    if (auto* field = dynamic_cast<FloatField*>(&element))
        return field;
    for (const auto& child : element.GetChildren())
        if (FloatField* found = FindFloatField(*child))
            return found;
    return nullptr;
}

class TerrainGrassSoilRowsTest : public ::testing::Test
{
protected:
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

    void Build(const TerrainGrass& grass)
    {
        RegisterTerrainInspector();
        entity = world.Create(grass).GetHandle();
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<TerrainGrass>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext ctx;
        ctx.Parent = &parent;
        ctx.World = &world;
        ctx.Entity = entity;
        ctx.OpenColorPickerWindow = [this](std::uint32_t argb, float, ColorPickerCallbacks callbacks)
        {
            openedArgb = argb;
            picker = std::move(callbacks);
        };
        (*inspector)(ctx);
    }

    // The float field of the row whose label reads `text`: the row is the label's cell's parent.
    FloatField* RowField(const std::string& text)
    {
        Label* label = FindLabel(parent, text);
        if (!label || !label->GetParent() || !label->GetParent()->GetParent())
            return nullptr;
        return FindFloatField(*label->GetParent()->GetParent());
    }

    const TerrainGrass& Grass() const { return *world.GetComponent<TerrainGrass>(entity); }

    ECS::World world{nullptr};
    ECS::EntityHandle entity{};
    UIElement parent{"div"};
    std::optional<ColorPickerCallbacks> picker;
    std::uint32_t openedArgb = 0;
};

TEST_F(TerrainGrassSoilRowsTest, DarkenGroundShowsThePercentAndWritesTheFraction)
{
    TerrainGrass grass{};
    grass.GroundDarkening = 0.55f;
    Build(grass);
    FloatField* field = RowField("Darken Ground");
    ASSERT_NE(field, nullptr) << "the grass settings show no Darken Ground row";
    EXPECT_FLOAT_EQ(field->GetValue(), 55.0f);
    field->SetValue(30.0f);
    field->NotifyValueChanged();
    EXPECT_FLOAT_EQ(Grass().GroundDarkening, 0.30f);
    field->SetValue(250.0f);
    field->NotifyValueChanged();
    EXPECT_FLOAT_EQ(Grass().GroundDarkening, 1.0f) << "Darken Ground left its 0 to 100% range";
}

TEST_F(TerrainGrassSoilRowsTest, TheSoilColorRowOpensAtTheSoilAndApplyAuthorsIt)
{
    Build(TerrainGrass{});
    Label* caption = FindLabel(parent, "Soil");
    ASSERT_NE(caption, nullptr) << "the grass settings show no Soil Color swatch";
    UIEvent e{};
    e.Id = kEventMouseDown;
    e.Button = 0;
    e.Target = caption;
    e.CurrentTarget = caption;
    caption->DispatchEvent(e);
    ASSERT_TRUE(picker.has_value()) << "the Soil caption did not open the picker";
    EXPECT_EQ(openedArgb, TerrainGrass{}.SoilColor);
    picker->onApply(0xFF203040u, 1.0f);
    EXPECT_EQ(Grass().SoilColor, 0xFF203040u);
}

} // namespace
