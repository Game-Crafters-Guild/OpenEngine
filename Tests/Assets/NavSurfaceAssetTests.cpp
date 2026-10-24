// The navigation surface data asset (.navsurface): the default parser set dispatches the extension
// to it, it loads a file the surface wrote, and it refuses a file of another format version.

#include <gtest/gtest.h>

#include "AssetCore/AssetTypes.h"
#include "Assets/NavSurfaceAsset.h"
#include "Assets/ParserRegistry.h"
#include "Pathfinding/NavSurfaceFile.h"

#include "TestTempDir.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace GameEngine;

namespace
{

const GUID kSurfaceDataGuid("3c5e7a91-24b6-4d8f-9e10-a2b4c6d8e0f2");

std::filesystem::path WriteFile(const char* directory, const std::vector<uint8>& bytes)
{
    const std::filesystem::path dir = TestUtils::MakeUniqueTempDirectory(directory);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "Level.navsurface";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::vector<uint8> SurfaceBytes()
{
    Pathfinding::NavSurfaceData data;
    data.BakeSettingsHash = 42;
    data.Areas = Pathfinding::DefaultNavSurfaceAreas();
    data.FlagNames = Pathfinding::DefaultNavSurfaceFlagNames();
    data.Profiles.emplace_back().Charts.emplace_back();
    std::vector<uint8> bytes;
    EXPECT_TRUE(Pathfinding::WriteNavSurface(data, bytes));
    return bytes;
}

} // namespace

TEST(NavSurfaceAsset, TheDefaultParserSetDispatchesTheExtension)
{
    EXPECT_EQ(GetAssetTypeFromExtension(".navsurface"), AssetType::NavigationSurfaceData);
    ParserRegistry registry;
    ASSERT_TRUE(registry.Initialize());
    std::shared_ptr<AssetParser> parser = registry.FindParser("Level.navsurface");
    ASSERT_NE(parser, nullptr) << ".navsurface reached no parser at all";
    EXPECT_EQ(parser->GetName(), "NavSurfaceAssetParser");
    EXPECT_EQ(parser->GetAssetType(), AssetType::NavigationSurfaceData);
}

TEST(NavSurfaceAsset, LoadsTheFileASurfaceWrote)
{
    NavSurfaceAsset asset(kSurfaceDataGuid, WriteFile("ge_navsurface_load", SurfaceBytes()));
    ASSERT_TRUE(asset.Load());
    EXPECT_TRUE(asset.IsLoaded());
    ASSERT_NE(asset.GetData(), nullptr);
    EXPECT_EQ(asset.GetData()->BakeSettingsHash, 42u);
    EXPECT_EQ(asset.GetData()->Profiles.size(), 1u);
}

// The asset reads one format version: a file of any other does not load, and the surface that names
// it bakes instead.
TEST(NavSurfaceAsset, RefusesAnotherFormatVersion)
{
    std::vector<uint8> bytes = SurfaceBytes();
    const uint32 otherVersion = Pathfinding::kNavSurfaceFormatVersion + 1;
    std::memcpy(bytes.data() + 4, &otherVersion, sizeof(otherVersion));
    NavSurfaceAsset asset(kSurfaceDataGuid, WriteFile("ge_navsurface_version", bytes));
    EXPECT_FALSE(asset.Load());
    EXPECT_EQ(asset.GetState(), AssetState::Failed);
    EXPECT_EQ(asset.GetData(), nullptr);
}
