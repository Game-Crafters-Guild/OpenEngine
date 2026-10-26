// The surface page bake: the splat, normal, grass control and color of a surface page of any level,
// made by the same passes as the tile bake (design page-streaming 3.8, the baked-page cache).

#include "TerrainECS/TerrainModifierSystem.h"

#include "TerrainECS/TerrainColorMap.h"
#include "TerrainECS/TerrainGrassField.h"

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageStoreFormat.h"

#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::TerrainECS
{

namespace
{

// The first level-0 sample of a page's stride of `format` along one axis: its first owned sample
// less the format's apron.
int64 PageFirstSample(uint32 page, PageStreaming::PageFieldFormat format)
{
    const int64 apron = (PageStreaming::PageStrideTexels(format) - PageStreaming::kPageOwnedSamples) / 2u;
    return static_cast<int64>(page) * PageStreaming::kPageOwnedSamples - apron;
}

uint32 ClampSample(int64 sample, uint32 samples)
{
    return static_cast<uint32>(std::clamp<int64>(sample, 0, static_cast<int64>(samples) - 1));
}

// The world position of each sample in [first, last] of `level` along one axis. Level-L sample s
// is level-0 sample s * 2^L, placed from the tile that owns it: the tile bake's own expression, so
// level 0 matches it bit for bit.
std::vector<float32> OwnerTilePositions(uint32 first, uint32 last, uint32 level, uint32 tileSamples, uint32 tiles,
                                        float32 terrainOrigin, float32 tileWorldSize)
{
    const uint64 interior = tileSamples - 1u;
    const float32 spacing = tileWorldSize / static_cast<float32>(interior);
    std::vector<float32> positions;
    positions.reserve(last - first + 1u);
    for (uint32 sample = first; sample <= last; ++sample)
    {
        const uint64 level0 = static_cast<uint64>(sample) << level;
        const uint64 tile = std::min<uint64>(level0 / interior, tiles - 1u);
        const float32 tileOrigin = TileWorldOrigin(terrainOrigin, static_cast<int32>(tile), tileWorldSize);
        positions.push_back(tileOrigin + static_cast<float32>(level0 - tile * interior) * spacing);
    }
    return positions;
}

// The terrain samples under a page's stride, at least two a side, and their world positions.
struct PageLattice
{
    int64 FirstX = 0;
    int64 FirstZ = 0;
    uint32 X0 = 0;
    uint32 Z0 = 0;
    uint32 Width = 0;
    uint32 Height = 0;
    std::vector<float32> WorldX;
    std::vector<float32> WorldZ;
};

bool MakePageLattice(const TiledTerrainData& tiled, const PageStreaming::PageAddress& address,
                     const PageStreaming::PageStoreLevel& level, PageStreaming::PageFieldFormat format,
                     PageLattice& lattice)
{
    const uint32 tileW = tiled.Config.TileConfig.HeightmapWidth;
    const uint32 tileH = tiled.Config.TileConfig.HeightmapHeight;
    if (tileW < 2u || tileH < 2u || address.X >= level.PagesX || address.Z >= level.PagesZ || level.SamplesX < 2u ||
        level.SamplesZ < 2u)
        return false;
    const uint32 stride = PageStreaming::PageStrideTexels(format);
    lattice.FirstX = PageFirstSample(address.X, format);
    lattice.FirstZ = PageFirstSample(address.Z, format);
    lattice.X0 = std::min(ClampSample(lattice.FirstX, level.SamplesX), level.SamplesX - 2u);
    lattice.Z0 = std::min(ClampSample(lattice.FirstZ, level.SamplesZ), level.SamplesZ - 2u);
    const uint32 x1 = std::max(ClampSample(lattice.FirstX + stride - 1, level.SamplesX), lattice.X0 + 1u);
    const uint32 z1 = std::max(ClampSample(lattice.FirstZ + stride - 1, level.SamplesZ), lattice.Z0 + 1u);
    lattice.Width = x1 - lattice.X0 + 1u;
    lattice.Height = z1 - lattice.Z0 + 1u;
    lattice.WorldX = OwnerTilePositions(lattice.X0, x1, address.Level, tileW, tiled.Config.TilesPerAxisX,
                                        tiled.WorldOriginX, tiled.Config.TileWorldSize);
    lattice.WorldZ = OwnerTilePositions(lattice.Z0, z1, address.Level, tileH, tiled.Config.TilesPerAxisZ,
                                        tiled.WorldOriginZ, tiled.Config.TileWorldSize);
    return true;
}

// Composes the grass control field on `lattice` under `modifiers`, its ground darkening included.
void ComposeLatticeGrassField(const PageLattice& lattice, std::span<const ResolvedModifier> modifiers,
                              TerrainGrassField& field)
{
    ComposeTerrainGrassField(field, lattice.Width, lattice.Height, lattice.WorldX.back() - lattice.WorldX.front(),
                             lattice.WorldZ.back() - lattice.WorldZ.front(), lattice.WorldX.front(),
                             lattice.WorldZ.front(), modifiers);
}

// Copies `texels` (the lattice's samples, `channels` bytes each) to the page's stride. A stride
// sample outside the terrain takes the nearest sample inside it, as the store's clamped edges do.
void CopyLatticeToStride(const PageLattice& lattice, const PageStreaming::PageStoreLevel& level, uint32 stride,
                         uint32 channels, std::span<const uint8> texels, std::vector<uint8>& outTexels)
{
    outTexels.resize(static_cast<std::size_t>(stride) * stride * channels);
    for (uint32 j = 0; j < stride; ++j)
    {
        const uint32 gz = ClampSample(lattice.FirstZ + j, level.SamplesZ);
        for (uint32 i = 0; i < stride; ++i)
        {
            const uint32 gx = ClampSample(lattice.FirstX + i, level.SamplesX);
            const std::size_t from =
                (static_cast<std::size_t>(gz - lattice.Z0) * lattice.Width + (gx - lattice.X0)) * channels;
            std::copy_n(&texels[from], channels, &outTexels[(static_cast<std::size_t>(j) * stride + i) * channels]);
        }
    }
}

} // namespace

PageStreaming::PageSampleRect SurfaceHeightRect(const PageStreaming::PageStoreLevel& level, uint32 pageX, uint32 pageZ,
                                                PageStreaming::PageFieldFormat format)
{
    // One sample past the stride on each side: the slope of an apron texel reads it.
    const int64 firstX = PageFirstSample(pageX, format) - 1;
    const int64 firstZ = PageFirstSample(pageZ, format) - 1;
    const int64 last = static_cast<int64>(PageStreaming::PageStrideTexels(format));
    return PageStreaming::PageSampleRect{ClampSample(firstX, level.SamplesX), ClampSample(firstZ, level.SamplesZ),
                                         ClampSample(firstX + last + 1, level.SamplesX),
                                         ClampSample(firstZ + last + 1, level.SamplesZ)};
}

bool BakeSurfaceNormalPage(const TiledTerrainData& tiled, const PageStreaming::PageAddress& address,
                           const PageStreaming::PageStoreLevel& level, const SurfaceHeightWindow& heights,
                           std::vector<uint8>& outTexels)
{
    constexpr PageStreaming::PageFieldFormat kFormat = PageStreaming::PageFieldFormat::Bc5;
    const uint32 tileW = tiled.Config.TileConfig.HeightmapWidth;
    const uint32 tileH = tiled.Config.TileConfig.HeightmapHeight;
    if (tileW < 2u || tileH < 2u || address.X >= level.PagesX || address.Z >= level.PagesZ)
        return false;
    const PageStreaming::PageSampleRect rect = SurfaceHeightRect(level, address.X, address.Z, kFormat);
    if (heights.Rect.MinX != rect.MinX || heights.Rect.MinZ != rect.MinZ || heights.Rect.MaxX != rect.MaxX ||
        heights.Rect.MaxZ != rect.MaxZ ||
        heights.Heights.size() != static_cast<std::size_t>(rect.Width()) * rect.Height())
        return false;

    Terrain::HeightfieldData field;
    field.Resize(rect.Width(), rect.Height());
    for (uint32 z = 0; z < rect.Height(); ++z)
        for (uint32 x = 0; x < rect.Width(); ++x)
            field.SetSample(x, z, heights.Heights[static_cast<std::size_t>(z) * rect.Width() + x]);

    // GenerateNormalmapFromHeightfield's spacing in normalized height units, times the level's step.
    const float32 heightScale = std::max(tiled.Config.HeightScale, 0.001f);
    const float32 levelScale = static_cast<float32>(1u << address.Level);
    const float32 spacingX =
        tiled.Config.TileWorldSize / (static_cast<float32>(tileW - 1u) * heightScale) * levelScale;
    const float32 spacingZ =
        tiled.Config.TileWorldSize / (static_cast<float32>(tileH - 1u) * heightScale) * levelScale;

    // A stride sample outside the terrain takes the nearest sample inside it, as the store's
    // clamped edges do. At the terrain's edge the difference clamps, as a tile with no neighbor does.
    const uint32 stride = PageStreaming::PageStrideTexels(kFormat);
    outTexels.assign(static_cast<std::size_t>(stride) * stride * PageStreaming::PageTexelBytes(kFormat), 0u);
    for (uint32 j = 0; j < stride; ++j)
    {
        const uint32 gz = ClampSample(PageFirstSample(address.Z, kFormat) + j, level.SamplesZ);
        for (uint32 i = 0; i < stride; ++i)
        {
            const uint32 gx = ClampSample(PageFirstSample(address.X, kFormat) + i, level.SamplesX);
            assert(rect.Contains(gx, gz));
            const Mathematics::Vector3 normal = field.ComputeNormalWithNeighbor(
                static_cast<int32>(gx - rect.MinX), static_cast<int32>(gz - rect.MinZ), spacingX, spacingZ, nullptr,
                nullptr, nullptr, nullptr);
            PageStreaming::EncodeNormalSample(normal, &outTexels[(static_cast<std::size_t>(j) * stride + i) * 2u]);
        }
    }
    return true;
}

bool TerrainModifierSystem::BakeSurfaceSplatPage(const TiledTerrainData& tiled,
                                                 const PageStreaming::PageAddress& address,
                                                 const PageStreaming::PageStoreLevel& level,
                                                 const SurfaceHeightWindow& heights, std::vector<uint8>& outTexels)
{
    const uint32 pageX = address.X;
    const uint32 pageZ = address.Z;
    const uint32 tileW = tiled.Config.TileConfig.HeightmapWidth;
    const uint32 tileH = tiled.Config.TileConfig.HeightmapHeight;
    if (tileW < 2u || tileH < 2u || tiled.Config.TilesPerAxisX == 0u || tiled.Config.TilesPerAxisZ == 0u ||
        !tiled.SplatBakeRangeValid || pageX >= level.PagesX || pageZ >= level.PagesZ)
        return false;
    constexpr PageStreaming::PageFieldFormat kFormat = PageStreaming::PageFieldFormat::Rgba8;
    const PageStreaming::PageSampleRect rect = SurfaceHeightRect(level, pageX, pageZ, kFormat);
    if (heights.Rect.MinX != rect.MinX || heights.Rect.MinZ != rect.MinZ || heights.Rect.MaxX != rect.MaxX ||
        heights.Rect.MaxZ != rect.MaxZ ||
        heights.Heights.size() != static_cast<std::size_t>(rect.Width()) * rect.Height())
        return false;

    Terrain::HeightfieldData field;
    field.Resize(rect.Width(), rect.Height());
    for (uint32 z = 0; z < rect.Height(); ++z)
        for (uint32 x = 0; x < rect.Width(); ++x)
            field.SetSample(x, z, heights.Heights[static_cast<std::size_t>(z) * rect.Width() + x]);

    SplatTexelLattice lattice;
    const float32 levelScale = static_cast<float32>(1u << address.Level);
    lattice.SpacingX = levelScale * tiled.Config.TileWorldSize / static_cast<float32>(tileW - 1u);
    lattice.SpacingZ = levelScale * tiled.Config.TileWorldSize / static_cast<float32>(tileH - 1u);
    lattice.WorldX = OwnerTilePositions(rect.MinX, rect.MaxX, address.Level, tileW, tiled.Config.TilesPerAxisX,
                                        tiled.WorldOriginX, tiled.Config.TileWorldSize);
    lattice.WorldZ = OwnerTilePositions(rect.MinZ, rect.MaxZ, address.Level, tileH, tiled.Config.TilesPerAxisZ,
                                        tiled.WorldOriginZ, tiled.Config.TileWorldSize);

    // Zero is the unbaked base, as ResetSplatmap makes it.
    std::vector<uint8> splat(static_cast<std::size_t>(rect.Width()) * rect.Height() * 4u, 0u);
    ApplySplatModifiers(splat, rect.Width(), rect.Height(), field, lattice, tiled.Config.HeightScale,
                        tiled.SplatBakeMinH, tiled.SplatBakeMaxH, m_AppliedModifiers, 0, 0, kRegionUnbounded,
                        kRegionUnbounded);

    // Cut the page's stride from the window. A stride sample outside the terrain takes the
    // nearest sample inside it, as the store's clamped edges do.
    constexpr uint32 kStride = PageStreaming::kPageStrideSamples;
    const uint32 samplesX = level.SamplesX;
    const uint32 samplesZ = level.SamplesZ;
    outTexels.assign(static_cast<std::size_t>(PageStreaming::kPageSampleCount) * 4u, 0u);
    for (uint32 j = 0; j < kStride; ++j)
    {
        const uint32 gz = ClampSample(PageFirstSample(pageZ, kFormat) + j, samplesZ);
        for (uint32 i = 0; i < kStride; ++i)
        {
            const uint32 gx = ClampSample(PageFirstSample(pageX, kFormat) + i, samplesX);
            assert(rect.Contains(gx, gz));
            const std::size_t from = (static_cast<std::size_t>(gz - rect.MinZ) * rect.Width() + (gx - rect.MinX)) * 4u;
            std::copy_n(&splat[from], 4u, &outTexels[(static_cast<std::size_t>(j) * kStride + i) * 4u]);
        }
    }
    return true;
}

bool TerrainModifierSystem::BakeSurfaceGrassPage(const TiledTerrainData& tiled,
                                                 const PageStreaming::PageAddress& address,
                                                 const PageStreaming::PageStoreLevel& level,
                                                 std::vector<uint8>& outTexels) const
{
    constexpr PageStreaming::PageFieldFormat kFormat = PageStreaming::PageFieldFormat::Rg8;
    PageLattice lattice;
    if (!MakePageLattice(tiled, address, level, kFormat, lattice))
        return false;
    TerrainGrassField field;
    ComposeLatticeGrassField(lattice, m_GrassModifiers, field);
    const uint32 stride = PageStreaming::PageStrideTexels(kFormat);
    // A field with no pixels is neutral.
    if (field.Pixels.empty())
    {
        outTexels.assign(static_cast<std::size_t>(stride) * stride * kTerrainGrassFieldChannels, 255u);
        return true;
    }
    CopyLatticeToStride(lattice, level, stride, kTerrainGrassFieldChannels, field.Pixels, outTexels);
    return true;
}

bool TerrainModifierSystem::BakeSurfaceColorPage(const TiledTerrainData& tiled,
                                                 const PageStreaming::PageAddress& address,
                                                 const PageStreaming::PageStoreLevel& level,
                                                 const TerrainColorGroundWrite& write,
                                                 std::span<const uint8> splatTexels,
                                                 std::vector<uint8>& outTexels) const
{
    constexpr PageStreaming::PageFieldFormat kFormat = PageStreaming::PageFieldFormat::Rgba8Srgb;
    const uint32 stride = PageStreaming::PageStrideTexels(kFormat);
    PageLattice lattice;
    if (splatTexels.size() != static_cast<std::size_t>(stride) * stride * 4u ||
        !MakePageLattice(tiled, address, level, kFormat, lattice))
        return false;
    // The splat under the lattice. The splat page holds the stride from (FirstX, FirstZ).
    std::vector<uint8> splat(static_cast<std::size_t>(lattice.Width) * lattice.Height * 4u);
    for (uint32 z = 0; z < lattice.Height; ++z)
    {
        const std::size_t row = static_cast<std::size_t>(lattice.Z0 + z - lattice.FirstZ) * stride;
        const std::size_t from = (row + static_cast<std::size_t>(lattice.X0 - lattice.FirstX)) * 4u;
        std::copy_n(&splatTexels[from], static_cast<std::size_t>(lattice.Width) * 4u,
                    &splat[static_cast<std::size_t>(z) * lattice.Width * 4u]);
    }
    TerrainGrassField grass;
    ComposeLatticeGrassField(lattice, m_GrassModifiers, grass);
    TerrainColorComposeInputs inputs;
    inputs.Width = lattice.Width;
    inputs.Height = lattice.Height;
    inputs.OriginX = lattice.WorldX.front();
    inputs.OriginZ = lattice.WorldZ.front();
    inputs.SizeX = lattice.WorldX.back() - lattice.WorldX.front();
    inputs.SizeZ = lattice.WorldZ.back() - lattice.WorldZ.front();
    inputs.Splat = {splat.data(), lattice.Width, lattice.Height, true};
    inputs.Grass = {&grass, inputs.OriginX, inputs.OriginZ, inputs.SizeX, inputs.SizeZ};
    inputs.Write = write;
    inputs.Zones = m_ColorZones;
    TerrainColorMap map;
    ComposeTerrainColorMap(map, inputs);
    // A map that no writer reaches holds no pixels: every texel is unwritten (0, 0, 0, 0).
    if (map.Pixels.empty())
    {
        outTexels.assign(static_cast<std::size_t>(stride) * stride * kTerrainColorMapChannels, 0u);
        return true;
    }
    CopyLatticeToStride(lattice, level, stride, kTerrainColorMapChannels, map.Pixels, outTexels);
    return true;
}

} // namespace GameEngine::TerrainECS
