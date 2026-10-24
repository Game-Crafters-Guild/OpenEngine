// AudioOutputTests.cpp: the Silent output keeps every observable voice
// behaviour of the Device output and opens no device. Both outputs run the same
// case: a voice starts, plays, reaches its end and is reaped, and the output
// callback carries the mix on the device clock. Voices play at volume 0 except
// in the Silent-only case, so nothing here is audible.

#include <gtest/gtest.h>

#include "AudioTestTone.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AudioAsset.h"
#include "Audio/AudioSystem.h"
#include "Core/EngineLoggerBridge.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using GameEngine::AssetManager;
using GameEngine::AudioAsset;
using GameEngine::GUID;
namespace Audio = GameEngine::Audio;

namespace
{

constexpr float kDt = 0.016f;
// The tone is 0.5 s; a voice is reaped, and a meter rises, well inside this.
constexpr std::chrono::seconds kTimeout{5};

// Every log line since the process first asked. Log::AddSink owns the sink, so the
// lines are shared_ptr-owned and the one sink serves every test.
struct LogLines
{
    std::mutex Mutex;
    std::vector<std::string> Lines;

    bool Contains(const std::string& needle)
    {
        Logger::Log::Flush();
        std::lock_guard lock(Mutex);
        for (const std::string& line : Lines)
        {
            if (line.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }
};

std::shared_ptr<LogLines> ProcessLog()
{
    static const std::shared_ptr<LogLines> log = []
    {
        // The audio module logs through Engine.dll's Logger state; adopt it, or the capture reads nothing.
        Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
        Logger::Log::Initialize({});
        auto lines = std::make_shared<LogLines>();
        auto sink = std::make_unique<Logger::CallbackSink>();
        sink->RegisterCallback([lines](const Logger::LogMessage& message)
                               {
                                   std::lock_guard lock(lines->Mutex);
                                   lines->Lines.push_back(message.Message);
                               });
        Logger::Log::AddSink(std::move(sink));
        return lines;
    }();
    return log;
}

} // namespace

class AudioOutputHarness : public ::testing::Test
{
protected:
    void Start(Audio::AudioOutput output)
    {
        m_Log = ProcessLog();
        m_Assets = std::make_unique<AssetManager>();
        m_Audio = std::make_unique<Audio::AudioSystem>(*m_Assets);

        Audio::AudioSystemConfig config{};
        config.output = output;
        ASSERT_TRUE(m_Audio->Initialize(config));

        m_Clip = std::make_shared<AudioAsset>(GUID::Generate(), std::filesystem::path("audio-output-test-tone.wav"));
        ASSERT_TRUE(m_Clip->LoadFromData(AudioTestSupport::MakeTestToneWav()));
        m_ClipHandle = m_Audio->ResolveClip(*m_Clip);
        ASSERT_TRUE(m_ClipHandle.IsValid());

        m_Audio->SetOutputCaptureCallback([this](const float*, std::uint32_t frameCount, std::uint32_t, std::uint32_t)
                                          { m_CapturedFrames.fetch_add(frameCount, std::memory_order_relaxed); });
    }

    void TearDown() override
    {
        if (m_Audio)
            m_Audio->SetOutputCaptureCallback(nullptr);
    }

    // Steps the system at the frame cadence until `done` holds; false on timeout.
    template <typename Predicate>
    bool UpdateUntil(Predicate done)
    {
        const auto deadline = std::chrono::steady_clock::now() + kTimeout;
        while (!done())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            m_Audio->Update(kDt);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        return true;
    }

    std::shared_ptr<LogLines> m_Log;
    std::unique_ptr<AssetManager> m_Assets;
    std::unique_ptr<Audio::AudioSystem> m_Audio;
    std::shared_ptr<AudioAsset> m_Clip;
    Audio::AudioClipHandle m_ClipHandle;
    std::atomic<std::uint64_t> m_CapturedFrames{0};
};

class AudioOutputTest : public AudioOutputHarness, public ::testing::WithParamInterface<Audio::AudioOutput>
{
protected:
    void SetUp() override { Start(GetParam()); }
};

TEST_P(AudioOutputTest, AVoiceStartsPlaysToItsEndAndIsReaped)
{
    Audio::PlayOptions options{};
    options.volume = 0.0f;
    const Audio::AudioEmitterHandle voice = m_Audio->Play2D(m_ClipHandle, options);
    ASSERT_TRUE(voice.IsValid());
    EXPECT_TRUE(m_Audio->IsAlive(voice));

    m_Audio->Update(kDt);
    EXPECT_TRUE(m_Audio->IsPlaying(voice));
    EXPECT_EQ(m_Audio->GetAliveVoiceCount(), 1u);

    EXPECT_TRUE(UpdateUntil([&] { return !m_Audio->IsAlive(voice); })) << "the voice never reached its end";
    EXPECT_FALSE(m_Audio->IsPlaying(voice));
    EXPECT_EQ(m_Audio->GetAliveVoiceCount(), 0u);
    EXPECT_GT(m_CapturedFrames.load(std::memory_order_relaxed), 0u) << "the output callback never ran";
}

INSTANTIATE_TEST_SUITE_P(BothOutputs, AudioOutputTest,
                         ::testing::Values(Audio::AudioOutput::Device, Audio::AudioOutput::Silent),
                         [](const ::testing::TestParamInfo<Audio::AudioOutput>& info)
                         { return info.param == Audio::AudioOutput::Silent ? "Silent" : "Device"; });

class SilentAudioOutputTest : public AudioOutputHarness
{
protected:
    void SetUp() override { Start(Audio::AudioOutput::Silent); }
};

// The null backend is the proof that no device was opened. It is asserted before
// the full-volume voice starts, so a regression to the device fails without a sound.
TEST_F(SilentAudioOutputTest, MixesOnTheNullBackendAndTheMeterReadsTheSignal)
{
    ASSERT_TRUE(m_Log->Contains("Miniaudio backend initialized (backend=Null"));

    Audio::PlayOptions options{};
    options.loop = true;
    options.bus = 2; // SFX: the master meter is then fed only by the mixed signal
    const Audio::AudioEmitterHandle voice = m_Audio->Play2D(m_ClipHandle, options);
    ASSERT_TRUE(voice.IsValid());

    EXPECT_TRUE(UpdateUntil([&] { return m_Audio->GetBusMeterLevel(0) > 0.0f; }))
        << "the master meter never saw the mixed signal";
    m_Audio->Stop(voice);
}
