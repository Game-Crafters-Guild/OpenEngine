// The AI state-tree program asset type (.scribe): the extension reaches the type, the core registrations
// create the asset that loads the file, and a reload publishes the new program while a bad edit keeps
// the last good program.
#include <gtest/gtest.h>

#include "AIECS/Assets/AiStateTree/AiStateTreeAsset.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeData.h"
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

const GUID kProgramGuid("6e0d4a93-1c58-4b27-9f3e-a5b8c2d71f40");

constexpr const char* kOneState = R"({ "root": { "id": 1, "name": "Guard" } })";
constexpr const char* kTwoStates = R"({ "root": { "id": 1, "name": "Guard", "children": [ { "id": 2 } ] } })";

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

size_t StateCount(const AIECS::AiStateTreeAsset& asset)
{
    const std::shared_ptr<const AIECS::AiStateTreeData> data = asset.GetData();
    return data ? data->Program.GetStates().size() : 0;
}

} // namespace

TEST(AiStateTreeAssetType, TheExtensionReachesTheType)
{
    EXPECT_EQ(GetAssetTypeFromExtension(GetCompoundExtensionFromPath("AI/Guard.scribe")), AssetType::AiStateTree);
}

TEST(AiStateTreeAssetType, TheCoreFactoryLoadsTheFileAndAReloadPublishesTheEdit)
{
    const std::filesystem::path dir = TestUtils::MakeUniqueTempDirectory("ge_ai_state_tree_asset");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "Guard.scribe";
    WriteText(path, kOneState);

    AssetTypeRegistry registry;
    RegisterCoreAssetTypes(registry);
    AssetMetadata metadata;
    metadata.Guid = kProgramGuid;
    metadata.Type = AssetType::AiStateTree;
    metadata.Path = path;
    const auto asset = std::dynamic_pointer_cast<AIECS::AiStateTreeAsset>(registry.CreateAsset(metadata));
    ASSERT_NE(asset, nullptr);
    ASSERT_TRUE(asset->Load());
    EXPECT_EQ(StateCount(*asset), 1u);

    WriteText(path, kTwoStates);
    EXPECT_EQ(asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_EQ(StateCount(*asset), 2u);

    WriteText(path, R"({ "root": { "id": 1, "policy": "best" } })");
    EXPECT_EQ(asset->Reload(), ReloadOutcome::Failed);
    EXPECT_EQ(StateCount(*asset), 2u);
}
