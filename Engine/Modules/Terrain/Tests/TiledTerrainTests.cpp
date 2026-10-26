#include "TerrainECS/TerrainService.h"
#include "Terrain/TerrainTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <utility>

using namespace GameEngine;
using namespace GameEngine::Terrain;
using namespace GameEngine::TerrainECS;

class TiledTerrainFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TerrainService::Initialize();
    }

    void TearDown() override
    {
        TerrainService::Shutdown();
    }
};

// ---------------------------------------------------------------------------
// TiledTerrainConfig derivation
// ---------------------------------------------------------------------------

TEST_F(TiledTerrainFixture, CreateTiledTerrain_ComputesTileConfig)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 256.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    ASSERT_NE(handle, (TiledTerrainHandle{}));

    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    EXPECT_GT(data->Config.TileWorldSize, 0.0f);
    EXPECT_GT(data->Config.TilesPerAxisX, 0u);
    EXPECT_GT(data->Config.TilesPerAxisZ, 0u);
    EXPECT_GT(data->Config.TileConfig.LODLevels, 0u);
    EXPECT_GT(data->Config.TileConfig.HeightmapWidth, 3u);
    EXPECT_GT(data->Config.StreamingRadius, 0.0f);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, CreateTiledTerrain_MultiTile)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 8192.0f;
    config.WorldSizeZ = 8192.0f;
    config.HeightScale = 256.0f;
    config.SamplesPerMeter = 2.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    EXPECT_GE(data->Config.TilesPerAxisX, 2u);
    EXPECT_GE(data->Config.TilesPerAxisZ, 2u);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// ---------------------------------------------------------------------------
// Tile loading / unloading
// ---------------------------------------------------------------------------

TEST_F(TiledTerrainFixture, LoadTile_CreatesHeightfield)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    TileCoord coord{0, 0};
    EXPECT_FALSE(TerrainService::Get().IsTileLoaded(handle, coord));

    auto* tile = TerrainService::Get().LoadTile(handle, coord);
    ASSERT_NE(tile, nullptr);
    EXPECT_TRUE(TerrainService::Get().IsTileLoaded(handle, coord));

    EXPECT_FALSE(tile->Heightfield.IsEmpty());
    EXPECT_TRUE(tile->HeightfieldDirty);
    EXPECT_GT(tile->SplatmapWidth, 0u);
    EXPECT_FALSE(tile->Splatmap.empty());

    // Tile world origin should match its grid position.
    EXPECT_FLOAT_EQ(tile->WorldOriginX, data->WorldOriginX);
    EXPECT_FLOAT_EQ(tile->WorldOriginZ, data->WorldOriginZ);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, RevisionTracksTileMutations)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->Revision, 0u);

    auto* tile = TerrainService::Get().LoadTile(handle, {0, 0});
    ASSERT_NE(tile, nullptr);
    const uint64 afterLoad = data->Revision;
    EXPECT_GT(afterLoad, 0u);

    HeightfieldData replacement;
    replacement.Resize(tile->Config.HeightmapWidth, tile->Config.HeightmapHeight, 1.0f);
    TerrainService::Get().SetTileHeightfield(handle, {0, 0}, std::move(replacement), 1.0f, 1.0f);
    EXPECT_GT(data->Revision, afterLoad);
    const uint64 afterHeightfield = data->Revision;

    TerrainService::Get().UnloadTile(handle, {0, 0});
    EXPECT_GT(data->Revision, afterHeightfield);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, LoadTile_DifferentCoords_DifferentOrigins)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 8192.0f;
    config.WorldSizeZ = 8192.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 2.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    ASSERT_GE(data->Config.TilesPerAxisX, 2u);

    auto* tile00 = TerrainService::Get().LoadTile(handle, {0, 0});
    auto* tile10 = TerrainService::Get().LoadTile(handle, {1, 0});
    ASSERT_NE(tile00, nullptr);
    ASSERT_NE(tile10, nullptr);

    // Tile (1,0) should be offset by one tile world size in X.
    EXPECT_FLOAT_EQ(tile10->WorldOriginX, tile00->WorldOriginX + data->Config.TileWorldSize);
    EXPECT_FLOAT_EQ(tile10->WorldOriginZ, tile00->WorldOriginZ);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, UnloadTile_RemovesTile)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    TerrainService::Get().LoadTile(handle, {0, 0});
    EXPECT_TRUE(TerrainService::Get().IsTileLoaded(handle, {0, 0}));

    TerrainService::Get().UnloadTile(handle, {0, 0});
    EXPECT_FALSE(TerrainService::Get().IsTileLoaded(handle, {0, 0}));

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, LoadTile_OutOfBounds_ReturnsNull)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    EXPECT_EQ(TerrainService::Get().LoadTile(handle, {-1, 0}), nullptr);
    EXPECT_EQ(TerrainService::Get().LoadTile(handle, {static_cast<int32>(data->Config.TilesPerAxisX), 0}), nullptr);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, LoadTile_DuplicateLoad_ReturnsSamePointer)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* tile1 = TerrainService::Get().LoadTile(handle, {0, 0});
    auto* tile2 = TerrainService::Get().LoadTile(handle, {0, 0});

    EXPECT_EQ(tile1, tile2) << "Loading same tile twice should return the same pointer";

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// Verify that adjacent tiles produce identical heightfield values at their
// shared boundary. World-space noise is deterministic per world position,
// so the last column of tile (0,0) must match the first column of tile (1,0).
TEST_F(TiledTerrainFixture, AdjacentTiles_HeightfieldBoundaryMatch)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 2048.0f;
    config.WorldSizeZ = 2048.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 2.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    ASSERT_GE(data->Config.TilesPerAxisX, 2u);

    auto* tile00 = TerrainService::Get().LoadTile(handle, {0, 0});
    auto* tile10 = TerrainService::Get().LoadTile(handle, {1, 0});
    ASSERT_NE(tile00, nullptr);
    ASSERT_NE(tile10, nullptr);

    const uint32 w = tile00->Heightfield.GetWidth();
    const uint32 h = tile00->Heightfield.GetHeight();

    // Right edge of tile(0,0) == left edge of tile(1,0)
    uint32 mismatches = 0;
    for (uint32 z = 0; z < h; ++z)
    {
        const float32 a = tile00->Heightfield.GetSample(w - 1, z);
        const float32 b = tile10->Heightfield.GetSample(0, z);
        if (std::abs(a - b) > 1e-6f)
        {
            ++mismatches;
            if (mismatches <= 3)
                ADD_FAILURE() << "Height mismatch at z=" << z << ": tile(0,0)[" << (w-1) << "]="
                              << a << " vs tile(1,0)[0]=" << b;
        }
    }
    EXPECT_EQ(mismatches, 0u) << "Found " << mismatches << " boundary height mismatches";

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// Verify that adjacent tiles also match on the Z boundary.
TEST_F(TiledTerrainFixture, AdjacentTiles_HeightfieldBoundaryMatch_Z)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 2048.0f;
    config.WorldSizeZ = 2048.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 2.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    ASSERT_GE(data->Config.TilesPerAxisZ, 2u);

    auto* tile00 = TerrainService::Get().LoadTile(handle, {0, 0});
    auto* tile01 = TerrainService::Get().LoadTile(handle, {0, 1});
    ASSERT_NE(tile00, nullptr);
    ASSERT_NE(tile01, nullptr);

    const uint32 w = tile00->Heightfield.GetWidth();
    const uint32 h = tile00->Heightfield.GetHeight();

    // Bottom edge of tile(0,0) == top edge of tile(0,1)
    uint32 mismatches = 0;
    for (uint32 x = 0; x < w; ++x)
    {
        const float32 a = tile00->Heightfield.GetSample(x, h - 1);
        const float32 b = tile01->Heightfield.GetSample(x, 0);
        if (std::abs(a - b) > 1e-6f)
            ++mismatches;
    }
    EXPECT_EQ(mismatches, 0u) << "Found " << mismatches << " Z-boundary height mismatches";

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// Verify deterministic noise: same tile coord -> same heightfield data.
TEST_F(TiledTerrainFixture, LoadTile_DeterministicNoise)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);

    auto* tile1 = TerrainService::Get().LoadTile(handle, {0, 0});
    ASSERT_NE(tile1, nullptr);
    float32 sample1 = tile1->Heightfield.GetSample(10, 10);

    // Unload and reload — should get the same noise.
    TerrainService::Get().UnloadTile(handle, {0, 0});
    auto* tile2 = TerrainService::Get().LoadTile(handle, {0, 0});
    ASSERT_NE(tile2, nullptr);
    float32 sample2 = tile2->Heightfield.GetSample(10, 10);

    EXPECT_FLOAT_EQ(sample1, sample2) << "Noise should be deterministic per tile coord";

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// C8 render-extent oracle: the CBT render size (params/info WorldSize) must be the REAL
// per-axis extent (tilesPerAxis * tileWorldSize), NOT a power-of-2 square. A non-square /
// non-pow2-tile-count terrain rendered with a square would stretch (CBT maps
// world = origin + uv*size over the unified heightmap).
TEST_F(TiledTerrainFixture, RenderExtent_NonSquareUsesRealPerAxisExtent)
{
    // 4096 x 2048 @ 1 sample/m -> 1024m tiles -> 4 x 2 tiles (a non-square grid).
    TiledTerrainConfig config{};
    config.WorldSizeX = 4096.0f;
    config.WorldSizeZ = 2048.0f;
    config.HeightScale = 200.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    ASSERT_NE(data->Config.TilesPerAxisX, data->Config.TilesPerAxisZ) << "expected a non-square tile grid";

    const TiledRenderExtent e = ComputeTiledRenderExtent(data->Config);
    // Real extent = tilesPerAxis * tileWorldSize, DISTINCT per axis (the bug collapsed
    // both to one square virtual size).
    EXPECT_FLOAT_EQ(e.WorldSizeX, data->Config.TilesPerAxisX * data->Config.TileWorldSize);
    EXPECT_FLOAT_EQ(e.WorldSizeZ, data->Config.TilesPerAxisZ * data->Config.TileWorldSize);
    EXPECT_NE(e.WorldSizeX, e.WorldSizeZ) << "render extent collapsed a non-square terrain to a square";
    // Unified resolution matches an untiled terrain of equal content, per axis.
    EXPECT_EQ(e.UnifiedWidth, data->Config.TilesPerAxisX * (data->Config.TileConfig.HeightmapWidth - 1) + 1);
    EXPECT_EQ(e.UnifiedHeight, data->Config.TilesPerAxisZ * (data->Config.TileConfig.HeightmapHeight - 1) + 1);
    EXPECT_NE(e.UnifiedWidth, e.UnifiedHeight);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, RenderExtent_NonPow2TileCountIsNotRoundedUp)
{
    // 3072 x 3072 @ 1 sample/m -> 1024m tiles -> 3 x 3 tiles. The render extent must stay
    // 3*tileWorldSize, not the 4 a power-of-2 square would round up to.
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 200.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    ASSERT_EQ(data->Config.TilesPerAxisX, 3u);

    const TiledRenderExtent e = ComputeTiledRenderExtent(data->Config);
    const float32 realX = 3.0f * data->Config.TileWorldSize; // 3072
    EXPECT_FLOAT_EQ(e.WorldSizeX, realX);
    // A pow2-rounded square (4 tiles wide) would be 4/3 too large — reject it.
    EXPECT_LT(e.WorldSizeX, 4.0f * data->Config.TileWorldSize);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// ---------------------------------------------------------------------------
// Auto streaming radius is anchored in world metres, independent of texel density
// ---------------------------------------------------------------------------

namespace
{
// Create a tiled terrain of the given world size + SamplesPerMeter, read back the
// auto-derived streaming radius, and destroy it. StreamingRadius left 0 => auto.
float32 AutoStreamingRadius(float32 worldSize, float32 samplesPerMeter)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = worldSize;
    config.WorldSizeZ = worldSize;
    config.HeightScale = 256.0f;
    config.SamplesPerMeter = samplesPerMeter;
    config.PatchGridSize = kDefaultGridSize;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    EXPECT_NE(data, nullptr);
    const float32 radius = data ? data->Config.StreamingRadius : 0.0f;
    TerrainService::Get().DestroyTiledTerrain(handle);
    return radius;
}
} // namespace

// Fails before the fix: the old derivation scaled the window with tileWorld
// (= (kMaxTileResolution-1)/spm), so a higher SPM HALVED the auto radius on a
// fixed-size terrain (e.g. 4096 m: 3072 -> 1536 m). The window is a view-distance
// concept and must not move with texel density.
TEST_F(TiledTerrainFixture, AutoStreamingRadius_IndependentOfSamplesPerMeter)
{
    for (const float32 worldSize : {2048.0f, 4096.0f, 6144.0f})
    {
        const float32 r1 = AutoStreamingRadius(worldSize, 1.0f);
        const float32 r2 = AutoStreamingRadius(worldSize, 2.0f);
        const float32 r4 = AutoStreamingRadius(worldSize, 4.0f);
        EXPECT_GT(r1, 0.0f) << "worldSize=" << worldSize;
        EXPECT_FLOAT_EQ(r1, r2) << "worldSize=" << worldSize << " spm 1 vs 2";
        EXPECT_FLOAT_EQ(r1, r4) << "worldSize=" << worldSize << " spm 1 vs 4";
    }
}

// Residency / no-tear oracle for a 4096 m terrain with a centered camera.
// The auto window must reach every corner of the terrain at BOTH SPM so raising
// SamplesPerMeter never drops a previously-resident band (the sky-through-terrain
// tear). Before the fix, SPM 2 => radius 1536 m < 2896 m half-diagonal (fails).
TEST_F(TiledTerrainFixture, AutoStreamingRadius_CoversTerrainDiagonalAcrossSpm)
{
    constexpr float32 kWorldSize = 4096.0f;
    const float32 halfDiagonal = 0.5f * kWorldSize * std::sqrt(2.0f); // ~2896 m from centre
    const float32 r1 = AutoStreamingRadius(kWorldSize, 1.0f);
    const float32 r2 = AutoStreamingRadius(kWorldSize, 2.0f);
    EXPECT_GE(r1, halfDiagonal);
    EXPECT_GE(r2, halfDiagonal); // the residency band that tore stays covered at SPM 2
}
