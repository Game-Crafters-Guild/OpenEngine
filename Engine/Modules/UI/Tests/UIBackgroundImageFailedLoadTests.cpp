#include "SolidTga.h"
#include "UIRgTestHarness.h"

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using namespace GameEngine;

namespace
{

constexpr const char* kLayout = R"(<uielement id='root'><uielement id='icon' /></uielement>)";
constexpr const char* kCss = R"(
#root { display: flex; width: 128px; height: 128px; }
#icon { width: 64px; height: 64px; background-image: url("icon.tga"); }
)";
constexpr const char* kWarningSubject = "UI: background image";
constexpr const char* kWarningResult = "it stays blank";
constexpr int kPaints = 60;
constexpr int kMaxWaitPaints = 2000;
constexpr uint32_t kSeededTextureId = 0xCAFEBABEu;

// Counts the log lines that report a background image that stays blank.
struct FailedLoadWarnings
{
    Logger::CallbackSink* Sink = nullptr;
    std::uint64_t CallbackId = 0;
    std::shared_ptr<std::atomic<int>> Count = std::make_shared<std::atomic<int>>(0);

    FailedLoadWarnings()
    {
        Logger::Log::Initialize({});
        auto sink = std::make_unique<Logger::CallbackSink>();
        Sink = sink.get();
        CallbackId = Sink->RegisterCallback(
            [count = Count](const Logger::LogMessage& message)
            {
                if (message.Message.find(kWarningSubject) != std::string::npos &&
                    message.Message.find(kWarningResult) != std::string::npos)
                    count->fetch_add(1);
            });
        Logger::Log::AddSink(std::move(sink));
    }
    ~FailedLoadWarnings() { Sink->UnregisterCallback(CallbackId); }

    int Read() const
    {
        Logger::Log::Flush();
        return Count->load();
    }
};

// One raster image drawn by one element, painted through the render graph the
// way the editor paints. The image file is not a TGA, so its load fails.
struct BrokenImageFixture
{
    std::filesystem::path Root;
    std::filesystem::path ImagePath;
    JobSystem::WorkStealingThreadPool Pool{2};
    AssetManager Assets;
    GUID Guid;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    FailedLoadWarnings Warnings;

    bool Build(Rendering::IDevice* device, const char* name)
    {
        UIRegistration::RegisterBuiltInControls();
        Root = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(Root);
        std::filesystem::create_directories(Root);
        ImagePath = Root / "icon.tga";
        {
            std::ofstream file(ImagePath, std::ios::binary);
            file << "this is not a tga";
        }
        if (!Assets.Initialize(Root, &Pool) || !Assets.GetRegistry().RegisterAsset(ImagePath))
            return false;
        Guid = Assets.GetRegistry().GetAssetGUID(ImagePath);
        if (Guid.IsNull())
            return false;

        std::unique_ptr<UIElement> root;
        if (!UIParsing::XMLParser::ParseLayoutFromString(kLayout, root))
            return false;
        Ui = std::make_unique<UIManager>(device, &Assets);
        Ui->SetLayoutSizeOverride(128, 128);
        Ui->SetRoot(std::move(root));
        Stylesheet sheet{};
        if (!UIParsing::CSSParser::ParseStylesFromString(kCss, sheet))
            return false;
        Ui->AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));
        Rg = std::make_unique<UiRgHarness>(device);
        return true;
    }

    // One update and one paint. A load the paint asks for completes through
    // the dispatcher on a later update.
    void Frame()
    {
        Ui->Update(1.0f / 60.0f, /*interactive=*/true);
        DriveUiRender(*Ui, *Rg);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Paints until the first load has failed and said so.
    bool PaintUntilTheFirstLoadFails()
    {
        for (int i = 0; i < kMaxWaitPaints && Warnings.Read() == 0; ++i)
            Frame();
        return Warnings.Read() == 1;
    }

    // Paints until a paint starts the load. The completion stays in the
    // dispatcher until the next update.
    bool PaintUntilTheLoadStarts()
    {
        for (int i = 0; i < kMaxWaitPaints && Ui->BackgroundImages().PendingCount() == 0; ++i)
            Frame();
        return Ui->BackgroundImages().PendingCount() > 0;
    }

    // Writes a valid image and sends the change the way the asset manager does
    // after a save.
    void FixTheFile()
    {
        UITesting::WriteSolidTga(ImagePath, 4, 4, 255, 255, 255);
        Assets.ClearLoadSuppressed(Guid);
        Assets.GetEventDispatcher().DispatchEvent(
            AssetEvent(AssetEventType::AssetModified, Guid, AssetType::Texture, ImagePath.string()));
    }
};

} // namespace

// A background image whose file does not decode is loaded once and reported
// once; later paints draw nothing and leave no load in flight.
TEST(UIBackgroundImageFailedLoad, LoadsOnceAndWarnsOnce)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    BrokenImageFixture fx;
    ASSERT_TRUE(fx.Build(device, "ui_background_image_failed_load"));
    ASSERT_TRUE(fx.PaintUntilTheFirstLoadFails());

    int paintsThatLoad = 0;
    for (int i = 0; i < kPaints; ++i)
    {
        fx.Frame();
        if (fx.Ui->BackgroundImages().PendingCount() > 0)
            ++paintsThatLoad;
    }

    EXPECT_EQ(paintsThatLoad, 0);
    EXPECT_EQ(fx.Warnings.Read(), 1);
    EXPECT_FALSE(fx.Ui->BackgroundImages().HasRasterForTesting(fx.Guid));
}

// A change to the file clears the failure: the next paint loads the image again.
TEST(UIBackgroundImageFailedLoad, AChangedFileLoadsAgain)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    BrokenImageFixture fx;
    ASSERT_TRUE(fx.Build(device, "ui_background_image_failed_load_fixed"));
    ASSERT_TRUE(fx.PaintUntilTheFirstLoadFails());

    fx.FixTheFile();

    for (int i = 0; i < kMaxWaitPaints && !fx.Ui->BackgroundImages().HasRasterForTesting(fx.Guid); ++i)
        fx.Frame();
    EXPECT_TRUE(fx.Ui->BackgroundImages().HasRasterForTesting(fx.Guid));
}

// A failed load that completes after a file change does not mark the new entry
// failed: the update applies the change before it delivers the completion.
TEST(UIBackgroundImageFailedLoad, AFailedLoadThatCompletesAfterAChangeDoesNotBlockTheFixedFile)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    BrokenImageFixture fx;
    ASSERT_TRUE(fx.Build(device, "ui_background_image_failed_load_late"));
    ASSERT_TRUE(fx.PaintUntilTheLoadStarts());
    // The completion waits in the dispatcher: no update runs until the change.
    UI::IUiDispatcher* dispatcher = fx.Ui->GetDispatcher();
    for (int i = 0; i < kMaxWaitPaints && dispatcher->PendingCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(fx.Assets.IsLoadSuppressed(fx.Guid));
    ASSERT_GT(dispatcher->PendingCount(), 0u);

    fx.FixTheFile();

    for (int i = 0; i < kMaxWaitPaints && !fx.Ui->BackgroundImages().HasRasterForTesting(fx.Guid); ++i)
        fx.Frame();
    EXPECT_TRUE(fx.Ui->BackgroundImages().HasRasterForTesting(fx.Guid));
    EXPECT_EQ(fx.Warnings.Read(), 0);
}

// A failed image answers a request at once with no texture and starts no load.
TEST(UIBackgroundImageFailedLoad, ARequestForAFailedImageAnswersAtOnce)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    BrokenImageFixture fx;
    ASSERT_TRUE(fx.Build(device, "ui_background_image_failed_load_request"));
    ASSERT_TRUE(fx.PaintUntilTheFirstLoadFails());

    int answers = 0;
    for (int i = 0; i < kPaints; ++i)
    {
        fx.Ui->BackgroundImages().Request(fx.Guid,
                                          [&answers](Rendering::TextureHandle handle, uint32_t width, uint32_t height)
                                          {
                                              EXPECT_FALSE(handle.IsValid());
                                              EXPECT_EQ(width, 0u);
                                              EXPECT_EQ(height, 0u);
                                              ++answers;
                                          });
        EXPECT_EQ(fx.Ui->BackgroundImages().PendingCount(), 0u);
    }

    EXPECT_EQ(answers, kPaints);
    EXPECT_EQ(fx.Warnings.Read(), 1);
}

// A load with no texture that completes after the image has a texture leaves
// the texture in place and reports nothing.
TEST(UIBackgroundImageFailedLoad, AnEmptyCompletionKeepsATextureThatIsAlreadyThere)
{
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    BrokenImageFixture fx;
    ASSERT_TRUE(fx.Build(device, "ui_background_image_failed_load_kept"));
    ASSERT_TRUE(fx.PaintUntilTheLoadStarts());

    // The seeded handle is never drawn: only updates run from here.
    Rendering::TextureHandle seeded{};
    seeded.id = kSeededTextureId;
    fx.Ui->BackgroundImages().SeedRasterForTesting(fx.Guid, seeded, 4, 4);
    for (int i = 0; i < kMaxWaitPaints && fx.Ui->BackgroundImages().PendingCount() > 0; ++i)
    {
        fx.Ui->Update(1.0f / 60.0f, /*interactive=*/true);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(fx.Ui->BackgroundImages().PendingCount(), 0u);

    Rendering::TextureHandle answered{};
    fx.Ui->BackgroundImages().Request(fx.Guid,
                                      [&answered](Rendering::TextureHandle handle, uint32_t, uint32_t)
                                      { answered = handle; });
    EXPECT_EQ(answered.id, kSeededTextureId);
    EXPECT_EQ(fx.Warnings.Read(), 0);
}
