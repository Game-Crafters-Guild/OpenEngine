#include "FixedScalePlatform.h"
#include "SvgBackgroundRaster.h"
#include "UIPixelReadback.h"
#include "UIRgTestHarness.h"

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Assets/TextureAsset.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Core/CommandList.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

// A 16 x 16 icon: the common editor icon size, drawn at several sizes below.
constexpr const char* kWhiteSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">)"
    R"(<rect width="16" height="16" fill="#ffffff"/></svg>)";
constexpr const char* kBlackSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">)"
    R"(<rect width="16" height="16" fill="#000000"/></svg>)";
// A white disc with a diagonal bar: antialiased edges in every direction, so a
// raster resampled by any fraction of a pixel no longer matches itself.
constexpr const char* kDiscSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">)"
    R"(<circle cx="8" cy="8" r="5.3" fill="#ffffff"/>)"
    R"(<path d="M1.5 14.2 L13.7 1.6 L14.6 2.5 L2.4 15.1 Z" fill="#ffffff"/></svg>)";

// An intrinsic size that is not a background-size of the sheet, for the auto
// size.
constexpr const char* kEighteenSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 18 18">)"
    R"(<rect width="18" height="18" fill="#ffffff"/></svg>)";

constexpr const char* kLayout =
    R"(<uielement id='root'><uielement id='icon' /><uielement id='twin' /></uielement>)";
constexpr const char* kCss = R"(
#root { display: flex; width: 256px; height: 128px; }
#icon { width: 64px; height: 64px; background-image: url("icon.svg"); }
#icon.s8 { background-size: 8px 8px; }
#icon.s15 { background-size: 15px 15px; }
#icon.s16 { background-size: 16px 16px; }
#icon.s16x12 { background-size: 16px 12px; }
#icon.s17 { background-size: 17px 17px; }
#icon.s20 { background-size: 20px 20px; }
#icon.s32 { background-size: 32px 32px; }
#icon.w20 { background-size: 20px; }
#icon.at4 { background-position: 4px 4px; }
#icon.at4half { background-position: 4.5px 4.5px; }
#icon.sliced { border-image-slice: 4 fill; }
#twin { width: 64px; height: 64px; }
#twin.t16 { background-image: url("icon.svg"); background-size: 16px 16px; }
#twin.t24 { background-image: url("icon.svg"); background-size: 24px 24px; }

)";

// One SVG asset drawn by one element through a manager at a fixed content
// scale, driven through the render graph the way the editor paints.
struct SvgIconFixture
{
    std::filesystem::path Root;
    std::filesystem::path SvgPath;
    JobSystem::WorkStealingThreadPool Pool{2};
    AssetManager Assets;
    GUID Guid;
    std::shared_ptr<TextureAsset> Texture;
    UITesting::FixedScalePlatform Platform;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    UIElement* Icon = nullptr;
    // A second element that draws the same SVG only when a test gives it a
    // t16 or t24 class.
    UIElement* Twin = nullptr;

    explicit SvgIconFixture(float scale) : Platform(scale) {}

    // With `preload` the asset is loaded and registered up front, as the editor's
    // startup preload does; without it the UI's own load path runs.
    bool Build(Rendering::IDevice* device, const char* name, float scale, const char* svg = kWhiteSvg,
               bool preload = true)
    {
        UIRegistration::RegisterBuiltInControls();
        Root = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(Root);
        SvgPath = Root / "icon.svg";
        WriteSvg(svg);

        if (!Assets.Initialize(Root, &Pool) || !Assets.GetRegistry().RegisterAsset(SvgPath))
            return false;
        Guid = Assets.GetRegistry().GetAssetGUID(SvgPath);
        if (Guid.IsNull())
            return false;
        if (preload)
        {
            Texture = std::make_shared<TextureAsset>(Guid, SvgPath);
            if (!Texture->Load())
                return false;
            Assets.RegisterLoadedAsset(Guid, Texture);
        }

        std::unique_ptr<UIElement> root;
        if (!UIParsing::XMLParser::ParseLayoutFromString(kLayout, root))
            return false;
        Ui = std::make_unique<UIManager>(device, &Assets);
        Ui->SetPlatform(&Platform);
        Ui->SetLayoutSizeOverride(static_cast<uint32_t>(256 * scale), static_cast<uint32_t>(128 * scale));
        Ui->SetRoot(std::move(root));
        Icon = Ui->GetRootElement()->FindById(HashStringId("icon"));
        Twin = Ui->GetRootElement()->FindById(HashStringId("twin"));

        Stylesheet sheet{};
        if (!UIParsing::CSSParser::ParseStylesFromString(kCss, sheet))
            return false;
        Ui->AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));
        Rg = std::make_unique<UiRgHarness>(device);
        return Icon != nullptr && Twin != nullptr;
    }

    void WriteSvg(const char* svg)
    {
        std::ofstream file(SvgPath, std::ios::binary | std::ios::trunc);
        file << svg;
    }

    void Frame()
    {
        Ui->Update(1.0f / 60.0f, /*interactive=*/true);
        DriveUiRender(*Ui, *Rg);
    }

    std::vector<UI::SvgRasterSize> Sizes() const { return Ui->BackgroundImages().SvgSizesForTesting(Guid); }

    // Paints until the size is baked and bound, or gives up: the bake is a
    // worker job that lands through the dispatcher on a later Update.
    bool WaitForVariant(UI::SvgRasterSize size)
    {
        for (int frame = 0; frame < 2000; ++frame)
        {
            Frame();
            for (const UI::SvgRasterSize baked : Sizes())
                if (baked == size)
                    return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    void SetSizeClass(const char* className)
    {
        for (const char* c : {"s8", "s15", "s16", "s16x12", "s17", "s20", "s32", "w20"})
            Icon->RemoveClass(c);
        if (className)
            Icon->AddClass(className);
    }

    // Level 0 of the variant baked at `size`, read back as RGBA8.
    std::vector<uint8_t> ReadVariant(Rendering::IDevice* device, UI::SvgRasterSize size)
    {
        const uint32_t width = size.Width;
        const uint32_t height = size.Height;
        const Rendering::TextureHandle texture = Ui->BackgroundImages().SvgTextureForTesting(Guid, size);
        std::vector<uint8_t> pixels;
        if (!texture.IsValid() || width == 0 || height == 0)
            return pixels;
        pixels.resize(size_t(width) * height * 4);
        const Rendering::BufferHandle readback = device->CreateReadbackBuffer(pixels.size());
        auto commands = device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
            texture, Rendering::ResourceState::ShaderResource, Rendering::ResourceState::CopySource, 0, 1));
        commands->CopyTextureSubresourceToBuffer(texture, 0, 0, readback, width, height);
        commands->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
            texture, Rendering::ResourceState::CopySource, Rendering::ResourceState::ShaderResource, 0, 1));
        commands->End();
        device->ExecuteCommandLists({commands.get()});
        device->WaitForIdle();
        const auto* gpu = static_cast<const uint8_t*>(device->MapBuffer(readback));
        if (gpu)
            std::copy(gpu, gpu + pixels.size(), pixels.begin());
        device->UnmapBuffer(readback);
        device->DestroyBuffer(readback);
        return pixels;
    }
};

uint64_t Rgba8ChainBytes(uint32_t width, uint32_t height, uint32_t mipLevels)
{
    uint64_t bytes = 0;
    for (uint32_t level = 0; level < mipLevels; ++level)
        bytes += uint64_t(std::max(1u, width >> level)) * std::max(1u, height >> level) * 4u;
    return bytes;
}

const UI::UIPrimitive* TexturedPrimitive(const UIManager& ui, const UIElement& el)
{
    for (uint16_t i = 0;; ++i)
    {
        const UI::UIPrimitive* p = ui.PeekPrimitiveForTesting(el, i);
        if (!p)
            return nullptr;
        if (UI::GetMode(p->ModeAndFlags) == UI::PrimitiveMode::Textured)
            return p;
    }
}

} // namespace

using Sizes = std::vector<UI::SvgRasterSize>;

// An SVG is baked once per drawn size: 16 and 17 px are two rasters, and a
// paint at a size already baked asks for nothing new.
TEST(UISvgBackgroundVariants, EachDrawnSizeBakesOnce)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_sizes", 1.0f));

    fx.SetSizeClass("s16");
    ASSERT_TRUE(fx.WaitForVariant({16, 16}));
    EXPECT_EQ(fx.Sizes(), (Sizes{{16, 16}}));

    fx.SetSizeClass("s17");
    ASSERT_TRUE(fx.WaitForVariant({17, 17}));
    EXPECT_EQ(fx.Sizes(), (Sizes{{16, 16}, {17, 17}}));

    // The request is synchronous even though the bake is not.
    fx.SetSizeClass("s16");
    fx.Frame();
    EXPECT_EQ(fx.Ui->BackgroundImages().PendingCount(), 0u) << "16 px is already baked";
    EXPECT_EQ(fx.Sizes(), (Sizes{{16, 16}, {17, 17}}));
}

// Every icon bakes at the pixels it is drawn at, below the old 16 px floor
// too, and the quad it draws is that size: a 15 px icon is a 15 x 15 raster,
// an 8 px icon an 8 x 8 raster.
TEST(UISvgBackgroundVariants, IconsBakeAtTheirDrawnSize)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_exact", 1.0f));

    fx.SetSizeClass("s15");
    ASSERT_TRUE(fx.WaitForVariant({15, 15}));
    fx.Frame();
    const UI::UIPrimitive* quad = TexturedPrimitive(*fx.Ui, *fx.Icon);
    ASSERT_NE(quad, nullptr);
    EXPECT_FLOAT_EQ(quad->W, 15.0f);
    EXPECT_FLOAT_EQ(quad->H, 15.0f);
    EXPECT_EQ(fx.ReadVariant(device, {15, 15}).size(), 15u * 15u * 4u);

    fx.SetSizeClass("s8");
    ASSERT_TRUE(fx.WaitForVariant({8, 8}));
    EXPECT_EQ(fx.ReadVariant(device, {8, 8}).size(), 8u * 8u * 4u);
}

// The raster takes the drawn box's aspect, not the SVG's: a square icon drawn
// in a 16 x 12 box is a 16 x 12 raster, so nothing is stretched by a pixel.
TEST(UISvgBackgroundVariants, NonSquareBoxBakesAtItsOwnAspect)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_nonsquare", 1.0f));

    fx.SetSizeClass("s16x12");
    ASSERT_TRUE(fx.WaitForVariant({16, 12}));
    EXPECT_EQ(fx.Sizes(), (Sizes{{16, 12}}));
    fx.Frame();
    const UI::UIPrimitive* quad = TexturedPrimitive(*fx.Ui, *fx.Icon);
    ASSERT_NE(quad, nullptr);
    EXPECT_FLOAT_EQ(quad->W, 16.0f);
    EXPECT_FLOAT_EQ(quad->H, 12.0f);
}

// Device pixels: at content scale 2 a 15 CSS px icon bakes 30 x 30 and a
// 16 CSS px icon 32 x 32.
TEST(UISvgBackgroundVariants, ContentScaleTwoBakesTwiceThePixels)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(2.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_scale2", 2.0f));

    fx.SetSizeClass("s15");
    ASSERT_TRUE(fx.WaitForVariant({30, 30}));
    fx.SetSizeClass("s16");
    ASSERT_TRUE(fx.WaitForVariant({32, 32}));
    EXPECT_EQ(fx.Sizes(), (Sizes{{30, 30}, {32, 32}}));
}

// While a new size bakes the draw binds the nearest size already baked, so a
// resize never blanks the icon, and swaps to the exact raster once it lands.
TEST(UISvgBackgroundVariants, DrawsTheNearestBakedSizeUntilItsOwnLands)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_nearest", 1.0f));

    fx.SetSizeClass("s16");
    ASSERT_TRUE(fx.WaitForVariant({16, 16}));
    fx.Frame();
    const UI::UIPrimitive* quad = TexturedPrimitive(*fx.Ui, *fx.Icon);
    ASSERT_NE(quad, nullptr);
    const uint32_t slot16 = quad->TextureIndex;

    // The first paint at 20 px cannot have the 20 px raster yet: the size
    // settles, then bakes on a worker and lands on a later update.
    fx.SetSizeClass("s20");
    fx.Frame();
    EXPECT_EQ(fx.Sizes(), (Sizes{{16, 16}}));
    quad = TexturedPrimitive(*fx.Ui, *fx.Icon);
    ASSERT_NE(quad, nullptr) << "the icon draws while its size bakes";
    EXPECT_EQ(quad->TextureIndex, slot16) << "from the 16 px raster";
    EXPECT_FLOAT_EQ(quad->W, 20.0f);

    ASSERT_TRUE(fx.WaitForVariant({20, 20}));
    fx.Frame();
    quad = TexturedPrimitive(*fx.Ui, *fx.Icon);
    ASSERT_NE(quad, nullptr);
    EXPECT_NE(quad->TextureIndex, slot16) << "the 20 px raster replaced it";
}

// A resize or a slider asks for a new size every frame. A size of an SVG that
// already draws from another bakes only once it has been asked for over the
// settle time: sizes passed through are drawn from the nearest baked size and
// never baked, and the size the drag rests on bakes without another paint
// being asked for.
TEST(UISvgBackgroundVariants, OnlyTheSizeAResizeRestsOnBakes)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_settle", 1.0f));

    fx.SetSizeClass("s16");
    ASSERT_TRUE(fx.WaitForVariant({16, 16}));

    for (int pass = 0; pass < 3; ++pass)
        for (const char* size : {"s20", "s32", "s17"})
        {
            fx.SetSizeClass(size);
            fx.Frame();
        }
    EXPECT_EQ(fx.Ui->BackgroundImages().PendingCount(), 0u) << "nothing bakes while the size moves";

    // Rest on 17 px, painting only what the manager itself asks for.
    for (int frame = 0; frame < 2000 && fx.Sizes().size() < 2; ++frame)
    {
        fx.Ui->Update(1.0f / 60.0f, /*interactive=*/false);
        DriveUiRender(*fx.Ui, *fx.Rg);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(fx.Sizes(), (Sizes{{16, 16}, {17, 17}})) << "20 and 32 px were passed through, not baked";
}

// A size more than twice the baked one does not wait for the settle time: a
// raster magnified that far would draw blurry for the whole drag.
TEST(UISvgBackgroundVariants, SizeFarFromTheBakedOneBakesAtOnce)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_far", 1.0f));

    fx.SetSizeClass("s8");
    ASSERT_TRUE(fx.WaitForVariant({8, 8}));

    fx.SetSizeClass("s20");
    fx.Frame();
    const Sizes baked = fx.Sizes();
    const bool asked = fx.Ui->BackgroundImages().PendingCount() == 1u ||
                       std::find(baked.begin(), baked.end(), UI::SvgRasterSize{20, 20}) != baked.end();
    EXPECT_TRUE(asked) << "20 px is 2.5x the 8 px raster: it bakes on the first paint";
}

// Two elements draw one SVG at two new sizes, each close to the baked one (a
// content scale change does this to every icon drawn at two sizes): both sizes
// settle and bake, neither resets the other's wait.
TEST(UISvgBackgroundVariants, TwoNewSizesOfOneSvgBothBake)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_two_sizes", 1.0f));

    fx.SetSizeClass("s16");
    fx.Twin->AddClass("t16");
    ASSERT_TRUE(fx.WaitForVariant({16, 16}));

    fx.SetSizeClass("s20");
    fx.Twin->RemoveClass("t16");
    fx.Twin->AddClass("t24");
    for (int frame = 0; frame < 2000 && fx.Sizes().size() < 3; ++frame)
    {
        fx.Ui->Update(1.0f / 60.0f, /*interactive=*/false);
        DriveUiRender(*fx.Ui, *fx.Rg);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(fx.Sizes(), (Sizes{{16, 16}, {20, 20}, {24, 24}}));
}

// A size no longer drawn ages out once the cache is over its budget; the
// size on screen stays.
TEST(UISvgBackgroundVariants, IdleSizesAreEvictedAndTheDrawnSizeStays)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_evict", 1.0f));

    fx.SetSizeClass("s16");
    ASSERT_TRUE(fx.WaitForVariant({16, 16}));
    fx.SetSizeClass("s17");
    ASSERT_TRUE(fx.WaitForVariant({17, 17}));
    fx.Frame();
    ASSERT_EQ(fx.Sizes(), (Sizes{{16, 16}, {17, 17}}));

    fx.Ui->BackgroundImages().SetBudgetForTesting(0);
    // Past the idle window (600 frames) and onto a sweep (every 120 frames).
    for (int frame = 0; frame < 840; ++frame)
        fx.Ui->EndBackgroundImageFrame();
    EXPECT_EQ(fx.Sizes(), (Sizes{{17, 17}}));
}

// A reload drops every size, and the next paint bakes the new picture.
TEST(UISvgBackgroundVariants, ReloadEvictsEveryRungAndRebakesTheNewSource)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_reload", 1.0f));

    fx.SetSizeClass("s16");
    ASSERT_TRUE(fx.WaitForVariant({16, 16}));
    fx.SetSizeClass("s32");
    ASSERT_TRUE(fx.WaitForVariant({32, 32}));
    ASSERT_EQ(fx.Sizes(), (Sizes{{16, 16}, {32, 32}}));
    const std::vector<uint8_t> before = fx.ReadVariant(device, {32, 32});
    ASSERT_EQ(before.size(), 32u * 32u * 4u);
    EXPECT_EQ(before[(16 * 32 + 16) * 4], 255u);

    fx.WriteSvg(kBlackSvg);
    fx.Texture->Unload();
    ASSERT_TRUE(fx.Texture->Load());
    fx.Assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetReloaded, fx.Guid, AssetType::Texture, fx.SvgPath.string()));
    fx.Ui->Update(0.0f, /*interactive=*/false);
    EXPECT_TRUE(fx.Sizes().empty()) << "the reload drops every size";

    ASSERT_TRUE(fx.WaitForVariant({32, 32}));
    const std::vector<uint8_t> after = fx.ReadVariant(device, {32, 32});
    ASSERT_EQ(after.size(), 32u * 32u * 4u);
    EXPECT_EQ(after[(16 * 32 + 16) * 4], 0u) << "the size was baked from the reloaded source";
    EXPECT_EQ(after[(16 * 32 + 16) * 4 + 3], 255u);
}

// A reload lands while a bake of the old source waits for a worker: the old
// picture is never cached, and the size bakes again from the new source.
TEST(UISvgBackgroundVariants, ABakeOfTheSourceBeforeAReloadIsDropped)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_reload_during_bake", 1.0f));

    // Hold both workers, so the bake of the white source waits until released.
    std::atomic<int> started{0};
    std::atomic<bool> release{false};
    for (int i = 0; i < 2; ++i)
        fx.Assets.GetJobSystem().Submit(
            [&]()
            {
                ++started;
                while (!release.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
    while (started.load() < 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    fx.SetSizeClass("s32");
    fx.Frame();
    EXPECT_EQ(fx.Ui->BackgroundImages().PendingCount(), 1u) << "the white bake is queued";

    fx.WriteSvg(kBlackSvg);
    fx.Texture->Unload();
    ASSERT_TRUE(fx.Texture->Load());
    fx.Assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetReloaded, fx.Guid, AssetType::Texture, fx.SvgPath.string()));
    fx.Ui->Update(0.0f, /*interactive=*/false);

    // The white bake runs now and its completion is drained without a paint.
    release = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    fx.Ui->Update(0.0f, /*interactive=*/false);
    EXPECT_TRUE(fx.Sizes().empty()) << "the raster of the source before the reload was cached";

    ASSERT_TRUE(fx.WaitForVariant({32, 32}));
    const std::vector<uint8_t> pixels = fx.ReadVariant(device, {32, 32});
    ASSERT_EQ(pixels.size(), 32u * 32u * 4u);
    EXPECT_EQ(pixels[(16 * 32 + 16) * 4], 0u) << "the size was baked from the reloaded source";
}

// background-size: auto draws an SVG at its intrinsic CSS size times the
// content scale: an 18 px icon at scale 2 draws 36 px from a 36 px raster.
TEST(UISvgBackgroundVariants, AutoSizeIsTheIntrinsicSizeAtTheContentScale)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(2.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_auto", 2.0f, kEighteenSvg));

    fx.SetSizeClass(nullptr);
    ASSERT_TRUE(fx.WaitForVariant({36, 36}));
    EXPECT_EQ(fx.Sizes(), (Sizes{{36, 36}}));
    fx.Frame();
    const UI::UIPrimitive* quad = TexturedPrimitive(*fx.Ui, *fx.Icon);
    ASSERT_NE(quad, nullptr);
    EXPECT_FLOAT_EQ(quad->W, 36.0f);
    EXPECT_FLOAT_EQ(quad->H, 36.0f);
}

// An SVG that never yields a source (a malformed file here; a build without
// ThorVG in general) must not be asked for again every paint: the request
// settles with nothing in flight and no raster, and the tree stops churning.
TEST(UISvgBackgroundVariants, UnloadableSvgSettlesWithoutRetrying)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_unloadable", 1.0f, "<svg this is not an svg", /*preload=*/false));

    fx.SetSizeClass("s16");
    for (int i = 0; i < 120; ++i)
    {
        fx.Frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // A paint may have just re-asked the raster path; its completion lands on
    // the next drain, so drain without painting before reading the count.
    for (int i = 0; i < 10; ++i)
    {
        fx.Ui->Update(1.0f / 60.0f, /*interactive=*/false);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(fx.Sizes().empty());
    EXPECT_EQ(fx.Ui->BackgroundImages().PendingCount(), 0u);
}

// The cache's byte budget counts every size, not one texture per asset. A
// variant is drawn at the size it was baked at, so it uploads level 0 only.
TEST(UISvgBackgroundVariants, CacheBytesCountEveryRung)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_bytes", 1.0f));

    fx.SetSizeClass("s16");
    ASSERT_TRUE(fx.WaitForVariant({16, 16}));
    fx.SetSizeClass("s32");
    ASSERT_TRUE(fx.WaitForVariant({32, 32}));
    ASSERT_EQ(fx.Sizes(), (Sizes{{16, 16}, {32, 32}}));

    EXPECT_EQ(fx.Ui->BackgroundImages().BytesForTesting(), Rgba8ChainBytes(16, 16, 1) + Rgba8ChainBytes(32, 32, 1));
}

// background-size with one length keeps the image's aspect on the other axis,
// and the raster and the drawn quad agree on it: a square 16 px SVG at
// `background-size: 20px` bakes 20 x 20 and draws 20 x 20.
TEST(UISvgBackgroundVariants, OneLengthKeepsTheAspectInTheBakeAndTheDraw)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_one_length", 1.0f));

    fx.SetSizeClass("w20");
    ASSERT_TRUE(fx.WaitForVariant({20, 20}));
    fx.Frame();
    const UI::UIPrimitive* quad = TexturedPrimitive(*fx.Ui, *fx.Icon);
    ASSERT_NE(quad, nullptr);
    EXPECT_FLOAT_EQ(quad->W, 20.0f);
    EXPECT_FLOAT_EQ(quad->H, 20.0f);
}

// A border-image measures its slices in the image's own pixels, so an SVG
// drawn with one draws from the asset's raster and bakes no drawn size.
TEST(UISvgBackgroundVariants, BorderImageDrawsFromTheAssetRaster)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_border_image", 1.0f));

    fx.SetSizeClass("s16");
    fx.Icon->AddClass("sliced");
    for (int frame = 0; frame < 20; ++frame)
        fx.Frame();
    EXPECT_TRUE(fx.Sizes().empty());
    EXPECT_TRUE(fx.Ui->BackgroundImages().HasRasterForTesting(fx.Guid));
}

namespace
{
// Red of every pixel of the 16 x 16 box at (x, y) in an 800 x 600 readback.
std::vector<uint8_t> BoxRed(const std::vector<uint8_t>& screen, uint32_t x, uint32_t y)
{
    std::vector<uint8_t> red;
    for (uint32_t row = 0; row < 16; ++row)
        for (uint32_t column = 0; column < 16; ++column)
            red.push_back(screen[((y + row) * UITesting::kReadbackW + x + column) * 4]);
    return red;
}

// The raster's alpha: a white picture composited over the cleared target
// writes alpha into each colour channel.
std::vector<uint8_t> RasterAlpha(const std::vector<uint8_t>& raster)
{
    std::vector<uint8_t> alpha;
    for (size_t i = 3; i < raster.size(); i += 4)
        alpha.push_back(raster[i]);
    return alpha;
}
} // namespace

// A raster baked at its drawn size lands texel for texel on the device grid:
// each texel is one screen pixel, so linear and nearest filtering draw the
// same bytes and those bytes are the raster's. At a half-pixel position the
// quad snaps to the grid (as the element's own paint box does) instead of
// resampling every texel between two pixels.
TEST(UISvgBackgroundVariants, ExactSizeRasterDrawsTexelForTexel)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    SvgIconFixture fx(1.0f);
    ASSERT_TRUE(fx.Build(device, "ui_svg_variants_texel_exact", 1.0f, kDiscSvg));

    fx.SetSizeClass("s16");
    ASSERT_TRUE(fx.WaitForVariant({16, 16}));
    const std::vector<uint8_t> alpha = RasterAlpha(fx.ReadVariant(device, {16, 16}));
    ASSERT_EQ(alpha.size(), 256u);

    struct Placement
    {
        const char* Class;
        uint32_t ScreenX;
    };
    for (const Placement placement : {Placement{"at4", 4u}, Placement{"at4half", 5u}})
    {
        fx.Icon->RemoveClass("at4");
        fx.Icon->RemoveClass("at4half");
        fx.Icon->AddClass(placement.Class);

        fx.Icon->RemoveClass("ui-bg-point-filter");
        fx.Frame();
        const std::vector<uint8_t> linear = UITesting::RenderUiToBytes(*fx.Ui, UI::UITargetSpace::EncodedSrgb());
        fx.Icon->AddClass("ui-bg-point-filter");
        fx.Frame();
        const std::vector<uint8_t> nearest = UITesting::RenderUiToBytes(*fx.Ui, UI::UITargetSpace::EncodedSrgb());
        ASSERT_FALSE(linear.empty());
        ASSERT_FALSE(nearest.empty());

        EXPECT_EQ(BoxRed(linear, placement.ScreenX, placement.ScreenX), alpha) << placement.Class << ", linear";
        EXPECT_EQ(BoxRed(nearest, placement.ScreenX, placement.ScreenX), alpha) << placement.Class << ", nearest";
    }
}
