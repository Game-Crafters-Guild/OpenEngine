// AudioVoiceControlTests.cpp — live control of a voice after it starts:
// volume, pitch and pan setters, pause and resume, on playing voices, pending
// voices (clip still loading) and stale handles.
//
// Harness shape: a REAL AudioSystem on the Silent output (miniaudio's null
// backend: voices advance on the device clock and no device opens). Playing
// voices use an in-memory clip; pending voices use a clip whose load is held
// on the asset job system by a gate job, so the voice stays pending until the
// test releases it.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AudioAsset.h"
#include "Audio/AudioSystem.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include "AudioTestTone.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <thread>

using GameEngine::AssetManager;
using GameEngine::AudioAsset;
using GameEngine::GUID;
namespace Audio = GameEngine::Audio;

namespace
{

constexpr float kDt = 0.016f;

// The tone clip is 0.5 s; every wait below allows ten times that.
constexpr std::chrono::seconds kPlaybackTimeout{5};

// Steps the audio system until the predicate holds or the timeout passes.
// Real time has to pass: voices advance on the audio device's own thread.
bool UpdateUntil(Audio::AudioSystem& audio, const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + kPlaybackTimeout;
    while (!done())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        audio.Update(kDt);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

Audio::PlayOptions LoopingOptions()
{
    Audio::PlayOptions opts{};
    opts.loop = true;
    return opts;
}

} // namespace

class AudioVoiceControlTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Assets = std::make_unique<AssetManager>();
        m_Audio = std::make_unique<Audio::AudioSystem>(*m_Assets);

        Audio::AudioSystemConfig config{};
        config.maxVoices = 32;
        config.output = Audio::AudioOutput::Silent; // voices advance on the null backend; no device
        if (!m_Audio->Initialize(config))
            GTEST_SKIP() << "no audio backend available (miniaudio engine init failed)";

        m_ClipAsset = std::make_shared<AudioAsset>(GUID::Generate(),
                                                   std::filesystem::path("voice-control-test-tone.wav"));
        ASSERT_TRUE(m_ClipAsset->LoadFromData(AudioTestSupport::MakeTestToneWav()));
        m_Clip = m_Audio->ResolveClip(*m_ClipAsset);
        ASSERT_TRUE(m_Clip.IsValid());
    }

    Audio::AudioEmitterHandle PlayStarted(const Audio::PlayOptions& opts)
    {
        const Audio::AudioEmitterHandle voice = m_Audio->Play2D(m_Clip, opts);
        EXPECT_TRUE(m_Audio->IsPlaying(voice)) << "a ready clip starts inside Play2D";
        return voice;
    }

    std::unique_ptr<AssetManager> m_Assets;
    std::unique_ptr<Audio::AudioSystem> m_Audio;
    std::shared_ptr<AudioAsset> m_ClipAsset;
    Audio::AudioClipHandle m_Clip{};
};

TEST_F(AudioVoiceControlTest, SettersReachPlayingVoice)
{
    const Audio::AudioEmitterHandle voice = PlayStarted(LoopingOptions());

    m_Audio->SetVolume(voice, 0.25f);
    m_Audio->SetPitch(voice, 1.5f);
    m_Audio->SetPan(voice, -0.5f);

    EXPECT_FLOAT_EQ(m_Audio->GetVolume(voice), 0.25f);
    EXPECT_FLOAT_EQ(m_Audio->GetPitch(voice), 1.5f);
    EXPECT_FLOAT_EQ(m_Audio->GetPan(voice), -0.5f);
    EXPECT_TRUE(m_Audio->IsPlaying(voice));
}

TEST_F(AudioVoiceControlTest, PanFromPlayOptionsAppliesAtStart)
{
    Audio::PlayOptions opts = LoopingOptions();
    opts.pan = 0.75f;
    const Audio::AudioEmitterHandle voice = PlayStarted(opts);

    EXPECT_FLOAT_EQ(m_Audio->GetPan(voice), 0.75f);
}

TEST_F(AudioVoiceControlTest, SettersClampOrIgnoreOutOfRangeValues)
{
    const Audio::AudioEmitterHandle voice = PlayStarted(LoopingOptions());
    m_Audio->SetPitch(voice, 1.25f);

    m_Audio->SetVolume(voice, -1.0f);
    m_Audio->SetPitch(voice, 0.0f);
    m_Audio->SetPitch(voice, -2.0f);
    m_Audio->SetPan(voice, 3.0f);

    EXPECT_FLOAT_EQ(m_Audio->GetVolume(voice), 0.0f);
    EXPECT_FLOAT_EQ(m_Audio->GetPitch(voice), 1.25f) << "non-positive pitch is ignored";
    EXPECT_FLOAT_EQ(m_Audio->GetPan(voice), 1.0f);
}

// A non-finite volume, pitch or pan would make every sample of the mix non-finite (every
// voice, not only this one): the voice refuses it, keeps its value, and the mix stays finite.
TEST_F(AudioVoiceControlTest, NonFiniteValuesAreRefusedAndTheMixStaysFinite)
{
    std::atomic<std::uint64_t> samples{0};
    std::atomic<std::uint64_t> nonFinite{0};
    m_Audio->SetOutputCaptureCallback(
        [&](const float* data, std::uint32_t frameCount, std::uint32_t channels, std::uint32_t)
        {
            std::uint64_t bad = 0;
            for (std::uint64_t i = 0; i < std::uint64_t{frameCount} * channels; ++i)
                bad += std::isfinite(data[i]) ? 0 : 1;
            samples.fetch_add(std::uint64_t{frameCount} * channels, std::memory_order_relaxed);
            nonFinite.fetch_add(bad, std::memory_order_relaxed);
        });

    Audio::PlayOptions opts = LoopingOptions();
    opts.volume = std::numeric_limits<float>::quiet_NaN();
    const Audio::AudioEmitterHandle voice = PlayStarted(opts);
    EXPECT_FLOAT_EQ(m_Audio->GetVolume(voice), 1.0f) << "a non-finite start volume takes the default";

    m_Audio->SetVolume(voice, 0.5f);
    m_Audio->SetPitch(voice, 1.25f);
    m_Audio->SetPan(voice, -0.25f);
    m_Audio->SetVolume(voice, std::numeric_limits<float>::quiet_NaN());
    m_Audio->SetVolume(voice, std::numeric_limits<float>::infinity());
    m_Audio->SetPitch(voice, std::numeric_limits<float>::infinity());
    m_Audio->SetPan(voice, std::numeric_limits<float>::quiet_NaN());
    EXPECT_FLOAT_EQ(m_Audio->GetVolume(voice), 0.5f);
    EXPECT_FLOAT_EQ(m_Audio->GetPitch(voice), 1.25f);
    EXPECT_FLOAT_EQ(m_Audio->GetPan(voice), -0.25f);

    const std::uint64_t before = samples.load();
    ASSERT_TRUE(UpdateUntil(*m_Audio, [&] { return samples.load() > before + 8192; }))
        << "the output callback carried no mix";
    m_Audio->SetOutputCaptureCallback(nullptr);
    EXPECT_EQ(nonFinite.load(), 0u);
}

TEST_F(AudioVoiceControlTest, SetPanLeavesSpatializedVoiceUntouched)
{
    const Audio::AudioEmitterHandle voice =
        m_Audio->Play3D(m_Clip, 0, GameEngine::Mathematics::Vector3{1.0f, 0.0f, 0.0f},
                        GameEngine::Mathematics::Vector3{}, LoopingOptions());
    ASSERT_TRUE(m_Audio->IsPlaying(voice));

    m_Audio->SetPan(voice, 0.7f);

    EXPECT_FLOAT_EQ(m_Audio->GetPan(voice), 0.0f);
}

TEST_F(AudioVoiceControlTest, StaleHandleIsIgnored)
{
    const Audio::AudioEmitterHandle stale = PlayStarted(LoopingOptions());
    m_Audio->Stop(stale);

    // The next voice reuses the freed slot under a new generation.
    const Audio::AudioEmitterHandle live = PlayStarted(LoopingOptions());
    ASSERT_EQ(live.Index(), stale.Index());

    m_Audio->SetVolume(stale, 0.5f);
    m_Audio->SetPitch(stale, 2.0f);
    m_Audio->SetPan(stale, 1.0f);
    m_Audio->Pause(stale);
    m_Audio->Resume(stale);

    EXPECT_FLOAT_EQ(m_Audio->GetVolume(stale), 0.0f);
    EXPECT_FALSE(m_Audio->IsPaused(stale));
    EXPECT_FLOAT_EQ(m_Audio->GetVolume(live), 1.0f);
    EXPECT_FLOAT_EQ(m_Audio->GetPitch(live), 1.0f);
    EXPECT_FLOAT_EQ(m_Audio->GetPan(live), 0.0f);
    EXPECT_FALSE(m_Audio->IsPaused(live));
    EXPECT_TRUE(m_Audio->IsPlaying(live));

    m_Audio->SetVolume(Audio::INVALID_AUDIO_EMITTER_HANDLE, 0.5f);
    m_Audio->Pause(Audio::INVALID_AUDIO_EMITTER_HANDLE);
    EXPECT_FALSE(m_Audio->IsPaused(Audio::INVALID_AUDIO_EMITTER_HANDLE));
}

TEST_F(AudioVoiceControlTest, PauseKeepsVoiceAliveAndResumePlaysIt)
{
    const Audio::AudioEmitterHandle voice = PlayStarted(LoopingOptions());

    m_Audio->Pause(voice);
    for (int i = 0; i < 4; ++i)
        m_Audio->Update(kDt);

    EXPECT_TRUE(m_Audio->IsAlive(voice));
    EXPECT_TRUE(m_Audio->IsPaused(voice));
    EXPECT_FALSE(m_Audio->IsPlaying(voice));

    m_Audio->Resume(voice);

    EXPECT_FALSE(m_Audio->IsPaused(voice));
    EXPECT_TRUE(m_Audio->IsPlaying(voice));
}

TEST_F(AudioVoiceControlTest, PausedOneShotIsNotReclaimedAndFinishesAfterResume)
{
    const Audio::AudioEmitterHandle paused = PlayStarted({});
    const Audio::AudioEmitterHandle control = PlayStarted({});
    m_Audio->Pause(paused);

    // The control one-shot running out proves the clip's length has passed
    // and the reaper has run.
    ASSERT_TRUE(UpdateUntil(*m_Audio, [&] { return !m_Audio->IsAlive(control); }))
        << "the unpaused one-shot never finished";
    EXPECT_TRUE(m_Audio->IsAlive(paused)) << "a paused voice was reclaimed as finished";
    EXPECT_TRUE(m_Audio->IsPaused(paused));

    m_Audio->Resume(paused);
    EXPECT_TRUE(m_Audio->IsPlaying(paused));
    EXPECT_TRUE(UpdateUntil(*m_Audio, [&] { return !m_Audio->IsAlive(paused); }))
        << "the resumed one-shot never finished";
}

TEST_F(AudioVoiceControlTest, OneShotPausedAtItsEndIsKeptUntilResumedThenReclaimed)
{
    const Audio::AudioEmitterHandle paused = PlayStarted({});
    const Audio::AudioEmitterHandle control = PlayStarted({});

    // No Update while both run out: neither is reaped before the pause.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    m_Audio->Pause(paused);
    m_Audio->Update(kDt);

    ASSERT_FALSE(m_Audio->IsAlive(control)) << "the clip had not reached its end yet";
    EXPECT_TRUE(m_Audio->IsAlive(paused)) << "a paused voice was reclaimed as finished";

    m_Audio->Resume(paused);
    EXPECT_FALSE(m_Audio->IsPlaying(paused)) << "a finished one-shot restarted on resume";
    m_Audio->Update(kDt);
    EXPECT_FALSE(m_Audio->IsAlive(paused));
}

// Pending voices: the clip's load is in flight on a one-worker job system
// whose worker is held by a gate job, so voices on it stay pending until the
// gate opens.
class AudioPendingVoiceControlTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Gate = m_GateOpen.get_future().share();
        m_JobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(1);
        m_JobSystem->Submit([gate = m_Gate] { gate.wait(); });

        m_Assets = std::make_unique<AssetManager>();
        ASSERT_TRUE(m_Assets->Initialize(m_JobSystem.get()));

        m_Audio = std::make_unique<Audio::AudioSystem>(*m_Assets);
        Audio::AudioSystemConfig config{};
        config.maxVoices = 32;
        config.output = Audio::AudioOutput::Silent; // voices advance on the null backend; no device
        if (!m_Audio->Initialize(config))
            GTEST_SKIP() << "no audio backend available (miniaudio engine init failed)";

        m_Dir = GameEngine::TestUtils::MakeUniqueTempDirectory("audio_pending_voice");
        std::filesystem::create_directories(m_Dir);
        const std::filesystem::path path = m_Dir / "pending-tone.wav";
        const auto wav = AudioTestSupport::MakeTestToneWav();
        {
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
        }

        GameEngine::AssetMetadata metadata;
        metadata.Guid = m_Assets->GetRegistry().GetOrCreateAssetGUID(path);
        metadata.Path = path;
        metadata.Name = path.stem().string();
        metadata.Extension = path.extension().string();
        metadata.Type = GameEngine::AssetType::Audio;
        metadata.FileSize = wav.size();
        metadata.LastModified = std::filesystem::last_write_time(path);
        m_Assets->GetRegistry().RegisterAssetMetadata(metadata);

        m_Clip = m_Audio->ResolveClip(metadata.Guid);
        ASSERT_TRUE(m_Clip.IsValid());
    }

    void TearDown() override
    {
        OpenGate();
        m_Audio.reset();
        if (m_Assets)
            m_Assets->Shutdown();
        m_Assets.reset();
        if (m_JobSystem)
            m_JobSystem->Shutdown();
        m_JobSystem.reset();
        if (!m_Dir.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(m_Dir, ec);
        }
    }

    void OpenGate()
    {
        if (m_GateOpened)
            return;
        m_GateOpened = true;
        m_GateOpen.set_value();
    }

    Audio::AudioEmitterHandle PlayPending(const Audio::PlayOptions& opts)
    {
        const Audio::AudioEmitterHandle voice = m_Audio->Play2D(m_Clip, opts);
        EXPECT_TRUE(m_Audio->IsAlive(voice));
        EXPECT_FALSE(m_Audio->IsPlaying(voice)) << "the clip's load is held, so the voice is pending";
        return voice;
    }

    std::promise<void> m_GateOpen;
    std::shared_future<void> m_Gate;
    bool m_GateOpened = false;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_JobSystem;
    std::unique_ptr<AssetManager> m_Assets;
    std::unique_ptr<Audio::AudioSystem> m_Audio;
    std::filesystem::path m_Dir;
    Audio::AudioClipHandle m_Clip{};
};

TEST_F(AudioPendingVoiceControlTest, SettersOnPendingVoiceApplyWhenItStarts)
{
    const Audio::AudioEmitterHandle voice = PlayPending(LoopingOptions());

    m_Audio->SetVolume(voice, 0.3f);
    m_Audio->SetPitch(voice, 0.8f);
    m_Audio->SetPan(voice, 0.4f);
    EXPECT_FLOAT_EQ(m_Audio->GetVolume(voice), 0.3f);

    OpenGate();
    ASSERT_TRUE(UpdateUntil(*m_Audio, [&] { return m_Audio->IsPlaying(voice); }))
        << "the pending voice never started";

    EXPECT_FLOAT_EQ(m_Audio->GetVolume(voice), 0.3f);
    EXPECT_FLOAT_EQ(m_Audio->GetPitch(voice), 0.8f);
    EXPECT_FLOAT_EQ(m_Audio->GetPan(voice), 0.4f);
}

TEST_F(AudioPendingVoiceControlTest, PausedPendingVoiceStartsOnlyWhenResumed)
{
    const Audio::AudioEmitterHandle paused = PlayPending(LoopingOptions());
    const Audio::AudioEmitterHandle control = PlayPending(LoopingOptions());
    m_Audio->Pause(paused);

    OpenGate();
    ASSERT_TRUE(UpdateUntil(*m_Audio, [&] { return m_Audio->IsPlaying(control); }))
        << "the unpaused pending voice never started";
    EXPECT_TRUE(m_Audio->IsAlive(paused));
    EXPECT_TRUE(m_Audio->IsPaused(paused));
    EXPECT_FALSE(m_Audio->IsPlaying(paused)) << "a paused pending voice started";

    m_Audio->Resume(paused);
    m_Audio->Update(kDt);

    EXPECT_TRUE(m_Audio->IsPlaying(paused));
}
