// The play-mode pause sets the world clock's time scale to 0, and Resume and Stop restore it.
// The pause stops the physics and audio systems and keeps the Animation system on.

#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/Engine.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "EditorChangeNotifications.h"
#include "ECSModules/Audio/Systems/AudioListenerSystem.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "PhysicsECS/Systems/PhysicsWritebackSystem.h"
#include "PlayMode/PlayModeManager.h"
#include "UndoRedo/UndoRedoService.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;
using GameEngine::Editor::EditorChangeNotifications;
using GameEngine::Editor::PlayModeManager;
using GameEngine::Editor::PlayModeState;

namespace
{

class PlayModePauseTests : public ::testing::Test
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

    // Runs after a failed ASSERT too, so the next test gets the clock at real time and no rendering loop.
    void TearDown() override
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (GameEngine::ECS::World* world = engine.GetPrimaryWorld())
            world->GetSimulationClock().SetTimeScale(1.0);
        engine.DisableRenderingLoop();
    }
};

} // namespace

TEST_F(PlayModePauseTests, PauseStopsTheWorldClockAndResumeAndStopRestoreItsTimeScale)
{
    GameEngine::Editor::UndoRedoService undo;
    EditorChangeNotifications notifications;
    PlayModeManager playMode;
    playMode.SetUndoRedo(&undo);
    playMode.SetChangeNotifications(&notifications);
    GameEngine::ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);
    world->Clear();
    world->AddComponentImmediate(world->CreateEntity(), GameEngine::Components::Transform{});
    world->ProcessCommands();
    GameEngine::ECS::SimulationClock& clock = world->GetSimulationClock();
    clock.SetTimeScale(0.5);
    playMode.SetWorld(world);

    playMode.EnterPlayMode();
    ASSERT_EQ(playMode.GetState(), PlayModeState::Play);
    playMode.TogglePause();
    ASSERT_EQ(playMode.GetState(), PlayModeState::Paused);
    EXPECT_EQ(clock.GetTimeScale(), 0.0) << "the pause stops gameplay time";
    playMode.TogglePause();
    ASSERT_EQ(playMode.GetState(), PlayModeState::Play);
    EXPECT_EQ(clock.GetTimeScale(), 0.5) << "resume restores the time scale";

    playMode.TogglePause();
    playMode.ExitPlayMode();
    ASSERT_EQ(playMode.GetState(), PlayModeState::Edit);
    EXPECT_EQ(clock.GetTimeScale(), 0.5) << "stop from the pause restores the time scale";
}

TEST_F(PlayModePauseTests, PauseStopsThePhysicsSystemsAndKeepsTheAnimationSystemOn)
{
    EngineCore& engine = EngineCore::GetInstance();
    ASSERT_TRUE(engine.EnableRenderingLoop(nullptr));
    GameEngine::ECS::SystemManager* systems = engine.GetRenderingLoop()->GetSystemManager();
    ASSERT_NE(systems, nullptr);
    const GameEngine::ECS::ISystem* physics = systems->GetSystem<GameEngine::PhysicsECS::PhysicsWritebackSystem>();
    const GameEngine::ECS::ISystem* animation = systems->GetSystem<GameEngine::Engine::Renderer::AnimationSystem>();
    ASSERT_NE(physics, nullptr);
    ASSERT_NE(animation, nullptr);
    // The schedule has the audio systems only when the engine opened an audio device.
    const GameEngine::ECS::ISystem* audio = systems->GetSystem<GameEngine::Engine::Audio::AudioListenerSystem>();
    GameEngine::Editor::UndoRedoService undo;
    EditorChangeNotifications notifications;
    PlayModeManager playMode;
    playMode.SetUndoRedo(&undo);
    playMode.SetChangeNotifications(&notifications);
    GameEngine::ECS::World* world = engine.GetPrimaryWorld();
    ASSERT_NE(world, nullptr);
    world->Clear();
    world->AddComponentImmediate(world->CreateEntity(), GameEngine::Components::Transform{});
    world->ProcessCommands();
    playMode.SetWorld(world);

    playMode.EnterPlayMode();
    ASSERT_EQ(playMode.GetState(), PlayModeState::Play);
    playMode.TogglePause();
    ASSERT_EQ(playMode.GetState(), PlayModeState::Paused);
    EXPECT_FALSE(physics->IsEnabled()) << "the physics writeback would write over an edit to a body";
    EXPECT_TRUE(animation->IsEnabled()) << "the stopped clock holds the pose";
    if (audio)
    {
        EXPECT_FALSE(audio->IsEnabled()) << "the audio engine plays when the clock stops";
    }
    playMode.TogglePause();
    EXPECT_TRUE(physics->IsEnabled()) << "resume restarts physics";

    playMode.ExitPlayMode();
    ASSERT_EQ(playMode.GetState(), PlayModeState::Edit);
}
