// AudioOggVorbisTests.cpp — Ogg Vorbis clips load, decode and play through
// the same miniaudio paths as WAV, FLAC and MP3: the asset's probe and
// decode-to-PCM (AudioAsset), and the per-voice decoder a streamed clip plays
// from (AudioSystem). Ogg Opus, which shares the .ogg extension, is refused
// with a message that names the codec.
//
// Fixtures (Engine/Tests/Fixtures/Audio, staged by StageTestAssets): 0.5 s of
// a 440 Hz sine at amplitude 1/8, mono, encoded once as Vorbis at 8 kHz and
// once as Opus at 48 kHz; and three corruptions of the Vorbis file (two
// malformed headers, one forged length), each with valid page CRCs.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Assets/AudioAsset.h"
#include "Audio/AudioSystem.h"
#include "EngineLogCapture.h"
#include "Logger/Logger.h"
#include "StagedTestPaths.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
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
    config.output = Audio::AudioOutput::Silent; // a voice is all this needs: no device
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
    std::vector<std::string> lines;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&lines);
        AudioAsset asset(GUID::Generate(), FixturePath("tone-440hz-opus.ogg"));
        EXPECT_FALSE(asset.Load());
    }
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(lines, "Ogg Opus"), 1u);
}

// Two corruptions of the Vorbis fixture with valid page CRCs whose comment headers are
// malformed. They decoded through stb_vorbis corrupted the heap (the first) and read through
// a null pointer (the second) while the decoder was being opened; a file a user drops into a
// project is decoded at import, so either took the editor down. libvorbis refuses both.
TEST(AudioOggVorbis, MalformedHeadersAreRefusedByName)
{
    for (const char* name : {"tone-440hz-vorbis-bad-header-a.ogg", "tone-440hz-vorbis-bad-header-b.ogg"})
    {
        std::vector<std::string> lines;
        {
            GameEngine::TestLog::ScopedEngineLogCapture capture(&lines);
            AudioAsset asset(GUID::Generate(), FixturePath(name));
            EXPECT_FALSE(asset.Load()) << name;
        }
        const std::string stem = std::filesystem::path(name).stem().string();
        EXPECT_FALSE(GameEngine::TestLog::FirstLineContaining(lines, "'" + stem + "'").empty())
            << name << ": no log line names the file";
    }
}

// The fixture with its last page's granule position forged to 2^40 (CRC fixed). The clip's
// PCM buffer is sized from the length the decoder reports (#4074 tracks that pre-size for
// every format); libvorbis reports the stream's real length, so this file decodes to its
// 0.5 s instead of committing gigabytes.
TEST(AudioOggVorbis, AForgedLengthDecodesToTheStreamsRealLength)
{
    AudioAsset asset(GUID::Generate(), FixturePath("tone-440hz-vorbis-forged-length.ogg"));
    ASSERT_TRUE(asset.Load());
    EXPECT_LE(asset.GetPCMFrameCount(), static_cast<GameEngine::uint64>(2 * kToneSeconds * kVorbisSampleRate));
    EXPECT_LT(asset.GetDuration(), 1.0f);
}
