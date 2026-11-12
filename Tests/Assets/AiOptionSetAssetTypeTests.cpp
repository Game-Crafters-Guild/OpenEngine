// The AI option-set asset type (.aioptions): the extension reaches the type, the core registrations
// create the asset that loads the file, and a reload publishes the new data while a bad edit keeps
// the last good data.
#include <gtest/gtest.h>

#include "AIECS/Assets/AiOptionSetAsset.h"
#include "AIECS/Assets/AiOptionSetData.h"
#include "AssetCore/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/CoreAssetRegistrations.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace GameEngine;

namespace
{

const GUID kOptionSetGuid("2b7c9e41-5d36-4a8f-b1e0-7c3d9f2a6e15");

constexpr const char* kNormalAt24 = R"({ "presets": [ { "name": "Normal", "cadenceTicks": 24 } ] })";
constexpr const char* kNormalAt13 = R"({ "presets": [ { "name": "Normal", "cadenceTicks": 13 } ] })";

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

uint16 NormalCadence(const AIECS::AiOptionSetAsset& asset)
{
    const std::shared_ptr<const AIECS::AiOptionSetData> data = asset.GetData();
    const AIECS::AiDifficultyPreset* preset = data ? data->FindPreset("Normal") : nullptr;
    return preset ? preset->CadenceTicks : 0;
}

} // namespace

TEST(AiOptionSetAssetType, TheExtensionReachesTheType)
{
    EXPECT_EQ(GetAssetTypeFromExtension(GetCompoundExtensionFromPath("AI/Commander.aioptions")),
              AssetType::AiOptionSet);
}

TEST(AiOptionSetAssetType, TheCoreFactoryLoadsTheFileAndAReloadPublishesTheEdit)
{
    const std::filesystem::path dir = TestUtils::MakeUniqueTempDirectory("ge_ai_option_set_asset");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "Commander.aioptions";
    WriteText(path, kNormalAt24);

    AssetTypeRegistry registry;
    RegisterCoreAssetTypes(registry);
    AssetMetadata metadata;
    metadata.Guid = kOptionSetGuid;
    metadata.Type = AssetType::AiOptionSet;
    metadata.Path = path;
    const auto asset = std::dynamic_pointer_cast<AIECS::AiOptionSetAsset>(registry.CreateAsset(metadata));
    ASSERT_NE(asset, nullptr);
    ASSERT_TRUE(asset->Load());
    EXPECT_EQ(NormalCadence(*asset), 24u);

    WriteText(path, kNormalAt13);
    EXPECT_EQ(asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_EQ(NormalCadence(*asset), 13u);

    WriteText(path, R"({ "presets": [ { "name": "Normal", "cadence": 5 } ] })");
    EXPECT_EQ(asset->Reload(), ReloadOutcome::Failed);
    EXPECT_EQ(NormalCadence(*asset), 13u);
}
