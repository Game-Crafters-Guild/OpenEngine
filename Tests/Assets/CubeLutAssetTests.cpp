// The .cube asset type: a .cube loads as a CubeLutAsset that keeps what the file declares and the
// importer's proposal of its input encoding, and a choice stored on the asset
// replaces the proposal until it is cleared.

#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/Engine.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/CubeLutAsset.h"
#include "Assets/CubeLutColorSettings.h"
#include "Assets/RuntimeAssetMetadata.h"

#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>

using namespace GameEngine;

namespace
{
// An identity 2^3 table whose title names ACEScct.
constexpr const char* kAcesCctLook = "TITLE \"Example log look (ACEScct)\"\n"
                                     "LUT_3D_SIZE 2\n"
                                     "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";

class CubeLutAssetTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Engine::Initialize moves the working directory into the workspace, and Windows refuses
        // to delete a directory that holds it.
        m_OriginalCwd = std::filesystem::current_path();
        m_Dir = TestUtils::MakeUniqueTempDirectory("cube_lut_asset_test");
        std::filesystem::create_directories(m_Dir);

        m_Engine = std::make_unique<EngineCore>();
        ApplicationConfig config;
        config.WorkspaceDirectory = m_Dir.string();
        config.AssetDirectory = m_Dir.string();
        config.AssetDatabaseFile = "AssetDatabase.assetdb";
        config.AssetDatabaseCacheDirectory = ".Cache/AssetDatabase";
        ASSERT_TRUE(m_Engine->Initialize(config));
        m_Assets = &m_Engine->GetAssetManager();

        m_Path = m_Dir / "look.cube";
        std::ofstream(m_Path, std::ios::binary) << kAcesCctLook;
        m_Assets->GetRegistry().RegisterAsset(m_Path);
        m_Guid = m_Assets->GetRegistry().GetAssetGUID(m_Path);
        ASSERT_FALSE(m_Guid.IsNull());
    }

    void TearDown() override
    {
        if (m_Engine)
        {
            m_Engine->Shutdown();
            m_Engine.reset();
        }
        std::error_code ec;
        std::filesystem::current_path(m_OriginalCwd, ec);
        std::filesystem::remove_all(m_Dir, ec);
    }

    std::filesystem::path m_OriginalCwd;
    std::filesystem::path m_Dir;
    std::filesystem::path m_Path;
    std::unique_ptr<EngineCore> m_Engine;
    AssetManager* m_Assets = nullptr;
    GUID m_Guid{};
};

} // namespace

TEST_F(CubeLutAssetTests, ACubeLoadsWithWhatItDeclaresAndTheProposal)
{
    const SharedPtr<Asset> asset = m_Assets->LoadAssetAsync(m_Guid, AssetLoadPriority::High).get();
    ASSERT_TRUE(asset) << "a .cube has a type factory";
    const auto* lut = dynamic_cast<const CubeLutAsset*>(asset.get());
    ASSERT_NE(lut, nullptr);
    EXPECT_EQ(lut->GetHeader().Title, "Example log look (ACEScct)");
    EXPECT_EQ(lut->GetHeader().Size3D, 2u);
    EXPECT_EQ(lut->GetHeader().Size1D, 0u);
    EXPECT_EQ(lut->GetProposal().InputEncoding, Rendering::CubeLutInputEncoding::ACEScct);
}

TEST_F(CubeLutAssetTests, AChoiceOnTheAssetReplacesTheProposalUntilCleared)
{
    Rendering::CubeLutColorProposal proposal{};
    proposal.InputEncoding = Rendering::CubeLutInputEncoding::ACEScct;
    AssetRegistry& registry = m_Assets->GetRegistry();

    CubeLutColorSettings settings = ResolveCubeLutColorSettings(registry, m_Path, proposal);
    EXPECT_EQ(settings.InputEncoding, Rendering::CubeLutInputEncoding::ACEScct);
    EXPECT_FALSE(settings.InputEncodingChosen);

    ASSERT_TRUE(registry.SetMetaValue(m_Path, kCubeLutInputEncodingMetaKey, "linear"));
    settings = ResolveCubeLutColorSettings(registry, m_Path, proposal);
    EXPECT_EQ(settings.InputEncoding, Rendering::CubeLutInputEncoding::Linear);
    EXPECT_TRUE(settings.InputEncodingChosen);

    // Choosing the proposal again stores nothing (the inspector writes an empty value).
    ASSERT_TRUE(registry.SetMetaValue(m_Path, kCubeLutInputEncodingMetaKey, ""));
    settings = ResolveCubeLutColorSettings(registry, m_Path, proposal);
    EXPECT_EQ(settings.InputEncoding, Rendering::CubeLutInputEncoding::ACEScct);
    EXPECT_FALSE(settings.InputEncodingChosen);
}
