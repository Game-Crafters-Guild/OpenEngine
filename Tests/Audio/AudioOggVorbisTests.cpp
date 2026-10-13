// AudioOggVorbisTests.cpp — Ogg Vorbis clips load, decode and play through
// the same miniaudio paths as WAV, FLAC and MP3: the asset's probe and
// decode-to-PCM (AudioAsset), and the per-voice decoder a streamed clip plays
// from (AudioSystem). Ogg Opus, which shares the .ogg extension, is refused
// with a message that names the codec.
//
// Fixtures (Engine/Tests/Fixtures/Audio, staged by StageTestAssets): 0.5 s of
// a 440 Hz sine at amplitude 1/8, mono, encoded once as Vorbis at 8 kHz and
// once as Opus at 48 kHz.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Assets/AudioAsset.h"
#include "Audio/AudioSystem.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "StagedTestPaths.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using GameEngine::AssetManager;
using GameEngine::AudioAsset;
using GameEngine::GUID;
namespace Audio = GameEngine::Audio;

namespace
{

constexpr float kToneSeconds = 0.5f;
constexpr float kToneHz = 440.0f;
constexpr float kToneAmplitude = 0.125f;
constexpr GameEngine::uint32 kVorbisSampleRate = 8000;

std::filesystem::path FixturePath(const char* name)
{
    return GameEngine::TestPaths::StagedRoot() / "Engine/Tests/Fixtures/Audio" / name;
}

GameEngine::Vector<GameEngine::uint8> ReadFixture(const char* name)
{
    GameEngine::Vector<GameEngine::uint8> bytes;
    EXPECT_TRUE(GameEngine::ReadFileBytesShared(FixturePath(name), bytes)) << FixturePath(name);
    return bytes;
}

// Sign changes across the mono PCM: a decoded 440 Hz tone crosses zero twice
// per cycle, so a wrong rate or garbage output lands far from 2 * Hz * seconds.
std::size_t CountZeroCrossings(const float* samples, std::size_t count)
{
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < count; ++i)
    {
        if ((samples[i - 1] < 0.0f) != (samples[i] < 0.0f))
            ++crossings;
    }
    return crossings;
}

// Collects log lines for a "did it say X" assertion. The logger's level is Off
// until configured, and delivery is asynchronous, so the capture configures it
// and Flush()es before reading.
class ScopedLogCapture
{
  public:
    ScopedLogCapture()
    {
        Logger::Log::Initialize({Logger::LogLevel::Info, false});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        m_Sink = sink.get();
        Logger::Log::AddSink(std::move(sink));
        m_CallbackId = m_Sink->RegisterCallback([this](const Logger::LogMessage& message) {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Lines.push_back(std::string(message.Message));
        });
    }

    ~ScopedLogCapture() { m_Sink->UnregisterCallback(m_CallbackId); }

    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

    bool AnyContaining(const std::string& needle)
    {
        Logger::Log::Flush();
        std::lock_guard<std::mutex> lock(m_Mutex);
        return std::any_of(m_Lines.begin(), m_Lines.end(), [&needle](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    }

  private:
    Logger::CallbackSink* m_Sink = nullptr;
    GameEngine::uint64 m_CallbackId = 0;
    std::mutex m_Mutex;
    std::vector<std::string> m_Lines;
};

} // namespace

TEST(AudioOggVorbis, AssetLoadsAndDecodesToPcm)
{
    AudioAsset asset(GUID::Generate(), FixturePath("tone-440hz-vorbis.ogg"));
    ASSERT_TRUE(asset.Load());

    EXPECT_EQ(asset.GetFormat(), GameEngine::AudioFormat::OGG);
    EXPECT_EQ(asset.GetChannels(), GameEngine::AudioChannels::Mono);
    EXPECT_EQ(asset.GetSampleRate(), kVorbisSampleRate);
    EXPECT_NEAR(asset.GetDuration(), kToneSeconds, 0.01f);

    ASSERT_EQ(asset.GetPCMFormat(), GameEngine::AudioPCMFormat::F32);
    ASSERT_NE(asset.GetPCMFloatData(), nullptr);
    const auto frames = static_cast<std::size_t>(asset.GetPCMFrameCount());
    EXPECT_NEAR(static_cast<double>(frames), kToneSeconds * kVorbisSampleRate, 80.0);

    const double expectedCrossings = 2.0 * kToneHz * kToneSeconds;
    EXPECT_NEAR(static_cast<double>(CountZeroCrossings(asset.GetPCMFloatData(), frames)),
                expectedCrossings, expectedCrossings * 0.05);

    const float peak = std::abs(*std::max_element(
        asset.GetPCMFloatData(), asset.GetPCMFloatData() + frames,
        [](float a, float b) { return std::abs(a) < std::abs(b); }));
    EXPECT_NEAR(peak, kToneAmplitude, 0.03f);
}

TEST(AudioOggVorbis, StreamedClipPlaysThroughPerVoiceDecoder)
{
    AssetManager assets;
    Audio::AudioSystem audio(assets);
    Audio::AudioSystemConfig config{};
    config.maxVoices = 4;
    if (!audio.Initialize(config))
        GTEST_SKIP() << "no audio backend available (miniaudio engine init failed)";

    AudioAsset clip(GUID::Generate(), "audio-ogg-vorbis-stream.ogg");
    AudioAsset::RuntimeSettings settings{};
    settings.loadPolicy = GameEngine::AudioLoadPolicy::Stream;
    ASSERT_TRUE(clip.LoadFromDataWithRuntimeSettings(ReadFixture("tone-440hz-vorbis.ogg"), settings));
    ASSERT_TRUE(clip.HasEncodedData());
    EXPECT_EQ(clip.GetSampleRate(), kVorbisSampleRate);
    EXPECT_NEAR(clip.GetDuration(), kToneSeconds, 0.01f);

    const Audio::AudioClipHandle handle = audio.ResolveClip(clip);
    ASSERT_TRUE(handle.IsValid());

    Audio::PlayOptions options{};
    options.volume = 0.0f; // silent
    options.loop = true;   // stays alive however long the update takes
    const Audio::AudioEmitterHandle voice = audio.Play2D(handle, options);
    ASSERT_TRUE(voice.IsValid());

    audio.Update(0.016f); // a voice whose decoder fails to open is stopped here
    EXPECT_TRUE(audio.IsAlive(voice));
    EXPECT_TRUE(audio.IsPlaying(voice));

    audio.Stop(voice);
    audio.Shutdown();
}

TEST(AudioOggVorbis, OggOpusIsRefusedByName)
{
    ScopedLogCapture log;
    AudioAsset asset(GUID::Generate(), FixturePath("tone-440hz-opus.ogg"));
    EXPECT_FALSE(asset.Load());
    EXPECT_TRUE(log.AnyContaining("Ogg Opus"));
}
