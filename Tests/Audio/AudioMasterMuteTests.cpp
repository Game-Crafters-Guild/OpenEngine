// AudioMasterMuteTests.cpp — AudioSystem::SetMasterMuted silences the speakers while
// every voice keeps playing and advancing, the output capture keeps the full mix (so a
// recording keeps its sound), and the bus volumes stay as they were.
//
// Instruments: the output capture callback sees the full mix the engine produced; the
// Master bus meter sees what the device was handed. Both come from the backend's own
// device callback. The tests run on the Silent output: the null backend's thread runs
// that same callback and opens no device, so nothing here is audible and the
// instruments exist on a machine with no audio device too.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AudioAsset.h"
#include "Audio/AudioSystem.h"

#include "AudioTestTone.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>

using GameEngine::AssetManager;
using GameEngine::AudioAsset;
using GameEngine::GUID;
namespace Audio = GameEngine::Audio;

namespace
{

constexpr float kDt = 0.016f;
constexpr float kQuietVolume = 0.02f;
// Blocks discarded after a state change so a block mixed before it cannot count.
constexpr std::uint64_t kSettleBlocks = 4;
// Blocks whose peak is measured after settling.
constexpr std::uint64_t kMeasureBlocks = 8;
constexpr auto kBlockWaitTimeout = std::chrono::seconds(3);
// The test tone lasts 0.5 s; a one-shot must be reaped well inside this.
constexpr auto kOneShotFinishTimeout = std::chrono::seconds(3);
constexpr auto kPollInterval = std::chrono::milliseconds(5);
// A meter update this long ends the meter's peak hold: the Master meter then reads the
// latest device block's peak when it is louder than the held value, and zero otherwise.
constexpr float kMeterReleaseDt = 1.0f;
constexpr Audio::AudioBusId kMasterBus = 0;

// Written from the audio thread, read from the test thread.
struct OutputProbe
{
    std::atomic<std::uint64_t> Blocks{0};
    std::atomic<float> MaxPeak{0.0f};

    void OnBlock(const float* samples, std::uint32_t frameCount, std::uint32_t channels)
    {
        float peak = 0.0f;
        const std::uint64_t count = static_cast<std::uint64_t>(frameCount) * channels;
        for (std::uint64_t i = 0; i < count; ++i)
            peak = std::max(peak, std::fabs(samples[i]));
        float previous = MaxPeak.load(std::memory_order_relaxed);
        while (peak > previous && !MaxPeak.compare_exchange_weak(previous, peak, std::memory_order_relaxed))
        {
        }
        Blocks.fetch_add(1, std::memory_order_release);
    }
};

} // namespace

class AudioMasterMuteTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Assets = std::make_unique<AssetManager>();
        m_Audio = std::make_unique<Audio::AudioSystem>(*m_Assets);
    }

    void TearDown() override
    {
        if (m_Audio)
        {
            m_Audio->SetOutputCaptureCallback(nullptr);
            m_Audio->Shutdown();
        }
    }

    // Initializes the system on the Silent output, installs the probe and loads the tone.
    // Returns why the instrument did not start, or nullptr when it is ready.
    const char* StartAudio()
    {
        Audio::AudioSystemConfig config{};
        config.maxVoices = 8;
        config.output = Audio::AudioOutput::Silent;
        if (!m_Audio->Initialize(config))
            return "the audio system did not initialize on the Silent output";
        m_Audio->SetOutputCaptureCallback(
            [probe = &m_Probe](const float* samples, std::uint32_t frames, std::uint32_t channels, std::uint32_t)
            { probe->OnBlock(samples, frames, channels); });
        if (!WaitForBlocks(1))
            return "the output capture never fired on the null backend";

        m_ClipGuid = GUID::Generate();
        m_Clip = std::make_shared<AudioAsset>(m_ClipGuid, std::filesystem::path("audio-mute-test-tone.wav"));
        EXPECT_TRUE(m_Clip->LoadFromData(AudioTestSupport::MakeTestToneWav()));
        EXPECT_TRUE(m_Audio->ResolveClip(*m_Clip).IsValid());
        return nullptr;
    }

    Audio::AudioEmitterHandle PlayTone(bool loop)
    {
        Audio::PlayOptions opts{};
        opts.volume = kQuietVolume;
        opts.loop = loop;
        const Audio::AudioEmitterHandle voice = m_Audio->Play2D(m_ClipGuid, opts);
        m_Audio->Update(kDt); // starts the pending voice
        return voice;
    }

    bool WaitForBlocks(std::uint64_t count)
    {
        const std::uint64_t target = m_Probe.Blocks.load(std::memory_order_acquire) + count;
        const auto deadline = std::chrono::steady_clock::now() + kBlockWaitTimeout;
        while (m_Probe.Blocks.load(std::memory_order_acquire) < target)
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(kPollInterval);
        }
        return true;
    }

    // Peak of the full mix the capture tap saw over kMeasureBlocks blocks mixed after the
    // current state took effect. Negative when the device stopped delivering blocks.
    float MeasureCapturePeak()
    {
        if (!WaitForBlocks(kSettleBlocks))
            return -1.0f;
        m_Probe.MaxPeak.store(0.0f, std::memory_order_relaxed);
        if (!WaitForBlocks(kMeasureBlocks))
            return -1.0f;
        return m_Probe.MaxPeak.load(std::memory_order_relaxed);
    }

    // Loudest Master meter reading over kMeasureBlocks device blocks, one meter update
    // per block: whatever the held value, an audible block shows within two readings, and
    // a silent device reads zero every time. Negative when the device stopped delivering.
    float MeasureSpeakerPeak()
    {
        if (!WaitForBlocks(kSettleBlocks))
            return -1.0f;
        float loudest = 0.0f;
        for (std::uint64_t i = 0; i < kMeasureBlocks; ++i)
        {
            if (!WaitForBlocks(1))
                return -1.0f;
            m_Audio->Update(kMeterReleaseDt);
            loudest = std::max(loudest, m_Audio->GetBusMeterLevel(kMasterBus));
        }
        return loudest;
    }

    // Destruction order (reverse of declaration): audio (Shutdown detaches the asset
    // callback while m_Assets is alive), then assets.
    std::unique_ptr<AssetManager> m_Assets;
    std::unique_ptr<Audio::AudioSystem> m_Audio;
    OutputProbe m_Probe;
    GUID m_ClipGuid;
    std::shared_ptr<AudioAsset> m_Clip;
};

TEST_F(AudioMasterMuteTest, MuteSilencesTheSpeakersAndUnmuteRestoresThem)
{
    if (const char* unavailable = StartAudio())
        FAIL() << unavailable;
    const Audio::AudioEmitterHandle voice = PlayTone(/*loop=*/true);
    ASSERT_TRUE(m_Audio->IsPlaying(voice));
    ASSERT_GT(MeasureSpeakerPeak(), 0.0f) << "the playing tone must reach the speakers before muting";

    m_Audio->SetMasterMuted(true);
    EXPECT_TRUE(m_Audio->IsMasterMuted());
    const float mutedPeak = MeasureSpeakerPeak();
    ASSERT_GE(mutedPeak, 0.0f) << "the device stopped delivering output blocks";
    EXPECT_EQ(mutedPeak, 0.0f) << "the speakers must receive exact silence while muted";
    EXPECT_TRUE(m_Audio->IsPlaying(voice)) << "muting must not stop or pause voices";

    m_Audio->SetMasterMuted(false);
    EXPECT_FALSE(m_Audio->IsMasterMuted());
    EXPECT_GT(MeasureSpeakerPeak(), 0.0f) << "unmuting must restore the speakers";
}

TEST_F(AudioMasterMuteTest, MutedOutputStillReachesTheCaptureAtFullLevel)
{
    if (const char* unavailable = StartAudio())
        FAIL() << unavailable;
    const Audio::AudioEmitterHandle voice = PlayTone(/*loop=*/true);
    ASSERT_TRUE(m_Audio->IsPlaying(voice));
    const float unmutedCapture = MeasureCapturePeak();
    ASSERT_GT(unmutedCapture, 0.0f);

    m_Audio->SetMasterMuted(true);
    const float mutedCapture = MeasureCapturePeak();
    EXPECT_GT(mutedCapture, 0.0f) << "a recording made while muted must keep its sound";
    EXPECT_NEAR(mutedCapture, unmutedCapture, unmutedCapture * 0.5f)
        << "the capture must receive the mix at its unmuted level";
}

TEST_F(AudioMasterMuteTest, MutedOneShotKeepsAdvancingAndFinishes)
{
    if (const char* unavailable = StartAudio())
        FAIL() << unavailable;
    m_Audio->SetMasterMuted(true);
    const Audio::AudioEmitterHandle voice = PlayTone(/*loop=*/false);
    ASSERT_TRUE(m_Audio->IsAlive(voice));

    // A paused voice would never reach its end; a muted one finishes on its own clock.
    const auto deadline = std::chrono::steady_clock::now() + kOneShotFinishTimeout;
    while (m_Audio->IsAlive(voice) && std::chrono::steady_clock::now() < deadline)
    {
        m_Audio->Update(kDt);
        std::this_thread::sleep_for(kPollInterval);
    }
    EXPECT_FALSE(m_Audio->IsAlive(voice)) << "the muted one-shot must play through and be reaped";
}

TEST_F(AudioMasterMuteTest, MuteLeavesBusVolumesUntouched)
{
    if (const char* unavailable = StartAudio())
        FAIL() << unavailable;
    constexpr Audio::AudioBusId kMusicBus = 1;
    m_Audio->SetBusVolume(kMasterBus, 0.75f);
    m_Audio->SetBusVolume(kMusicBus, 0.5f);

    m_Audio->SetMasterMuted(true);
    EXPECT_FLOAT_EQ(m_Audio->GetBusVolume(kMasterBus), 0.75f);
    EXPECT_FLOAT_EQ(m_Audio->GetBusVolume(kMusicBus), 0.5f);

    m_Audio->SetMasterMuted(false);
    EXPECT_FLOAT_EQ(m_Audio->GetBusVolume(kMasterBus), 0.75f);
    EXPECT_FLOAT_EQ(m_Audio->GetBusVolume(kMusicBus), 0.5f);
}

TEST_F(AudioMasterMuteTest, MuteSetBeforeInitializeAppliesOnceTheDeviceStarts)
{
    m_Audio->SetMasterMuted(true);
    if (const char* unavailable = StartAudio())
        FAIL() << unavailable;
    EXPECT_TRUE(m_Audio->IsMasterMuted());
    const Audio::AudioEmitterHandle voice = PlayTone(/*loop=*/true);
    ASSERT_TRUE(m_Audio->IsPlaying(voice));
    const float mutedPeak = MeasureSpeakerPeak();
    ASSERT_GE(mutedPeak, 0.0f) << "the device stopped delivering output blocks";
    EXPECT_EQ(mutedPeak, 0.0f) << "a mute requested before the device started must apply to it";
}
