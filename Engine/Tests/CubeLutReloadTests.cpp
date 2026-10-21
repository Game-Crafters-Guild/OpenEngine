// A volume's Cube LUT renders with its asset's input encoding, and a change of that encoding reaches
// the frame: the asset's reload event drops the uploaded tables at the next BeginWorldDrawFrame, and
// the next lookup resolves the asset's new choice.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/RuntimeAssetMetadata.h"
#include "AssetCore/AssetEvents.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Utils/CubeLutColorDomain.h"
#include "TestDeviceHelper.h"
#include "TestTempDir.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

// An identity 2^3 table whose title names ACEScct, so its proposal is not Linear (the value an
// unwritten push member would read as).
constexpr const char* kAcesCctLook = "TITLE \"ACEScct look\"\n"
                                     "LUT_3D_SIZE 2\n"
                                     "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";

class CubeLutReload : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Workspace = TestUtils::MakeUniqueTempDirectory("ge-cube-lut-reload");
        fs::create_directories(m_Workspace / "Assets");
        m_PreviousDirectory = fs::current_path();
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
        // Initialized after the engine, so it subscribes to the asset events.
        m_Services = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_Services->Initialize(m_Device.get()));
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_Services)
            m_Services->Shutdown();
        m_Services.reset();
        m_Device->Shutdown();
        m_Device.reset();
        EngineCore::GetInstance().Shutdown();
        fs::current_path(m_PreviousDirectory);
        std::error_code error;
        fs::remove_all(m_Workspace, error);
    }

    int32_t PushedEncoding(const GUID& lut)
    {
        PostProcessSettings settings{};
        settings.LutIntensity = 1.0f;
        std::memcpy(settings.LutAssetGuidWords, lut.GetData().data(), sizeof(settings.LutAssetGuidWords));
        CubeLutGpuBindingState binding{};
        EXPECT_TRUE(m_Services->Textures().TryGetCubeLutGpuBinding(settings, binding));
        EXPECT_NE(binding.Flags, 0) << "the table uploaded";
        return binding.InputEncoding;
    }

    fs::path m_Workspace;
    fs::path m_PreviousDirectory;
    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
};

} // namespace

TEST_F(CubeLutReload, AnEncodingChoiceReachesTheFrameAfterTheAssetReloads)
{
    const fs::path path = m_Workspace / "Assets" / "look.cube";
    std::ofstream(path, std::ios::binary) << kAcesCctLook;
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    assets.GetRegistry().RegisterAsset(path);
    const GUID lut = assets.GetRegistry().GetAssetGUID(path);
    ASSERT_FALSE(lut.IsNull());

    EXPECT_EQ(PushedEncoding(lut), static_cast<int32_t>(CubeLutInputEncoding::ACEScct)) << "the proposal";

    ASSERT_TRUE(assets.GetRegistry().SetMetaValue(path, kCubeLutInputEncodingMetaKey, "linear"));
    EXPECT_EQ(PushedEncoding(lut), static_cast<int32_t>(CubeLutInputEncoding::ACEScct))
        << "the uploaded tables keep their encoding until the asset reloads";

    assets.GetEventDispatcher().DispatchEvent(AssetEvent{AssetEventType::AssetReloaded, lut, AssetType::CubeLut, path.string()});
    m_Services->BeginWorldDrawFrame();
    EXPECT_EQ(PushedEncoding(lut), static_cast<int32_t>(CubeLutInputEncoding::Linear)) << "the choice, after the reload";
}
