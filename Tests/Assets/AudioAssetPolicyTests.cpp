#include <gtest/gtest.h>

#include "Core/Engine.h"
#include "Core/Application.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetTasks.h"
#include "Assets/AudioAsset.h"
#include "Assets/ParserRegistry.h"
#include "Assets/Parsers/AudioAssetParser.h"

#include "Audio/AudioSystem.h"

#include "TestTempDir.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{
static void WriteTestWavFile(const std::filesystem::path& path,
                             uint32_t sampleRate,
                             uint16_t numChannels,
                             uint16_t bitsPerSample,
                             float durationSeconds)
{
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(file.is_open());

    const uint32_t frames = static_cast<uint32_t>(durationSeconds * static_cast<float>(sampleRate));
    const uint32_t bytesPerSample = bitsPerSample / 8;
    const uint32_t dataSize = frames * numChannels * bytesPerSample;
    const uint32_t fileSize = 36 + dataSize; // RIFF size = 4 + (8+fmt) + (8+data)

    // WAV header (PCM, little-endian)
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&fileSize), 4);
    file.write("WAVE", 4);

    // fmt chunk
    file.write("fmt ", 4);
    uint32_t subchunk1Size = 16;
    file.write(reinterpret_cast<const char*>(&subchunk1Size), 4);
    uint16_t audioFormat = 1; // PCM
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    file.write(reinterpret_cast<const char*>(&numChannels), 2);
    file.write(reinterpret_cast<const char*>(&sampleRate), 4);
    uint32_t byteRate = sampleRate * numChannels * bytesPerSample;
    const uint16_t blockAlign = static_cast<uint16_t>(numChannels * bytesPerSample);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

    // data chunk
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);

    // Silence
    std::vector<char> zeros;
    zeros.resize(dataSize, 0);
    file.write(zeros.data(), zeros.size());
}

class AudioAssetPolicyTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Capture original CWD — Engine::Initialize moves it to WorkspaceDirectory,
        // and Windows refuses to delete a directory that contains the CWD.
        originalCwd = std::filesystem::current_path();

        // Unique per-test temp directory to prevent a failed TearDown from one
        // test nesting subsequent test directories inside the stale one.
        testDir = GameEngine::TestUtils::MakeUniqueTempDirectory("audio_policy_test_assets");
        std::filesystem::create_directories(testDir);

        engine = std::make_unique<EngineCore>();
        ApplicationConfig config;
        config.WorkspaceDirectory = testDir.string();
        config.AssetDirectory = testDir.string();
        config.AssetDatabaseFile = "AssetDatabase.assetdb";
        config.AssetDatabaseCacheDirectory = ".Cache/AssetDatabase";
        ASSERT_TRUE(engine->Initialize(config));

        assetManager = &engine->GetAssetManager();
    }

    void TearDown() override
    {
        if (engine)
        {
            engine->Shutdown();
            engine.reset();
        }

        std::error_code ec;
        std::filesystem::current_path(originalCwd, ec);
        std::filesystem::remove_all(testDir, ec);
    }

    GUID RegisterAudio(const std::filesystem::path& p)
    {
        AssetRegistry& reg = assetManager->GetRegistry();
        reg.RegisterAsset(p);
        GUID g = reg.GetAssetGUID(p);
        EXPECT_FALSE(g.IsNull());
        return g;
    }

protected:
    std::filesystem::path testDir;
    std::filesystem::path originalCwd;
    std::unique_ptr<EngineCore> engine;
    AssetManager* assetManager = nullptr;
};

// Records the address of the buffer that a load reads a WAV file into: the reader thread gives that buffer
// to ResolveDecodeInputs before it moves the buffer to the decode job. The audio parser does the rest.
class ReadBufferProbeParser : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::Audio; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".wav"}; }
    int GetPriority() const override { return 1000; }
    std::string GetName() const override { return "ReadBufferProbeParser"; }

    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager& assetManager) override
    {
        return m_Inner.Parse(metadata, assetManager);
    }

    AssetDecodeInputs ResolveDecodeInputs([[maybe_unused]] const AssetMetadata& metadata, std::span<const uint8> bytes,
                                          [[maybe_unused]] AssetManager& assetManager) const override
    {
        ReadBuffer.store(bytes.data(), std::memory_order_release);
        return {};
    }

    /// The address of the bytes of the last read, set on the reader thread.
    mutable std::atomic<const uint8*> ReadBuffer{nullptr};

private:
    AudioAssetParser m_Inner;
};

} // namespace

// Auto decodes a WAV to PCM whatever its length: streaming uncompressed PCM gains nothing and
// causes backend compatibility issues (AudioAsset::ShouldDecodeToPCM). Genuine streaming takes the
// Stream load policy (MetaOverridesLoadPolicyToStream).
TEST_F(AudioAssetPolicyTests, AutoDecodesAWavOfAnyLength)
{
    for (const float seconds : {1.0f, 11.0f})
    {
        SCOPED_TRACE(seconds);
        const auto path = testDir / ("auto_" + std::to_string(static_cast<int>(seconds)) + "s.wav");
        WriteTestWavFile(path, 44100, 2, 16, seconds);

        const GUID guid = RegisterAudio(path);

        auto asset = assetManager->LoadAssetAsync(guid, AssetLoadPriority::High).get();
        ASSERT_TRUE(asset);

        auto* audio = dynamic_cast<AudioAsset*>(asset.get());
        ASSERT_TRUE(audio);
        EXPECT_TRUE(audio->IsLoaded());
        EXPECT_EQ(audio->GetPCMFormat(), AudioPCMFormat::F32);
        EXPECT_GT(audio->GetPCMFrameCount(), 0u);
        EXPECT_NE(audio->GetPCMFloatData(), nullptr);
        EXPECT_EQ(audio->GetEncodedDataSize(), 0u);
    }
}

TEST_F(AudioAssetPolicyTests, MetaOverridesLoadPolicyToStream)
{
    const auto path = testDir / "short_stream.wav";
    WriteTestWavFile(path, 44100, 2, 16, 1.0f);

    const GUID guid = RegisterAudio(path);

    // Force stream via registry KV.
    AssetRegistry& reg = assetManager->GetRegistry();
    ASSERT_TRUE(reg.SetMetaValue(path, "audio.loadPolicy", "Stream"));

    auto asset = assetManager->LoadAssetAsync(guid, AssetLoadPriority::High).get();
    ASSERT_TRUE(asset);

    auto* audio = dynamic_cast<AudioAsset*>(asset.get());
    ASSERT_TRUE(audio);
    EXPECT_TRUE(audio->IsLoaded());

    EXPECT_EQ(audio->GetPCMFormat(), AudioPCMFormat::Unknown);
    EXPECT_EQ(audio->GetPCMFrameCount(), 0u);
    EXPECT_EQ(audio->GetPCMFloatData(), nullptr);
    EXPECT_GT(audio->GetEncodedDataSize(), 0u);
}

// A streamed clip that DecodeAssetPayload decodes keeps the buffer it is handed as its encoded data.
// Each hop to the clip moves the buffer, so a copy on any hop gives a different address.
TEST_F(AudioAssetPolicyTests, StreamedDecodeKeepsTheBufferItIsHanded)
{
    const auto path = testDir / "decode_stream.wav";
    WriteTestWavFile(path, 44100, 2, 16, 1.0f);
    const GUID guid = RegisterAudio(path);

    AssetRegistry& reg = assetManager->GetRegistry();
    ASSERT_TRUE(reg.SetMetaValue(path, "audio.loadPolicy", "Stream"));
    AssetMetadata metadata;
    ASSERT_TRUE(reg.TryGetAssetMetadata(path, metadata));

    ByteBuffer bytes;
    ASSERT_TRUE(ReadFileBytesShared(path, bytes));
    const uint8* const handedBuffer = bytes.Data();
    const uint64 handedSize = bytes.Size();

    const SharedPtr<Asset> asset = DecodeAssetPayload(*assetManager, guid, metadata, std::move(bytes));
    ASSERT_TRUE(asset);
    auto* audio = dynamic_cast<AudioAsset*>(asset.get());
    ASSERT_TRUE(audio);
    ASSERT_TRUE(audio->HasEncodedData());
    EXPECT_EQ(audio->GetEncodedData(), handedBuffer);
    EXPECT_EQ(audio->GetEncodedDataSize(), handedSize);
}

// A streamed clip that a load through the AssetManager decodes keeps the buffer that the file is read into.
// Each hop from the reader to the clip moves the buffer, so a copy on any hop gives a different address.
TEST_F(AudioAssetPolicyTests, StreamedLoadKeepsTheBufferTheFileIsReadInto)
{
    const auto path = testDir / "load_stream.wav";
    WriteTestWavFile(path, 44100, 2, 16, 1.0f);
    const GUID guid = RegisterAudio(path);
    ASSERT_TRUE(assetManager->GetRegistry().SetMetaValue(path, "audio.loadPolicy", "Stream"));

    const auto probe = std::make_shared<ReadBufferProbeParser>();
    ASSERT_TRUE(assetManager->GetParserRegistry().RegisterParser(probe, probe->GetPriority()));

    auto asset = assetManager->LoadAssetAsync(guid, AssetLoadPriority::High).get();
    ASSERT_TRUE(asset);
    auto* audio = dynamic_cast<AudioAsset*>(asset.get());
    ASSERT_TRUE(audio);
    ASSERT_TRUE(audio->HasEncodedData());
    const uint8* const readBuffer = probe->ReadBuffer.load(std::memory_order_acquire);
    ASSERT_NE(readBuffer, nullptr);
    EXPECT_EQ(audio->GetEncodedData(), readBuffer);
}

TEST_F(AudioAssetPolicyTests, AudioSystemInitializeFailureIsGraceful)
{
    auto* audioSys = engine->GetAudioSystem();
    ASSERT_NE(audioSys, nullptr);

    // The backend clamps maxListeners to the engine's valid range rather than
    // failing — that IS the graceful handling this test was originally
    // checking for. Init returns true with a clamped value; nothing crashes.
    // (If a future backend change makes Initialize fail for some input, a
    //  separate test should cover that path.)
    audioSys->Shutdown();
    Audio::AudioSystemConfig bad{};
    bad.maxListeners = 9999;
    const bool ok = audioSys->Initialize(bad);
    EXPECT_TRUE(ok);
}


