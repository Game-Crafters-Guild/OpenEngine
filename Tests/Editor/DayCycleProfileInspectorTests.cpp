// The Day Cycle profile's inspector: a keyed track on its graph, a left-out track named, and the save
// the profile's source offers.

#include "DayCycleEditorTestFixture.h"

#include "DayCycle/DayCycleAxis.h"
#include "DayCycle/DayCycleDescriptionField.h"
#include "DayCycle/DayCycleShippedProfiles.h"
#include "DayCycle/DayCycleSunNow.h"
#include "DayCycle/DayCycleTrackRows.h"
#include "UI/Controls/CurveField.h"
#include "UI/Controls/Toggle.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"

using namespace GameEngine;
using namespace GameEngine::Editor;
using namespace GameEngine::DayCycleEditorTesting;

TEST_F(DayCycleEditorTests, TheInspectorShowsAKeyedTrackOnItsGraphAndNamesALeftOutOne)
{
    {
        DayCycleProfile asset(GUID::Generate(), WriteProjectProfile(kMarshProfile));
        UIElement root;
        BuildInspector(root, asset);
        const std::vector<CurveField*> graphs = All<CurveField>(root);
        ASSERT_EQ(graphs.size(), 1u);
        EXPECT_EQ(graphs.front()->GetKeys().size(), 3u);
        EXPECT_FALSE(HasText(root, "not keyed by this profile"));
        EXPECT_TRUE(HasButton(root, "Save"));
        EXPECT_FALSE(HasButton(root, "Save a copy to the project"));
    }
    DayCycleWorkingCopy::CloseAll();
    {
        DayCycleProfile asset(GUID::Generate(), WriteProjectProfile(kEmptyProfile));
        UIElement root;
        BuildInspector(root, asset);
        EXPECT_TRUE(All<CurveField>(root).empty()) << "an asset has no volume below to draw";
        EXPECT_TRUE(HasText(root, "not keyed by this profile"));
        EXPECT_TRUE(HasButton(root, "Add track"));
    }
    {
        DayCycleProfile asset(Assets.ResolveAssetGuid(ShippedDefault()), ShippedDefault());
        UIElement root;
        BuildInspector(root, asset);
        EXPECT_TRUE(HasButton(root, "Save a copy to the project"));
        EXPECT_FALSE(HasButton(root, "Save"));
    }
}

// Remove track rebuilds its row right after the click returns: the next drain of the UI's posted work
// shows the left-out track, with no wait for a playhead update.
TEST_F(DayCycleEditorTests, RemoveTrackRebuildsTheRowRightAfterTheClick)
{
    UI::UiDispatcher dispatcher;
    UI::UiContextScope scope{&dispatcher, nullptr};
    DayCycleProfile asset = RegisteredAsset(WriteProjectProfile(kMarshProfile));
    UIElement root;
    BuildInspector(root, asset);
    Button* remove = nullptr;
    for (Button* button : All<Button>(root))
        if (button->GetText() == "Remove track")
            remove = button;
    ASSERT_NE(remove, nullptr);
    remove->TriggerClick();
    dispatcher.Drain();
    EXPECT_TRUE(HasText(root, "not keyed by this profile"));
    EXPECT_TRUE(HasButton(root, "Add track"));
}

// The inspector follows its profile for as long as it is shown: a preset loaded onto a split profile
// joins the dusk, and the rows show the preset's track.
TEST_F(DayCycleEditorTests, TheInspectorFollowsAPresetLoad)
{
    UI::UiDispatcher dispatcher;
    UI::UiContextScope scope{&dispatcher, nullptr};
    DayCycleProfile asset = RegisteredAsset(WriteProjectProfile(R"({ "name": "Marsh", "tracks": { "exposureOffset": {
        "keys": "-11,0;-6,-1.5;0,0", "separateDusk": true, "duskKeys": "-14,0;-6,-2;0,0" } } })"));
    UIElement root;
    BuildInspector(root, asset);
    ASSERT_TRUE(HasText(root, "Separate dusk"));
    ASSERT_EQ(All<Toggle>(root).size(), 1u);
    ASSERT_TRUE(All<Toggle>(root).front()->GetValue());
    Button* clearDay = nullptr;
    for (Button* button : All<Button>(root))
        if (button->GetText() == "Clear day")
            clearDay = button;
    ASSERT_NE(clearDay, nullptr);
    clearDay->TriggerClick();
    dispatcher.Drain();
    ASSERT_EQ(All<Toggle>(root).size(), 1u);
    EXPECT_FALSE(All<Toggle>(root).front()->GetValue()) << "the inspector still shows the split dusk after the load";
    EXPECT_EQ(All<CurveField>(root).front()->GetKeys().size(), 1u);
}

// Save is enabled only while there is something to save; Save a copy, always.
TEST_F(DayCycleEditorTests, SaveIsEnabledOnlyWithUnsavedChanges)
{
    UI::UiDispatcher dispatcher;
    UI::UiContextScope scope{&dispatcher, nullptr};
    {
        DayCycleProfile asset = RegisteredAsset(WriteProjectProfile(kMarshProfile));
        UIElement root;
        BuildInspector(root, asset);
        Button* save = nullptr;
        Button* clearDay = nullptr;
        for (Button* button : All<Button>(root))
        {
            if (button->GetText() == "Save")
                save = button;
            if (button->GetText() == "Clear day")
                clearDay = button;
        }
        ASSERT_NE(save, nullptr);
        ASSERT_NE(clearDay, nullptr);
        EXPECT_FALSE(save->IsEnabled()) << "nothing to save yet";
        clearDay->TriggerClick();
        dispatcher.Drain();
        EXPECT_TRUE(save->IsEnabled()) << "the preset load left unsaved changes";
    }
    DayCycleWorkingCopy::CloseAll();
    {
        DayCycleProfile asset(Assets.ResolveAssetGuid(ShippedDefault()), ShippedDefault());
        UIElement root;
        BuildInspector(root, asset);
        for (Button* button : All<Button>(root))
            if (button->GetText() == "Save a copy to the project")
                EXPECT_TRUE(button->IsEnabled());
        EXPECT_TRUE(HasButton(root, "Save a copy to the project"));
    }
}

namespace
{
// The text of the first label with `cls` under `root`.
std::string TextWithClass(UIElement& root, const char* cls)
{
    for (Label* label : All<Label>(root))
        if (label->HasClass(cls))
            return label->GetText();
    return {};
}
} // namespace

// A track the profile leaves out: the window's rows read the shipped Default's value and say so; the
// profile's inspector, where an asset has no volume around it, names the track not keyed and gives no
// value.
TEST_F(DayCycleEditorTests, ALeftOutTrackReadsTheDefaultsValueInTheWindowOnly)
{
    Components::SkyEnvironment sky = SkyNow();
    sky.TimeOfDayHours = 18.5f;
    SetSky(sky);
    DayCycleProfile shipped(Assets.ResolveAssetGuid(ShippedDefault()), ShippedDefault());
    UIElement shippedRoot;
    BuildInspector(shippedRoot, shipped);
    const std::string defaultNow = TextWithClass(shippedRoot, "day-cycle-track-now");
    ASSERT_FALSE(defaultNow.empty());
    ASSERT_NE(defaultNow, "+0.00 stops") << "the hour must sit in the Default's dip";

    DayCycleProfile asset = RegisteredAsset(WriteProjectProfile(kEmptyProfile));
    UIElement inspector;
    BuildInspector(inspector, asset);
    EXPECT_TRUE(HasText(inspector, "not keyed by this profile"));
    EXPECT_TRUE(HasButton(inspector, "Add track"));
    EXPECT_TRUE(All<CurveField>(inspector).empty());
    EXPECT_EQ(TextWithClass(inspector, "day-cycle-track-readout").find("stops"), std::string::npos)
        << "the inspector gives a left-out track no value";

    // The window's rows, built as the window builds them.
    std::string error;
    DayCycleTrackRowsContext context;
    context.Profile = DayCycleWorkingCopy::Open(Assets, asset.GetPath(), error);
    ASSERT_NE(context.Profile, nullptr) << error;
    context.Below = ReadDefaultDayCycleProfile(Assets);
    ASSERT_NE(context.Below, nullptr);
    context.Path = DayCyclePathOf(SkyNow());
    UIElement window;
    DayCycleTrackRows rows(window, context);
    const DayCycleSunNow now = ReadDayCycleSunNow(World, nullptr, nullptr);
    rows.SetPlayhead(now.Hours, now.ElevationDegrees, now.DuskHalf, now.HourPlacesSun);
    EXPECT_EQ(TextWithClass(window, "day-cycle-track-now"), defaultNow);
    EXPECT_EQ(TextWithClass(window, "day-cycle-track-source"), "not keyed: the Default profile's value");
}

// An undo past a save leaves the profile unlike its file: the mark comes back and Save with it.
TEST_F(DayCycleEditorTests, AnUndoPastASaveMarksTheProfileUnsavedAgain)
{
    UI::UiDispatcher dispatcher;
    UI::UiContextScope scope{&dispatcher, nullptr};
    DayCycleProfile asset = RegisteredAsset(WriteProjectProfile(kMarshProfile));
    UIElement root;
    BuildInspector(root, asset);
    Button* save = nullptr;
    Button* clearDay = nullptr;
    for (Button* button : All<Button>(root))
    {
        if (button->GetText() == "Save")
            save = button;
        if (button->GetText() == "Clear day")
            clearDay = button;
    }
    ASSERT_NE(save, nullptr);
    ASSERT_NE(clearDay, nullptr);
    clearDay->TriggerClick();
    dispatcher.Drain();
    save->TriggerClick();
    dispatcher.Drain();
    ASSERT_FALSE(save->IsEnabled()) << "saved";
    ASSERT_TRUE(Undo.CanUndo());
    Undo.Undo();
    dispatcher.Drain();
    EXPECT_TRUE(HasText(root, "Unsaved changes"));
    EXPECT_TRUE(save->IsEnabled());
}

// The description is edited in a wrapping field and committed once, when the focus leaves it.
TEST_F(DayCycleEditorTests, TheDescriptionCommitsWhenTheFocusLeaves)
{
    UI::UiDispatcher dispatcher;
    UI::UiContextScope scope{&dispatcher, nullptr};
    DayCycleProfile asset = RegisteredAsset(WriteProjectProfile(kMarshProfile));
    UIElement root;
    BuildInspector(root, asset);
    const std::vector<DayCycleDescriptionField*> fields = All<DayCycleDescriptionField>(root);
    ASSERT_EQ(fields.size(), 1u);
    const std::string text = "A darker dusk over the marsh; the dawn as the Default keeps it.";
    fields.front()->OnFocusChanged(true);
    fields.front()->SetValue(text);
    fields.front()->OnFocusChanged(false);
    dispatcher.Drain();
    std::string error;
    const std::shared_ptr<DayCycleWorkingCopy> profile = DayCycleWorkingCopy::Open(Assets, asset.GetPath(), error);
    ASSERT_NE(profile, nullptr) << error;
    EXPECT_EQ(profile->GetData().Description, text);
    EXPECT_TRUE(profile->IsDirty());
    EXPECT_STREQ(Undo.PeekUndoName(), "Edit the Day Cycle profile's description");
}

// The scrub strip runs on the tracks' own axis: the same range and ticks, and a side column beside it
// as beside every track, so one playhead x is one hour in the strip and in every track.
TEST_F(DayCycleEditorTests, TheScrubStripSharesTheTracksAxis)
{
    std::string error;
    DayCycleTrackRowsContext context;
    context.Profile = DayCycleWorkingCopy::Open(Assets, ShippedDefault(), error);
    ASSERT_NE(context.Profile, nullptr) << error;
    context.Path = DayCyclePathOf(SkyNow());
    context.OnScrub = [](float) {};
    context.ScrubRow = true;
    UIElement root;
    DayCycleTrackRows rows(root, context);
    const std::vector<CurveField*> graphs = All<CurveField>(root);
    ASSERT_EQ(graphs.size(), 2u) << "the scrub strip and the exposure track";
    const CurveField::Config& scrub = graphs[0]->GetConfig();
    const CurveField::Config& track = graphs[1]->GetConfig();
    EXPECT_EQ(scrub.TimeMin, track.TimeMin);
    EXPECT_EQ(scrub.TimeMax, track.TimeMax);
    EXPECT_EQ(scrub.TimeTicks, track.TimeTicks);
    EXPECT_FLOAT_EQ(track.TimeMax, 50.0f) << "the axis ends at this scene's noon, 50 degrees";
    for (CurveField* graph : graphs)
    {
        const UIElement* body = graph->GetParent();
        ASSERT_NE(body, nullptr);
        ASSERT_EQ(body->GetChildren().size(), 2u);
        EXPECT_TRUE(body->GetChildren().back()->HasClass("day-cycle-track-side"))
            << "a graph without the side column is wider than the others";
    }
}

// Exposure's zero lies a quarter of the range from the top: the zero, the value that changes nothing,
// keeps its label, and the +1.0 at the top gives way to it.
TEST_F(DayCycleEditorTests, TheEndLabelNearTheZeroGivesWay)
{
    std::string error;
    DayCycleTrackRowsContext context;
    context.Profile = DayCycleWorkingCopy::Open(Assets, ShippedDefault(), error);
    ASSERT_NE(context.Profile, nullptr) << error;
    context.Path = DayCyclePathOf(SkyNow());
    UIElement root;
    DayCycleTrackRows rows(root, context);
    const std::vector<CurveField*> graphs = All<CurveField>(root);
    ASSERT_EQ(graphs.size(), 1u);
    const CurveField::Config& config = graphs.front()->GetConfig();
    ASSERT_EQ(config.ValueAxisMarks.size(), 3u);
    EXPECT_EQ(config.ValueLabel(config.ValueAxisMarks[0]), "-3.0");
    EXPECT_EQ(config.ValueLabel(0.0f), "0");
    EXPECT_TRUE(config.ValueLabel(config.ValueAxisMarks[2]).empty());
}
