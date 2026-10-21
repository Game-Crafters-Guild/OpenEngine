// AudioEmitterSystemTests.cpp — lock tests for AudioEmitterSystem's
// lifecycle-events consumer behavior (P3 #389 + its must-fix f0d4c05) and
// the shared SwapGenerationGuard gap recovery it migrated onto (S1 #406).
// Both migrations previously shipped proven by inspection only. Charter 8
// covers the distance falloff of a spatialized voice, read back from its sound
// through AudioSystem::GetFalloff.
//
// Harness shape: a REAL AudioSystem initialized headlessly (miniaudio's
// default backend priority ends at the null backend, so device-less CI still
// initializes), fed a clip synthesized in memory — no disk, no asset
// registry, immune to the per-GUID failed-load caching. The observable is the alive-voice count
// (AudioSystem::GetAliveVoiceCount); emitters are shaped so voices persist
// deterministically: loop=true (never reaped), spatialized=false (2D lane,
// no WorldTransform dependency), volume=0 (tests stay silent).

#include <gtest/gtest.h>

#include "AudioTestTone.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AudioAsset.h"
#include "Audio/AudioSystem.h"
#include "Components/Audio/AudioEmitter.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECSModules/Audio/Systems/AudioEmitterSystem.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

using GameEngine::AssetManager;
using GameEngine::AudioAsset;
using GameEngine::GUID;
using GameEngine::Components::AudioEmitter;
using GameEngine::Engine::Audio::AudioEmitterSystem;
namespace ECS = GameEngine::ECS;
namespace Audio = GameEngine::Audio;

namespace
{

constexpr float kDt = 0.016f;

} // namespace

class AudioEmitterSystemTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Deliberately never Initialize()d: the audio system only touches the
        // event dispatcher (a plain member) and takes benign misses from
        // GetAsset/LoadAssetAsync — the clip below never touches disk or the
        // registry.
        m_Assets = std::make_unique<AssetManager>();
        m_Audio = std::make_unique<Audio::AudioSystem>(*m_Assets);

        Audio::AudioSystemConfig config{};
        config.maxVoices = 32;
        if (!m_Audio->Initialize(config))
            GTEST_SKIP() << "no audio backend available (miniaudio engine init failed)";

        // Warm the GUID->clip cache from a loaded in-memory asset so the
        // system's ResolveClip(guid) hits the ReadyPcm fast path.
        m_ClipGuid = GUID::Generate();
        m_Clip = std::make_shared<AudioAsset>(m_ClipGuid,
                                              std::filesystem::path("audio-ecs-test-tone.wav"));
        ASSERT_TRUE(m_Clip->LoadFromData(AudioTestSupport::MakeTestToneWav()));
        ASSERT_TRUE(m_Audio->ResolveClip(*m_Clip).IsValid());

        m_World = std::make_unique<ECS::World>();
        m_World->EnableLifecycleEvents<AudioEmitter>();
        m_System = std::make_unique<AudioEmitterSystem>(m_Audio.get());
    }

    AudioEmitter MakeEmitter() const
    {
        AudioEmitter emitter{};
        emitter.clipGuid.Set(m_ClipGuid);
        emitter.spatialized = false; // 2D lane: no WorldTransform dependency
        emitter.loop = true;         // persists until stopped — the orphan shape
        emitter.playOnStart = true;
        emitter.volume = 0.0f;       // silent
        return emitter;
    }

    ECS::EntityHandle CreateEmitterEntity() { return m_World->CreateHandle(MakeEmitter()); }

    // One engine-tick frame boundary + one system step.
    void SwapAndRun()
    {
        m_World->SwapLifecycleEvents();
        m_System->Update(*m_World, kDt);
    }

    std::size_t AliveVoices() const { return m_Audio->GetAliveVoiceCount(); }

    // Destruction order (reverse of declaration): system, world, audio
    // (Shutdown detaches the asset callback while m_Assets is alive), assets.
    std::unique_ptr<AssetManager> m_Assets;
    std::unique_ptr<Audio::AudioSystem> m_Audio;
    std::unique_ptr<ECS::World> m_World;
    std::unique_ptr<AudioEmitterSystem> m_System;
    GUID m_ClipGuid;
    std::shared_ptr<AudioAsset> m_Clip;
};

// ---------------------------------------------------------------------------
// Charter 1 — GetAdded init-seed: an added AudioEmitter starts a voice per
// its component fields, across the structural shapes.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, AddedEmitterStartsVoice)
{
    ASSERT_EQ(AliveVoices(), 0u);
    (void)CreateEmitterEntity();
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

TEST_F(AudioEmitterSystemTest, DisabledEmitterStartsNoVoice)
{
    (void)m_World->CreateHandle(MakeEmitter(), ECS::ComponentDisabled<AudioEmitter>{});
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 0u);
}

TEST_F(AudioEmitterSystemTest, ClonedEmitterStartsSecondVoice)
{
    auto e = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    (void)m_World->CloneEntity(e); // the Ctrl+D shape
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 2u);
}

// Preserve-handle destroy then revive + byte-restore across windows (the
// editor undo shape): teardown fires on the Removed window, and the restore
// re-emits Added through the unified add body, starting a fresh voice.
TEST_F(AudioEmitterSystemTest, UndoReviveRestartsVoice)
{
    auto e = CreateEmitterEntity();
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(
        m_World->CaptureComponentBytes(e, ECS::GetComponentTypeId<AudioEmitter>(), bytes));
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    m_World->DestroyEntityImmediatePreserveHandle(e);
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 0u);

    ASSERT_TRUE(m_World->ReviveEntityImmediatePreserveHandle(e));
    ASSERT_TRUE(
        m_World->ApplyComponentBytesImmediate(e, ECS::GetComponentTypeId<AudioEmitter>(), bytes));
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

// ---------------------------------------------------------------------------
// Charter 2 — GetRemoved handle-keyed teardown: component removal and every
// per-entity destroy path stop the voice and erase the record.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, ComponentRemoveStopsVoice)
{
    auto e = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    m_World->RemoveComponentImmediate<AudioEmitter>(e); // entity stays alive
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 0u);
}

TEST_F(AudioEmitterSystemTest, EntityDestroyStopsVoice)
{
    auto e1 = CreateEmitterEntity();
    auto e2 = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 2u);

    m_World->DestroyEntityImmediate(e1);
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);

    m_World->DestroyEntity(e2); // deferred path
    m_World->ProcessCommands();
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 0u);
}

// ---------------------------------------------------------------------------
// Charter 2b — switching an emitter off (its own tag, or its entity) stops its
// voice through GetDisabled<AudioEmitter>; the component stays, and switching
// it back on plays it again from the start.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, SwitchedOffEmitterStopsVoiceAndPlaysAgainWhenBackOn)
{
    auto e = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    ECS::Entity(m_World.get(), e).SetEnabled<AudioEmitter>(false);
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 0u);
    EXPECT_TRUE(m_World->HasComponent<AudioEmitter>(e)) << "switching off keeps the component";

    ECS::Entity(m_World.get(), e).SetEnabled<AudioEmitter>(true);
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

TEST_F(AudioEmitterSystemTest, DisabledEntityStopsVoice)
{
    auto e = CreateEmitterEntity();
    auto survivor = CreateEmitterEntity();
    (void)survivor;
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 2u);

    m_World->SetEntityEnabledImmediate(e, false);
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

TEST_F(AudioEmitterSystemTest, PauseGapSweepStopsSwitchedOffVoice)
{
    auto e = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    // Pause: the switch-off's window is promoted and discarded unseen.
    ECS::Entity(m_World.get(), e).SetEnabled<AudioEmitter>(false);
    m_World->SwapLifecycleEvents();
    m_World->SwapLifecycleEvents();
    ASSERT_TRUE(m_World->GetDisabled<AudioEmitter>().empty());

    m_System->Update(*m_World, kDt);
    EXPECT_EQ(AliveVoices(), 0u);
}

// ---------------------------------------------------------------------------
// Charter 3 — removed-then-re-added within one event window: the Removed
// event fires but HasComponent sees the restored component, so the record
// (and its voice) survive.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, RemoveReAddSameWindowKeepsVoice)
{
    auto e = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    m_World->RemoveComponentImmediate<AudioEmitter>(e);
    m_World->AddComponentImmediate<AudioEmitter>(e, MakeEmitter()); // component swap
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);

    // Bookkeeping must still be live: a plain remove afterwards tears down.
    m_World->RemoveComponentImmediate<AudioEmitter>(e);
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 0u);
}

TEST_F(AudioEmitterSystemTest, DestroyReviveRestoreSameWindowKeepsVoice)
{
    auto e = CreateEmitterEntity();
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(
        m_World->CaptureComponentBytes(e, ECS::GetComponentTypeId<AudioEmitter>(), bytes));
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    // Undo revive+restore before the window closes.
    m_World->DestroyEntityImmediatePreserveHandle(e);
    ASSERT_TRUE(m_World->ReviveEntityImmediatePreserveHandle(e));
    ASSERT_TRUE(
        m_World->ApplyComponentBytesImmediate(e, ECS::GetComponentTypeId<AudioEmitter>(), bytes));
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

// ---------------------------------------------------------------------------
// Charter 4 — WorldReset generation: Clear() fires no per-entity Removed
// events; the reset branch drops all bookkeeping and stops every voice.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, WorldClearStopsAllVoices)
{
    (void)CreateEmitterEntity();
    (void)CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 2u);

    m_World->Clear();
    m_System->Update(*m_World, kDt); // reset generation, not an event — no swap needed
    EXPECT_EQ(AliveVoices(), 0u);

    // The system keeps working for the next scene.
    (void)CreateEmitterEntity();
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

// ---------------------------------------------------------------------------
// Charter 5 — the pause hole (f0d4c05): the system is disabled while the
// engine tick keeps swapping windows; an emitter entity dies during the gap
// and its Removed event is promoted and discarded unseen. On resume the
// shared SwapGenerationGuard reports gap > 1 and the one-shot IsValid sweep
// stops the orphaned voice — leaving the survivor's voice alone.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, PauseGapSweepStopsOrphanedVoice)
{
    auto doomed = CreateEmitterEntity();
    auto survivor = CreateEmitterEntity();
    (void)survivor;
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 2u);

    // Pause: the engine tick keeps swapping, the system does not run.
    m_World->DestroyEntityImmediate(doomed);
    m_World->SwapLifecycleEvents(); // Removed(doomed) promoted to current...
    m_World->SwapLifecycleEvents(); // ...and discarded unseen — the hole

    ASSERT_TRUE(m_World->GetRemoved<AudioEmitter>().empty()); // event gone for good

    m_System->Update(*m_World, kDt); // resume: guard fires, sweep runs once
    EXPECT_EQ(AliveVoices(), 1u);
}

// ---------------------------------------------------------------------------
// Charter 6 — window idempotency: the editor multi-steps waves between
// swaps, so the consumer sees the same Added/Removed spans repeatedly and
// must not double-start or double-stop.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, DoubleStepWithinWindowIsIdempotent)
{
    auto e = CreateEmitterEntity();
    m_World->SwapLifecycleEvents();
    m_System->Update(*m_World, kDt);
    m_System->Update(*m_World, kDt); // same Added span again
    EXPECT_EQ(AliveVoices(), 1u);

    m_World->RemoveComponentImmediate<AudioEmitter>(e);
    m_World->SwapLifecycleEvents();
    m_System->Update(*m_World, kDt);
    m_System->Update(*m_World, kDt); // same Removed span again
    EXPECT_EQ(AliveVoices(), 0u);
}

// ---------------------------------------------------------------------------
// Charter 7 — live fields: volume and pitch edits on the component reach the
// voice it is already playing, without a restart.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, VolumeAndPitchEditsReachLiveVoice)
{
    m_Audio->SetBusVolume(0, 0.0f); // master bus muted: the edited volume is audible otherwise
    auto e = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    // The first voice of a fresh AudioSystem occupies slot 0, generation 1.
    const Audio::AudioEmitterHandle voice(0, 1);
    ASSERT_TRUE(m_Audio->IsAlive(voice));

    AudioEmitter* emitter = m_World->GetComponentForWrite<AudioEmitter>(e);
    ASSERT_NE(emitter, nullptr);
    emitter->volume = 0.125f;
    emitter->pitch = 1.5f;
    SwapAndRun();

    EXPECT_TRUE(m_Audio->IsAlive(voice)) << "a field edit restarted the voice";
    EXPECT_FLOAT_EQ(m_Audio->GetVolume(voice), 0.125f);
    EXPECT_FLOAT_EQ(m_Audio->GetPitch(voice), 1.5f);
}

// ---------------------------------------------------------------------------
// Charter 8 — distance falloff: PlayOptions::falloff and the emitter's falloff
// fields reach the voice's miniaudio sound (read back from the sound), the
// defaults are AudioFalloff's, and out-of-range values are refused or sanitized.
// ---------------------------------------------------------------------------

namespace
{
const GameEngine::Mathematics::Vector3 kZero{0.0f, 0.0f, 0.0f};

Audio::PlayOptions SilentLoopOptions()
{
    Audio::PlayOptions opts{};
    opts.volume = 0.0f;
    opts.loop = true;
    return opts;
}

void ExpectFalloff(const Audio::AudioFalloff& actual, Audio::AudioAttenuationModel model,
                   float minDistance, float maxDistance, float rolloff)
{
    EXPECT_EQ(actual.model, model);
    EXPECT_EQ(actual.minDistance, minDistance);
    EXPECT_EQ(actual.maxDistance, maxDistance);
    EXPECT_EQ(actual.rolloff, rolloff);
}
} // namespace

TEST_F(AudioEmitterSystemTest, DefaultFalloffIsInverseFromOneUnitTo10000)
{
    const auto voice = m_Audio->Play3D(m_ClipGuid, 0, kZero, kZero, SilentLoopOptions());
    ASSERT_TRUE(voice.IsValid());

    ExpectFalloff(m_Audio->GetFalloff(voice), Audio::AudioAttenuationModel::Inverse, 1.0f, 10000.0f, 1.0f);
}

TEST_F(AudioEmitterSystemTest, Play3DFalloffReachesTheSound)
{
    Audio::PlayOptions opts = SilentLoopOptions();
    opts.falloff.model = Audio::AudioAttenuationModel::Linear;
    opts.falloff.minDistance = 50.0f;
    opts.falloff.maxDistance = 2000.0f;
    opts.falloff.rolloff = 0.5f;
    const auto voice = m_Audio->Play3D(m_ClipGuid, 0, kZero, kZero, opts);
    ASSERT_TRUE(voice.IsValid());

    ExpectFalloff(m_Audio->GetFalloff(voice), Audio::AudioAttenuationModel::Linear, 50.0f, 2000.0f, 0.5f);
}

TEST_F(AudioEmitterSystemTest, SetFalloffReachesAPlayingSpatializedVoiceOnly)
{
    const auto voice3D = m_Audio->Play3D(m_ClipGuid, 0, kZero, kZero, SilentLoopOptions());
    const auto voice2D = m_Audio->Play2D(m_ClipGuid, SilentLoopOptions());
    ASSERT_TRUE(voice3D.IsValid());
    ASSERT_TRUE(voice2D.IsValid());

    Audio::AudioFalloff falloff{};
    falloff.model = Audio::AudioAttenuationModel::Linear;
    falloff.minDistance = 50.0f;
    falloff.maxDistance = 2000.0f;
    falloff.rolloff = 0.5f;
    m_Audio->SetFalloff(voice3D, falloff);
    m_Audio->SetFalloff(voice2D, falloff);

    ExpectFalloff(m_Audio->GetFalloff(voice3D), Audio::AudioAttenuationModel::Linear, 50.0f, 2000.0f, 0.5f);
    // A 2D voice has no distance: it reports, and keeps, the default.
    EXPECT_EQ(m_Audio->GetFalloff(voice2D), Audio::AudioFalloff{});
}

TEST_F(AudioEmitterSystemTest, SpatializedPlay2DFalloffReachesTheSound)
{
    Audio::PlayOptions opts = SilentLoopOptions();
    opts.spatialized = true;
    opts.falloff.model = Audio::AudioAttenuationModel::Exponential;
    opts.falloff.minDistance = 4.0f;
    opts.falloff.maxDistance = 400.0f;
    opts.falloff.rolloff = 2.0f;
    const auto voice = m_Audio->Play2D(m_ClipGuid, opts);
    ASSERT_TRUE(voice.IsValid());

    ExpectFalloff(m_Audio->GetFalloff(voice), Audio::AudioAttenuationModel::Exponential, 4.0f, 400.0f, 2.0f);
}

TEST_F(AudioEmitterSystemTest, OutOfRangeFalloffIsSanitized)
{
    Audio::PlayOptions negative = SilentLoopOptions();
    negative.falloff.model = Audio::AudioAttenuationModel::None;
    negative.falloff.minDistance = -5.0f;
    negative.falloff.maxDistance = -10.0f;
    negative.falloff.rolloff = -2.0f;
    const auto negativeVoice = m_Audio->Play3D(m_ClipGuid, 0, kZero, kZero, negative);
    ASSERT_TRUE(negativeVoice.IsValid());

    Audio::PlayOptions nonFinite = SilentLoopOptions();
    nonFinite.falloff.minDistance = std::numeric_limits<float>::quiet_NaN();
    nonFinite.falloff.maxDistance = std::numeric_limits<float>::infinity();
    nonFinite.falloff.rolloff = std::numeric_limits<float>::quiet_NaN();
    const auto nonFiniteVoice = m_Audio->Play3D(m_ClipGuid, 0, kZero, kZero, nonFinite);
    ASSERT_TRUE(nonFiniteVoice.IsValid());

    Audio::PlayOptions inverted = SilentLoopOptions();
    inverted.falloff.minDistance = 100.0f;
    inverted.falloff.maxDistance = 10.0f;
    const auto invertedVoice = m_Audio->Play3D(m_ClipGuid, 0, kZero, kZero, inverted);
    ASSERT_TRUE(invertedVoice.IsValid());

    // A negative or NaN minimum distance takes the default (1); the rest is sanitized.
    ExpectFalloff(m_Audio->GetFalloff(negativeVoice), Audio::AudioAttenuationModel::None, 1.0f, 1.0f, 0.0f);
    ExpectFalloff(m_Audio->GetFalloff(nonFiniteVoice), Audio::AudioAttenuationModel::Inverse, 1.0f,
                  std::numeric_limits<float>::max(), 1.0f);
    ExpectFalloff(m_Audio->GetFalloff(invertedVoice), Audio::AudioAttenuationModel::Inverse, 100.0f, 100.0f, 1.0f);
}

// A minimum distance of 0 silences a voice from the first unit out (miniaudio's curves divide
// by it): SetFalloff refuses it and the voice keeps the falloff it had; a voice started with
// one takes the default minimum distance.
TEST_F(AudioEmitterSystemTest, AZeroMinimumDistanceIsRefused)
{
    Audio::PlayOptions opts = SilentLoopOptions();
    opts.falloff.minDistance = 0.0f;
    const auto voice = m_Audio->Play3D(m_ClipGuid, 0, kZero, kZero, opts);
    ASSERT_TRUE(voice.IsValid());
    EXPECT_EQ(m_Audio->GetFalloff(voice).minDistance, 1.0f);

    Audio::AudioFalloff kept{};
    kept.minDistance = 20.0f;
    m_Audio->SetFalloff(voice, kept);
    Audio::AudioFalloff zero = kept;
    zero.minDistance = 0.0f;
    m_Audio->SetFalloff(voice, zero);
    EXPECT_EQ(m_Audio->GetFalloff(voice).minDistance, 20.0f);
}

TEST_F(AudioEmitterSystemTest, SpatializedEmitterFalloffReachesItsVoiceAndFollowsEdits)
{
    AudioEmitter authored = MakeEmitter();
    authored.spatialized = true;
    authored.attenuation = GameEngine::Components::AudioEmitterAttenuation::Linear;
    authored.minDistance = 50.0f;
    authored.maxDistance = 2000.0f;
    authored.rolloff = 0.75f;
    const ECS::EntityHandle e = m_World->CreateHandle(authored, GameEngine::Components::WorldTransform{});
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    // The first voice of a fresh AudioSystem occupies slot 0, generation 1.
    const Audio::AudioEmitterHandle voice(0, 1);
    ASSERT_TRUE(m_Audio->IsAlive(voice));
    ExpectFalloff(m_Audio->GetFalloff(voice), Audio::AudioAttenuationModel::Linear, 50.0f, 2000.0f, 0.75f);

    AudioEmitter* emitter = m_World->GetComponentForWrite<AudioEmitter>(e);
    ASSERT_NE(emitter, nullptr);
    emitter->attenuation = GameEngine::Components::AudioEmitterAttenuation::Exponential;
    emitter->minDistance = 4.0f;
    emitter->maxDistance = 400.0f;
    emitter->rolloff = 2.0f;
    SwapAndRun();

    EXPECT_TRUE(m_Audio->IsAlive(voice)) << "a falloff edit restarted the voice";
    ExpectFalloff(m_Audio->GetFalloff(voice), Audio::AudioAttenuationModel::Exponential, 4.0f, 400.0f, 2.0f);
}
