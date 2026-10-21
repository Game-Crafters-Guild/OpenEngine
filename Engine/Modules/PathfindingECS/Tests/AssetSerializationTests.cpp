#include "Assets/NavGridAsset.h"
#include "AssetCore/GUID.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class AssetSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_TempDir = std::filesystem::temp_directory_path() / "ge_nav_test";
        std::filesystem::create_directories(m_TempDir);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
    }

    std::filesystem::path m_TempDir;

    // Helper: write raw bytes to a file
    static void WriteBytes(const std::filesystem::path& path, const void* data, size_t size)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    // Helper: write a string to a file
    static void WriteString(const std::filesystem::path& path, const std::string& content)
    {
        std::ofstream file(path, std::ios::binary);
        file << content;
    }
};

// ===========================================================================
// NavGridAsset Tests
// ===========================================================================

TEST_F(AssetSerializationTest, NavGrid_SaveAndLoadRoundTrip)
{
    GridSettings settings;
    settings.Type = GridType::Hex;
    settings.CellSize = 2.5f;
    settings.OriginX = 10.0f;
    settings.OriginY = -5.0f;
    settings.OriginZ = 3.0f;
    settings.Width = 4;
    settings.Depth = 3;
    settings.MaxSlope = 30.0f;
    settings.MaxStepHeight = 0.8f;

    const uint32 cellCount = settings.Width * settings.Depth;
    std::vector<float32> costs(cellCount);
    std::vector<uint8> blocked(cellCount);
    for (uint32 i = 0; i < cellCount; ++i)
    {
        costs[i] = static_cast<float32>(i) * 1.5f;
        blocked[i] = (i % 3 == 0) ? 1 : 0;
    }

    auto filePath = m_TempDir / "roundtrip.navgrid";
    ASSERT_TRUE(NavGridAsset::SaveToFile(filePath, settings, costs.data(), blocked.data(), cellCount));

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, filePath);
    ASSERT_TRUE(asset.Load());

    const auto& loaded = asset.GetGridSettings();
    EXPECT_EQ(static_cast<uint8>(loaded.Type), static_cast<uint8>(GridType::Hex));
    EXPECT_FLOAT_EQ(loaded.CellSize, 2.5f);
    EXPECT_FLOAT_EQ(loaded.OriginX, 10.0f);
    EXPECT_FLOAT_EQ(loaded.OriginY, -5.0f);
    EXPECT_FLOAT_EQ(loaded.OriginZ, 3.0f);
    EXPECT_EQ(loaded.Width, 4u);
    EXPECT_EQ(loaded.Depth, 3u);
    EXPECT_FLOAT_EQ(loaded.MaxSlope, 30.0f);
    EXPECT_FLOAT_EQ(loaded.MaxStepHeight, 0.8f);

    EXPECT_EQ(asset.GetCosts().size(), cellCount);
    EXPECT_EQ(asset.GetBlocked().size(), cellCount);
    for (uint32 i = 0; i < cellCount; ++i)
    {
        EXPECT_FLOAT_EQ(asset.GetCosts()[i], costs[i]) << "cost mismatch at " << i;
        EXPECT_EQ(asset.GetBlocked()[i], blocked[i]) << "blocked mismatch at " << i;
    }
}

TEST_F(AssetSerializationTest, NavGrid_TruncatedFileFailsGracefully)
{
    // Write a valid header but truncate mid-costs-array
    GridSettings settings;
    settings.Width = 8;
    settings.Depth = 8;
    const uint32 cellCount = settings.Width * settings.Depth;
    std::vector<float32> costs(cellCount, 1.0f);
    std::vector<uint8> blocked(cellCount, 0);

    auto validPath = m_TempDir / "valid_tmp.navgrid";
    ASSERT_TRUE(NavGridAsset::SaveToFile(validPath, settings, costs.data(), blocked.data(), cellCount));

    // Read back and truncate
    std::ifstream validFile(validPath, std::ios::binary | std::ios::ate);
    auto fullSize = validFile.tellg();
    validFile.seekg(0);
    std::vector<char> fullData(static_cast<size_t>(fullSize));
    validFile.read(fullData.data(), fullSize);
    validFile.close();

    // Truncate partway through the costs array (header is 41 bytes, costs start there)
    size_t truncatedSize = 41 + 8; // only 8 bytes of costs instead of 64*4=256
    auto truncPath = m_TempDir / "truncated.navgrid";
    WriteBytes(truncPath, fullData.data(), truncatedSize);

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, truncPath);
    EXPECT_FALSE(asset.Load());
}

TEST_F(AssetSerializationTest, NavGrid_WrongMagicFails)
{
    // Write a file with wrong magic but otherwise plausible data
    auto filePath = m_TempDir / "wrongmagic.navgrid";
    std::ofstream file(filePath, std::ios::binary);
    file.write("XXXX", 4); // wrong magic
    uint32 version = 1;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    // Pad to minimum header size
    std::vector<uint8> padding(128, 0);
    file.write(reinterpret_cast<const char*>(padding.data()), padding.size());
    file.close();

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, filePath);
    EXPECT_FALSE(asset.Load());
}

TEST_F(AssetSerializationTest, NavGrid_VersionMismatchFails)
{
    auto filePath = m_TempDir / "badversion.navgrid";
    std::ofstream file(filePath, std::ios::binary);
    file.write("NGRD", 4); // correct magic
    uint32 version = 99;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    std::vector<uint8> padding(128, 0);
    file.write(reinterpret_cast<const char*>(padding.data()), padding.size());
    file.close();

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, filePath);
    EXPECT_FALSE(asset.Load());
}

TEST_F(AssetSerializationTest, NavGrid_ZeroDimensionsFails)
{
    // Build a header with Width=0 by manually writing the binary format
    auto filePath = m_TempDir / "zerodim.navgrid";
    std::ofstream file(filePath, std::ios::binary);
    file.write("NGRD", 4);
    uint32 version = 1;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    uint8 gridType = 0;
    file.write(reinterpret_cast<const char*>(&gridType), 1);
    float32 cellSize = 1.0f;
    file.write(reinterpret_cast<const char*>(&cellSize), sizeof(cellSize));
    float32 originX = 0.0f, originY = 0.0f, originZ = 0.0f;
    file.write(reinterpret_cast<const char*>(&originX), sizeof(originX));
    file.write(reinterpret_cast<const char*>(&originY), sizeof(originY));
    file.write(reinterpret_cast<const char*>(&originZ), sizeof(originZ));
    uint32 width = 0, depth = 10;
    file.write(reinterpret_cast<const char*>(&width), sizeof(width));
    file.write(reinterpret_cast<const char*>(&depth), sizeof(depth));
    float32 maxSlope = 45.0f, maxStep = 0.4f;
    file.write(reinterpret_cast<const char*>(&maxSlope), sizeof(maxSlope));
    file.write(reinterpret_cast<const char*>(&maxStep), sizeof(maxStep));
    file.close();

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, filePath);
    EXPECT_FALSE(asset.Load());
}

TEST_F(AssetSerializationTest, NavGrid_EmptyFileFailsGracefully)
{
    auto filePath = m_TempDir / "empty.navgrid";
    std::ofstream file(filePath, std::ios::binary);
    file.close();

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, filePath);
    EXPECT_FALSE(asset.Load());
}
