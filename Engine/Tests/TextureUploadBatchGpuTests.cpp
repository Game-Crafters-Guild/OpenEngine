// The texture drain copies every texture of a flush with one command list, submitted once, and a
// texture that does not upload fails alone, as a failed load does: its binds stop waiting on it.

#include <gtest/gtest.h>
#if defined(RENDERING_HAS_VULKAN) && RENDERING_HAS_VULKAN
#include "Assets/TextureAsset.h"
#include "Assets/XmlAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "EngineLogCapture.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Core/CommandList.h"
#include "Source/Vulkan/VulkanDevice/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace GameEngine::Engine::Renderer
{
// Lets the embedded-image upload jobs finish before the drain, so one flush takes all of them.
struct TextureUploadBatchTestAccess
{
    static void ScheduleEmbeddedUpload(TextureService& service, const GUID& texture, const Vector<uint8>& png)
    {
        service.ScheduleEmbeddedUploadJob(texture, GUID::Generate(), StringId{}, png, /*linear=*/true);
    }
    static void WaitForUploadJobs(TextureService& service)
    {
        EngineCore::GetInstance().GetJobSystem().Wait(service.m_UploadJobCounter);
    }
    // A material's bind of `texture` through an upload job, as BindMaterialTextureSlot schedules it.
    static void ScheduleUploadJob(TextureService& service, const GUID& texture)
    {
        service.ScheduleTextureUploadJob(texture, GUID::Generate(), StringId{});
    }
    // Queues `asset` as a finished AssetManager load of `texture` that one material slot waits on.
    static void QueueLoaded(TextureService& service, const GUID& texture, const SharedPtr<Asset>& asset)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        service.m_PendingGpuUploads.push_back({texture, asset, service.m_TextureEpochByGuid[texture]});
        service.m_PendingTextureBinds.push_back({GUID::Generate(), StringId{}, texture});
        service.m_TextureLoadHandles.try_emplace(texture);
    }
    // The hot swap's upload in place of `resident`, the AssetManager's instance of `texture`.
    static TextureGPUCache::Entry UploadResident(TextureService& service, const GUID& texture,
                                                 SharedPtr<TextureAsset> resident)
    {
        const auto prepared = TextureService::PrepareUpload(texture, std::move(resident), nullptr, false);
        return service.SubmitPreparedUpload(texture, *prepared);
    }
    static TextureHandle Cached(TextureService& service, const GUID& texture)
    {
        TextureGPUCache::Entry entry;
        service.m_TextureGPUCache.TryGet(texture, entry);
        return entry.Texture;
    }
    // Cached as no texture: a failed load, which the bind pass binds as the slot default.
    static bool CachedAsFailed(TextureService& service, const GUID& texture)
    {
        TextureGPUCache::Entry entry;
        return service.m_TextureGPUCache.TryGet(texture, entry) && !entry.Texture.IsValid();
    }
};
} // namespace GameEngine::Engine::Renderer

namespace
{
namespace fs = std::filesystem;
using Access = Engine::Renderer::TextureUploadBatchTestAccess;
using Texel = std::array<uint8, 4>;

// 1x1 RGBA PNGs: (200, 120, 40, 255) and (40, 200, 120, 255).
constexpr uint8 kOrangePng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x38, 0x51, 0xa1, 0xf1, 0x1f, 0x00, 0x05, 0xdc, 0x02, 0x68,
    0x88, 0x92, 0x98, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
constexpr uint8 kGreenPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xd0, 0x38, 0x51, 0xf1, 0x1f, 0x00, 0x04, 0xec, 0x02, 0x68,
    0x3f, 0xdb, 0x74, 0x34, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
constexpr Texel kOrange{200, 120, 40, 255};
constexpr Texel kGreen{40, 200, 120, 255};

// Counts command lists and submits, refuses the staging buffer request RefuseStagingAt (from 1),
// and gives no command list while RefuseCommandLists is set.
class UploadCountingDevice final : public VulkanDevice
{
  public:
    uint32_t CommandLists = 0, Submits = 0, StagingRequests = 0, RefuseStagingAt = 0;
    bool RefuseCommandLists = false;
    BufferHandle CreateBuffer(const BufferDesc& desc) override
    {
        if (desc.memoryUsage == BufferMemoryUsage::Upload && ++StagingRequests == RefuseStagingAt)
            return {};
        return VulkanDevice::CreateBuffer(desc);
    }
    std::unique_ptr<CommandList> CreateCommandList(QueueType queue) override
    {
        ++CommandLists;
        if (RefuseCommandLists)
            return nullptr;
        return VulkanDevice::CreateCommandList(queue);
    }
    void ExecuteCommandLists(const std::vector<CommandList*>& lists) override
    {
        ++Submits;
        VulkanDevice::ExecuteCommandLists(lists);
    }
};

class TextureUploadBatch : public testing::Test
{
  protected:
    inline static fs::path s_Workspace, s_PreviousDirectory;
    UploadCountingDevice m_Device;
    Engine::Renderer::MaterialRegistry m_Materials;
    Engine::Renderer::TextureService m_Textures;
    bool m_DeviceReady = false;

    static void SetUpTestSuite()
    {
        s_PreviousDirectory = fs::current_path();
        s_Workspace = fs::temp_directory_path() / ("ge-upload-batch-" + GUID::Generate().ToString());
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
    }
    static void TearDownTestSuite()
    {
        EngineCore::GetInstance().Shutdown();
        fs::current_path(s_PreviousDirectory);
        std::error_code error;
        fs::remove_all(s_Workspace, error);
    }
    void SetUp() override
    {
        DeviceDesc desc{};
        desc.enableSwapchain = false;
        desc.enableDebugLayer = false;
        desc.enableDynamicRendering = true;
        if (!m_Device.Initialize(desc))
            GTEST_SKIP() << "No Vulkan device available";
        m_DeviceReady = true;
        m_Materials.Initialize(&m_Device);
        ASSERT_TRUE(m_Textures.Initialize(&m_Device, m_Materials,
                                          RendererProfile::FromCapabilities(m_Device.GetCapabilities())));
    }
    void TearDown() override
    {
        if (!m_DeviceReady)
            return;
        m_Device.WaitForIdle();
        m_Textures.Shutdown();
        m_Materials.Shutdown();
        m_Device.Shutdown();
    }

    // Queues one embedded-image upload per image and waits for every decode job, so the next
    // flush stages them all. Returns the texture of each image, in order.
    std::vector<GUID> QueueDecodedUploads(const std::vector<Vector<uint8>>& images)
    {
        std::vector<GUID> textures;
        for (const Vector<uint8>& png : images)
        {
            textures.push_back(GUID::Generate());
            Access::ScheduleEmbeddedUpload(m_Textures, textures.back(), png);
        }
        Access::WaitForUploadJobs(m_Textures);
        return textures;
    }

    // Flushes the queued uploads and returns the error lines the flush logged.
    std::vector<std::string> FlushCapturingErrors()
    {
        std::vector<std::string> errors;
        {
            TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
            m_Textures.FlushPendingUploads();
            Logger::Log::Flush();
        }
        return errors;
    }

    // Flushes one queued load of `texture` and expects it to fail as a refused texture does:
    // cached as no texture, one error line that names it, and no bind left waiting on it.
    void ExpectQueuedLoadFails(const GUID& texture)
    {
        ASSERT_EQ(m_Textures.PendingMaterialTextureBindCount(), 1u);
        const std::vector<std::string> errors = FlushCapturingErrors();
        EXPECT_TRUE(Access::CachedAsFailed(m_Textures, texture));
        EXPECT_EQ(TestLog::CountLinesContaining(errors, texture.ToString()), 1u);
        EXPECT_EQ(m_Textures.PendingMaterialTextureBindCount(), 0u);
    }

    // A registered 4x4 SVG. The import cook does not read SVG, so GetOrUpload uploads it on the
    // calling thread.
    static GUID RegisterSvg()
    {
        const fs::path path = s_Workspace / "Assets" / (GUID::Generate().ToString() + ".svg");
        std::ofstream(path) << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\">"
                               "<rect width=\"4\" height=\"4\" fill=\"#4a7\"/></svg>";
        auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        EXPECT_TRUE(registry.RegisterAsset(path));
        return registry.GetAssetGUID(path);
    }

    // Texel (0, 0) of an uploaded 1x1 RGBA8 texture, copied back from the GPU.
    Texel ReadTexel(TextureHandle texture)
    {
        const BufferHandle readback = m_Device.CreateReadbackBuffer(sizeof(Texel));
        auto commands = m_Device.CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(
            ResourceBarrier::CreateTextureBarrier(texture, ResourceState::ShaderResource, ResourceState::CopySource));
        commands->CopyTextureToBuffer(texture, readback, 1, 1);
        commands->End();
        m_Device.ExecuteCommandLists({commands.get()});
        m_Device.WaitForIdle();
        Texel texel{};
        if (const auto* mapped = static_cast<const uint8*>(m_Device.MapBuffer(readback)))
            std::copy_n(mapped, texel.size(), texel.begin());
        m_Device.UnmapBuffer(readback);
        m_Device.DestroyBuffer(readback);
        return texel;
    }

    template <size_t N>
    static Vector<uint8> Png(const uint8 (&bytes)[N])
    {
        return Vector<uint8>(std::begin(bytes), std::end(bytes));
    }
};

TEST_F(TextureUploadBatch, AFlushCopiesEveryTextureWithOneCommandListAndOneSubmit)
{
    const std::vector<GUID> textures = QueueDecodedUploads({Png(kOrangePng), Png(kGreenPng), Png(kOrangePng)});
    m_Device.CommandLists = 0;
    m_Device.Submits = 0;
    m_Textures.FlushPendingUploads();
    EXPECT_EQ(m_Device.CommandLists, 1u);
    EXPECT_EQ(m_Device.Submits, 1u);

    const std::array<Texel, 3> expected{kOrange, kGreen, kOrange};
    for (size_t i = 0; i < textures.size(); ++i)
    {
        const TextureHandle texture = Access::Cached(m_Textures, textures[i]);
        ASSERT_TRUE(texture.IsValid()) << "texture " << i << " was not uploaded";
        // Every texture of the batch leaves the upload ready to sample, not only the first.
        EXPECT_EQ(m_Device.GetTrackedImageLayout(texture), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) << "texture " << i;
        EXPECT_EQ(ReadTexel(texture), expected[i]) << "texture " << i;
    }
}

// The refused texture is cached as failed, not published without its pixels, and its bind leaves
// the pending set. The others of the flush upload.
TEST_F(TextureUploadBatch, ARefusedStagingBufferFailsOnlyItsOwnTexture)
{
    const std::vector<GUID> textures = QueueDecodedUploads({Png(kOrangePng), Png(kGreenPng), Png(kOrangePng)});
    ASSERT_EQ(m_Textures.PendingMaterialTextureBindCount(), textures.size());
    m_Device.StagingRequests = 0;
    m_Device.RefuseStagingAt = 2;
    m_Device.Submits = 0;
    const std::vector<std::string> errors = FlushCapturingErrors();
    m_Device.RefuseStagingAt = 0;
    // The textures the device takes still go out in the flush's one submit.
    EXPECT_EQ(m_Device.Submits, 1u);

    const std::array<Texel, 3> expected{kOrange, kGreen, kOrange};
    size_t uploaded = 0, failed = 0;
    for (size_t i = 0; i < textures.size(); ++i)
    {
        const size_t errorLines = TestLog::CountLinesContaining(errors, textures[i].ToString());
        const TextureHandle texture = Access::Cached(m_Textures, textures[i]);
        if (!texture.IsValid())
        {
            failed += Access::CachedAsFailed(m_Textures, textures[i]) ? 1 : 0;
            EXPECT_EQ(errorLines, 1u) << "texture " << i;
            continue;
        }
        ++uploaded;
        EXPECT_EQ(errorLines, 0u) << "texture " << i;
        EXPECT_EQ(ReadTexel(texture), expected[i]) << "texture " << i;
    }
    EXPECT_EQ(uploaded, 2u);
    EXPECT_EQ(failed, 1u);
    EXPECT_EQ(m_Textures.PendingMaterialTextureBindCount(), 0u);
}

// Without a command list every texture of the flush fails with an error line of its own, none is
// published unwritten, and no bind keeps waiting on them.
TEST_F(TextureUploadBatch, AFlushWithoutACommandListFailsEveryTexture)
{
    const std::vector<GUID> textures = QueueDecodedUploads({Png(kOrangePng), Png(kGreenPng)});
    ASSERT_EQ(m_Textures.PendingMaterialTextureBindCount(), textures.size());
    m_Device.RefuseCommandLists = true;
    const std::vector<std::string> errors = FlushCapturingErrors();
    m_Device.RefuseCommandLists = false;
    for (size_t i = 0; i < textures.size(); ++i)
    {
        EXPECT_TRUE(Access::CachedAsFailed(m_Textures, textures[i])) << "texture " << i;
        EXPECT_EQ(TestLog::CountLinesContaining(errors, textures[i].ToString()), 1u) << "texture " << i;
    }
    EXPECT_EQ(m_Textures.PendingMaterialTextureBindCount(), 0u);
}

// GetOrUpload's own upload, refused, is cached as failed: the next call stages nothing and logs
// nothing. A reupload (what an edit of the file requests) tries again.
TEST_F(TextureUploadBatch, ARefusedGetOrUploadIsTriedOnceUntilAReupload)
{
    const GUID texture = RegisterSvg();
    m_Device.StagingRequests = 0;
    m_Device.RefuseCommandLists = true;
    std::vector<std::string> errors;
    {
        TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
        EXPECT_FALSE(m_Textures.GetOrUpload(texture).IsValid());
        EXPECT_FALSE(m_Textures.GetOrUpload(texture).IsValid());
        Logger::Log::Flush();
    }
    m_Device.RefuseCommandLists = false;
    EXPECT_EQ(m_Device.StagingRequests, 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(errors, texture.ToString()), 1u);
    EXPECT_TRUE(Access::CachedAsFailed(m_Textures, texture));

    m_Textures.RequestReupload(texture);
    m_Textures.FlushPendingUploads();
    EXPECT_TRUE(m_Textures.GetOrUpload(texture).IsValid());
}

TEST_F(TextureUploadBatch, AQueuedLoadThatIsNotATextureFails)
{
    const GUID texture = GUID::Generate();
    Access::QueueLoaded(m_Textures, texture, MakeShared<XmlAsset>(texture, s_Workspace / "Assets" / "notes.xml"));
    ExpectQueuedLoadFails(texture);
}

TEST_F(TextureUploadBatch, AQueuedTextureWithoutPixelsFails)
{
    const GUID texture = GUID::Generate();
    // Never loaded: no pixels, and no registry entry, so nothing loads it on the way.
    Access::QueueLoaded(m_Textures, texture, MakeShared<TextureAsset>(texture, s_Workspace / "Assets" / "empty.png"));
    ExpectQueuedLoadFails(texture);
}

// An upload in place of a texture with no pixels is cached as failed, with one error line:
// GetOrUpload then does not read the file again. A reupload tries again.
TEST_F(TextureUploadBatch, AnUploadInPlaceWithoutPixelsIsTriedOnceUntilAReupload)
{
    const GUID texture = RegisterSvg();
    m_Device.StagingRequests = 0;
    std::vector<std::string> errors;
    {
        TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
        // Never loaded: no pixels.
        const auto resident = MakeShared<TextureAsset>(texture, s_Workspace / "Assets" / "empty.svg");
        EXPECT_FALSE(Access::UploadResident(m_Textures, texture, resident).Texture.IsValid());
        EXPECT_FALSE(m_Textures.GetOrUpload(texture).IsValid());
        Logger::Log::Flush();
    }
    EXPECT_EQ(m_Device.StagingRequests, 0u);
    EXPECT_EQ(TestLog::CountLinesContaining(errors, texture.ToString()), 1u);
    EXPECT_TRUE(Access::CachedAsFailed(m_Textures, texture));

    m_Textures.RequestReupload(texture);
    m_Textures.FlushPendingUploads();
    EXPECT_TRUE(m_Textures.GetOrUpload(texture).IsValid());
}

// An upload job with no pixels fails at the flush as a queued load does, so the next model that
// embeds the same bytes finds it cached and does not decode them again.
TEST_F(TextureUploadBatch, AnUploadJobWithoutPixelsFailsAtTheFlush)
{
    std::vector<std::string> warnings;
    GUID texture;
    {
        TestLog::ScopedEngineLogCapture capture(&warnings, Logger::LogLevel::Warning);
        texture = QueueDecodedUploads({Vector<uint8>{1, 2, 3, 4}}).front(); // not an image
        m_Textures.FlushPendingUploads();
        Logger::Log::Flush();
    }
    EXPECT_TRUE(Access::CachedAsFailed(m_Textures, texture));
    EXPECT_EQ(TestLog::CountLinesContaining(warnings, texture.ToString()), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(warnings, "its embedded image does not decode"), 1u);
    EXPECT_EQ(m_Textures.PendingMaterialTextureBindCount(), 0u);
}

// A material texture whose GUID has no asset record fails at the flush as a queued load does:
// one error line that names it and the reason, cached as no texture, and no bind left.
TEST_F(TextureUploadBatch, AMaterialTextureWithNoAssetRecordLogsOneErrorLine)
{
    const GUID texture = GUID::Generate();
    std::vector<std::string> errors;
    {
        TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
        Access::ScheduleUploadJob(m_Textures, texture);
        Access::WaitForUploadJobs(m_Textures);
        m_Textures.FlushPendingUploads();
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(errors, texture.ToString()), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(errors, "it has no asset record"), 1u);
    EXPECT_TRUE(Access::CachedAsFailed(m_Textures, texture));
    EXPECT_EQ(m_Textures.PendingMaterialTextureBindCount(), 0u);
}

} // namespace
#endif
