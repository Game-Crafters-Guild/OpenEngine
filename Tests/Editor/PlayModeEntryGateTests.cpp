// Entering play mode while the game's scripts build: the entry waits for the build and enters when
// it finishes; Cancel or an exit request drops it; a failed build, native or C#, keeps the editor out
// of play mode until the module rebuilds.

#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"

#include "PlayMode/PlayModeEntryGate.h"
#include "PlayMode/PlayModeManager.h"

#include <string>
#include <vector>

using GameEngine::EngineCore;
using GameEngine::Editor::PlayModeEntryGate;
using GameEngine::Editor::PlayModeManager;
using GameEngine::Editor::PlayModeState;
using GameEngine::Editor::ScriptBuildSnapshot;

namespace
{

// The script build the gate sees, set by each test.
struct FakeBuild
{
    ScriptBuildSnapshot Snapshot;

    PlayModeEntryGate::BuildProbe Probe()
    {
        return [this]() { return Snapshot; };
    }
};

class PlayModeEntryGateTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            GameEngine::ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    // A world with one entity, so the pre-play snapshot is not empty.
    static void AttachWorld(PlayModeManager& playMode)
    {
        auto* world = EngineCore::GetInstance().EnsurePrimaryWorld();
        ASSERT_NE(world, nullptr);
        world->Clear();
        (void)world->CreateEntity();
        world->ProcessCommands();
        playMode.SetWorld(world);
    }
};

} // namespace

TEST_F(PlayModeEntryGateTests, PlayWaitsForTheScriptBuildAndStartsWhenItFinishes)
{
    FakeBuild build;
    build.Snapshot.Building = true;
    PlayModeManager playMode(build.Probe());
    AttachWorld(playMode);

    playMode.EnterPlayMode();
    EXPECT_EQ(playMode.GetState(), PlayModeState::Edit) << "play started while scripts were building";
    EXPECT_EQ(playMode.GetEntryGate().GetPhase(), PlayModeEntryGate::Phase::Waiting);

    playMode.Tick(0.0f);
    EXPECT_EQ(playMode.GetState(), PlayModeState::Edit);

    build.Snapshot.Building = false;
    playMode.Tick(0.0f);
    EXPECT_EQ(playMode.GetState(), PlayModeState::Play);
    EXPECT_EQ(playMode.GetEntryGate().GetPhase(), PlayModeEntryGate::Phase::Idle);

    playMode.ExitPlayMode();
}

TEST_F(PlayModeEntryGateTests, CancelOrAnExitRequestDropsTheWaitingEntry)
{
    FakeBuild build;
    build.Snapshot.Building = true;
    PlayModeManager playMode(build.Probe());
    AttachWorld(playMode);

    playMode.EnterPlayMode();
    playMode.GetEntryGate().Cancel();
    EXPECT_EQ(playMode.GetEntryGate().GetPhase(), PlayModeEntryGate::Phase::Idle);

    playMode.EnterPlayMode();
    playMode.ExitPlayMode();
    EXPECT_EQ(playMode.GetEntryGate().GetPhase(), PlayModeEntryGate::Phase::Idle);

    build.Snapshot.Building = false;
    playMode.Tick(0.0f);
    EXPECT_EQ(playMode.GetState(), PlayModeState::Edit) << "a dropped entry entered play mode when the build finished";
}

TEST_F(PlayModeEntryGateTests, AFailedBuildKeepsTheEditorOutOfPlayUntilTheModuleRebuilds)
{
    FakeBuild build;
    build.Snapshot.Building = true;
    PlayModeManager playMode(build.Probe());
    AttachWorld(playMode);

    playMode.EnterPlayMode();
    build.Snapshot.Building = false;
    build.Snapshot.FailedModules = {"Breakout"};
    build.Snapshot.FirstError = "BreakoutSystem.cpp(296): error C2059: syntax error: 'this'";
    playMode.Tick(0.0f);
    EXPECT_EQ(playMode.GetState(), PlayModeState::Edit);
    EXPECT_EQ(playMode.GetEntryGate().GetPhase(), PlayModeEntryGate::Phase::Failed);
    EXPECT_EQ(playMode.GetEntryGate().GetSnapshot().FirstError, build.Snapshot.FirstError);

    // Play again with no build in flight: the failed C# compile is the latest outcome, so play is
    // refused rather than run on the last good code.
    playMode.GetEntryGate().Cancel();
    build.Snapshot.FailedModules = {"C# scripts"};
    playMode.EnterPlayMode();
    EXPECT_EQ(playMode.GetState(), PlayModeState::Edit) << "play ran on the code of a failed build";
    EXPECT_EQ(playMode.GetEntryGate().GetPhase(), PlayModeEntryGate::Phase::Failed);

    build.Snapshot.FailedModules.clear();
    playMode.EnterPlayMode();
    EXPECT_EQ(playMode.GetState(), PlayModeState::Play);

    playMode.ExitPlayMode();
}
