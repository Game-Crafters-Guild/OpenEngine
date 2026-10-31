#include "Assets/AssetManager.h"
#include "UI/BackgroundImageCache.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/UIManager.h"
#include "UIPixelReadback.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace GameEngine;

namespace
{
struct TemporaryBackground
{
    const std::filesystem::path Directory = std::filesystem::temp_directory_path() /
                                            ("ui-minification-" + GUID::Generate().ToString());
    TemporaryBackground() { std::filesystem::create_directories(Directory); }
    ~TemporaryBackground()
    {
        std::error_code error;
        std::filesystem::remove_all(Directory, error);
    }
};
} // namespace

TEST(BackgroundMinificationTests, SmallBackgroundPreservesTheAreaAverage)
{
    SharedDeviceReleaseRetirement retirement;
    auto* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    TemporaryBackground source;
    const auto path = source.Directory / "stripes.tga";
    {
        // One white column followed by three black columns: 25% linear white.
        // At 8:1 minification, sampling only level zero hits stripe edges and
        // returns 50% instead of averaging the complete source footprint.
        std::ofstream file(path, std::ios::binary);
        unsigned char header[18]{};
        header[2] = 2; // uncompressed true color
        header[12] = 64;
        header[14] = 64;
        header[16] = 32;
        header[17] = 0x28; // top origin, eight alpha bits
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
            {
                const unsigned char value = x % 4 == 0 ? 255 : 0;
                const unsigned char pixel[] = {value, value, value, 255};
                file.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
            }
    }
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(source.Directory, &pool));
    ASSERT_TRUE(assets.GetRegistry().RegisterAsset(path));
    const GUID guid = assets.GetRegistry().GetAssetGUID(path);
    ASSERT_TRUE(assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get());

    UIManager ui(device, &assets);
    ui.SetLayoutSizeOverride(800, 600);
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(UIParsing::XMLParser::ParseLayoutFromString(
        "<uielement id='root'><uielement id='icon'/><uielement id='point' class='ui-bg-point-filter'/></uielement>", root));
    ui.SetRoot(std::move(root));
    Stylesheet sheet;
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(
        "#root {width:800px;height:600px;flex-direction:row;background-color:black;}"
        "#icon, #point {width:8px;height:8px;background-image:url('stripes.tga');background-size:contain;}", sheet));
    ui.AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));
    ui.BackgroundImages().EnsureUploaded(guid);
    UiRgHarness graph(device);
    for (int frame = 0; frame < 3; ++frame)
    {
        ui.Update(1.0f / 60.0f, true);
        DriveUiRender(ui, graph);
    }
    const auto image = UITesting::RenderUiToBytes(ui, UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(image.empty());
    // sRGB encoding of 25% linear white is 137/255. Check several interior
    // pixels so a fortunate sample phase cannot hide missing minification.
    for (uint32_t y = 2; y < 6; ++y)
        for (uint32_t x = 2; x < 6; ++x)
        {
            const auto pixel = UITesting::PixelAt(image, x, y);
            EXPECT_NEAR(pixel.R, 137, 5) << x << "," << y;
            EXPECT_NEAR(pixel.G, 137, 5);
            EXPECT_NEAR(pixel.B, 137, 5);
            const auto point = UITesting::PixelAt(image, x + 8, y);
            EXPECT_EQ(point.R, 255) << "Explicit point filtering must keep base-level texels";
            EXPECT_EQ(point.G, 255);
            EXPECT_EQ(point.B, 255);
        }
}
