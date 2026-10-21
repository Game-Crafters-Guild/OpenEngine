// The scripting ABI's animation events: GE_Animator_PollEventsOnEntity hands a script the events an Animator's
// playback fired in the last animation wave, each once. These drive the export AnimatorApi.PollEvents calls, with no
// CLR, against the animation wave's own systems.

#include "Animation/AnimationEvent.h"
#include "Animation/AnimationEventCollectorStore.h"
#include "Animation/AnimationGraphPlayer.h"
#include "Animation/AnimationGraphStore.h"
#include "Animation/Nodes/ClipPlayerNode.h"
#include "Assets/AnimationClip.h"
#include "Components/Animation/Animator.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/AnimationEventCollectorSystem.h"
#include "ECSModules/Rendering/Systems/AnimationGraphSystem.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "GltfTestFiles.h"
#include "Scripting/AnimatorABI.h"
#include "Engine/Build/PackagedGameLayout.h"
#include "EngineLogCapture.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "StagedTestPaths.h"
#include "Types/StringId.h"

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace
{
using GameEngine::ECS::World;
using GameEngine::Animation::AnimationEventCollectorStore;
using GameEngine::Animation::AnimationGraphStore;

GE_Handle HandleOf(World* world)
{
    return static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(world));
}

std::string_view NameOf(const GE_AnimationEventRecord& record)
{
    return {record.name, record.nameLength};
}

// A one-second clip sampled at 30 frames per second whose glTF clip schema puts "hit" on frame 19, read the way the
// importer reads a file.
std::shared_ptr<GameEngine::AnimationClip> ClipWithHitOnFrame19()
{
    constexpr size_t kKeys = 31;
    constexpr float kFramesPerSecond = 30.0f;
    const std::string file = GameEngine::TestFiles::ClipGlb(
        kKeys, kFramesPerSecond, R"({"clip": {"schemaVersion": 1, "events": [{"frame": 19, "name": "hit"}]}})");
    auto clip = std::make_shared<GameEngine::AnimationClip>(GameEngine::GUID::Generate(), "hit.glb");
    EXPECT_TRUE(clip->LoadFromData(GameEngine::Vector<GameEngine::uint8>(file.begin(), file.end())));
    return clip;
}
} // namespace

TEST(AnimatorEventsAbi, AFiredEventReachesPollEventsOnceWithItsNameAndTime)
{
    AnimationGraphStore::Instance().ClearForTest();
    AnimationEventCollectorStore::Instance().ClearForTest();
    World world(nullptr);
    GameEngine::Engine::Renderer::RegisterRenderWorldHooks(world);
    auto clipPlayer = std::make_unique<GameEngine::Animation::ClipPlayerNode>();
    clipPlayer->SetClip(ClipWithHitOnFrame19());
    clipPlayer->SetTime(0.6f);
    auto player = std::make_unique<GameEngine::Animation::AnimationGraphPlayer>();
    player->RootNode = std::move(clipPlayer);
    GameEngine::Components::Animator animator{};
    animator.source = GameEngine::Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = AnimationGraphStore::Instance().Create(std::move(player));
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, animator);

    // One animation wave from 0.60 s to 0.65 s crosses frame 19.
    GameEngine::Engine::Renderer::AnimationEventCollectorSystem collectorSystem;
    GameEngine::Engine::Renderer::AnimationGraphSystem graphSystem(nullptr);
    collectorSystem.Update(world, 0.05f);
    graphSystem.Update(world, 0.05f);

    GE_AnimationEventRecord records[4] = {};
    int32_t count = -1;
    ASSERT_EQ(GE_Animator_PollEventsOnEntity(HandleOf(&world), entity.id, records, 4, &count), GE_Result_Ok);
    ASSERT_EQ(count, 1);
    EXPECT_EQ(NameOf(records[0]), "hit");
    EXPECT_EQ(records[0].nameId, GameEngine::HashStringId("hit"));
    EXPECT_EQ(records[0].timeSeconds, static_cast<float>(19.0 / 30.0));

    count = -1;
    ASSERT_EQ(GE_Animator_PollEventsOnEntity(HandleOf(&world), entity.id, records, 4, &count), GE_Result_Ok);
    EXPECT_EQ(count, 0) << "a second poll in the same frame returns none";

    world.DestroyEntityImmediate(entity);
    AnimationGraphStore::Instance().ClearForTest();
    AnimationEventCollectorStore::Instance().ClearForTest();
}

TEST(AnimatorEventsAbi, PollEventsFillsWhatFitsAndRefusesBadArguments)
{
    AnimationEventCollectorStore::Instance().ClearForTest();
    World world(nullptr);
    GameEngine::Components::Animator animator{};
    animator.eventCollectorId = AnimationEventCollectorStore::Instance().Create();
    GameEngine::Animation::AnimationEventCollector* collector =
        AnimationEventCollectorStore::Instance().Get(animator.eventCollectorId);
    ASSERT_NE(collector, nullptr);
    collector->Add({0.1f, "step", {}});
    collector->Add({0.2f, "land", {}});
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, animator);
    const GameEngine::ECS::EntityHandle bare = world.CreateEntity();
    const GE_Handle handle = HandleOf(&world);

    GE_AnimationEventRecord record{};
    int32_t count = -1;
    ASSERT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, &record, 1, &count), GE_Result_Ok);
    ASSERT_EQ(count, 1);
    EXPECT_EQ(NameOf(record), "step");
    ASSERT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, &record, 1, &count), GE_Result_Ok);
    ASSERT_EQ(count, 1) << "a buffer that filled leaves the rest for the next poll";
    EXPECT_EQ(NameOf(record), "land");
    ASSERT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, nullptr, 0, &count), GE_Result_Ok);
    EXPECT_EQ(count, 0);

    EXPECT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, &record, 1, nullptr), GE_Result_InvalidArg);
    count = -1;
    EXPECT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, &record, -1, &count), GE_Result_InvalidArg);
    EXPECT_EQ(count, 0);
    EXPECT_EQ(GE_Animator_PollEventsOnEntity(handle, entity.id, nullptr, 1, &count), GE_Result_InvalidArg);
    EXPECT_EQ(GE_Animator_PollEventsOnEntity(0, entity.id, &record, 1, &count), GE_Result_InvalidArg);
    EXPECT_EQ(GE_Animator_PollEventsOnEntity(handle, bare.id, &record, 1, &count), GE_Result_NotFound)
        << "an entity with no Animator";
    world.DestroyEntityImmediate(bare);
    EXPECT_EQ(GE_Animator_PollEventsOnEntity(handle, bare.id, &record, 1, &count), GE_Result_NotFound)
        << "an entity that is gone";

    AnimationEventCollectorStore::Instance().ClearForTest();
}

namespace
{
// An engine initialized on this thread for `scripts`, which marks this thread as its main thread, shut down at
// scope exit.
class ScopedEngine
{
  public:
    explicit ScopedEngine(const GameEngine::ScriptsConfig& scripts)
    {
        GameEngine::ApplicationConfig config{};
        config.AssetDirectory = GameEngine::TestPaths::StagedEngineAssetsDir().string();
        GameEngine::EngineCore::GetInstance().SetScriptsConfig(scripts);
        m_Initialized = GameEngine::EngineCore::GetInstance().Initialize(config);
    }
    ~ScopedEngine() { GameEngine::EngineCore::GetInstance().Shutdown(); }
    ScopedEngine(const ScopedEngine&) = delete;
    ScopedEngine& operator=(const ScopedEngine&) = delete;
    bool Initialized() const { return m_Initialized; }

  private:
    bool m_Initialized = false;
};

GameEngine::ScriptsConfig NoScripting()
{
    GameEngine::ScriptsConfig scripts{};
    scripts.disableClr = true;
    scripts.enableHotReload = false;
    scripts.enableAsyncHotReload = false;
    scripts.enableAutoProjectGeneration = false;
    return scripts;
}

// A world with one entity whose Animator's collector holds one unpolled "step" event.
struct OneEventWorld
{
    OneEventWorld()
    {
        GameEngine::Components::Animator animator{};
        animator.eventCollectorId = AnimationEventCollectorStore::Instance().Create();
        AnimationEventCollectorStore::Instance().Get(animator.eventCollectorId)->Add({0.1f, "step", {}});
        Entity = World.CreateEntity();
        World.AddComponentImmediate(Entity, animator);
    }
    GameEngine::ECS::World World{nullptr};
    GameEngine::ECS::EntityHandle Entity;
};

// Polls the world's one entity on a thread of this test's own.
GE_Result PollFromAThreadOfOurOwn(OneEventWorld& world, int32_t& count)
{
    GE_AnimationEventRecord record{};
    GE_Result result = GE_Result_Ok;
    std::thread thread([&] {
        result = GE_Animator_PollEventsOnEntity(HandleOf(&world.World), world.Entity.id, &record, 1, &count);
    });
    thread.join();
    return result;
}
} // namespace

// A poll from a thread the engine does not schedule is refused and leaves the events for the engine's threads. The
// NativeAOT Player's configuration marks the main thread too, so the rule is the same there. The refusal's log line,
// which names the fix, is once per process: an earlier refusal in the process may already have written it.
TEST(AnimatorEventsAbi, PollEventsFromAForeignThreadIsRefused)
{
    std::vector<std::string> lines;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&lines);
    const GameEngine::ScriptsConfig configurations[] = {
        NoScripting(), GameEngine::BuildPlayerScriptsConfig(GameEngine::PackagedGameLayout{}, /*useNativeAOT=*/true)};
    for (const GameEngine::ScriptsConfig& scripts : configurations)
    {
        AnimationEventCollectorStore::Instance().ClearForTest();
        ScopedEngine engine(scripts);
        ASSERT_TRUE(engine.Initialized());
        OneEventWorld world;

        int32_t foreignCount = -1;
        EXPECT_EQ(PollFromAThreadOfOurOwn(world, foreignCount), GE_Result_Fail)
            << "nativeAot=" << scripts.nativeAot;
        EXPECT_EQ(foreignCount, 0);

        GE_AnimationEventRecord record{};
        int32_t count = -1;
        ASSERT_EQ(GE_Animator_PollEventsOnEntity(HandleOf(&world.World), world.Entity.id, &record, 1, &count),
                  GE_Result_Ok);
        EXPECT_EQ(count, 1) << "the refused poll left the event for the main thread";
    }
    Logger::Log::Flush();
    EXPECT_LE(GameEngine::TestLog::CountLinesContaining(lines, "post the poll to the main thread with MainThread.Post"),
              1u)
        << "the refusal is logged once per process";
    AnimationEventCollectorStore::Instance().ClearForTest();
}

// A system's update is admitted wherever the scheduler runs it: a job body on a pool worker polls the events.
TEST(AnimatorEventsAbi, PollEventsFromAWaveBodyOnAWorkerIsAdmitted)
{
    AnimationEventCollectorStore::Instance().ClearForTest();
    ScopedEngine engine(NoScripting());
    ASSERT_TRUE(engine.Initialized());
    OneEventWorld world;
    JobSystem::WorkStealingThreadPool pool(2);

    std::promise<std::pair<GE_Result, bool>> polled;
    int32_t count = -1;
    pool.EnqueueWork([&] {
        GE_AnimationEventRecord record{};
        const GE_Result result =
            GE_Animator_PollEventsOnEntity(HandleOf(&world.World), world.Entity.id, &record, 1, &count);
        polled.set_value({result, GameEngine::EngineCore::GetInstance().IsMainThread()});
    });
    const auto [result, onMainThread] = polled.get_future().get();
    pool.Shutdown();

    EXPECT_FALSE(onMainThread) << "the body ran on a pool worker";
    EXPECT_EQ(result, GE_Result_Ok);
    EXPECT_EQ(count, 1);
    AnimationEventCollectorStore::Instance().ClearForTest();
}
