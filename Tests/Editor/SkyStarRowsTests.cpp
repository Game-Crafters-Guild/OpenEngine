// The Sky Environment's star rows (Inspectors/SkyStarLookRow, Inspectors/SkyStarSeenByRow): the Preset
// list holds the four looks, each with a picture; a pick applies the look as one undo step and leaves
// Seen by alone; the field reads Custom when a dial or the night tint differs, and picking the look
// then resets it; a caption names a night tint that alone reads Custom; the Seen by tiles set the observer as one undo step.

#include "Inspectors/SkyStarLookRow.h"
#include "Inspectors/SkyStarSeenByRow.h"

#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Input/KeyCodes.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/PictureChoiceField.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <string>

using namespace GameEngine;

namespace
{

template <typename T>
T* FindControl(UIElement& element)
{
    if (auto* control = dynamic_cast<T*>(&element))
        return control;
    for (const auto& child : element.GetChildren())
    {
        if (T* found = FindControl<T>(*child))
            return found;
    }
    return nullptr;
}

UIElement* ChildWithClass(const UIElement& parent, const char* className)
{
    for (const auto& child : parent.GetChildren())
    {
        if (child->HasClass(className))
            return child.get();
    }
    return nullptr;
}

// The Preset row's caption text, or empty when the caption is hidden.
std::string ShownCaption(const UIElement& element)
{
    for (const auto& child : element.GetChildren())
    {
        if (child->HasClass("inspector-field-caption"))
            return child->HasClass("hidden") ? std::string() : static_cast<const Label&>(*child).GetText();
        const std::string found = ShownCaption(*child);
        if (!found.empty())
            return found;
    }
    return {};
}

std::string ShownText(const Dropdown& presets)
{
    return presets.GetHeaderLabel() ? presets.GetHeaderLabel()->GetText() : std::string();
}

// A sky at its defaults and one global volume, with the Preset and Seen by rows built for it.
struct StarRows
{
    ECS::World World;
    Editor::EditorChangeNotifications Notifications;
    Editor::UndoRedoService Undo;
    UIElement Parent;
    ECS::EntityHandle Sky{};
    ECS::EntityHandle Volume{};
    Dropdown* Presets = nullptr;
    EditorUI::PictureChoiceField* SeenBy = nullptr;
    int Rebuilds = 0;
    Editor::EditorChangeNotifications::SubscriptionToken RebuildSubscription{};

    StarRows()
    {
        Sky = World.CreateEntity();
        World.AddComponentImmediate(Sky, Components::SkyEnvironment{});
        Volume = World.CreateEntity();
        World.AddComponentImmediate(Volume, Components::PostProcessVolume{});
        AddSkyStarLookRow(&Parent, &World, Sky, &Notifications, &Undo);
        AddSkyStarSeenByRow(&Parent, &World, Sky, &Notifications, &Undo);
        Presets = FindControl<Dropdown>(Parent);
        SeenBy = FindControl<EditorUI::PictureChoiceField>(Parent);
        RebuildSubscription = Notifications.SubscribeComponentChanged(
            [this](const Editor::EditorChangeNotifications::ComponentChangedEvent& event)
            {
                if (event.kind == Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild)
                    ++Rebuilds;
            });
    }

    ~StarRows() { Notifications.Unsubscribe(RebuildSubscription); }

    const Components::SkyEnvironment& SkyValues() { return *World.GetComponent<Components::SkyEnvironment>(Sky); }
    const Components::PostProcessVolume& VolumeValues()
    {
        return *World.GetComponent<Components::PostProcessVolume>(Volume);
    }
};

void PressLeft(UIElement& tile)
{
    UIEvent e{};
    e.Id = kEventMouseDown;
    e.Button = Input::kMouseButton_Left;
    e.Target = &tile;
    e.CurrentTarget = &tile;
    tile.DispatchEvent(e);
}

} // namespace

// Vivid, Restrained, Classic and Physical in that order, each with its picture; a new sky is Vivid.
TEST(SkyStarRows, ThePresetListIsTheFourLooksEachWithAPicture)
{
    StarRows rows;
    ASSERT_NE(rows.Presets, nullptr);
    EXPECT_EQ(ShownText(*rows.Presets), "Vivid");
    const char* names[] = {"Vivid", "Restrained", "Classic", "Physical"};
    const char* pictures[] = {"option-picture--star-look-vivid", "option-picture--star-look-restrained",
                              "option-picture--star-look-classic", "option-picture--star-look-physical"};
    UIElement* picture = ChildWithClass(*rows.Presets->GetHeaderContainer(), "dropdown-header-picture");
    ASSERT_NE(picture, nullptr) << "the closed field shows the selected look's picture";
    for (int i = 0; i < 4; ++i)
    {
        rows.Presets->SetSelectedIndexWithoutNotify(i);
        EXPECT_EQ(rows.Presets->GetSelectedLabel(), names[i]);
        EXPECT_TRUE(picture->HasClass(pictures[i])) << names[i];
    }
    rows.Presets->SetSelectedIndexWithoutNotify(4);
    EXPECT_EQ(rows.Presets->GetSelectedIndex(), -1) << "the list offers only the four looks, no Custom entry";
}

// A pick applies the look's dials as one undo step, asks for the dial rows to be rebuilt, and leaves
// Seen by as it was.
TEST(SkyStarRows, APickAppliesTheLookAsOneUndoStepAndLeavesSeenBy)
{
    StarRows rows;
    ASSERT_NE(rows.Presets, nullptr);
    rows.Presets->SetSelectedIndex(2);
    EXPECT_EQ(rows.SkyValues().StarShape, Components::StarShapeKind::Sparkle);
    EXPECT_EQ(rows.SkyValues().StarObserver, Components::StarObserverKind::Camera) << "a look never changes Seen by";
    EXPECT_EQ(ShownText(*rows.Presets), "Classic");
    EXPECT_EQ(rows.Undo.GetUndoCount(), 1u) << "one pick, one undo step";
    EXPECT_EQ(rows.Rebuilds, 1) << "the dial rows are rebuilt to show the look's values";
}

// A hand-changed dial or night tint reads Custom, with nothing selected; picking the look then sets
// its values again, the tint included.
TEST(SkyStarRows, AChangedDialOrTintReadsCustomAndPickingTheLookResetsIt)
{
    StarRows rows;
    ASSERT_NE(rows.Presets, nullptr);
    Components::PostProcessVolume tuned = rows.VolumeValues();
    tuned.NightTintStrength = 0.6f;
    rows.World.AddComponentImmediate(rows.Volume, tuned);
    rows.Notifications.NotifyComponentCommit<Components::PostProcessVolume>(&rows.World, rows.Volume);
    EXPECT_EQ(rows.Presets->GetSelectedIndex(), -1) << "Custom is a state, not an entry";
    EXPECT_EQ(ShownText(*rows.Presets), "Custom");

    rows.Presets->SetSelectedIndex(0);
    EXPECT_FLOAT_EQ(rows.VolumeValues().NightTintStrength, 1.0f) << "picking Vivid resets the hand-tuned tint";
    EXPECT_EQ(ShownText(*rows.Presets), "Vivid");

    Components::SkyEnvironment changed = rows.SkyValues();
    changed.StarGlow = 0.5f;
    rows.World.AddComponentImmediate(rows.Sky, changed);
    rows.Notifications.NotifyComponentCommit<Components::SkyEnvironment>(&rows.World, rows.Sky);
    EXPECT_EQ(ShownText(*rows.Presets), "Custom");
}

// When only the night tint makes the field read Custom, the caption says so and names the next step; a
// changed dial is in the row already, so it shows no caption, and picking a look hides it.
TEST(SkyStarRows, ACustomTintAloneIsNamedInTheCaption)
{
    StarRows rows;
    EXPECT_EQ(ShownCaption(rows.Parent), "") << "a look shows no caption";
    Components::PostProcessVolume tuned = rows.VolumeValues();
    tuned.NightTintStrength = 0.6f;
    rows.World.AddComponentImmediate(rows.Volume, tuned);
    rows.Notifications.NotifyComponentCommit<Components::PostProcessVolume>(&rows.World, rows.Volume);
    EXPECT_EQ(ShownCaption(rows.Parent),
              "The night tint on the Post Process Volume differs from every preset. Pick a preset to reset it.");

    Components::SkyEnvironment changed = rows.SkyValues();
    changed.StarGlow = 0.5f;
    rows.World.AddComponentImmediate(rows.Sky, changed);
    rows.Notifications.NotifyComponentCommit<Components::SkyEnvironment>(&rows.World, rows.Sky);
    EXPECT_EQ(ShownCaption(rows.Parent), "") << "a changed dial is the visible cause";

    rows.Presets->SetSelectedIndex(0);
    EXPECT_EQ(ShownText(*rows.Presets), "Vivid");
    EXPECT_EQ(ShownCaption(rows.Parent), "");
}

// Camera, the default, is the left tile and starts selected; a pick on Eye, the right tile, sets the
// observer as one undo step and changes no star dial.
TEST(SkyStarRows, TheSeenByTilesSetTheObserverAsOneUndoStep)
{
    StarRows rows;
    ASSERT_NE(rows.SeenBy, nullptr);
    const auto& tiles = rows.SeenBy->GetChildren();
    ASSERT_EQ(tiles.size(), 2u);
    EXPECT_TRUE(tiles[0]->HasClass("picture-choice-tile-selected")) << "a default sky is seen by a camera";
    EXPECT_FALSE(tiles[1]->HasClass("picture-choice-tile-selected"));

    PressLeft(*tiles[1]);
    EXPECT_EQ(rows.SkyValues().StarObserver, Components::StarObserverKind::Eye);
    EXPECT_EQ(rows.Undo.GetUndoCount(), 1u);
    EXPECT_EQ(ShownText(*rows.Presets), "Vivid") << "Seen by changes no star dial";
}
