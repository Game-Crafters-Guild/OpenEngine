// The atlas tile resample (ResampleFieldIntoTile) against a per-texel, per-channel reference:
// every output byte equal, on one thread and across the pool.

#include "TerrainFieldUploader.h" // TerrainECS/Source
#include "TerrainColorMap/TerrainColorMapCompose.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/Interpolation.h"
#include "TerrainECS/TerrainColorMap/TerrainColorMap.h"
#include "TerrainECS/TerrainGrass/TerrainGrassField.h"
#include "Types/Color.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;

namespace
{
constexpr uint32 kSerialTileRes = 67;   // below the band threshold: one call on this thread
constexpr uint32 kBandedTileRes = 401;  // above it: row bands across the pool
constexpr uint32 kPoolThreads = 4;
constexpr uint32 kTilesX = 5;      // the coarse field covers a 5 x 3 grid of tiles
constexpr uint32 kTilesZ = 3;

TerrainFieldData RandomField(uint32 width, uint32 height, uint32 channels, uint32 seed)
{
    TerrainFieldData field;
    field.Width = width;
    field.Height = height;
    field.Initialized = true;
    field.Pixels.resize(static_cast<size_t>(width) * height * channels);
    std::mt19937 random(seed);
    std::uniform_int_distribution<uint32> byte(0, 255);
    for (uint8& value : field.Pixels)
        value = static_cast<uint8>(byte(random));
    return field;
}

// One channel of one sample, decoded with Detail::SRGBChannelToLinear on every read.
uint8 ReferenceSample(const TerrainFieldUploadDesc& desc, const TerrainFieldData& field, float32 u, float32 v,
                      uint32 channel)
{
    if (!field.IsActive()) return desc.Neutral;
    const float32 x = Math::Clamp01(u) * (field.Width - 1);
    const float32 z = Math::Clamp01(v) * (field.Height - 1);
    const uint32 x0 = static_cast<uint32>(x), z0 = static_cast<uint32>(z);
    const uint32 x1 = std::min(x0 + 1, field.Width - 1), z1 = std::min(z0 + 1, field.Height - 1);
    const bool srgb = desc.Kind == TerrainField::Color && channel < 3;
    const auto at = [&](uint32 sx, uint32 sz)
    {
        const uint8 byte = field.Pixels[(static_cast<size_t>(sz) * field.Width + sx) * desc.Channels + channel];
        return srgb ? Detail::SRGBChannelToLinear(byte / 255.0f) : static_cast<float32>(byte);
    };
    const float32 a = std::lerp(at(x0, z0), at(x1, z0), x - x0);
    const float32 b = std::lerp(at(x0, z1), at(x1, z1), x - x0);
    const float32 value = std::lerp(a, b, z - z0);
    return srgb ? EncodeLinearToSrgbByte(value) : static_cast<uint8>(std::lround(value));
}

// A tile cut from a field covering tilesX x tilesZ tiles (an uncomposed tile reading the coarse map).
std::vector<uint8> ReferenceCoarseTile(const TerrainFieldUploadDesc& desc, const TerrainFieldData& coarse,
                                       int32 tileX, int32 tileZ, uint32 tilesX, uint32 tilesZ, uint32 tileRes)
{
    std::vector<uint8> out(static_cast<size_t>(tileRes) * tileRes * desc.Channels);
    for (uint32 z = 0; z < tileRes; ++z)
        for (uint32 x = 0; x < tileRes; ++x)
            for (uint32 channel = 0; channel < desc.Channels; ++channel)
                out[(static_cast<size_t>(z) * tileRes + x) * desc.Channels + channel] = ReferenceSample(
                    desc, coarse, (tileX + static_cast<float32>(x) / (tileRes - 1)) / tilesX,
                    (tileZ + static_cast<float32>(z) / (tileRes - 1)) / tilesZ, channel);
    return out;
}

// A tile's own field stretched over the whole tile.
std::vector<uint8> ReferenceOwnTile(const TerrainFieldUploadDesc& desc, const TerrainFieldData& field, uint32 tileRes)
{
    std::vector<uint8> out(static_cast<size_t>(tileRes) * tileRes * desc.Channels);
    for (uint32 z = 0; z < tileRes; ++z)
        for (uint32 x = 0; x < tileRes; ++x)
            for (uint32 channel = 0; channel < desc.Channels; ++channel)
                out[(static_cast<size_t>(z) * tileRes + x) * desc.Channels + channel] = ReferenceSample(
                    desc, field, static_cast<float32>(x) / (tileRes - 1), static_cast<float32>(z) / (tileRes - 1),
                    channel);
    return out;
}

size_t FirstDifference(const std::vector<uint8>& expected, const std::vector<uint8>& actual)
{
    const auto [expectedAt, actualAt] = std::mismatch(expected.begin(), expected.end(), actual.begin(), actual.end());
    return static_cast<size_t>(expectedAt - expected.begin());
}

void ExpectEveryTileByteEqual(const TerrainFieldUploadDesc& desc, const TerrainFieldData& coarse,
                              const TerrainFieldData& own, JobSystem::WorkStealingThreadPool& pool)
{
    // Tile (4, 2) is the far corner of the grid: its last row and column clamp to the field's edge.
    const TileCoord tiles[] = {{0, 0}, {2, 1}, {4, 2}};
    std::vector<uint8> actual;
    for (const uint32 tileRes : {kSerialTileRes, kBandedTileRes})
    {
        for (const auto& tile : tiles)
        {
            const std::vector<uint8> expected = ReferenceCoarseTile(desc, coarse, tile.X, tile.Z, kTilesX, kTilesZ, tileRes);
            const TileResampleWindow window{{static_cast<float32>(tile.X), static_cast<float32>(tile.Z)},
                                            {static_cast<float32>(kTilesX), static_cast<float32>(kTilesZ)}};
            for (JobSystem::WorkStealingThreadPool* runPool : {static_cast<JobSystem::WorkStealingThreadPool*>(nullptr), &pool})
            {
                actual.clear(); // a row the call does not write reads 0, not the last run's bytes
                ResampleFieldIntoTile(desc, coarse, window, tileRes, runPool, actual);
                ASSERT_EQ(actual.size(), expected.size());
                const size_t at = FirstDifference(expected, actual);
                EXPECT_EQ(at, expected.size()) << "coarse tile (" << tile.X << ", " << tile.Z << ") res " << tileRes
                                               << (runPool ? " banded" : " serial") << ": first byte that differs " << at;
            }
        }
        const std::vector<uint8> expected = ReferenceOwnTile(desc, own, tileRes);
        for (JobSystem::WorkStealingThreadPool* runPool : {static_cast<JobSystem::WorkStealingThreadPool*>(nullptr), &pool})
        {
            actual.clear();
            ResampleFieldIntoTile(desc, own, TileResampleWindow{}, tileRes, runPool, actual);
            const size_t at = FirstDifference(expected, actual);
            EXPECT_EQ(at, expected.size()) << "own field res " << tileRes << (runPool ? " banded" : " serial")
                                           << ": first byte that differs " << at;
        }
    }
}
} // namespace

TEST(TerrainFieldResample, ColorTilesMatchThePerChannelReferenceByteForByte)
{
    const TerrainFieldUploadDesc& desc = ColorMapUploadDesc();
    JobSystem::WorkStealingThreadPool pool(kPoolThreads);
    ExpectEveryTileByteEqual(desc, RandomField(37, 23, desc.Channels, 11u), RandomField(29, 31, desc.Channels, 12u), pool);
}

TEST(TerrainFieldResample, GrassTilesMatchThePerChannelReferenceByteForByte)
{
    const TerrainFieldUploadDesc& desc = GrassFieldUploadDesc();
    JobSystem::WorkStealingThreadPool pool(kPoolThreads);
    ExpectEveryTileByteEqual(desc, RandomField(37, 23, desc.Channels, 21u), RandomField(29, 31, desc.Channels, 22u), pool);
}

TEST(TerrainFieldResample, AnInactiveSourceWritesTheNeutralByte)
{
    for (const TerrainFieldUploadDesc* desc : {&ColorMapUploadDesc(), &GrassFieldUploadDesc()})
    {
        TerrainFieldData empty;
        empty.Width = empty.Height = 9;
        empty.Initialized = true;
        std::vector<uint8> actual(3, 7);
        ResampleFieldIntoTile(*desc, empty, TileResampleWindow{}, kSerialTileRes, nullptr, actual);
        ASSERT_EQ(actual.size(), static_cast<size_t>(kSerialTileRes) * kSerialTileRes * desc->Channels);
        EXPECT_TRUE(std::all_of(actual.begin(), actual.end(), [&](uint8 value) { return value == desc->Neutral; }));
    }
}

TEST(TerrainFieldResample, TheDecodeTableHoldsTheEngineConversionOfEveryByte)
{
    const std::array<float32, 256>& table = SrgbByteToLinearTable();
    for (uint32 byte = 0; byte < 256; ++byte)
        EXPECT_EQ(table[byte], Detail::SRGBChannelToLinear(static_cast<float32>(byte) / 255.0f)) << "byte " << byte;
}
