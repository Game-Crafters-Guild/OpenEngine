#include "Assets/AssetManager.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Core/Engine.h"
#include "Engine/Build/BuildPipeline.h"
#include "Terrain/Heightfield.h"
#include "TestTempDir.h"

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

using namespace GameEngine;
namespace fs = std::filesystem;
namespace
{
void WriteFile(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good()) << path;
}

std::set<std::string> DirectoryNames(const fs::path& directory)
{
    std::set<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error))
        names.insert(entry.path().filename().string());
    return names;
}

std::string ImageBytes()
{
    std::string bytes(18, '\0');
    bytes[2] = 2; bytes[12] = 8; bytes[14] = 8; bytes[16] = 32; bytes[17] = 0x28;
    for (int i = 0; i < 64; ++i)
        bytes.append("\xB4\x78\x50\xFF", 4);
    return bytes;
}

class PackagedTextureExportTest : public testing::Test
{
protected:
    void SetUp() override
    {
        WriteFile(Assets / "Color.tga", ImageBytes());
        WriteFile(Assets / "Unclassified.tga", ImageBytes());
        // A terrain heightmap: no material binds it, and TerrainService opens its source
        // by path for samples deeper than the RGBA8 payload.
        WriteFile(Assets / "Height.tga", ImageBytes());
        // A float terrain stamp mask: no material binds it, and TerrainModifierSystem reads its
        // texels on the CPU (RGB 4, 2, 1 in each of 4x4 texels).
        std::string stamp = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n";
        for (int i = 0; i < 16; ++i)
            stamp.append("\x80\x40\x20\x83", 4);
        WriteFile(Assets / "Stamp.hdr", stamp);
        WriteFile(Assets / "Icon.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16"><rect width="32" height="16" fill="red"/></svg>)");
        // Opaque mandatory dependencies; this fixture does not render text or retarget a rig.
        WriteFile(Assets / "Fonts/Roboto-Regular.ttf", "TTFDATA");
        WriteFile(Assets / kRuntimeHumanoidProfilePath, "{}");
        // The public prebuilt-runtime route stages this header without launching it.
        std::array<char, 20> elf{};
        elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
        elf[4] = 2; elf[5] = 1; elf[18] = 62;
        WriteFile(Runtime / "Player", {elf.data(), elf.size()});
        ApplicationConfig config;
        config.WorkspaceDirectory = Project.string();
        config.AssetDirectory = Assets.string();
        config.AssetDatabaseFile = (Project / "AssetDatabase.assetdb").string();
        config.AssetDatabaseCacheDirectory = (Project / ".Cache/AssetDatabase").string();
        ASSERT_TRUE(Engine.Initialize(config));
        Engine.GetAssetManager().WaitForStartupScan();
        ASSERT_TRUE(Engine.GetAssetManager().GetRegistry().SetMetaValue(
            Assets / "Color.tga", kTextureUsageMetaKey, "color"));
    }
    void TearDown() override
    {
        TextureAsset::SetGpuTranscodeTarget(PreviousTarget);
        Engine.Shutdown();
        fs::current_path(PreviousDirectory);
    }
    TestUtils::ScopedTempDir Temporary{TestUtils::MakeUniqueTempDirectory("packaged_texture_export")};
    const fs::path PreviousDirectory = fs::current_path();
    const fs::path Project = Temporary.Path() / "Project";
    const fs::path Assets = Project / "Assets";
    const fs::path Runtime = Temporary.Path() / "Runtime";
    const fs::path Output = Temporary.Path() / "Export";
    const TextureGpuTranscodeTarget PreviousTarget = TextureAsset::GetGpuTranscodeTarget();
    EngineCore Engine;
};
} // namespace

TEST_F(PackagedTextureExportTest, ShipsCookedTexturesAndKeepsOnlySourcesAPathReaderOpens)
{
    const GUID color = Engine.GetAssetManager().GetRegistry().GetAssetGUID(Assets / "Color.tga");
    const GUID unclassified = Engine.GetAssetManager().GetRegistry().GetAssetGUID(Assets / "Unclassified.tga");
    ASSERT_FALSE(color.IsNull());
    ASSERT_FALSE(unclassified.IsNull());
    BuildSettings build;
    build.platformName = "Linux";
    build.projectRoot = Project;
    build.prebuiltPlayerDirectory = Runtime;
    build.outputDirectory = Output;
    build.compileScripts = false;
    build.buildConfiguration = "Release";
    build.playerConfig.gameName = "TextureExportProbe";
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    ASSERT_TRUE(pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; }))
        << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
    EXPECT_FALSE(fs::exists(Output / "Assets/Color.tga"));
    // Unclassified textures keep their sources beside their payloads.
    EXPECT_TRUE(fs::exists(Output / "Assets/Unclassified.tga"));
    EXPECT_TRUE(fs::exists(Output / "Assets/Height.tga"));
    EXPECT_TRUE(fs::exists(Output / "Assets/Icon.svg"));
    EXPECT_TRUE(fs::exists(Output / "Assets/Fonts/Roboto-Regular.ttf"));
    // One payload per texture (BC7 for the colour texture and BC6H for the HDR stamp where the
    // encoder exists, RGBA8 for the two LDR textures no material classifies), and each one also
    // lands in the project's cache.
    std::set<std::string> cooked;
    for (const auto& entry : fs::recursive_directory_iterator(Output))
        if (entry.is_regular_file() && entry.path().extension() == ".ktx2")
            cooked.insert(entry.path().filename().string());
    EXPECT_EQ(cooked.size(), 4u);
    const fs::path projectCache = Project / ".Cache/Tex";
    for (const auto& name : cooked)
        EXPECT_TRUE(fs::exists(projectCache / name)) << name << " is not in the project cache";

    // The editor's next load of the colour texture adopts the export's artifact: the cook
    // would publish a file under another name and sweep this one.
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    const auto cacheBefore = DirectoryNames(projectCache);
    {
        auto future = Engine.GetAssetManager().LoadAssetAsync(color);
        ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        const auto texture = std::dynamic_pointer_cast<TextureAsset>(future.get());
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(texture->GetFormat(), IsTextureCookEncoderAvailable() ? TextureFormat::BC7 : TextureFormat::RGBA8);
    }
    EXPECT_EQ(DirectoryNames(projectCache), cacheBefore);

    // Exercise the same logical paths and GUIDs through both Player load paths.
    for (const bool asynchronous : {false, true})
    {
        SCOPED_TRACE(asynchronous ? "reader thread" : "synchronous");
        AssetManager player;
        ASSERT_TRUE(player.Initialize(Output / "Assets", asynchronous ? &Engine.GetJobSystem() : nullptr,
            Temporary.Path() / "UserData/AssetDatabase.assetdb", Temporary.Path() / "UserData/.Cache/AssetDatabase"));
        EXPECT_EQ(player.ResolveAssetGuid("Color.tga"), color);
        EXPECT_EQ(player.ResolveAssetGuid("Unclassified.tga"), unclassified);
        auto future = player.LoadAssetAsync(color);
        ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        const auto texture = std::dynamic_pointer_cast<TextureAsset>(future.get());
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(texture->GetWidth(), 8u);
        EXPECT_EQ(texture->GetMipmapLevels(), 4u);
        EXPECT_EQ(texture->GetFormat(), IsTextureCookEncoderAvailable() ? TextureFormat::BC7 : TextureFormat::RGBA8);
        auto uncompressedFuture = player.LoadAssetAsync(unclassified);
        ASSERT_EQ(uncompressedFuture.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        const auto uncompressed = std::dynamic_pointer_cast<TextureAsset>(uncompressedFuture.get());
        ASSERT_NE(uncompressed, nullptr);
        EXPECT_EQ(uncompressed->GetFormat(), TextureFormat::RGBA8);
        player.Shutdown();
    }

    // A direct CPU reader uses the owning engine outside an async load.
    Engine.Shutdown();
    ApplicationConfig runtime;
    runtime.WorkspaceDirectory = Output.string();
    runtime.AssetDirectory = (Output / "Assets").string();
    runtime.AssetDatabaseFile = (Temporary.Path() / "RuntimeUserData/AssetDatabase.assetdb").string();
    runtime.AssetDatabaseCacheDirectory = (Temporary.Path() / "RuntimeUserData/.Cache/AssetDatabase").string();
    ASSERT_TRUE(Engine.Initialize(runtime));
    // The heightmap reads the way TerrainService::ResolveHeightmapAsset does: the registered
    // path, decoded with 16-bit precision.
    AssetMetadata heightmap{};
    ASSERT_TRUE(Engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(
        Engine.GetAssetManager().GetRegistry().GetAssetGUID(Output / "Assets/Height.tga"), heightmap));
    Terrain::HeightfieldData heightfield;
    EXPECT_TRUE(heightfield.LoadFromPNG16(heightmap.Path)) << heightmap.Path;
    // A CPU reader of the BC7 payload reads its RGBA8 decode, every level.
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    TextureAsset cpu(color, Output / "Assets/Color.tga");
    ASSERT_TRUE(cpu.Load());
    EXPECT_EQ(cpu.GetFormat(), TextureFormat::RGBA8);
    EXPECT_EQ(cpu.GetMipmapLevels(), 4u);
    ASSERT_EQ(cpu.GetMipChain().size(), 4u);
    EXPECT_EQ(cpu.GetMipChain()[3].Size, 4u);
    ASSERT_NE(cpu.GetPixelData(), nullptr);
    EXPECT_NEAR(cpu.GetPixelData()[0], 80, 2);
    EXPECT_NEAR(cpu.GetPixelData()[2], 180, 2);
}

TEST_F(PackagedTextureExportTest, TemplateSymbolsFollowTheRequestedBuildConfiguration)
{
    WriteFile(Runtime / "Player.pdb", "symbols");
    WriteFile(Runtime / "lib/Engine.debug", "symbols");
    WriteFile(Runtime / "lib/Engine.dwp", "symbols");
    WriteFile(Runtime / ".debug/build-id", "symbols");
    WriteFile(Runtime / "lib/libEngine.so", "runtime");
    WriteFile(Runtime / "Assets/stale.tga", "old game content");
    for (const char* configuration : {"Release", "MinSizeRel", "Debug", "RelWithDebInfo"})
    {
        SCOPED_TRACE(configuration);
        BuildSettings build;
        build.platformName = "Linux";
        build.projectRoot = Project;
        build.prebuiltPlayerDirectory = Runtime;
        build.outputDirectory = Output;
        build.compileScripts = false;
        build.buildConfiguration = configuration;
        build.playerConfig.gameName = "TextureExportProbe";
        BuildPipeline pipeline(nullptr);
        BuildProgress progress;
        ASSERT_TRUE(pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; }))
            << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
        const bool symbols = std::string_view(configuration) == "Debug" || std::string_view(configuration) == "RelWithDebInfo";
        EXPECT_EQ(fs::exists(Output / "Player.pdb"), symbols);
        EXPECT_EQ(fs::exists(Output / "lib/Engine.debug"), symbols);
        EXPECT_EQ(fs::exists(Output / "lib/Engine.dwp"), symbols);
        EXPECT_EQ(fs::exists(Output / ".debug/build-id"), symbols);
        EXPECT_TRUE(fs::exists(Output / "lib/libEngine.so"));
        EXPECT_FALSE(fs::exists(Output / "Assets/stale.tga"));
    }
}

// A packaged HDR texture a CPU reader opens (TerrainModifierSystem's float stamp mask) ships as
// BC6H only, and the reader gets its float decode: the source is not shipped beside it.
TEST_F(PackagedTextureExportTest, ACpuReaderOfAPackagedHdrTextureReadsItsFloatDecode)
{
    BuildSettings build;
    build.platformName = "Linux";
    build.projectRoot = Project;
    build.prebuiltPlayerDirectory = Runtime;
    build.outputDirectory = Output;
    build.compileScripts = false;
    build.buildConfiguration = "Release";
    build.playerConfig.gameName = "TextureExportProbe";
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    ASSERT_TRUE(pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; }))
        << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
    EXPECT_FALSE(fs::exists(Output / "Assets/Stamp.hdr"));

    Engine.Shutdown();
    ApplicationConfig runtime;
    runtime.WorkspaceDirectory = Output.string();
    runtime.AssetDirectory = (Output / "Assets").string();
    runtime.AssetDatabaseFile = (Temporary.Path() / "RuntimeUserData/AssetDatabase.assetdb").string();
    runtime.AssetDatabaseCacheDirectory = (Temporary.Path() / "RuntimeUserData/.Cache/AssetDatabase").string();
    ASSERT_TRUE(Engine.Initialize(runtime));
    const GUID stamp = Engine.GetAssetManager().GetRegistry().GetAssetGUID(Output / "Assets/Stamp.hdr");
    ASSERT_FALSE(stamp.IsNull());
    TextureAsset cpu(stamp, Output / "Assets/Stamp.hdr");
    ASSERT_TRUE(cpu.Load()) << "the packaged HDR texture's CPU read was refused";
    ASSERT_EQ(cpu.GetFormat(), TextureFormat::RGBA32F);
    ASSERT_EQ(cpu.GetWidth(), 4u);
    ASSERT_GE(cpu.GetDataSize(), 4u * sizeof(float));
    float texel[4];
    std::memcpy(texel, cpu.GetPixelData(), sizeof(texel));
    // BC6H keeps a half-float mantissa: well within 1 % here.
    EXPECT_NEAR(texel[0], 4.0f, 0.04f);
    EXPECT_NEAR(texel[1], 2.0f, 0.02f);
    EXPECT_NEAR(texel[2], 1.0f, 0.01f);
}

// The project cache is a convenience: a cache folder the export cannot create (here a file is
// in its place) costs the editor a cook, not the export.
TEST_F(PackagedTextureExportTest, AProjectCacheThatCannotBeWrittenDoesNotFailTheExport)
{
    std::error_code error;
    fs::remove_all(Project / ".Cache/Tex", error);
    WriteFile(Project / ".Cache/Tex", "not a directory");
    BuildSettings build;
    build.platformName = "Linux";
    build.projectRoot = Project;
    build.prebuiltPlayerDirectory = Runtime;
    build.outputDirectory = Output;
    build.compileScripts = false;
    build.buildConfiguration = "Release";
    build.playerConfig.gameName = "TextureExportProbe";
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    EXPECT_TRUE(pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; }))
        << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
    EXPECT_FALSE(fs::exists(Output / "Assets/Color.tga")) << "the export did not stage its cooked textures";
}

// Publishing a cooked texture into the project cache removes that texture's artifacts under older
// keys there (an editor that adopts the export's artifact never cooks it, so never sweeps them);
// another texture's files stay.
TEST_F(PackagedTextureExportTest, PublishingIntoTheProjectCacheRemovesTheTexturesOlderArtifacts)
{
    const GUID color = Engine.GetAssetManager().GetRegistry().GetAssetGUID(Assets / "Color.tga");
    ASSERT_FALSE(color.IsNull());
    const fs::path cache = Project / ".Cache/Tex";
    const fs::path stale = cache / (color.ToString() + "-0000000000000000-0000000000000000.ktx2");
    const fs::path other = cache / (GUID::Generate().ToString() + "-0000000000000000-0000000000000000.ktx2");
    WriteFile(stale, "an artifact under an older key");
    WriteFile(other, "another texture's artifact");
    BuildSettings build;
    build.platformName = "Linux";
    build.projectRoot = Project;
    build.prebuiltPlayerDirectory = Runtime;
    build.outputDirectory = Output;
    build.compileScripts = false;
    build.buildConfiguration = "Release";
    build.playerConfig.gameName = "TextureExportProbe";
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    ASSERT_TRUE(pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; }))
        << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
    size_t current = 0;
    for (const auto& name : DirectoryNames(cache))
        current += name.rfind(color.ToString(), 0) == 0 ? 1 : 0;
    EXPECT_FALSE(fs::exists(stale)) << "the colour texture's older artifact stayed in the project cache";
    EXPECT_EQ(current, 1u) << "the colour texture's current artifact is not the one file left for it";
    EXPECT_TRUE(fs::exists(other)) << "another texture's artifact was removed";
}
