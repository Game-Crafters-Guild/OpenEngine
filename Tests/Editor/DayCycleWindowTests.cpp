// The Day Cycle window: the working copy it shares with the profile's inspector, and its scrub of the
// scene's time of day through the world's preview hour on the production sky system.

#include "DayCycleEditorTestFixture.h"

#include "Components/Rendering/Light.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Transform.h"
#include "DayCycle/DayCycleAxis.h"
#include "DayCycle/DayCycleScrub.h"
#include "DayCycle/DayCycleWindow.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "UI/Controls/CurveField.h"

using namespace GameEngine;
using namespace GameEngine::Editor;
using namespace GameEngine::DayCycleEditorTesting;

// The window opens the shipped Default by its place in the editor source; the asset browser hands the
// inspector the same file under the path the registry stores. Both are one profile, so one copy.
TEST_F(DayCycleEditorTests, TheWindowAndTheInspectorOpenTheShippedDefaultAsOneCopy)
{
    DayCycleWindow window(Services());
    ASSERT_NE(window.GetProfile(), nullptr);
    DayCycleProfile asset = RegisteredAsset(ShippedDefault());
    UIElement inspector;
    BuildInspector(inspector, asset);
    EXPECT_EQ(DayCycleWorkingCopy::MostRecent().get(), window.GetProfile().get())
        << "the inspector opened a second copy of " << asset.GetPath() << " beside " << window.GetProfile()->GetPath();
}

// An edit on either surface is the other's: one undo step, one unsaved mark, one profile. The window's
// layout binds only under a UI manager, so the window is read through the copy it shows.
TEST_F(DayCycleEditorTests, TheInspectorAndTheWindowShareOneWorkingCopy)
{
    DayCycleProfile asset = RegisteredAsset(WriteProjectProfile(kMarshProfile));
    UIElement inspector;
    BuildInspector(inspector, asset);
    DayCycleWindow window(Services());
    ASSERT_NE(window.GetProfile(), nullptr);
    EXPECT_EQ(window.GetProfile().get(), DayCycleWorkingCopy::MostRecent().get())
        << "the window shows the profile the inspector opened";

    CurveField* inspectorGraph = All<CurveField>(inspector).front();
    inspectorGraph->DeleteKey(1);
    window.Refresh();
    EXPECT_EQ(window.GetProfile()->GetData().Tracks.front().Keys.KeyCount, 2u)
        << "the window's profile did not take the inspector's edit";
    EXPECT_EQ(Undo.GetUndoCount(), 1u);
    EXPECT_TRUE(window.GetProfile()->IsDirty());
    EXPECT_TRUE(HasText(inspector, "Unsaved changes"));

    Undo.Undo();
    EXPECT_EQ(window.GetProfile()->GetData().Tracks.front().Keys.KeyCount, 3u);
    EXPECT_EQ(inspectorGraph->GetKeys().size(), 3u);
    EXPECT_FALSE(window.GetProfile()->IsDirty());
    EXPECT_FALSE(HasText(inspector, "Unsaved changes"));

    // One Save, from either surface, clears the mark on both.
    Undo.Redo();
    std::string error;
    ASSERT_TRUE(window.GetProfile()->Save(error)) << error;
    EXPECT_FALSE(HasText(inspector, "Unsaved changes"));
    DayCycleProfileData saved;
    ASSERT_TRUE(ParseDayCycleProfile(ReadText(Root / "Project" / "DayCycles" / "Marsh.daycycle.json"), saved, error))
        << error;
    EXPECT_EQ(saved.Tracks.front().Keys.KeyCount, 2u);
}

// Save a copy moves the edits into the copy: the shipped profile goes back to its file and shows no
// unsaved mark, and the copy is the one the window shows.
TEST_F(DayCycleEditorTests, SaveACopyMovesTheEditsIntoTheCopy)
{
    std::string error;
    const std::shared_ptr<DayCycleWorkingCopy> shipped = DayCycleWorkingCopy::Open(Assets, ShippedDefault(), error);
    ASSERT_NE(shipped, nullptr) << error;
    DayCycleProfileData edited = shipped->GetData();
    edited.Tracks.front().Keys.Keys[1].Value = -2.0f;
    shipped->Commit(edited, "Edit", &Undo);
    const std::shared_ptr<DayCycleWorkingCopy> copy = shipped->SaveCopy(error);
    ASSERT_NE(copy, nullptr) << error;
    EXPECT_FALSE(shipped->IsDirty());
    EXPECT_FLOAT_EQ(shipped->GetData().Tracks.front().Keys.Keys[1].Value, -1.5f);
    EXPECT_FALSE(copy->IsDirty());
    EXPECT_FLOAT_EQ(copy->GetData().Tracks.front().Keys.Keys[1].Value, -2.0f);
    EXPECT_EQ(copy->GetData().Name, "Default copy") << "the copy reads like the shipped profile";
    EXPECT_EQ(copy->GetPath().filename(), "Default copy.daycycle.json");
    EXPECT_EQ(DayCycleWorkingCopy::MostRecent().get(), copy.get());
}

// A project switch closes the copies: the window then shows the new project's Default, never the old
// project's profile.
TEST_F(DayCycleEditorTests, AProjectSwitchClosesTheWorkingCopies)
{
    DayCycleWorkingCopy::CloseAllOnProjectSwitch(Notifications);
    std::string error;
    ASSERT_NE(DayCycleWorkingCopy::Open(Assets, WriteProjectProfile(kMarshProfile), error), nullptr) << error;
    Notifications.NotifyProjectSwitched();
    EXPECT_EQ(DayCycleWorkingCopy::MostRecent(), nullptr);
}

// The scrub previews the hour: the world renders it in the same frame, the volume the window works
// in is the selected one, and the component, the undo history and the scene's unsaved mark (driven
// by component notifications) see nothing.
TEST_F(DayCycleEditorTests, AScrubPreviewsTheHourAndLeavesTheSceneAlone)
{
    const ECS::EntityHandle global = World.CreateEntity();
    World.AddComponentImmediate(global, Components::PostProcessVolume{});
    World.AddComponentImmediate(global, NameOf("World"));
    const ECS::EntityHandle marsh = World.CreateEntity();
    Components::PostProcessVolume local{};
    local.IsGlobal = false;
    World.AddComponentImmediate(marsh, local);
    World.AddComponentImmediate(marsh, NameOf("Marsh"));

    int notifications = 0;
    Notifications.SubscribeComponentChanged([&notifications](const auto&) { ++notifications; });
    DayCycleWindow window(Services());
    EXPECT_EQ(window.ContextVolume(), global);
    Selected = {marsh};
    window.Refresh();
    EXPECT_EQ(window.ContextVolume(), marsh) << "the window works in the selected volume";

    window.SetScrubHeld(true);
    window.ScrubTo(18.5f);
    window.SetScrubHeld(false);
    ASSERT_EQ(Sky.GetPreviewHour(World.GetWorldId()), std::optional<float>(18.5f));
    Sky.Update(World, 0.0f);
    const float elevation = Render.GetFeature<Engine::Renderer::SkyRenderFeature>()->GetSunState().SunElevation;
    EXPECT_NEAR(elevation * kRadiansToDegrees, ElevationDegreesAtHour(DayCyclePathOf(SkyNow()), 18.5f), 0.01f);

    EXPECT_EQ(SkyNow().TimeOfDayHours, 10.0f);
    EXPECT_EQ(Undo.GetUndoCount(), 0u);
    EXPECT_EQ(notifications, 0);
}

TEST_F(DayCycleEditorTests, ClosingTheWindowEndsThePreview)
{
    DayCycleWindow window(Services());
    window.ScrubTo(18.5f);
    ASSERT_TRUE(Sky.GetPreviewHour(World.GetWorldId()).has_value());
    window.OnOwnerManagerChanged(nullptr);
    EXPECT_FALSE(Sky.GetPreviewHour(World.GetWorldId()).has_value());
    EXPECT_EQ(SkyNow().TimeOfDayHours, 10.0f);
}

// A Time of Day edit made while the window is open ends the preview so the edit shows, and is still
// the scene's hour after the window closes.
TEST_F(DayCycleEditorTests, ATimeOfDayEditMadeWhileTheWindowIsOpenSurvivesTheClose)
{
    auto window = std::make_unique<DayCycleWindow>(Services());
    window->ScrubTo(18.5f);
    Components::SkyEnvironment edited = SkyNow();
    edited.TimeOfDayHours = 7.0f;
    Editor::CommitComponentUpdate(&World, SkyEntity, &Notifications, edited);
    EXPECT_FALSE(Sky.GetPreviewHour(World.GetWorldId()).has_value()) << "the edit did not show";
    window.reset();
    EXPECT_EQ(SkyNow().TimeOfDayHours, 7.0f);
    EXPECT_FALSE(Sky.GetPreviewHour(World.GetWorldId()).has_value());
}

// Under AnimateTimeOfDay the clock holds while the playhead is held, and the release makes the
// scrubbed hour the scene's as one undoable edit.
TEST_F(DayCycleEditorTests, UnderAnAnimatedClockTheReleaseWritesTheHourAsOneUndoableEdit)
{
    Components::SkyEnvironment sky = SkyNow();
    sky.AnimateTimeOfDay = true;
    SetSky(sky);
    DayCycleWindow window(Services());
    window.SetScrubHeld(true);
    window.ScrubTo(18.5f);
    Sky.Update(World, 5.0f);
    EXPECT_EQ(SkyNow().TimeOfDayHours, 10.0f) << "the clock ran while the playhead was held";

    window.SetScrubHeld(false);
    EXPECT_FLOAT_EQ(SkyNow().TimeOfDayHours, 18.5f);
    EXPECT_FALSE(Sky.GetPreviewHour(World.GetWorldId()).has_value());
    EXPECT_EQ(Undo.GetUndoCount(), 1u);
    Undo.Undo();
    EXPECT_EQ(SkyNow().TimeOfDayHours, 10.0f);
}

TEST_F(DayCycleEditorTests, TheScrubIsOffWithANoticeUnderAFollowedLightAndASunSetByHand)
{
    const ECS::EntityHandle light = World.CreateEntity();
    Components::Light directional{};
    directional.Type = Components::LightType::Directional;
    World.AddComponentImmediate(light, directional);
    World.AddComponentImmediate(light, Components::Transform{});
    World.AddComponentImmediate(light, NameOf("Sun"));
    Components::SkyEnvironment followed = SkyNow();
    followed.SunLight = light;
    followed.TimeOfDayDrivesSunLight = false;
    SetSky(followed);
    {
        DayCycleScrub scrub(World, Sky, &Undo, &Notifications, {});
        EXPECT_NE(scrub.BlockedNotice().find("The sun follows the light Sun"), std::string::npos)
            << scrub.BlockedNotice();
        DayCycleWindow window(Services());
        window.ScrubTo(18.5f);
        EXPECT_FALSE(Sky.GetPreviewHour(World.GetWorldId()).has_value());
    }

    Components::SkyEnvironment byHand = SkyNow();
    byHand.SunLight = {};
    byHand.AutoSunMoon = false;
    SetSky(byHand);
    DayCycleScrub scrub(World, Sky, &Undo, &Notifications, {});
    EXPECT_NE(scrub.BlockedNotice().find("The sun is set by hand"), std::string::npos) << scrub.BlockedNotice();
    DayCycleWindow window(Services());
    window.ScrubTo(18.5f);
    EXPECT_FALSE(Sky.GetPreviewHour(World.GetWorldId()).has_value());
}

// A preview belongs to the scene it was set for: once the scene's sky is another entity (a scene
// opened, or the sky deleted and another added), the scene renders its own hour.
TEST_F(DayCycleEditorTests, ASceneChangeEndsThePreview)
{
    DayCycleWindow window(Services());
    window.ScrubTo(18.5f);
    ASSERT_TRUE(Sky.GetPreviewHour(World.GetWorldId()).has_value());
    World.DestroyEntityImmediate(SkyEntity);
    SkyEntity = World.CreateEntity();
    World.AddComponentImmediate(SkyEntity, Components::SkyEnvironment{});
    Editor::NotifyWorldStructure(&Notifications, &World);
    EXPECT_FALSE(Sky.GetPreviewHour(World.GetWorldId()).has_value());
}

// Play runs in the scene's own world: the preview ends when play starts, and the scrub is off, with
// its notice, while the editor plays.
TEST_F(DayCycleEditorTests, PlayModeEndsThePreviewAndTurnsTheScrubOff)
{
    DayCycleWindow window(Services());
    window.ScrubTo(18.5f);
    ASSERT_TRUE(Sky.GetPreviewHour(World.GetWorldId()).has_value());
    Playing = true;
    window.Refresh();
    EXPECT_FALSE(Sky.GetPreviewHour(World.GetWorldId()).has_value()) << "the preview played";
    window.ScrubTo(12.0f);
    EXPECT_FALSE(Sky.GetPreviewHour(World.GetWorldId()).has_value()) << "the scrub ran in Play mode";
    DayCycleScrub scrub(World, Sky, &Undo, &Notifications, [this]() { return Playing; });
    EXPECT_EQ(scrub.BlockedNotice(), "The scrub is off in Play mode.");
}
