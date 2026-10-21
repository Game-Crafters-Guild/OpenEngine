// The grass inspector's Root Color row: Auto (RootColor == kTerrainGrassAutoRootColor, derived from
// Tip Color) or an authored colour. The row must never turn Auto into an explicit colour behind the
// user's back: the swatch shows the derived colour, so a preview or a cancel that wrote the displayed
// colour back would author it.
//
// BOUNDARY: the inspector is built in process over a live World, the picker is a stub that keeps
// the callbacks it is handed, and events go through UIElement::DispatchEvent, as in
// TerrainMaterialLibraryInspectorTests. Hit-testing, UIManager routing and the native picker window
// are not covered.

#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Terrain/TerrainInspector.h"

#include "Components/Terrain/TerrainGrass.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "UI/Controls/Button.h"
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

constexpr const char* kAutoCaption = "Auto (from Tip)";
constexpr const char* kAuthoredCaption = "Root";
constexpr const char* kAutoButtonId = "terrain-grass-root-color-auto";
constexpr std::uint32_t kAuthoredRoot = 0xFF233712u;
constexpr std::uint32_t kPickedRoot = 0xFF102030u;

Label* FindCaption(UIElement& element, const std::string& text)
{
    if (auto* label = dynamic_cast<Label*>(&element); label && label->HasClass("inspector-text") && label->GetText() == text)
        return label;
    for (const auto& child : element.GetChildren())
        if (Label* found = FindCaption(*child, text))
            return found;
    return nullptr;
}

void Dispatch(UIElement& element, EventId id)
{
    UIEvent e{};
    e.Id = id;
    e.Button = 0;
    e.Target = &element;
    e.CurrentTarget = &element;
    element.DispatchEvent(e);
}

class TerrainGrassRootColorRowTest : public ::testing::Test
{
protected:
    // The grass inspector's texture rows are asset fields, which read the engine's asset registry.
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

    // Opens the picker from the Root Color row's caption, as a left mouse-down on it does.
    void OpenRootPicker(const std::string& caption)
    {
        Label* label = FindCaption(parent, caption);
        ASSERT_NE(label, nullptr) << "the Root Color row shows no '" << caption << "' caption";
        Dispatch(*label, kEventMouseDown);
        ASSERT_TRUE(picker.has_value()) << "the Root Color caption did not open the picker";
    }

    std::uint32_t RootColor() const { return world.GetComponent<TerrainGrass>(entity)->RootColor; }

    ECS::World world{nullptr};
    ECS::EntityHandle entity{};
    UIElement parent{"div"};
    std::optional<ColorPickerCallbacks> picker;
    std::uint32_t openedArgb = 0;
};

TEST_F(TerrainGrassRootColorRowTest, ADefaultComponentShowsAutoAndOffersNoAutoButton)
{
    Build(TerrainGrass{});
    EXPECT_NE(FindCaption(parent, kAutoCaption), nullptr) << "an Auto root does not say Auto";
    EXPECT_EQ(FindCaption(parent, kAuthoredCaption), nullptr);
    EXPECT_EQ(parent.FindById(HashStringId(kAutoButtonId)), nullptr)
        << "the Auto button is offered on a root that is already Auto";
}

TEST_F(TerrainGrassRootColorRowTest, ThePickerOpensAtTheDerivedRootAndApplyAuthorsIt)
{
    const TerrainGrass grass{};
    Build(grass);
    ASSERT_NO_FATAL_FAILURE(OpenRootPicker(kAutoCaption));
    EXPECT_EQ(openedArgb, Components::TerrainGrassResolvedRootColor(grass))
        << "the picker opened on a colour the blades do not use";

    picker->onApply(kPickedRoot, 1.0f);
    EXPECT_EQ(RootColor(), kPickedRoot) << "applying a colour did not author the root";
}

TEST_F(TerrainGrassRootColorRowTest, CancelAfterAPreviewLeavesTheRootAuto)
{
    Build(TerrainGrass{});
    ASSERT_NO_FATAL_FAILURE(OpenRootPicker(kAutoCaption));

    picker->onValueChanging(kPickedRoot, 1.0f);
    EXPECT_EQ(RootColor(), kPickedRoot) << "the preview did not reach the component";
    picker->onCancel();
    EXPECT_EQ(RootColor(), Components::kTerrainGrassAutoRootColor)
        << "cancel wrote the displayed colour back and turned Auto into an authored root";
}

TEST_F(TerrainGrassRootColorRowTest, TheAutoButtonClearsAnAuthoredRoot)
{
    TerrainGrass grass{};
    grass.RootColor = kAuthoredRoot;
    Build(grass);
    EXPECT_NE(FindCaption(parent, kAuthoredCaption), nullptr) << "an authored root does not say Root";
    EXPECT_EQ(FindCaption(parent, kAutoCaption), nullptr) << "an authored root claims to be Auto";

    auto* button = dynamic_cast<Button*>(parent.FindById(HashStringId(kAutoButtonId)));
    ASSERT_NE(button, nullptr) << "an authored root offers no way back to Auto";
    Dispatch(*button, kEventButtonClick);
    EXPECT_EQ(RootColor(), Components::kTerrainGrassAutoRootColor) << "the Auto button left the root authored";
}

} // namespace
