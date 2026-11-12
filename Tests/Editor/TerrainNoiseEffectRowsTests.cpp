// The Noise effect inspector's Mode and Noise Preset rows, and the notices for a Generated noise that no
// terrain reads or that another Generated noise outranks (#6606).
//
// BOUNDARY: the inspector is built in process over a live World; a pick goes through
// Dropdown::SetSelectedValue. Hit-testing and the dropdown's menu are not covered.
#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Terrain/TerrainBaseSourceSection.h"
#include "Terrain/TerrainModifier/TerrainModifierInspectors.h"

#include "Components/Name.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstring>
#include <string>

using namespace GameEngine;

namespace
{
bool HasLabel(const UIElement& element, const std::string& text)
{
    if (const auto* label = dynamic_cast<const Label*>(&element); label && label->GetText() == text)
        return true;
    for (const auto& child : element.GetChildren())
        if (HasLabel(*child, text))
            return true;
    return false;
}

// The tooltip of the first label that reads `text` in `element`'s tree, or empty.
std::string LabelTooltip(const UIElement& element, const std::string& text)
{
    if (const auto* label = dynamic_cast<const Label*>(&element); label && label->GetText() == text)
        return label->GetTooltip();
    for (const auto& child : element.GetChildren())
        if (std::string tooltip = LabelTooltip(*child, text); !tooltip.empty())
            return tooltip;
    return {};
}

// The first label that reads `text` in `element`'s tree, or null.
const Label* FindLabel(const UIElement& element, const std::string& text)
{
    if (const auto* label = dynamic_cast<const Label*>(&element); label && label->GetText() == text)
        return label;
    for (const auto& child : element.GetChildren())
        if (const Label* found = FindLabel(*child, text))
            return found;
    return nullptr;
}

// The first slider in `element`'s tree, or null.
Slider* FindSlider(UIElement& element)
{
    if (auto* slider = dynamic_cast<Slider*>(&element))
        return slider;
    for (const auto& child : element.GetChildren())
        if (Slider* found = FindSlider(*child))
            return found;
    return nullptr;
}

// The slider in the inspector row whose label reads `label`, or null.
Slider* RowSlider(const UIElement& element, const std::string& label)
{
    const Label* rowLabel = FindLabel(element, label);
    UIElement* row = rowLabel ? rowLabel->GetParent() : nullptr;
    while (row && !row->HasClass("inspector-row"))
        row = row->GetParent();
    return row ? FindSlider(*row) : nullptr;
}

Dropdown* FindDropdown(UIElement& element, const std::string& id)
{
    if (auto* dropdown = dynamic_cast<Dropdown*>(&element); dropdown && dropdown->GetId() == id)
        return dropdown;
    for (const auto& child : element.GetChildren())
        if (Dropdown* found = FindDropdown(*child, id))
            return found;
    return nullptr;
}

// The text of every label under `element`.
std::string AllText(const UIElement& element)
{
    std::string text;
    if (const auto* label = dynamic_cast<const Label*>(&element))
        text += label->GetText() + '\n';
    for (const auto& child : element.GetChildren())
        text += AllText(*child);
    return text;
}

// The text of every notice under `element`.
// The shown button labeled `label` in `element`'s tree, or null.
Button* FindButton(UIElement& element, const std::string& label)
{
    if (auto* button = dynamic_cast<Button*>(&element); button && button->GetText() == label && !button->HasClass("hidden"))
        return button;
    for (const auto& child : element.GetChildren())
        if (Button* found = FindButton(*child, label))
            return found;
    return nullptr;
}

// Clicks inside an event dispatch, as the input path does: an action that rebuilds the notice host then
// defers the removal of the clicked button.
void Click(Button& button)
{
    UIEvent event{};
    event.Id = kEventButtonClick;
    event.Target = &button;
    event.CurrentTarget = &button;
    UIElement::SetInEventDispatch(true);
    button.DispatchEvent(event);
    UIElement::SetInEventDispatch(false);
}

std::string NoticeText(const UIElement& element)
{
    if (dynamic_cast<const EditorUI::InspectorNotice*>(&element))
        return AllText(element);
    std::string text;
    for (const auto& child : element.GetChildren())
        text += NoticeText(*child);
    return text;
}

// True when the notice that holds `text` under `element` is drawn as information, not as a warning.
bool IsInformationNotice(const UIElement& element, const std::string& text)
{
    if (dynamic_cast<const EditorUI::InspectorNotice*>(&element) && AllText(element).find(text) != std::string::npos)
        return element.HasClass("inspector-notice-information");
    for (const auto& child : element.GetChildren())
        if (IsInformationNotice(*child, text))
            return true;
    return false;
}

class TerrainNoiseEffectRowsTest : public ::testing::Test
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

    // A volume of `shape` with a Noise effect in `mode`, beside a planar terrain with `base`.
    void Build(Components::TerrainVolumeShape shape, Components::TerrainNoiseMode mode,
               Components::TerrainBaseSource base = Components::TerrainBaseSource::Flat)
    {
        RegisterTerrainModifierInspectors();
        Components::Terrain terrain{};
        terrain.BaseSource = base;
        world.Create(terrain);
        Components::TerrainModifierVolume volume{};
        volume.Shape = shape;
        Components::TerrainNoiseEffect noise{};
        noise.Mode = mode;
        noise.Preset = Components::TerrainNoisePreset::Rugged;
        entity = world.Create(volume, noise).GetHandle();
        Inspect();
    }

    // The section of `TComponent` on `entity`.
    template <typename TComponent = Components::TerrainNoiseEffect>
    void Inspect()
    {
        parent.RemoveAllChildren();
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<TComponent>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext ctx;
        ctx.Parent = &parent;
        ctx.World = &world;
        ctx.Entity = entity;
        ctx.ChangeNotifications = &notifications;
        ctx.Undo = &undo;
        (*inspector)(ctx);
    }

    // The Base section of a Flat planar terrain, made at the first call.
    void InspectFlatTerrainBase()
    {
        parent.RemoveAllChildren();
        if (!flatTerrain.IsValid())
        {
            Components::Terrain terrain{};
            terrain.BaseSource = Components::TerrainBaseSource::Flat;
            flatTerrain = world.Create(terrain).GetHandle();
        }
        InspectorContext ctx;
        ctx.Parent = &parent;
        ctx.World = &world;
        ctx.Entity = flatTerrain;
        ctx.ChangeNotifications = &notifications;
        ctx.Undo = &undo;
        ctx.SelectEntity = [this](ECS::EntityHandle picked) { selected = picked; };
        AddTerrainBaseSourceSection(ctx);
    }

    ECS::World world{nullptr};
    ECS::EntityHandle entity{};
    ECS::EntityHandle flatTerrain{};
    ECS::EntityHandle selected{};
    Editor::EditorChangeNotifications notifications;
    Editor::UndoRedoService undo;
    UIElement parent{"div"};
};

TEST_F(TerrainNoiseEffectRowsTest, GeneratedModeShowsTheNoisePresetAndHidesTheDisplaceRows)
{
    Build(Components::TerrainVolumeShape::Global, Components::TerrainNoiseMode::Generated);
    Dropdown* mode = FindDropdown(parent, "terrain-noise-mode");
    ASSERT_NE(mode, nullptr) << "the Noise effect shows no Mode row";
    EXPECT_EQ(mode->GetSelectedLabel(), "Generated");
    Dropdown* preset = FindDropdown(parent, "terrain-noise-preset");
    ASSERT_NE(preset, nullptr) << "a Generated noise shows no Noise Preset row";
    EXPECT_EQ(preset->GetSelectedLabel(), "Rugged");
    EXPECT_FALSE(HasLabel(parent, "Frequency")) << "a Generated noise shows the Displace rows it does not read";
    EXPECT_EQ(NoticeText(parent), "") << "a Global volume over a Flat terrain shows a notice";
}

TEST_F(TerrainNoiseEffectRowsTest, DisplaceModeShowsTheDisplaceRowsAndNoPreset)
{
    Build(Components::TerrainVolumeShape::Circle, Components::TerrainNoiseMode::Displace);
    ASSERT_NE(FindDropdown(parent, "terrain-noise-mode"), nullptr);
    EXPECT_EQ(FindDropdown(parent, "terrain-noise-preset"), nullptr);
    EXPECT_TRUE(HasLabel(parent, "Frequency"));
}

TEST_F(TerrainNoiseEffectRowsTest, APresetPickWritesTheEffect)
{
    Build(Components::TerrainVolumeShape::Global, Components::TerrainNoiseMode::Generated);
    Dropdown* preset = FindDropdown(parent, "terrain-noise-preset");
    ASSERT_NE(preset, nullptr);
    preset->SetSelectedValue(std::to_string(static_cast<int>(Components::TerrainNoisePreset::Smooth)));
    const auto* noise = world.GetComponent<Components::TerrainNoiseEffect>(entity);
    ASSERT_NE(noise, nullptr);
    EXPECT_EQ(noise->Preset, Components::TerrainNoisePreset::Smooth);
}

TEST_F(TerrainNoiseEffectRowsTest, AnUndoOfAModeChangeRestoresTheMode)
{
    Build(Components::TerrainVolumeShape::Global, Components::TerrainNoiseMode::Generated);
    Dropdown* mode = FindDropdown(parent, "terrain-noise-mode");
    ASSERT_NE(mode, nullptr);
    mode->SetSelectedValue(std::to_string(static_cast<int>(Components::TerrainNoiseMode::Displace)));
    ASSERT_EQ(world.GetComponent<Components::TerrainNoiseEffect>(entity)->Mode, Components::TerrainNoiseMode::Displace);
    undo.Undo();
    EXPECT_EQ(world.GetComponent<Components::TerrainNoiseEffect>(entity)->Mode, Components::TerrainNoiseMode::Generated);
}

// Each notice states its fix: the volume's shape, or the terrain's base.
TEST_F(TerrainNoiseEffectRowsTest, AGeneratedNoiseOnAPartTerrainShapeStatesTheFix)
{
    Build(Components::TerrainVolumeShape::Circle, Components::TerrainNoiseMode::Generated);
    EXPECT_NE(NoticeText(parent).find("Shape to Global"), std::string::npos) << NoticeText(parent);
}

TEST_F(TerrainNoiseEffectRowsTest, AGeneratedNoiseWithNoFlatTerrainStatesTheFix)
{
    Build(Components::TerrainVolumeShape::Global, Components::TerrainNoiseMode::Generated,
          Components::TerrainBaseSource::HeightmapAsset);
    EXPECT_NE(NoticeText(parent).find("Base Source to Flat"), std::string::npos) << NoticeText(parent);
}

TEST_F(TerrainNoiseEffectRowsTest, AGeneratedNoiseOnAVolumeOfWeightZeroStatesTheFix)
{
    Build(Components::TerrainVolumeShape::Global, Components::TerrainNoiseMode::Generated);
    world.GetComponentForWrite<Components::TerrainModifierVolume>(entity)->Weight = 0.0f;
    Inspect();
    EXPECT_NE(NoticeText(parent).find("Weight above 0"), std::string::npos) << NoticeText(parent);
}

TEST_F(TerrainNoiseEffectRowsTest, TheWeightRowOfAGeneratedVolumeSaysThatItScalesTheRelief)
{
    Build(Components::TerrainVolumeShape::Global, Components::TerrainNoiseMode::Generated);
    Inspect<Components::TerrainModifierVolume>();
    EXPECT_EQ(LabelTooltip(parent, "Weight"), "Scales the relief: 0.5 gives half the preset's height. 0 turns it off. For more relief, raise the terrain's Height Scale.");

    world.GetComponentForWrite<Components::TerrainNoiseEffect>(entity)->Mode = Components::TerrainNoiseMode::Displace;
    Inspect<Components::TerrainModifierVolume>();
    EXPECT_EQ(LabelTooltip(parent, "Weight"), "Master strength for every effect in this volume (0-1)");
}

// The Weight row is a slider over the range the volume keeps, 0 to 1, in both modes of the Noise effect.
TEST_F(TerrainNoiseEffectRowsTest, TheWeightRowIsASliderFromZeroToOne)
{
    Build(Components::TerrainVolumeShape::Global, Components::TerrainNoiseMode::Generated);
    for (const Components::TerrainNoiseMode mode : {Components::TerrainNoiseMode::Generated,
                                                     Components::TerrainNoiseMode::Displace})
    {
        world.GetComponentForWrite<Components::TerrainNoiseEffect>(entity)->Mode = mode;
        Inspect<Components::TerrainModifierVolume>();
        const Slider* weight = RowSlider(parent, "Weight");
        ASSERT_NE(weight, nullptr) << "mode " << static_cast<int>(mode);
        EXPECT_EQ(weight->GetMin(), 0.0f);
        EXPECT_EQ(weight->GetMax(), 1.0f);
    }
}

// With two Global Generated noises, the one that loses names the one that wins.
TEST_F(TerrainNoiseEffectRowsTest, AGeneratedNoiseThatLosesNamesTheWinner)
{
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Global;
    Components::TerrainNoiseEffect noise{};
    noise.Mode = Components::TerrainNoiseMode::Generated;
    Components::Name name{};
    std::strncpy(name.value, "Main Hills", sizeof(name.value) - 1);
    world.Create(volume, noise, name);
    Build(Components::TerrainVolumeShape::Global, Components::TerrainNoiseMode::Generated);
    EXPECT_NE(NoticeText(parent).find("\"Main Hills\" (Priority 0) supplies the relief; only one Generated noise "
                                      "can. Raise this volume's Priority above 0, or disable \"Main Hills\"."),
              std::string::npos)
        << NoticeText(parent);

    world.GetComponentForWrite<Components::TerrainNoiseEffect>(entity)->Enabled = false;
    Inspect();
    EXPECT_EQ(NoticeText(parent), "") << "a disabled noise that is not the winner shows the loser notice";

    world.GetComponentForWrite<Components::TerrainNoiseEffect>(entity)->Enabled = true;
    world.GetComponentForWrite<Components::TerrainModifierVolume>(entity)->Priority = 1.0f;
    Inspect();
    EXPECT_EQ(NoticeText(parent), "") << "the winner shows a notice";
}

// The notice of the shown noise follows a Generated noise created or changed after the Inspector is built.
TEST_F(TerrainNoiseEffectRowsTest, TheLoserNoticeFollowsAnotherVolumeWithNoReselect)
{
    Build(Components::TerrainVolumeShape::Global, Components::TerrainNoiseMode::Generated);
    ASSERT_EQ(NoticeText(parent), "") << "the only Generated noise shows a notice";

    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Global;
    volume.Priority = 5.0f;
    Components::TerrainNoiseEffect noise{};
    noise.Mode = Components::TerrainNoiseMode::Generated;
    Components::Name name{};
    std::strncpy(name.value, "Terrain Noise B", sizeof(name.value) - 1);
    const ECS::EntityHandle other = world.Create(volume, noise, name).GetHandle();
    notifications.NotifyWorldStructureChanged({&world, Editor::EditorChangeNotifications::ChangeKind::Commit});
    EXPECT_NE(NoticeText(parent).find("\"Terrain Noise B\" (Priority 5) supplies the relief"), std::string::npos)
        << NoticeText(parent);

    world.GetComponentForWrite<Components::TerrainModifierVolume>(other)->Priority = -1.0f;
    notifications.NotifyComponentCommit<Components::TerrainModifierVolume>(&world, other);
    EXPECT_EQ(NoticeText(parent), "") << "the shown noise wins again, but the loser notice stays";
}

// The Base section of a Flat terrain names the volume whose Generated noise is the relief.
TEST_F(TerrainNoiseEffectRowsTest, TheBaseSectionNamesTheReliefSource)
{
    Components::Terrain terrain{};
    terrain.BaseSource = Components::TerrainBaseSource::Flat;
    const ECS::EntityHandle terrainEntity = world.Create(terrain).GetHandle();
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Global;
    Components::TerrainNoiseEffect noise{};
    noise.Mode = Components::TerrainNoiseMode::Generated;
    Components::Name name{};
    std::strncpy(name.value, "Terrain Noise A", sizeof(name.value) - 1);
    const ECS::EntityHandle volumeEntity = world.Create(volume, noise, name).GetHandle();

    InspectorContext ctx;
    ctx.Parent = &parent;
    ctx.World = &world;
    ctx.Entity = terrainEntity;
    ctx.ChangeNotifications = &notifications;
    ctx.SelectEntity = [this](ECS::EntityHandle picked) { selected = picked; };
    AddTerrainBaseSourceSection(ctx);
    EXPECT_NE(NoticeText(parent).find("The relief is the Rolling noise of \"Terrain Noise A\"."), std::string::npos)
        << NoticeText(parent);
    EXPECT_TRUE(IsInformationNotice(parent, "The relief is")) << "the relief line reads as a warning";
    Button* select = FindButton(parent, "Select");
    ASSERT_NE(select, nullptr) << "the relief line gives no way to reach the volume";
    Click(*select);
    EXPECT_EQ(selected, volumeEntity);
}

// The relief line names the volume's Weight when it is not 1.
TEST_F(TerrainNoiseEffectRowsTest, TheBaseSectionNamesTheWeightOfTheReliefSource)
{
    Components::Terrain terrain{};
    terrain.BaseSource = Components::TerrainBaseSource::Flat;
    const ECS::EntityHandle terrainEntity = world.Create(terrain).GetHandle();
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Global;
    volume.Weight = 0.5f;
    Components::TerrainNoiseEffect noise{};
    noise.Mode = Components::TerrainNoiseMode::Generated;
    Components::Name name{};
    std::strncpy(name.value, "Terrain Noise", sizeof(name.value) - 1);
    world.Create(volume, noise, name);

    InspectorContext ctx;
    ctx.Parent = &parent;
    ctx.World = &world;
    ctx.Entity = terrainEntity;
    ctx.ChangeNotifications = &notifications;
    AddTerrainBaseSourceSection(ctx);
    EXPECT_NE(NoticeText(parent).find("The relief is the Rolling noise of \"Terrain Noise\" at Weight 0.5."),
              std::string::npos)
        << NoticeText(parent);
}

// A Flat terrain with no relief offers it: Add Relief makes a Rolling Generated volume in one undo step
// and selects it.
TEST_F(TerrainNoiseEffectRowsTest, AddReliefMakesARollingVolumeThatTheBaseSectionNames)
{
    InspectFlatTerrainBase();
    EXPECT_NE(NoticeText(parent).find("No volume supplies relief."), std::string::npos) << NoticeText(parent);
    EXPECT_TRUE(IsInformationNotice(parent, "No volume supplies relief."))
        << "the no-relief line reads as a warning";
    Button* addRelief = FindButton(parent, "Add Relief");
    ASSERT_NE(addRelief, nullptr) << NoticeText(parent);

    Click(*addRelief);
    const ECS::EntityHandle volume = TerrainECS::FindGeneratedNoiseVolume(world);
    ASSERT_TRUE(volume.IsValid()) << "Add Relief made no Generated noise";
    EXPECT_EQ(world.GetComponent<Components::TerrainNoiseEffect>(volume)->Preset, Components::TerrainNoisePreset::Rolling);
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_EQ(selected, volume) << "Add Relief did not select the volume it made";
    EXPECT_NE(NoticeText(parent).find("The relief is the Rolling noise of \"Terrain Noise\"."), std::string::npos)
        << NoticeText(parent);
}

// A Generated volume that supplies no relief is named with the reason and a Select action; Add Relief is
// offered only when no Generated volume exists.
TEST_F(TerrainNoiseEffectRowsTest, TheBaseSectionNamesASkippedReliefVolumeAndTheReason)
{
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Global;
    Components::TerrainNoiseEffect noise{};
    noise.Mode = Components::TerrainNoiseMode::Generated;
    noise.Enabled = false;
    Components::Name name{};
    std::strncpy(name.value, "Terrain Noise", sizeof(name.value) - 1);
    ECS::Entity volumeEntity = world.Create(volume, noise, name);
    const std::string effectLine = "\"Terrain Noise\" supplies no relief: its Noise effect is disabled.";

    InspectFlatTerrainBase();
    EXPECT_TRUE(IsInformationNotice(parent, effectLine)) << NoticeText(parent);
    EXPECT_EQ(FindButton(parent, "Add Relief"), nullptr) << "a disabled volume offers a second one";
    Button* select = FindButton(parent, "Select");
    ASSERT_NE(select, nullptr) << "the line gives no way to reach the disabled volume";
    Click(*select);
    EXPECT_EQ(selected, volumeEntity.GetHandle());

    world.GetComponentForWrite<Components::TerrainNoiseEffect>(volumeEntity.GetHandle())->Enabled = true;
    volumeEntity.SetEnabled(false);
    const std::string entityLine = "\"Terrain Noise\" supplies no relief: its entity is disabled.";
    InspectFlatTerrainBase();
    EXPECT_TRUE(IsInformationNotice(parent, entityLine)) << NoticeText(parent);

    // With more than one part off, the line names the outermost part.
    world.GetComponentForWrite<Components::TerrainNoiseEffect>(volumeEntity.GetHandle())->Enabled = false;
    InspectFlatTerrainBase();
    EXPECT_TRUE(IsInformationNotice(parent, entityLine)) << "the entity and the effect are off: " << NoticeText(parent);
    world.GetComponentForWrite<Components::TerrainNoiseEffect>(volumeEntity.GetHandle())->Enabled = true;

    volumeEntity.SetEnabled(true);
    const ECS::ComponentTypeId noiseType = ECS::GetComponentTypeId<Components::TerrainNoiseEffect>();
    ASSERT_TRUE(world.SetComponentEnabledImmediate(volumeEntity.GetHandle(), noiseType, false));
    InspectFlatTerrainBase();
    EXPECT_TRUE(IsInformationNotice(parent, effectLine)) << "a disabled effect component: " << NoticeText(parent);

    ASSERT_TRUE(world.SetComponentEnabledImmediate(volumeEntity.GetHandle(), noiseType, true));
    const ECS::ComponentTypeId volumeType = ECS::GetComponentTypeId<Components::TerrainModifierVolume>();
    ASSERT_TRUE(world.SetComponentEnabledImmediate(volumeEntity.GetHandle(), volumeType, false));
    InspectFlatTerrainBase();
    EXPECT_TRUE(IsInformationNotice(parent, "\"Terrain Noise\" supplies no relief: its volume component is disabled."))
        << NoticeText(parent);

    ASSERT_TRUE(world.SetComponentEnabledImmediate(volumeEntity.GetHandle(), volumeType, true));
    world.GetComponentForWrite<Components::TerrainModifierVolume>(volumeEntity.GetHandle())->Weight = 0.0f;
    InspectFlatTerrainBase();
    EXPECT_TRUE(IsInformationNotice(parent, "\"Terrain Noise\" supplies no relief: its Weight is 0."))
        << NoticeText(parent);
    EXPECT_EQ(FindButton(parent, "Add Relief"), nullptr) << "a volume at Weight 0 offers a second one";

    world.DestroyEntityImmediate(volumeEntity.GetHandle());
    InspectFlatTerrainBase();
    EXPECT_NE(NoticeText(parent).find("No volume supplies relief."), std::string::npos) << NoticeText(parent);
    EXPECT_NE(FindButton(parent, "Add Relief"), nullptr) << NoticeText(parent);
}
} // namespace
