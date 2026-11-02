// The cook usage a material bind writes onto a texture asset (assets.texture.usage), which the
// import cook compresses by. A material document's textures are an unordered map, so the tag
// must not depend on which slot binds first. TextureService::DeclareTextureClassification runs
// the same tag write as a slot bind (BindMaterialTextureRef and UpdateMaterialTextures hand it
// TextureCookUsageForMaterialSlot); it is the entry point used here because it needs no GPU
// device and starts no texture load of its own.

#include "Assets/AssetManager.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Assets/TextureCookActivity.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/TextureService.h"
#include "EngineLogCapture.h"
#include "Logger/LogSink.h"
#include "TestTempDir.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{
namespace fs = std::filesystem;

// Raises a flag when a line containing the needle is logged, so a test can act at a moment only
// the log marks, such as a cook that has started.
class LogLineFlagSink final : public Logger::LogSink
{
  public:
    LogLineFlagSink(std::atomic<bool>* flag, std::string needle) : m_Flag(flag), m_Needle(std::move(needle)) {}

    void Write(const Logger::LogMessage& message) override
    {
        if (std::string_view(message.Message).find(m_Needle) != std::string_view::npos)
            m_Flag->store(true);
    }
    void Flush() override {}
    bool ShouldLog(Logger::LogLevel) const override { return true; }
    std::string GetName() const override { return "LogLineFlagSink"; }

  private:
    std::atomic<bool>* m_Flag;
    std::string m_Needle;
};

class TextureCookUsageTag : public testing::Test
{
  protected:
    inline static fs::path s_Workspace;
    Engine::Renderer::TextureService m_Textures;
    TextureGpuTranscodeTarget m_PreviousTranscodeTarget = TextureAsset::GetGpuTranscodeTarget();

    void TearDown() override { TextureAsset::SetGpuTranscodeTarget(m_PreviousTranscodeTarget); }

    static void SetUpTestSuite()
    {
        // The canonical temp root: the engine resolves the workspace root, and the registry
        // stores meta values only for asset paths spelled under that resolved root.
        s_Workspace = TestUtils::MakeUniqueTempDirectory("ge-cook-usage-tag");
        fs::create_directories(s_Workspace / "Assets");
        auto& engine = EngineCore::GetInstance();
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = s_Workspace.string();
        config.EnableEditor = false;
        ASSERT_TRUE(engine.Initialize(config));
        // As in the editor, the only host that writes these tags: an async reload's payload is
        // adopted by the asset hot-reload drain.
        engine.GetAssetManager().SetHotReloadEnabled(true);
    }

    static void TearDownTestSuite()
    {
        EngineCore::GetInstance().Shutdown();
        std::error_code error;
        fs::remove_all(s_Workspace, error);
    }

    static AssetManager& Assets() { return EngineCore::GetInstance().GetAssetManager(); }

    // A fresh square RGBA source in the project with no usage tag: R, G and B differ per texel,
    // as they do in an occlusion-roughness-metallic map.
    static fs::path WriteTexture(GUID& guid, uint32 size = 4)
    {
        std::vector<uint8> texels;
        texels.reserve(size_t(size) * size * 4);
        for (uint32 y = 0; y < size; ++y)
            for (uint32 x = 0; x < size; ++x)
                texels.insert(texels.end(), {uint8(x * 7 + y * 3), uint8(x ^ y), uint8((x * y) >> 5), uint8(255)});
        return WriteTga(guid, size, texels);
    }

    // A fresh square source in the project with no usage tag, from `size` x `size` RGBA texels.
    static fs::path WriteTga(GUID& guid, uint32 size, const std::vector<uint8>& texels)
    {
        const fs::path path = s_Workspace / "Assets" / (GUID::Generate().ToString() + ".tga");
        std::vector<uint8> bytes(18, 0);
        bytes[2] = 2;
        bytes[12] = uint8(size & 0xff);
        bytes[13] = uint8(size >> 8);
        bytes[14] = uint8(size & 0xff);
        bytes[15] = uint8(size >> 8);
        bytes[16] = 32;
        bytes[17] = 0x28;
        bytes.reserve(bytes.size() + texels.size());
        for (size_t i = 0; i + 3 < texels.size(); i += 4)
            bytes.insert(bytes.end(), {texels[i + 2], texels[i + 1], texels[i], texels[i + 3]});
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        const auto registered = Assets().GetRegistry().RegisterAssetByPath(path);
        guid = registered.IsOk() ? registered.Value() : GUID::Null();
        return path;
    }

    void Bind(const GUID& texture, const char* slot)
    {
        const TextureCookUsage usage = TextureCookUsageForMaterialSlot(HashStringId(slot));
        m_Textures.DeclareTextureClassification(
            texture, IsLinearTextureUsage(usage) ? TextureColorSpace::Linear : TextureColorSpace::Unknown,
            usage);
    }

    static std::string StoredUsage(const fs::path& path)
    {
        std::string value;
        Assets().GetRegistry().TryGetMetaValue(path, kTextureUsageMetaKey, value);
        return value;
    }

    // What the import cook encodes the texture to under its stored settings, on a device and
    // build that can block-compress.
    static TextureCookOutput CookOutput(const fs::path& path)
    {
        TextureCookInputs inputs;
        std::string error;
        EXPECT_TRUE(ResolveTextureCookInputs(".tga", [&](const char* key, std::string& value) {
            return Assets().GetRegistry().TryGetMetaValue(path, key, value);
        }, inputs, error)) << error;
        return ResolveTextureCookOutput(inputs.Settings, false, true, true);
    }

    static TextureFormat ResidentFormat(const SharedPtr<Asset>& asset)
    {
        const auto* texture = dynamic_cast<const TextureAsset*>(asset.get());
        return texture ? texture->GetFormat() : TextureFormat::Unknown;
    }

    static bool PumpUntil(const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!done())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            Assets().Update();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
};

// The game's defect: an occlusion-roughness-metallic map bound to aoMap and metallicRoughnessMap
// cooked to single-channel BC4 whenever the aoMap bind ran first.
TEST_F(TextureCookUsageTag, OneTextureOnOcclusionAndMetallicRoughnessSlotsCooksPackedInEitherOrder)
{
    const char* orders[2][2] = {{"aoMap", "metallicRoughnessMap"}, {"metallicRoughnessMap", "aoMap"}};
    for (const auto& order : orders)
    {
        SCOPED_TRACE(std::string(order[0]) + " first");
        GUID texture;
        const fs::path path = WriteTexture(texture);
        ASSERT_FALSE(texture.IsNull());
        Bind(texture, order[0]);
        Bind(texture, order[1]);
        EXPECT_EQ(StoredUsage(path), "packed");
        EXPECT_EQ(CookOutput(path), TextureCookOutput::BC7);
    }
}

// A widened tag changes the cook key, so the texture already loaded under the narrow tag has to
// be decoded again; the bind asks the asset manager for that reload. The reload stays in flight
// until an Update adopts it, and none runs between the bind and the check.
TEST_F(TextureCookUsageTag, WideningATagReloadsTheLoadedTexture)
{
    GUID texture;
    const fs::path path = WriteTexture(texture);
    ASSERT_FALSE(texture.IsNull());
    Bind(texture, "aoMap");
    ASSERT_EQ(StoredUsage(path), "mask");
    const AssetLoadHandle load = Assets().LoadAsset(texture, [](Result<SharedPtr<Asset>, AssetError>) {});
    ASSERT_TRUE(PumpUntil([&] { return load.IsComplete(); }));
    const SharedPtr<Asset> loaded = load.GetResult();
    ASSERT_NE(loaded, nullptr);
    ASSERT_FALSE(Assets().IsReloadInFlight(texture));

    Bind(texture, "metallicRoughnessMap");
    EXPECT_EQ(StoredUsage(path), "packed");
    EXPECT_TRUE(Assets().IsReloadInFlight(texture));
}

// Changing a cooked texture's type in the texture inspector writes the usage and asks for a
// recook (TextureInspector's WriteTextureCookMeta): the next load cooks under the new usage. A
// normal map gets its variance companion and a colour texture has none, in both directions.
TEST_F(TextureCookUsageTag, ChangingATexturesTypeRecooksItWithAndWithoutTheVarianceCompanion)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "BC encoder unavailable";
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    constexpr uint32 kSize = 16;
    std::vector<uint8> checker;
    for (uint32 y = 0; y < kSize; ++y)
        for (uint32 x = 0; x < kSize; ++x)
            checker.insert(checker.end(), {uint8(128), uint8(((x + y) & 1u) ? 64 : 191), uint8(238), uint8(255)});
    GUID texture;
    const fs::path path = WriteTga(texture, kSize, checker);
    ASSERT_FALSE(texture.IsNull());
    const auto setType = [this, &path, &texture](const char* usage) {
        ASSERT_TRUE(Assets().GetRegistry().SetMetaValue(path, kTextureUsageMetaKey, usage));
        m_Textures.RequestRecook(texture, AssetLoadPriority::High);
    };
    const auto companionOf = [](const SharedPtr<Asset>& asset) {
        const auto* cooked = dynamic_cast<const TextureAsset*>(asset.get());
        return cooked ? cooked->GetNormalVarianceCompanion() : nullptr;
    };
    ASSERT_TRUE(Assets().GetRegistry().SetMetaValue(path, kTextureUsageMetaKey, "color"));
    const AssetLoadHandle load = Assets().LoadAsset(texture, [](Result<SharedPtr<Asset>, AssetError>) {});
    ASSERT_TRUE(PumpUntil([&] { return load.IsComplete(); }));
    const SharedPtr<Asset> loaded = load.GetResult();
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(companionOf(loaded), nullptr) << "a colour cook has no variance companion";

    setType("normal");
    EXPECT_TRUE(PumpUntil([&] { return ResidentFormat(loaded) == TextureFormat::BC5 && companionOf(loaded) != nullptr; }))
        << "after the change to a normal map, the cook must write the variance companion";

    setType("color");
    EXPECT_TRUE(PumpUntil([&] { return ResidentFormat(loaded) == TextureFormat::BC7 && companionOf(loaded) == nullptr; }))
        << "after the change back to colour, the companion must be gone";
}

// A colour texture a data slot also binds: no one cooked artefact serves both, so the tag the
// texture carries stays, whether a bind or the texture inspector wrote it, and the conflict is
// reported once however often it is bound again.
TEST_F(TextureCookUsageTag, ColourTagAndDataSlotKeepTheTagAndWarnOnce)
{
    GUID texture;
    const fs::path path = WriteTexture(texture);
    ASSERT_FALSE(texture.IsNull());
    ASSERT_TRUE(Assets().GetRegistry().SetMetaValue(path, kTextureUsageMetaKey, "color"));

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("TextureCookUsageTag log-capture self test");
        Bind(texture, "metallicRoughnessMap");
        Bind(texture, "metallicRoughnessMap");
        Bind(texture, "aoMap");
        Logger::Log::Flush();
    }
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "log-capture self test"), 1u);
    EXPECT_EQ(StoredUsage(path), "color");
    EXPECT_EQ(TestLog::CountLinesContaining(lines, path.filename().string()), 1u);
    const std::string warning = TestLog::FirstLineContaining(lines, path.filename().string());
    EXPECT_NE(warning.find("'color'"), std::string::npos) << warning;
    EXPECT_NE(warning.find("'packed'"), std::string::npos) << warning;
    EXPECT_NE(warning.find("texture inspector"), std::string::npos) << warning;
}

// A texture's first load cooks under the usage the texture has when that cook starts. A bind
// that tags the texture while the cook runs (a second material binding it as a scene loads)
// re-cooks it once the load lands, instead of leaving the untagged cook resident. The source is
// large enough that its cook is still running when the bind lands; a missed window fails the
// test rather than passing it.
TEST_F(TextureCookUsageTag, ATagWrittenWhileTheFirstLoadCooksReachesTheLoadedTexture)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "BC encoder unavailable";
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    GUID texture;
    const fs::path path = WriteTexture(texture, 2048);
    ASSERT_FALSE(texture.IsNull());
    ASSERT_EQ(StoredUsage(path), "");

    AssetLoadHandle load;
    {
        std::vector<std::string> lines;
        TestLog::ScopedEngineLogCapture capture(&lines);
        std::atomic<bool> cookStarted{false};
        Logger::Log::AddSink(
            std::make_unique<LogLineFlagSink>(&cookStarted, path.stem().string() + " -> Uncompressed started"));
        load = Assets().LoadAsset(texture, [](Result<SharedPtr<Asset>, AssetError>) {});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!cookStarted.load() && !load.IsComplete() && std::chrono::steady_clock::now() < deadline)
        {
            Logger::Log::Flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_TRUE(cookStarted.load()) << "the first load's cook start was never logged";
        ASSERT_FALSE(load.IsComplete()) << "the first load finished before the bind; the window was missed";
        Bind(texture, "aoMap");
        ASSERT_EQ(StoredUsage(path), "mask");
    }
    ASSERT_TRUE(PumpUntil([&] { return load.IsComplete(); }));
    const SharedPtr<Asset> loaded = load.GetResult();
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(PumpUntil([&] { return ResidentFormat(loaded) == TextureFormat::BC4; }))
        << "resident format " << static_cast<int>(ResidentFormat(loaded)) << ", stored usage " << StoredUsage(path);
}

// A first load's cook on a miss is reported while it runs, by texture, and not once the cooked
// payload has loaded: the editor's "Preparing N textures" status and a texture inspector waiting
// on the load read it. The 2048 source keeps the cook running long enough to be seen.
TEST_F(TextureCookUsageTag, AFirstLoadsCookIsReportedWhileItRunsAndNotAfter)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "BC encoder unavailable";
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    GUID texture;
    const fs::path path = WriteTexture(texture, 2048);
    ASSERT_FALSE(texture.IsNull());
    const TextureCookActivity& activity = Assets().GetTextureCookActivity();
    ASSERT_EQ(activity.CookingCount(), 0u);

    std::atomic<bool> cookStarted{false};
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines);
    Logger::Log::AddSink(
        std::make_unique<LogLineFlagSink>(&cookStarted, path.stem().string() + " -> Uncompressed started"));
    AssetLoadHandle load = Assets().LoadAsset(texture, [](Result<SharedPtr<Asset>, AssetError>) {});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!cookStarted.load() && !load.IsComplete() && std::chrono::steady_clock::now() < deadline)
    {
        Logger::Log::Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(cookStarted.load()) << "the first load's cook start was never logged";
    ASSERT_FALSE(load.IsComplete()) << "the load finished before it was read; the window was missed";
    EXPECT_TRUE(activity.IsCooking(texture));
    EXPECT_EQ(activity.CookingCount(), 1u);

    ASSERT_TRUE(PumpUntil([&] { return load.IsComplete(); }));
    ASSERT_NE(load.GetResult(), nullptr);
    EXPECT_FALSE(activity.IsCooking(texture));
    EXPECT_EQ(activity.CookingCount(), 0u);
}

} // namespace
