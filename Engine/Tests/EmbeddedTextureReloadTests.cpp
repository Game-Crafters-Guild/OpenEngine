// A model's embedded images decode on jobs and bind at the drain. A hot reload of a model hands its
// embedded images to the live materials that sample them before it releases the old textures: no
// material binds a texture when it is destroyed (issue #3153).

#include <gtest/gtest.h>

#include "Assets/ModelAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "SceneLoadTracker/SceneLoadTracker.h"
#include "SceneLoadTrackerTestAccess.h"
#include "TestDeviceHelper.h"
#include "TestTempDir.h"
#include "Types/Fnv1a.h"

#include <chrono>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{

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

constexpr StringId kAlbedo = HashStringId("albedoMap");

template <size_t N>
Vector<EmbeddedImage> OneImage(const uint8 (&png)[N])
{
    Vector<EmbeddedImage> images(1);
    images[0].MimeType = "image/png";
    images[0].Data = ByteBuffer::CopyOf(png);
    images[0].ContentHash = Hashing::Fnv1a64(images[0].Data.Data(), images[0].Data.Size());
    return images;
}

class EmbeddedTextureReload : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        // The engine runs the job system that decodes embedded images.
        m_Workspace = TestUtils::MakeUniqueTempDirectory("ge-embedded-reload");
        std::filesystem::create_directories(m_Workspace / "Assets");
        auto& engine = EngineCore::GetInstance();
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = m_Workspace.string();
        config.EnableEditor = false;
        ASSERT_TRUE(engine.Initialize(config));
        m_Services = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_Services->Initialize(m_Device.get()));
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        if (m_Services)
            m_Services->Shutdown();
        m_Device->Shutdown();
        EngineCore::GetInstance().Shutdown();
        std::error_code error;
        std::filesystem::remove_all(m_Workspace, error);
    }

    // A material of the model whose albedo is the model's embedded image 0, as model spawning
    // registers it.
    Material* SpawnMaterial(const Vector<EmbeddedImage>& images)
    {
        Material* mat = m_Services->Materials().RegisterMaterialFromDocument(GUID::Generate(), AlbedoDocument());
        if (mat)
            BindAlbedo(mat, images);
        return mat;
    }

    // Binds the material's albedo to image 0 of `images`.
    void BindAlbedo(Material* mat, const Vector<EmbeddedImage>& images)
    {
        Textures().ResolveEmbeddedTextures(mat, AlbedoDocument(), m_ModelGuid, images);
    }

    static MaterialDocument AlbedoDocument()
    {
        MaterialDocument doc = MaterialDocument::CreateDefaultPBR("EmbeddedReload");
        doc.textures["albedoMap"] = std::string(kEmbeddedTexturePrefix) + "0";
        return doc;
    }

    // Pumps the drain until the material has no bind left waiting on a job.
    void PumpUntilSettled(const Material& mat)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!Textures().IsMaterialTextureBindingComplete(mat.GetGuid()) &&
               std::chrono::steady_clock::now() < deadline)
        {
            Textures().FlushPendingUploads();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Textures().FlushPendingUploads();
        ASSERT_TRUE(Textures().IsMaterialTextureBindingComplete(mat.GetGuid())) << "the decode job never settled";
    }

    TextureService& Textures() { return m_Services->Textures(); }

    const GUID m_ModelGuid = GUID::Generate();
    std::filesystem::path m_Workspace;
    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
};

} // namespace

// The common hot reload rewrites the file with the same images: the material keeps sampling a
// live texture (the shared content survives the reload rather than being released under it).
TEST_F(EmbeddedTextureReload, AReloadWithTheSameImagesKeepsTheMaterialOnALiveTexture)
{
    Material* mat = SpawnMaterial(OneImage(kOrangePng));
    ASSERT_NE(mat, nullptr);
    PumpUntilSettled(*mat);
    const TextureHandle before = mat->GetTexture(kAlbedo);
    ASSERT_TRUE(m_Device->IsTextureAlive(before));

    Textures().ReloadEmbeddedForModel(m_ModelGuid, OneImage(kOrangePng));

    EXPECT_TRUE(m_Device->IsTextureAlive(mat->GetTexture(kAlbedo)))
        << "the reload released the texture its material still binds";
    EXPECT_EQ(mat->GetTexture(kAlbedo), before) << "content both payloads share is not uploaded again";
    EXPECT_FALSE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kAlbedo)));
}

// A reload with a new image rebinds the material to it, and only then releases the old one.
TEST_F(EmbeddedTextureReload, AReloadWithANewImageRebindsTheMaterialAndReleasesTheOldTexture)
{
    Material* mat = SpawnMaterial(OneImage(kOrangePng));
    ASSERT_NE(mat, nullptr);
    PumpUntilSettled(*mat);
    const TextureHandle before = mat->GetTexture(kAlbedo);
    ASSERT_TRUE(m_Device->IsTextureAlive(before));

    Textures().ReloadEmbeddedForModel(m_ModelGuid, OneImage(kGreenPng));
    PumpUntilSettled(*mat);

    const TextureHandle after = mat->GetTexture(kAlbedo);
    EXPECT_NE(after, before) << "the material still binds the previous payload's texture";
    EXPECT_TRUE(m_Device->IsTextureAlive(after));
    EXPECT_FALSE(m_Device->IsTextureAlive(before)) << "no material binds it any more, so it is released";
}

// A model that left memory has no images to hand over: its materials' slots await, and the old
// texture is released.
TEST_F(EmbeddedTextureReload, AModelThatLeftMemoryLeavesItsMaterialSlotsAwaited)
{
    Material* mat = SpawnMaterial(OneImage(kOrangePng));
    ASSERT_NE(mat, nullptr);
    PumpUntilSettled(*mat);
    const TextureHandle before = mat->GetTexture(kAlbedo);
    ASSERT_TRUE(m_Device->IsTextureAlive(before));

    Textures().ReloadEmbeddedForModel(m_ModelGuid, {});

    EXPECT_FALSE(mat->GetTexture(kAlbedo).IsValid());
    EXPECT_TRUE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kAlbedo)));
    EXPECT_FALSE(m_Device->IsTextureAlive(before));
}

// A slot left awaiting because its image had no bytes takes the image once a reload supplies it.
TEST_F(EmbeddedTextureReload, AnAwaitedSlotTakesTheImageAReloadSupplies)
{
    Vector<EmbeddedImage> empty(1);
    empty[0].MimeType = "image/png";
    Material* mat = SpawnMaterial(empty);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kAlbedo)));

    Textures().ReloadEmbeddedForModel(m_ModelGuid, OneImage(kOrangePng));
    PumpUntilSettled(*mat);

    EXPECT_TRUE(m_Device->IsTextureAlive(mat->GetTexture(kAlbedo)));
    EXPECT_FALSE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kAlbedo)));
}

// Binding a model's embedded image never decodes on the calling thread: the slot shows its default
// and counts as a pending bind until the drain binds what a job decoded (issue #5328).
TEST_F(EmbeddedTextureReload, AnEmbeddedImageDecodesOnAJobAndBindsAtTheDrain)
{
    Material* mat = SpawnMaterial(OneImage(kOrangePng));
    ASSERT_NE(mat, nullptr);
    EXPECT_TRUE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kAlbedo)))
        << "the image was decoded and uploaded on the binding thread";
    EXPECT_EQ(Textures().PendingMaterialTextureBindCount(), 1u);

    PumpUntilSettled(*mat);

    EXPECT_TRUE(m_Device->IsTextureAlive(mat->GetTexture(kAlbedo)));
    EXPECT_FALSE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kAlbedo)));
    EXPECT_EQ(Textures().PendingMaterialTextureBindCount(), 0u);
}

// A pending material texture bind holds the render.texture-upload row until the drain binds it.
TEST_F(EmbeddedTextureReload, APendingBindHoldsTheReadyQueryUntilTheDrainBindsIt)
{
    SceneLoadTracker tracker;
    Textures().ReportLoadingWork(tracker);
    Material* mat = SpawnMaterial(OneImage(kOrangePng));
    ASSERT_NE(mat, nullptr);
    SceneLoadTrackerTestAccess::Tick(tracker,std::chrono::steady_clock::now());
    EXPECT_FALSE(tracker.IsReadyToShow()) << "a pending bind does not hold the query";

    PumpUntilSettled(*mat);
    SceneLoadTrackerTestAccess::Tick(tracker,std::chrono::steady_clock::now());
    EXPECT_TRUE(tracker.IsReadyToShow()) << tracker.Describe();
}

// A later bind of a slot replaces an earlier one that still waits on its job: the earlier job can
// land last, and the slot takes the later image.
TEST_F(EmbeddedTextureReload, ALaterBindOfAWaitingSlotWinsOverTheEarlierOne)
{
    Material* mat = SpawnMaterial(OneImage(kOrangePng));
    ASSERT_NE(mat, nullptr);
    BindAlbedo(mat, OneImage(kGreenPng));
    EXPECT_EQ(Textures().PendingMaterialTextureBindCount(), 1u) << "the earlier bind still waits";
    PumpUntilSettled(*mat);

    Material* green = SpawnMaterial(OneImage(kGreenPng));
    ASSERT_NE(green, nullptr);
    PumpUntilSettled(*green);
    EXPECT_EQ(mat->GetTexture(kAlbedo), green->GetTexture(kAlbedo));
}

// A resident image bound to a slot that waits on another image's job replaces that bind: the job
// lands later and does not overwrite the slot.
TEST_F(EmbeddedTextureReload, AResidentImageBoundToAWaitingSlotStaysWhenTheWaitLands)
{
    Material* green = SpawnMaterial(OneImage(kGreenPng));
    ASSERT_NE(green, nullptr);
    PumpUntilSettled(*green);
    Material* mat = SpawnMaterial(OneImage(kOrangePng));
    ASSERT_NE(mat, nullptr);
    ASSERT_EQ(Textures().PendingMaterialTextureBindCount(), 1u);

    BindAlbedo(mat, OneImage(kGreenPng));
    EXPECT_EQ(Textures().PendingMaterialTextureBindCount(), 0u) << "the orange bind still waits";
    // This material waits on the same orange job, so its drain is the one that lands that job.
    Material* orange = SpawnMaterial(OneImage(kOrangePng));
    ASSERT_NE(orange, nullptr);
    PumpUntilSettled(*orange);
    EXPECT_EQ(mat->GetTexture(kAlbedo), green->GetTexture(kAlbedo));
}

// An image with no content hash is refused: with a hash of 0, all images of one size would share
// one texture.
TEST_F(EmbeddedTextureReload, AnImageWithNoContentHashIsRefused)
{
    Vector<EmbeddedImage> unhashed = OneImage(kOrangePng);
    unhashed[0].ContentHash = 0;
    Material* mat = SpawnMaterial(unhashed);
    ASSERT_NE(mat, nullptr);
    EXPECT_TRUE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kAlbedo)));
    EXPECT_EQ(Textures().PendingMaterialTextureBindCount(), 0u) << "the unhashed image was queued for upload";
}
