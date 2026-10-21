#include "UI/UIManager.h"
#include "UI/UIElement.h"
#include "UIRgTestHarness.h"
#include "Assets/AssetManager.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Engine/Rendering/TextureMipUpload.h"
#include "Rendering/Core/CommandList.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

// A thin stroke like the editor's tool icons: drawn far below its raster size, it stair-steps
// unless the UI texture carries the levels between.
constexpr const char* kThinStrokeSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">)"
    R"(<path d="M2 3 L14 13" stroke="#ffffff" stroke-width="1.25" fill="none"/></svg>)";

// Writes an uncompressed 32-bit TGA of `width` x `height` opaque gray texels.
void WriteTga(const std::filesystem::path& path, uint16_t width, uint16_t height)
{
    std::vector<uint8_t> bytes(18, 0);
    bytes[2] = 2;
    bytes[12] = uint8_t(width & 0xff);
    bytes[13] = uint8_t(width >> 8);
    bytes[14] = uint8_t(height & 0xff);
    bytes[15] = uint8_t(height >> 8);
    bytes[16] = 32;
    bytes[17] = 0x28;
    bytes.resize(bytes.size() + size_t(width) * height * 4, 128);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

// Loads the image file at `fileName` (written into `assetRoot` by the caller) as a texture
// asset, uploads it the way a CSS background-image is uploaded, and returns the asset with
// the level count the UI texture cache recorded.
struct UiUpload
{
    std::shared_ptr<TextureAsset> Texture;
    uint32_t MipLevels = 0;
};
UiUpload UploadThroughUi(Rendering::IDevice* device, const std::filesystem::path& assetRoot,
                         const std::filesystem::path& file)
{
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    EXPECT_TRUE(assets.Initialize(assetRoot, &pool));
    EXPECT_TRUE(assets.GetRegistry().RegisterAsset(file));
    const GUID guid = assets.GetRegistry().GetAssetGUID(file);
    EXPECT_FALSE(guid.IsNull());

    UiUpload upload;
    upload.Texture = std::make_shared<TextureAsset>(guid, file);
    EXPECT_TRUE(upload.Texture->Load());
    assets.RegisterLoadedAsset(guid, upload.Texture);

    UIManager ui(device, &assets);
    ui.SetRoot(std::make_unique<UIElement>());
    ui.BackgroundImages().EnsureUploaded(guid);
    upload.MipLevels = ui.BackgroundImages().RasterMipLevelsForTesting(guid);
    return upload;
}

std::filesystem::path MakeAssetRoot(const char* name)
{
    const auto root = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(root);
    return root;
}

} // namespace

// The UI samples images with a trilinear sampler, so an image uploaded with one level aliases
// whenever it is drawn smaller than its pixels. The upload carries the cook's full chain.
TEST(UIBackgroundTextureMips, SvgImageUploadsTheFullMipChain)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";

    const auto assetRoot = MakeAssetRoot("ui_background_texture_mips_svg");
    const auto svgPath = assetRoot / "thin_stroke.svg";
    {
        std::ofstream file(svgPath, std::ios::binary);
        file << kThinStrokeSvg;
    }

    const UiUpload upload = UploadThroughUi(device, assetRoot, svgPath);
    const uint32_t expectedLevels = ComputeTextureCookMipCount(upload.Texture->GetWidth(),
                                                               upload.Texture->GetHeight(), TextureCookSettings{});
    ASSERT_GT(expectedLevels, 1u) << "the raster must be large enough to have a chain";
    EXPECT_EQ(upload.MipLevels, expectedLevels);
}

// The chain is a CPU resample on the UI thread, so it is built for icon-sized images only: an
// image edge of 1024 px still gets it, a texture preview above that uploads one level.
TEST(UIBackgroundTextureMips, ImagesAbove1024PixelsUploadOneLevel)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";

    const auto assetRoot = MakeAssetRoot("ui_background_texture_mips_limit");
    const auto atLimit = assetRoot / "edge_1024.tga";
    const auto aboveLimit = assetRoot / "edge_1025.tga";
    WriteTga(atLimit, 1024, 4);
    WriteTga(aboveLimit, 1025, 4);

    EXPECT_EQ(UploadThroughUi(device, assetRoot, atLimit).MipLevels,
              ComputeTextureCookMipCount(1024, 4, TextureCookSettings{}));
    EXPECT_EQ(UploadThroughUi(device, assetRoot, aboveLimit).MipLevels, 1u);
}

// Every level the GPU holds must be the level the cook built, for an odd-sized image too: a
// level whose rows land at the wrong pitch draws as a haze instead of the icon.
TEST(UIBackgroundTextureMips, OddSizedImageLevelsMatchTheCookChain)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";

    constexpr uint32_t kWidth = 50;
    constexpr uint32_t kHeight = 52;
    std::vector<TextureCookMipLevel> chain(1);
    chain[0].Width = kWidth;
    chain[0].Height = kHeight;
    chain[0].Bytes.assign(size_t(kWidth) * kHeight * 4, 0);
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
            if (x == y || x + y == kWidth - 1 || x == 2 || y == 3)
            {
                uint8_t* texel = &chain[0].Bytes[(size_t(y) * kWidth + x) * 4];
                texel[0] = texel[1] = texel[2] = texel[3] = 255;
            }

    uint32_t levels = 0;
    const Rendering::TextureHandle texture =
        UploadRgba8WithMips(device, chain[0].Bytes.data(), kWidth, kHeight, Rendering::TextureFormat::RGBA8_SRGB,
                            TextureCookSettings{}, /*srgb*/ true, "OddSizedMips", "OddSizedMipsStaging", levels);
    ASSERT_TRUE(texture.IsValid());
    ASSERT_TRUE(BuildTextureCookMipChain(chain, levels, /*isFloat*/ false, /*srgb*/ true, TextureCookSettings{}));
    ASSERT_EQ(chain.size(), levels);

    for (uint32_t level = 0; level < levels; ++level)
    {
        const uint32_t width = chain[level].Width;
        const uint32_t height = chain[level].Height;
        const Rendering::BufferHandle readback = device->CreateReadbackBuffer(size_t(width) * height * 4);
        auto commands = device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
            texture, Rendering::ResourceState::ShaderResource, Rendering::ResourceState::CopySource, level, 1));
        commands->CopyTextureSubresourceToBuffer(texture, level, 0, readback, width, height);
        commands->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
            texture, Rendering::ResourceState::CopySource, Rendering::ResourceState::ShaderResource, level, 1));
        commands->End();
        device->ExecuteCommandLists({commands.get()});
        device->WaitForIdle();

        const auto* gpu = static_cast<const uint8_t*>(device->MapBuffer(readback));
        ASSERT_NE(gpu, nullptr);
        size_t mismatches = 0;
        for (size_t i = 0; i < chain[level].Bytes.size(); ++i)
            mismatches += gpu[i] != chain[level].Bytes[i];
        device->UnmapBuffer(readback);
        device->DestroyBuffer(readback);
        EXPECT_EQ(mismatches, 0u) << "level " << level << " (" << width << "x" << height << ")";
    }
    device->DestroyTexture(texture);
}

// A block-compressed image previews from its own decoded blocks, with no other file to read:
// an exported game's cooked texture ships without its source.
TEST(UIBackgroundTextureMips, BlockCompressedImageWithoutASourceUploads)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "BC encoder unavailable";

    const auto assetRoot = MakeAssetRoot("ui_background_texture_bc7");
    const auto source = assetRoot / "source.tga";
    WriteTga(source, 64, 64);
    std::ifstream sourceFile(source, std::ios::binary);
    const std::vector<uint8> sourceBytes((std::istreambuf_iterator<char>(sourceFile)), std::istreambuf_iterator<char>());
    sourceFile.close();
    std::vector<uint8> cooked;
    std::string error;
    ASSERT_TRUE(CookTexture(sourceBytes.data(), sourceBytes.size(), "fixture.tga", ".tga", TextureCookInputs{},
                            TextureCookOutput::BC7, cooked, error)) << error;
    std::filesystem::remove(source);
    const auto artifact = assetRoot / "cooked.ktx2";
    {
        std::ofstream file(artifact, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(cooked.data()), std::streamsize(cooked.size()));
    }

    const UiUpload upload = UploadThroughUi(device, assetRoot, artifact);
    ASSERT_EQ(upload.Texture->GetFormat(), TextureFormat::BC7);
    EXPECT_EQ(upload.MipLevels, ComputeTextureCookMipCount(64, 64, TextureCookSettings{}));
}
