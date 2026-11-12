#include "TerrainECS/TerrainService.h"

#include "Mathematics/Interpolation.h"
#include "PhysicsECS/HeightFieldDataProvider.h"
#include "Physics/PhysicsShapes.h"
#include "Terrain/CDLODSelection.h"
#include "TerrainECS/Planet/PlanetPageRequester.h"
#include "TerrainECS/Planet/PlanetSculptDab.h"
#include "TerrainECS/BakeWarningLatch.h"
#include "TerrainECS/TerrainHeightFunction/RevocableTerrainHeightFunction.h"
#include "TerrainHeightFunction/TerrainHeightFunctionInstances.h"
#include "TerrainHeightFunction/TerrainHeightFunctionRegistry.h"
#include "PageStreaming/Height/HeightLayerStore.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "TerrainECS/RawHeightmap.h"
#include "TerrainECS/TerrainSizingPlan.h"
#include "TerrainECS/TerrainPages.h"
#include "TerrainECS/HeightPageStoreLoader.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "Logger/Logger.h"
#include "Types/Fnv1a.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <utility>

namespace GameEngine::TerrainECS
{

void ResetSplatmap(
    const Terrain::HeightfieldData& heightfield,
    std::vector<uint8>& outSplatmap,
    uint32& outWidth, uint32& outHeight)
{
    const uint32 w = heightfield.GetWidth();
    const uint32 h = heightfield.GetHeight();
    if (w == 0 || h == 0)
        return;

    outWidth = w;
    outHeight = h;
    outSplatmap.assign(static_cast<size_t>(w) * h * 4, 0);
}

void ResetSplatmapRegion(
    const Terrain::HeightfieldData& heightfield,
    std::vector<uint8>& splatmap,
    int32 minX, int32 minZ, int32 maxX, int32 maxZ)
{
    const uint32 w = heightfield.GetWidth();
    const uint32 h = heightfield.GetHeight();
    if (w == 0 || h == 0 || splatmap.size() != static_cast<size_t>(w) * h * 4)
        return;

    const uint32 x0 = static_cast<uint32>(std::max(minX, 0));
    const uint32 z0 = static_cast<uint32>(std::max(minZ, 0));
    const uint32 x1 = static_cast<uint32>(std::min(maxX, static_cast<int32>(w) - 1));
    const uint32 z1 = static_cast<uint32>(std::min(maxZ, static_cast<int32>(h) - 1));
    if (x0 > x1 || z0 > z1)
        return;

    const size_t spanBytes = static_cast<size_t>(x1 - x0 + 1) * 4;
    for (uint32 z = z0; z <= z1; ++z)
        std::memset(splatmap.data() + (static_cast<size_t>(z) * w + x0) * 4, 0, spanBytes);
}

// IEEE 754 float32 to float16 conversion (round-to-nearest-even).
static uint16 FloatToHalf(float32 value)
{
    uint32 f;
    std::memcpy(&f, &value, sizeof(f));
    uint32 sign = (f >> 16) & 0x8000;
    int32 exp = static_cast<int32>((f >> 23) & 0xFF) - 127 + 15;
    uint32 frac = f & 0x7FFFFF;
    if (exp <= 0)
        return static_cast<uint16>(sign); // flush to zero
    if (exp >= 31)
        return static_cast<uint16>(sign | 0x7C00); // infinity
    return static_cast<uint16>(sign | (static_cast<uint32>(exp) << 10) | (frac >> 13));
}

void GenerateNormalmapFromHeightfield(
    const Terrain::HeightfieldData& heightfield,
    float32 worldSizeX, float32 worldSizeZ,
    float32 heightScale,
    std::vector<uint8>& outNormalmap,
    uint32& outWidth, uint32& outHeight,
    const Terrain::HeightfieldData* neighborLeft,
    const Terrain::HeightfieldData* neighborRight,
    const Terrain::HeightfieldData* neighborUp,
    const Terrain::HeightfieldData* neighborDown)
{
    const uint32 w = heightfield.GetWidth();
    const uint32 h = heightfield.GetHeight();
    if (w == 0 || h == 0)
        return;

    outWidth = w;
    outHeight = h;
    // R16G16_FLOAT: 4 bytes per texel (two float16 values).
    outNormalmap.resize(static_cast<size_t>(w) * h * 4);

    // Divide spacing by HeightScale so the central-difference gradient
    // reflects actual world-space height: dh/dx = HeightScale * (hR - hL) / spacing.
    const float32 hs = std::max(heightScale, 0.001f);
    const float32 spacingX = worldSizeX / (static_cast<float32>(w > 1 ? w - 1 : 1) * hs);
    const float32 spacingZ = worldSizeZ / (static_cast<float32>(h > 1 ? h - 1 : 1) * hs);

    auto* out16 = reinterpret_cast<uint16*>(outNormalmap.data());

    for (uint32 z = 0; z < h; ++z)
    {
        for (uint32 x = 0; x < w; ++x)
        {
            auto normal = heightfield.ComputeNormalWithNeighbor(
                static_cast<int32>(x), static_cast<int32>(z),
                spacingX, spacingZ,
                neighborLeft, neighborRight, neighborUp, neighborDown);

            const size_t idx = (static_cast<size_t>(z) * w + x) * 2;
            out16[idx + 0] = FloatToHalf(normal.x);
            out16[idx + 1] = FloatToHalf(normal.z);
        }
    }
}

void GenerateNormalmapRegionFromHeightfield(
    const Terrain::HeightfieldData& heightfield,
    float32 worldSizeX, float32 worldSizeZ,
    float32 heightScale,
    std::vector<uint8>& normalmap,
    int32 minX, int32 minZ, int32 maxX, int32 maxZ,
    const Terrain::HeightfieldData* neighborLeft,
    const Terrain::HeightfieldData* neighborRight,
    const Terrain::HeightfieldData* neighborUp,
    const Terrain::HeightfieldData* neighborDown)
{
    const uint32 w = heightfield.GetWidth();
    const uint32 h = heightfield.GetHeight();
    if (w == 0 || h == 0 || normalmap.size() != static_cast<size_t>(w) * h * 4)
        return;

    const uint32 x0 = static_cast<uint32>(std::max(minX, 0));
    const uint32 z0 = static_cast<uint32>(std::max(minZ, 0));
    const uint32 x1 = static_cast<uint32>(std::min(maxX, static_cast<int32>(w) - 1));
    const uint32 z1 = static_cast<uint32>(std::min(maxZ, static_cast<int32>(h) - 1));
    if (x0 > x1 || z0 > z1)
        return;

    // Same spacing math as the full generator (see comment there).
    const float32 hs = std::max(heightScale, 0.001f);
    const float32 spacingX = worldSizeX / (static_cast<float32>(w > 1 ? w - 1 : 1) * hs);
    const float32 spacingZ = worldSizeZ / (static_cast<float32>(h > 1 ? h - 1 : 1) * hs);

    auto* out16 = reinterpret_cast<uint16*>(normalmap.data());

    for (uint32 z = z0; z <= z1; ++z)
    {
        for (uint32 x = x0; x <= x1; ++x)
        {
            auto normal = heightfield.ComputeNormalWithNeighbor(
                static_cast<int32>(x), static_cast<int32>(z),
                spacingX, spacingZ,
                neighborLeft, neighborRight, neighborUp, neighborDown);

            const size_t idx = (static_cast<size_t>(z) * w + x) * 2;
            out16[idx + 0] = FloatToHalf(normal.x);
            out16[idx + 1] = FloatToHalf(normal.z);
        }
    }
}

namespace
{
// Source coordinate + interpolation weight for one destination texel of a
// dstDim upsample of a srcDim source. dst 0 maps to src 0 and dst (dstDim-1) to
// src (srcDim-1) exactly (fx==0 at both ends), so shared tile edges stay bit-
// identical. srcHi is clamped so the last texel does not read out of bounds.
struct UpsampleTap
{
    uint32 srcLo;
    uint32 srcHi;
    float32 frac;
};

UpsampleTap ComputeUpsampleTap(uint32 dstIndex, uint32 srcDim, uint32 dstDim)
{
    const float32 pos = static_cast<float32>(dstIndex) *
                        static_cast<float32>(srcDim - 1) / static_cast<float32>(dstDim - 1);
    const uint32 lo = static_cast<uint32>(pos);
    const uint32 hi = std::min(lo + 1u, srcDim - 1u);
    return {lo, hi, pos - static_cast<float32>(lo)};
}
// The lattice samples on one axis that lie on the footprint: those at or before its far edge. A
// tiled terrain's lattice reaches past its authored size; those samples carry no ground.
uint64 FootprintSamples(float32 footprintSize, float32 spacing, uint32 latticeSamples)
{
    constexpr float32 kEdgeTolerance = 1.0e-3f; // in lattice spacings: an edge on a sample keeps it
    if (!(spacing > 0.0f))
        return 0u;
    const uint64 inside = static_cast<uint64>(std::floor(footprintSize / spacing + kEdgeTolerance)) + 1u;
    return std::min<uint64>(inside, latticeSamples);
}

// Reads finest page (pageX, pageZ)'s body samples (SurfaceHeightQuery::FillFinestPage) into `out`,
// kFinestPageGridSamples squared, the samples past the footprint's last sample holes: no ground past
// the edge. False, `out` unspecified, when the page is not on the CPU at level 0.
bool ReadPageColliderSamples(const SurfaceHeightQuery& query, uint32 pageX, uint32 pageZ, std::span<float32> out)
{
    if (!query.FillFinestPage(pageX, pageZ, out))
        return false;
    const uint64 firstX = static_cast<uint64>(pageX) * PageStreaming::kPageOwnedSamples;
    const uint64 firstZ = static_cast<uint64>(pageZ) * PageStreaming::kPageOwnedSamples;
    const uint64 samplesX =
        FootprintSamples(query.FootprintSizeX(), query.LatticeSpacingX(), query.LatticeSamplesX());
    const uint64 samplesZ =
        FootprintSamples(query.FootprintSizeZ(), query.LatticeSpacingZ(), query.LatticeSamplesZ());
    const uint64 insideX = firstX < samplesX ? std::min<uint64>(kFinestPageGridSamples, samplesX - firstX) : 0u;
    const uint64 insideZ = firstZ < samplesZ ? std::min<uint64>(kFinestPageGridSamples, samplesZ - firstZ) : 0u;
    for (uint64 z = 0; z < kFinestPageGridSamples; ++z)
        for (uint64 x = z < insideZ ? insideX : 0u; x < kFinestPageGridSamples; ++x)
            out[z * kFinestPageGridSamples + x] = Physics::kHeightFieldNoCollision;
    return true;
}

// True when an overlay edit over level-0 samples `rect` (inclusive) touches a sample of finest page
// (pageX, pageZ)'s body: lattice samples 128 pageX to 128 pageX + 128 on X, the same on Z.
bool OverlayRectReachesPage(const PageStreaming::PageSampleRect& rect, uint32 pageX, uint32 pageZ)
{
    const uint64 firstX = static_cast<uint64>(pageX) * PageStreaming::kPageOwnedSamples;
    const uint64 firstZ = static_cast<uint64>(pageZ) * PageStreaming::kPageOwnedSamples;
    return rect.MaxX >= firstX && rect.MinX <= firstX + PageStreaming::kPageOwnedSamples && rect.MaxZ >= firstZ &&
           rect.MinZ <= firstZ + PageStreaming::kPageOwnedSamples;
}

// The bounds of the samples where two page body grids (kFinestPageGridSamples squared) differ; empty
// when they are equal.
DirtyRegionLog::Region DifferingSamples(std::span<const float32> before, std::span<const float32> after)
{
    constexpr int32 kGrid = static_cast<int32>(kFinestPageGridSamples);
    DirtyRegionLog::Region region;
    for (int32 z = 0; z < kGrid; ++z)
        for (int32 x = 0; x < kGrid; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(z) * kGrid + x;
            if (before[index] != after[index])
                region.Union(DirtyRegionLog::Region{x, z, x + 1, z + 1});
        }
    return region;
}
} // namespace

void UpsampleHeightfieldBilinear(const float32* src, uint32 srcDim,
                                 std::vector<float32>& out, uint32 dstDim)
{
    if (!src || srcDim < 2 || dstDim < 2)
        return;
    out.resize(static_cast<size_t>(dstDim) * dstDim);

    std::vector<UpsampleTap> taps(dstDim);
    for (uint32 i = 0; i < dstDim; ++i)
        taps[i] = ComputeUpsampleTap(i, srcDim, dstDim);

    for (uint32 z = 0; z < dstDim; ++z)
    {
        const UpsampleTap& tz = taps[z];
        const float32* row0 = src + static_cast<size_t>(tz.srcLo) * srcDim;
        const float32* row1 = src + static_cast<size_t>(tz.srcHi) * srcDim;
        float32* dstRow = out.data() + static_cast<size_t>(z) * dstDim;
        for (uint32 x = 0; x < dstDim; ++x)
        {
            const UpsampleTap& tx = taps[x];
            const float32 a = row0[tx.srcLo] + (row0[tx.srcHi] - row0[tx.srcLo]) * tx.frac;
            const float32 b = row1[tx.srcLo] + (row1[tx.srcHi] - row1[tx.srcLo]) * tx.frac;
            dstRow[x] = a + (b - a) * tz.frac;
        }
    }
}

void UpsampleSplatmapBilinear(const uint8* src, uint32 srcDim,
                              std::vector<uint8>& out, uint32 dstDim)
{
    if (!src || srcDim < 2 || dstDim < 2)
        return;
    out.resize(static_cast<size_t>(dstDim) * dstDim * 4);

    std::vector<UpsampleTap> taps(dstDim);
    for (uint32 i = 0; i < dstDim; ++i)
        taps[i] = ComputeUpsampleTap(i, srcDim, dstDim);

    for (uint32 z = 0; z < dstDim; ++z)
    {
        const UpsampleTap& tz = taps[z];
        const uint8* row0 = src + static_cast<size_t>(tz.srcLo) * srcDim * 4;
        const uint8* row1 = src + static_cast<size_t>(tz.srcHi) * srcDim * 4;
        uint8* dstRow = out.data() + static_cast<size_t>(z) * dstDim * 4;
        for (uint32 x = 0; x < dstDim; ++x)
        {
            const UpsampleTap& tx = taps[x];
            const uint32 lo0 = tx.srcLo * 4, hi0 = tx.srcHi * 4;
            for (uint32 c = 0; c < 4; ++c)
            {
                const float32 a = static_cast<float32>(row0[lo0 + c]) +
                    (static_cast<float32>(row0[hi0 + c]) - static_cast<float32>(row0[lo0 + c])) * tx.frac;
                const float32 b = static_cast<float32>(row1[lo0 + c]) +
                    (static_cast<float32>(row1[hi0 + c]) - static_cast<float32>(row1[lo0 + c])) * tx.frac;
                const float32 v = a + (b - a) * tz.frac;
                dstRow[x * 4 + c] = static_cast<uint8>(std::clamp(v + 0.5f, 0.0f, 255.0f));
            }
        }
    }
}

CoarsePatchRect ComputeCoarsePatchRect(uint32 regionDim, const CoarsePatchNeighbors& n)
{
    CoarsePatchRect r;
    if (regionDim < 2)
        return r;
    // Drop the shared boundary texel on any side whose neighbor is resident Full —
    // that texel belongs to the Full neighbor's edge (exact + modifiers).
    r.X0 = n.LeftFull ? 1u : 0u;
    r.Z0 = n.TopFull ? 1u : 0u;
    const uint32 x1 = n.RightFull ? (regionDim - 1u) : regionDim;   // exclusive
    const uint32 z1 = n.BottomFull ? (regionDim - 1u) : regionDim;  // exclusive
    r.Width = (x1 > r.X0) ? (x1 - r.X0) : 0u;
    r.Height = (z1 > r.Z0) ? (z1 - r.Z0) : 0u;
    return r;
}

void WidenToNominalHeightBand(float32& minH, float32& maxH, float32 bandTop)
{
    // Normalized heights: 0 is the terrain's origin, 1 is HeightScale above it.
    constexpr float32 kNominalBandBottom = 0.0f;
    minH = std::min(minH, kNominalBandBottom);
    maxH = std::max(maxH, bandTop);
}

void TerrainData::ResetSplatmapAndCommitRange()
{
    // The reset does not read the range: GetHeightRangeMeters reports it.
    float32 minH = std::numeric_limits<float32>::max();
    float32 maxH = std::numeric_limits<float32>::lowest();
    const uint32 w = Heightfield.GetWidth();
    const uint32 h = Heightfield.GetHeight();
    for (uint32 z = 0; z < h; ++z)
        for (uint32 x = 0; x < w; ++x)
        {
            const float32 s = Heightfield.GetSample(x, z);
            minH = std::min(minH, s);
            maxH = std::max(maxH, s);
        }
    WidenToNominalHeightBand(minH, maxH, kNominalHeightBandTop);

    ResetSplatmap(Heightfield, Splatmap, SplatmapWidth, SplatmapHeight);
    HeightRangeMinH = minH;
    HeightRangeMaxH = maxH;
    HeightRangeValid = true;
    MarkSplatChanged();
    // Every texel was reset, so the GPU upload must be full-texture, never a height-rect band.
    SplatmapFullDirty = true;
}

std::unique_ptr<TerrainService> TerrainService::s_Instance;
uint64 TerrainService::s_Generation = 0;

TerrainService::TerrainService() = default;
TerrainService::~TerrainService() = default;

TerrainHeightRangeMeters TerrainService::GetHeightRangeMeters(
    const Components::Terrain& terrain, std::optional<GeneratedNoiseRelief> generatedNoise) const
{
    float32 minH = 0.0f;
    float32 maxH = kNominalHeightBandTop;
    float32 bandTop = kNominalHeightBandTop;
    if (const TiledTerrainData* tiled =
            GetTiledTerrainData(TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration}))
    {
        minH = tiled->CachedGlobalMinH;
        maxH = tiled->CachedGlobalMaxH;
        bandTop = NominalHeightBandTop(tiled->Config);
    }
    else if (const TerrainData* data =
                 GetTerrainData(TerrainHandle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration});
             data && data->HeightRangeValid)
    {
        minH = data->HeightRangeMinH;
        maxH = data->HeightRangeMaxH;
    }
    else if (terrain.BaseSource == Components::TerrainBaseSource::Flat && generatedNoise)
        bandTop = GeneratedNoiseStackOf(terrain, *generatedNoise).RealizedTop(); // the tiles do not exist yet
    WidenToNominalHeightBand(minH, maxH, bandTop);
    return {minH * terrain.HeightScale, maxH * terrain.HeightScale};
}

SurfaceHeightQuery TerrainService::SurfaceQuery(const Components::Terrain& terrain,
                                                const Components::WorldTransform& transform)
{
    const float32 originY = (transform.GetPosition() - Mathematics::WorldPosition{}).y;
    if (const TiledTerrainData* tiled =
            GetTiledTerrainData(TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration}))
    {
        if (m_TerrainPages)
        {
            SurfaceHeightQuery paged = m_TerrainPages->SurfaceQuery(
                TerrainHandle{tiled->GlobalGpuHandleIndex, tiled->GlobalGpuHandleGeneration});
            if (paged.IsValid())
            {
                // The tile lattice covers whole tiles; the terrain ends at its authored size.
                paged.ClipFootprint(tiled->Config.WorldSizeX, tiled->Config.WorldSizeZ);
                return paged;
            }
        }
        // No page source yet (a height store still cooking, or before its first request): the tile
        // lattice clipped to the authored size, every sample unsampled.
        const TiledRenderExtent extent = ComputeTiledRenderExtent(tiled->Config);
        if (extent.UnifiedWidth < 2u || extent.UnifiedHeight < 2u)
            return {};
        PageRequestTerrain shape;
        shape.OriginX = tiled->WorldOriginX;
        shape.OriginZ = tiled->WorldOriginZ;
        shape.Level0TexelX = extent.WorldSizeX / static_cast<float32>(extent.UnifiedWidth - 1u);
        shape.Level0TexelZ = extent.WorldSizeZ / static_cast<float32>(extent.UnifiedHeight - 1u);
        shape.SamplesX = extent.UnifiedWidth;
        shape.SamplesZ = extent.UnifiedHeight;
        shape.MinHeight = std::min(originY, originY + terrain.HeightScale);
        shape.MaxHeight = std::max(originY, originY + terrain.HeightScale);
        SurfaceHeightQuery unsourced(shape, originY, terrain.HeightScale);
        unsourced.ClipFootprint(tiled->Config.WorldSizeX, tiled->Config.WorldSizeZ);
        return unsourced;
    }
    const TerrainHandle handle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
    const TerrainData* data = GetTerrainData(handle);
    // An allocated-but-empty heightfield is not a surface: sampling it would report the origin
    // altitude everywhere.
    if (!data || data->Heightfield.GetWidth() < 2u || data->Heightfield.GetHeight() < 2u)
        return {};
    // The terrain's transform is a translation for this purpose: the footprint is centered on it.
    PageRequestTerrain shape;
    const Mathematics::Vector3 position = transform.GetPosition() - Mathematics::WorldPosition{};
    shape.OriginX = position.x - terrain.SizeX * 0.5f;
    shape.OriginZ = position.z - terrain.SizeZ * 0.5f;
    shape.Level0TexelX = terrain.SizeX / static_cast<float32>(data->Heightfield.GetWidth() - 1u);
    shape.Level0TexelZ = terrain.SizeZ / static_cast<float32>(data->Heightfield.GetHeight() - 1u);
    shape.SamplesX = data->Heightfield.GetWidth();
    shape.SamplesZ = data->Heightfield.GetHeight();
    shape.MinHeight = std::min(originY, originY + terrain.HeightScale);
    shape.MaxHeight = std::max(originY, originY + terrain.HeightScale);
    return SurfaceHeightQuery(*data, handle, shape, originY, terrain.HeightScale);
}

bool TerrainService::FetchFinestHeightPage(const Components::Terrain& terrain, const Mathematics::Vector3& worldPoint)
{
    const TiledTerrainData* tiled =
        GetTiledTerrainData(TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
    if (!tiled || !m_TerrainPages)
        return false;
    return m_TerrainPages->FetchFinestPage(TerrainHandle{tiled->GlobalGpuHandleIndex, tiled->GlobalGpuHandleGeneration},
                                           worldPoint);
}

std::span<const PageStreaming::PageAddress> TerrainService::FinestWindowPages(const Components::Terrain& terrain)
{
    const TiledTerrainData* tiled =
        GetTiledTerrainData(TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
    if (!tiled || !m_TerrainPages)
        return {};
    return m_TerrainPages->CpuWindowPages(TerrainHandle{tiled->GlobalGpuHandleIndex, tiled->GlobalGpuHandleGeneration});
}

const TerrainPages* TerrainService::FindTerrainPages() const
{
    return m_TerrainPages.get();
}

TerrainPages& TerrainService::GetTerrainPages()
{
    if (!m_TerrainPages)
    {
        HeightPageProfile profile = PlatformHeightPageProfile();
        if (const char* overrideSlots = std::getenv("GE_TERRAIN_PAGE_SLOTS"))
            profile.Slots = std::max(1u, static_cast<uint32>(std::strtoul(overrideSlots, nullptr, 10)));
        // No reader threads: the residency reads cooked pages on Background jobs, as it evaluates
        // generated ones.
        m_TerrainPages = std::make_unique<TerrainPages>(&EngineCore::GetInstance().GetJobSystem(), nullptr, profile,
                                                        PlatformSurfacePageProfile());
    }
    return *m_TerrainPages;
}

TerrainHeightFunctionInstances& TerrainService::GetHeightFunctionInstances()
{
    if (!m_HeightFunctions)
    {
        EngineCore& engine = EngineCore::GetInstance();
        m_HeightFunctions =
            std::make_unique<TerrainHeightFunctionInstances>(engine.IsInitialized() ? &engine.GetJobSystem() : nullptr);
    }
    return *m_HeightFunctions;
}

void TerrainService::RevokeHeightFunctions(std::string_view moduleId, uint64 firstGeneration, uint64 endGeneration)
{
    if (TerrainService* terrain = TryGet())
        terrain->GetHeightFunctionInstances().RevokeModuleGenerations(moduleId, firstGeneration, endGeneration);
    TerrainHeightFunctionRegistry::Get().RemoveModuleGenerations(moduleId, firstGeneration, endGeneration);
}

HeightPageStoreLoader& TerrainService::GetHeightPageStores()
{
    if (!m_HeightPageStores)
        m_HeightPageStores = std::make_unique<HeightPageStoreLoader>();
    return *m_HeightPageStores;
}

void TerrainService::SetHeightPageStoreLocation(HeightPageStoreLocation location)
{
    GetHeightPageStores().SetLocation(std::move(location));
}

void TerrainService::Initialize()
{
    assert(!s_Instance && "TerrainService already initialized");
    s_Instance = std::make_unique<TerrainService>();
    ++s_Generation;

    // Hot-reload lane for stamp masks and base heightmaps. Initialize runs
    // from EnableRenderingLoop, after the AssetManager exists; standalone
    // (test) construction leaves the lane inactive.
    s_Instance->InstallAssetReloadInvalidators();

    // Register as the heightfield data provider for PhysicsInitSystem. The handle is a
    // single-terrain slot index or a page body handle (kPagePhysicsHandleBit, planar or planet);
    // each resolves to samples + version + dirty-region log, so the region-scoped in-place collider
    // path is the same for both.
    PhysicsECS::SetHeightFieldDataProvider([](uint32 handle, uint32 generation, uint64 sinceVersion) -> PhysicsECS::HeightFieldData
    {
        auto* svc = TerrainService::TryGet();
        if (!svc)
            return {};

        // Resolve the raw sample grid + version + dirty log for whichever addressing
        // scheme the handle carries: a page body buffer or a single-terrain heightfield. Both
        // feed the same region tail.
        const float32* samples = nullptr;
        uint32 sampleCount = 0;
        uint64 version = 0;
        const DirtyRegionLog* dirtyLog = nullptr;
        if (handle & TerrainService::kPagePhysicsHandleBit)
        {
            if (const auto* page = svc->ResolvePageCollider(handle, generation))
            {
                samples = page->Samples.data();
                sampleCount = kFinestPageGridSamples;
                version = page->Version;
                dirtyLog = &page->DirtyLog;
            }
        }
        else if (TerrainData* data = svc->GetTerrainData(TerrainHandle{handle, generation});
                 data && !data->Heightfield.IsEmpty())
        {
            samples = data->Heightfield.GetRawSamples();
            sampleCount = std::min(data->Heightfield.GetWidth(), data->Heightfield.GetHeight());
            version = data->HeightfieldVersion;
            dirtyLog = &data->HeightfieldDirtyLog;
        }

        if (!samples || sampleCount < 4 || version == 0)
            return {};

        PhysicsECS::HeightFieldData out;
        out.samples = samples;
        out.sampleCount = sampleCount;
        out.version = version;

        // Physics keeps its own cursor into the region log (its shape's
        // last-built version); the union of everything newer is the region
        // its collider is missing.
        DirtyRegionLog::Region region;
        if (dirtyLog && dirtyLog->CollectSince(sinceVersion, region))
        {
            const int32 gridN = static_cast<int32>(out.sampleCount);
            region.MinX = std::clamp(region.MinX, 0, gridN);
            region.MinZ = std::clamp(region.MinZ, 0, gridN);
            region.MaxX = std::clamp(region.MaxX, 0, gridN);
            region.MaxZ = std::clamp(region.MaxZ, 0, gridN);
            if (!region.IsEmpty())
            {
                out.hasRegion = true;
                out.regionMinX = region.MinX;
                out.regionMinZ = region.MinZ;
                out.regionMaxX = region.MaxX;
                out.regionMaxZ = region.MaxZ;
            }
        }
        return out;
    });
}

void TerrainService::Shutdown()
{
    PhysicsECS::SetHeightFieldDataProvider(nullptr);
    s_Instance.reset();
    ++s_Generation;
}

TerrainService& TerrainService::Get()
{
    assert(s_Instance && "TerrainService not initialized");
    return *s_Instance;
}

TerrainService* TerrainService::TryGet()
{
    return s_Instance.get();
}

bool TerrainService::IsInitialized()
{
    return s_Instance != nullptr;
}

uint64 TerrainService::GetGeneration()
{
    return s_Generation;
}

// ---- Base-source heightmap assets + asset-reload lane ----

void TerrainService::InstallAssetReloadInvalidators()
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;

    auto& dispatcher = engine.GetAssetManager().GetEventDispatcher();

    // ContentEvents: these caches decode straight from disk, so a plain file
    // edit dispatches AssetModified — never AssetReloaded, which fires only
    // for assets resident in the AssetManager's loaded set (our one-shot
    // TextureAsset / HeightfieldData decodes never enter it) — and
    // AssetCreated re-arms negative-cached decode failures when the file
    // appears. Handlers only ENQUEUE: every event here can fire on the
    // file-watcher thread; the modifier system drains on the main thread and
    // evicts the decoded caches there.
    //
    // TRACKED: AssetEventDispatcher::RemoveCallback doesn't fence an
    // in-flight dispatch on another thread, so a shutdown window exists where
    // a watcher-thread event can invoke this handler mid-teardown. Engine-wide
    // dispatcher flaw (every subscriber shares it) — not closable locally.
    m_TextureReloadInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::Texture,
        [this](const GUID& guid) { EnqueueAssetInvalidation(guid); },
        AssetReloadInvalidator::EventSet::ContentEvents);

    m_HeightmapReloadInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::TerrainHeightmap,
        [this](const GUID& guid) { EnqueueAssetInvalidation(guid); },
        AssetReloadInvalidator::EventSet::ContentEvents);

    // Zone payloads decode straight from disk too, so a .tzone edited on disk
    // (or reverted by another tool) evicts the resident payload and re-bakes.
    m_ZonePayloadReloadInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::TerrainZoneData,
        [this](const GUID& guid) { EnqueueAssetInvalidation(guid); },
        AssetReloadInvalidator::EventSet::ContentEvents);
}

void TerrainService::EnqueueAssetInvalidation(const GUID& guid)
{
    std::lock_guard lock(m_PendingAssetInvalidationsMutex);
    m_PendingAssetInvalidations.push_back(guid);
}

std::vector<GUID> TerrainService::TakePendingAssetInvalidations()
{
    // TRACKED: single-consumer queue — TerrainModifierSystem drains it and
    // evicts BOTH caches, including the mask cache that lives per-system. A
    // second consumer (another world's modifier system, a future zone-payload
    // cache) would starve; needs per-consumer cursors if one appears.
    std::lock_guard lock(m_PendingAssetInvalidationsMutex);
    return std::exchange(m_PendingAssetInvalidations, {});
}

std::shared_ptr<const Terrain::HeightfieldData> TerrainService::ResolveHeightmapAsset(const GUID& guid)
{
    if (guid.IsNull())
        return nullptr;

    // A failed decode caches a null entry, so a hit returns null for it too.
    if (const auto it = m_HeightmapCache.find(guid); it != m_HeightmapCache.end())
        return it->second.Heightfield;

    DecodedHeightmapEntry entry;
    entry.ContentVersion = m_HeightmapVersions[guid];
    Terrain::HeightfieldData decoded;
    bool loadFailed = true;

    // TRACKED: decode size is unbounded (here and in the modifier system's
    // stamp-mask decode) — a multi-hundred-MB source stalls the gather-time
    // sync decode and stays resident in the cache; needs a size cap / async
    // decode lane if imports of that scale appear.
    auto& engine = EngineCore::GetInstance();
    if (engine.IsInitialized())
    {
        AssetMetadata meta{};
        if (engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, meta) && !meta.Path.empty())
        {
            std::string ext = meta.Path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (ext == ".r16" || ext == ".r32")
            {
                // A raw file records no grid: its width and height are the asset's import
                // settings (square when unset). RawHeightmap.h says why a guessed grid is wrong.
                RawHeightmapLayout declared;
                entry.DecodeError = ResolveDeclaredRawHeightmapLayout(
                    ReadRawHeightmapSettings(engine.GetAssetManager().GetRegistry(), meta.Path), declared);
                if (entry.DecodeError.empty())
                    entry.DecodeError = DecodeRawHeightmap(meta.Path, declared.Width, declared.Height, decoded);
                loadFailed = !entry.DecodeError.empty();
            }
            else
            {
                // Texture-typed heightmap reference (any image the decoder reads).
                // stb's 16-bit path keeps full PNG16 precision and promotes 8-bit
                // sources. A PNG decodes whole, so one past the decoder's limit is
                // refused with the fix; other formats have no such check.
                if (ext == ".png")
                {
                    Terrain::PngHeightmapSize pngSize;
                    entry.DecodeError = Terrain::ResolvePngHeightmapSize(meta.Path, pngSize);
                }
                loadFailed = !entry.DecodeError.empty() || !decoded.LoadFromPNG16(meta.Path);
                if (loadFailed && entry.DecodeError.empty())
                    entry.DecodeError = "the image could not be decoded";
            }
        }
        else
        {
            entry.DecodeError = "the asset is not in the asset database";
        }

        if (loadFailed)
        {
            // A user's data, refused with the fix the inspector also shows; not an engine fault.
            Logger::Log::Warning("Terrain base heightmap {} ({}) did not decode: {}. The terrain's base is flat "
                               "until it does.",
                               meta.Path.filename().string(), guid.ToString(), entry.DecodeError);
        }
    }

    if (!loadFailed)
        entry.Heightfield = std::make_shared<const Terrain::HeightfieldData>(std::move(decoded));
    const auto ins = m_HeightmapCache.emplace(guid, std::move(entry)).first;
    return ins->second.Heightfield;
}

std::string TerrainService::GetHeightmapDecodeError(const GUID& guid) const
{
    const auto it = m_HeightmapCache.find(guid);
    return it != m_HeightmapCache.end() ? it->second.DecodeError : std::string{};
}

uint64 TerrainService::GetHeightmapContentVersion(const GUID& guid) const
{
    const auto it = m_HeightmapVersions.find(guid);
    return it != m_HeightmapVersions.end() ? it->second : 0;
}

bool TerrainService::EvictDecodedHeightmap(const GUID& guid)
{
    const bool referenced = m_HeightmapCache.count(guid) != 0 || m_HeightmapVersions.count(guid) != 0;
    if (!referenced)
        return false;
    m_HeightmapCache.erase(guid);
    ++m_HeightmapVersions[guid];
    return true;
}

void TerrainService::SeedDecodedHeightmapForTests(const GUID& guid, Terrain::HeightfieldData heightfield)
{
    DecodedHeightmapEntry entry;
    entry.Heightfield = std::make_shared<const Terrain::HeightfieldData>(std::move(heightfield));
    entry.ContentVersion = m_HeightmapVersions[guid];
    m_HeightmapCache[guid] = std::move(entry);
}

namespace
{

// The terrain's height function name, without the buffer's trailing zeros.
std::string_view HeightFunctionNameOf(const Components::Terrain& terrain)
{
    return {terrain.HeightFunction, strnlen(terrain.HeightFunction, sizeof(terrain.HeightFunction))};
}

// What a Function terrain's height function is built for now: its name's id, the name's registration
// (0 when no module registers it), its seed and its footprint.
TerrainHeightFunctionKey HeightFunctionKeyOf(const Components::Terrain& terrain)
{
    TerrainHeightFunctionKey key;
    key.Name = HashStringId(HeightFunctionNameOf(terrain));
    if (const auto registration = TerrainHeightFunctionRegistry::Get().Find(key.Name))
        key.Generation = registration->Generation;
    key.Context = {terrain.BaseSeed, terrain.SizeX, terrain.SizeZ};
    return key;
}

// A Function base's whole-field height range, from the function's range over the footprint, so level
// selection and the height rules have it before any tile streams. Other bases keep their seed.
void SeedFunctionHeightRange(TiledTerrainData& tiled)
{
    const TiledTerrainConfig& config = tiled.Config;
    const float32 band = FunctionBaseHeightBand(config);
    if (config.Base.Source != Components::TerrainBaseSource::Function || !config.Base.HeightFunction || !(band > 0.0f))
        return;
    const std::optional<TerrainHeightRange> range =
        config.Base.HeightFunction->HeightRange(0.0f, 0.0f, config.WorldSizeX, config.WorldSizeZ, 0.0f);
    if (!range)
        return;
    float32 minH = range->Min / band;
    float32 maxH = range->Max / band;
    WidenToNominalHeightBand(minH, maxH, kNominalHeightBandTop);
    tiled.CachedGlobalMinH = minH;
    tiled.CachedGlobalMaxH = maxH;
}

} // namespace

TiledTerrainBase TerrainService::ResolveTiledTerrainBase(Components::TerrainBaseSource source,
                                                         const GUID& heightmapGuid)
{
    TiledTerrainBase base;
    base.Source = source;
    if (source != Components::TerrainBaseSource::HeightmapAsset)
        return base;
    base.HeightmapGuid = heightmapGuid;
    base.HeightmapContentVersion = GetHeightmapContentVersion(heightmapGuid);
    base.Heightmap = ResolveHeightmapAsset(heightmapGuid);
    return base;
}

bool TerrainService::IsTiledTerrainBaseCurrent(const TiledTerrainBase& base,
                                               Components::TerrainBaseSource source,
                                               const GUID& heightmapGuid) const
{
    if (base.Source != source)
        return false;
    if (source != Components::TerrainBaseSource::HeightmapAsset)
        return true;
    return base.HeightmapGuid == heightmapGuid &&
           base.HeightmapContentVersion == GetHeightmapContentVersion(heightmapGuid);
}

namespace
{

// The stack a Flat `terrain` evaluates under `generatedNoise`; null for every other base.
std::optional<Terrain::TerrainNoiseStack> GeneratedNoiseOf(const Components::Terrain& terrain,
                                                         std::optional<GeneratedNoiseRelief> generatedNoise)
{
    if (terrain.BaseSource != Components::TerrainBaseSource::Flat || !generatedNoise)
        return std::nullopt;
    return GeneratedNoiseStackOf(terrain, *generatedNoise);
}

} // namespace

TiledTerrainConfig TerrainService::BuildTiledTerrainConfig(const Components::Terrain& terrain,
                                                           std::string_view terrainName,
                                                           std::optional<GeneratedNoiseRelief> generatedNoise)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = terrain.SizeX;
    config.WorldSizeZ = terrain.SizeZ;
    config.HeightScale = terrain.HeightScale;
    config.SamplesPerMeter = terrain.SamplesPerMeter;
    config.PatchGridSize = Terrain::kDefaultGridSize;
    config.LODRangeScale = Terrain::kDefaultLODRangeScale;
    config.StreamingRadius = terrain.StreamingRadius;
    config.Base = ResolveTiledTerrainBase(terrain.BaseSource, terrain.TerrainAssetGuid.ToGuid());
    config.Base.Noise = GeneratedNoiseOf(terrain, generatedNoise);
    if (terrain.BaseSource != Components::TerrainBaseSource::Function)
        return config;
    config.Base.HeightFunctionKey = HeightFunctionKeyOf(terrain);
    config.Base.Names = {std::string(terrainName), std::string(HeightFunctionNameOf(terrain))};
    const auto registration = TerrainHeightFunctionRegistry::Get().Find(config.Base.HeightFunctionKey.Name);
    if (!registration)
    {
        LOG_WARNING("Terrain '{}': no loaded module registers the height function '{}'. The terrain is flat. "
                    "Build the game module that registers it, or set the terrain's Base Source.",
                    config.Base.Names.Terrain, config.Base.Names.Function);
        return config;
    }
    config.Base.HeightFunction =
        GetHeightFunctionInstances().Request(config.Base.HeightFunctionKey, *registration);
    return config;
}

bool TerrainService::IsTiledTerrainBaseReady(const Components::Terrain& terrain)
{
    if (terrain.BaseSource != Components::TerrainBaseSource::Function)
        return true;
    const TerrainHeightFunctionKey key = HeightFunctionKeyOf(terrain);
    const auto registration = TerrainHeightFunctionRegistry::Get().Find(key.Name);
    return !registration || GetHeightFunctionInstances().Request(key, *registration) != nullptr;
}

std::vector<std::string> TerrainService::GetHeightFunctionNames()
{
    return TerrainHeightFunctionRegistry::Get().Names();
}

bool TerrainService::IsHeightFunctionRegistered(std::string_view name)
{
    return TerrainHeightFunctionRegistry::Get().Find(HashStringId(name)).has_value();
}

TiledTerrainEdit TerrainService::CompareTiledTerrainConfig(const TiledTerrainConfig& config,
                                                           const Components::Terrain& terrain,
                                                           std::optional<GeneratedNoiseRelief> generatedNoise)
{
    TiledTerrainEdit edit;
    edit.ResolutionChanged = config.SamplesPerMeter != terrain.SamplesPerMeter;
    edit.SizeChanged = config.WorldSizeX != terrain.SizeX || config.WorldSizeZ != terrain.SizeZ;
    edit.BaseChanged =
        !IsTiledTerrainBaseCurrent(config.Base, terrain.BaseSource, terrain.TerrainAssetGuid.ToGuid()) ||
        config.Base.Noise != GeneratedNoiseOf(terrain, generatedNoise);
    if (terrain.BaseSource != Components::TerrainBaseSource::Function)
        return edit;
    // The old world stays drawn until the function instance for the new seed, name and size is built.
    if (!IsTiledTerrainBaseReady(terrain))
        return {};
    edit.BaseChanged = edit.BaseChanged || config.Base.HeightFunctionKey != HeightFunctionKeyOf(terrain) ||
                       config.HeightScale != terrain.HeightScale;
    return edit;
}

// ---- Zone payload store ----

TerrainZonePayload* TerrainService::GetZonePayload(const GUID& guid)
{
    const auto it = m_ZonePayloads.find(guid);
    return it != m_ZonePayloads.end() ? &it->second : nullptr;
}

bool TerrainService::AnyZonePayloadNeedsSave() const
{
    for (const auto& [guid, payload] : m_ZonePayloads)
    {
        if (payload.NeedsSave)
            return true;
    }
    return false;
}

void TerrainService::ClearZonePayloads()
{
    if (m_ZonePayloads.empty())
        return;
    m_ZonePayloads.clear();
    ++m_ZonePayloadEditEpoch;
}

const TerrainZonePayload* TerrainService::ResolveZonePayload(const GUID& guid)
{
    if (guid.IsNull())
        return nullptr;

    if (const auto it = m_ZonePayloads.find(guid); it != m_ZonePayloads.end())
        return &it->second;

    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return nullptr;

    AssetMetadata meta{};
    if (!engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, meta) || meta.Path.empty())
        return nullptr;

    BinaryAsset blob(guid, meta.Path, AssetType::TerrainZoneData);
    TerrainZonePayload payload;
    if (!blob.Load() || !DecodeZonePayload(blob.GetData(), blob.GetDataSize(), payload))
    {
        Logger::Log::Warning("Terrain zone payload {} failed to decode", guid.ToString());
        return nullptr;
    }

    return &m_ZonePayloads.emplace(guid, std::move(payload)).first->second;
}

TerrainZonePayload& TerrainService::EnsureZonePayload(const GUID& guid, ZonePayloadFormat format,
                                                      uint32 width, uint32 height)
{
    TerrainZonePayload& payload = m_ZonePayloads[guid];
    payload.Allocate(format, width, height);
    payload.DataVersion = 1;
    payload.ClearDirty();
    payload.NeedsSave = true;
    ++m_ZonePayloadEditEpoch;
    return payload;
}

void TerrainService::NotifyZonePayloadEdited(const GUID& guid, int32 minX, int32 minZ,
                                             int32 maxX, int32 maxZ)
{
    auto it = m_ZonePayloads.find(guid);
    if (it == m_ZonePayloads.end())
        return;
    TerrainZonePayload& payload = it->second;
    ++payload.DataVersion;
    payload.MarkDirtyTexels(minX, minZ, maxX, maxZ);
    payload.NeedsSave = true;
    ++m_ZonePayloadEditEpoch;
}

void TerrainService::ClearZonePayloadDirty(const GUID& guid)
{
    if (auto it = m_ZonePayloads.find(guid); it != m_ZonePayloads.end())
        it->second.ClearDirty();
}

bool TerrainService::EvictZonePayload(const GUID& guid)
{
    if (m_ZonePayloads.erase(guid) == 0)
        return false;
    ++m_ZonePayloadEditEpoch;
    return true;
}

void TerrainService::SetZonePayload(const GUID& guid, TerrainZonePayload payload)
{
    TerrainZonePayload& slot = m_ZonePayloads[guid];
    const uint64 nextVersion = slot.DataVersion + 1;
    slot = std::move(payload);
    slot.DataVersion = nextVersion; // monotonic so the bake diff sees a change
    slot.ClearDirty();              // no sub-rect: re-bake the whole footprint
    slot.NeedsSave = true;
    ++m_ZonePayloadEditEpoch;
}

void TerrainService::SeedZonePayloadForTests(const GUID& guid, TerrainZonePayload payload)
{
    m_ZonePayloads[guid] = std::move(payload);
    ++m_ZonePayloadEditEpoch;
}

void TerrainService::SnapshotZonePayloadsForPlay()
{
    m_ZonePayloadPlaySnapshot = m_ZonePayloads;
    m_HasZonePayloadPlaySnapshot = true;
}

void TerrainService::RestoreZonePayloadsFromPlaySnapshot()
{
    if (!m_HasZonePayloadPlaySnapshot)
        return;
    m_ZonePayloads = std::move(m_ZonePayloadPlaySnapshot);
    m_ZonePayloadPlaySnapshot.clear();
    m_HasZonePayloadPlaySnapshot = false;
    // Force the next bake to re-read the reverted payloads.
    ++m_ZonePayloadEditEpoch;
}

TiledRenderExtent ComputeTiledRenderExtent(const TiledTerrainConfig& config)
{
    TiledRenderExtent e{};
    e.TilesPerAxisX = config.TilesPerAxisX;
    e.TilesPerAxisZ = config.TilesPerAxisZ;
    e.TileInteriorX = config.TileConfig.HeightmapWidth > 0 ? config.TileConfig.HeightmapWidth - 1 : 0;
    e.TileInteriorZ = config.TileConfig.HeightmapHeight > 0 ? config.TileConfig.HeightmapHeight - 1 : 0;
    e.UnifiedWidth = e.TilesPerAxisX * e.TileInteriorX + 1;
    e.UnifiedHeight = e.TilesPerAxisZ * e.TileInteriorZ + 1;
    e.WorldSizeX = static_cast<float32>(e.TilesPerAxisX) * config.TileWorldSize;
    e.WorldSizeZ = static_cast<float32>(e.TilesPerAxisZ) * config.TileWorldSize;
    return e;
}

bool TiledRenderExtentUsesAtlas(const TiledRenderExtent& extent)
{
    return extent.UnifiedWidth > kMaxUnifiedTiledResolution || extent.UnifiedHeight > kMaxUnifiedTiledResolution;
}

bool BehindCameraDetailApplies(const Components::Terrain& terrain, const TerrainService& service)
{
    if (terrain.Domain != Components::TerrainDomain::Planar)
        return false;
    const TiledTerrainData* tiled =
        service.GetTiledTerrainData(TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
    return tiled && TiledRenderExtentUsesAtlas(ComputeTiledRenderExtent(tiled->Config));
}

namespace
{

// The samples [minX, maxX] x [minZ, maxZ] of a field laid from (fieldOriginX, fieldOriginZ) at the given
// spacing, from a Function base, one FillFunctionBaseRow a row. A position is the tile fill's world
// expression minus the terrain's corner, so the page provider's level 0 forms the same bits.
void FillFunctionBaseRegion(Terrain::HeightfieldData& field, const RevocableTerrainHeightFunction& function,
                            const FunctionBaseNames& names, float32 terrainOriginX, float32 terrainOriginZ, float32 fieldOriginX,
                            float32 fieldOriginZ, float32 spacingX, float32 spacingZ, int32 minX, int32 minZ,
                            int32 maxX, int32 maxZ, float32 minResolvedWavelength, float32 heightBand)
{
    const std::size_t count = static_cast<std::size_t>(maxX - minX + 1);
    std::vector<float32> localX(count);
    std::vector<float32> row(count);
    for (int32 x = minX; x <= maxX; ++x)
        localX[static_cast<std::size_t>(x - minX)] =
            (fieldOriginX + static_cast<float32>(x) * spacingX) - terrainOriginX;
    for (int32 z = minZ; z <= maxZ; ++z)
    {
        const float32 localZ = (fieldOriginZ + static_cast<float32>(z) * spacingZ) - terrainOriginZ;
        FillFunctionBaseRow(function, names, localZ, localX, minResolvedWavelength, heightBand, row);
        for (int32 x = minX; x <= maxX; ++x)
            field.SetSample(static_cast<uint32>(x), static_cast<uint32>(z), row[static_cast<std::size_t>(x - minX)]);
    }
}

} // namespace

void FillHeightfieldBaseRegion(Terrain::HeightfieldData& heightfield,
                               Components::TerrainBaseSource source,
                               const Terrain::HeightfieldData* heightmap,
                               int32 minX, int32 minZ, int32 maxX, int32 maxZ)
{
    const uint32 w = heightfield.GetWidth();
    const uint32 h = heightfield.GetHeight();
    if (w == 0 || h == 0)
        return;

    minX = std::max(minX, 0);
    minZ = std::max(minZ, 0);
    maxX = std::min(maxX, static_cast<int32>(w - 1));
    maxZ = std::min(maxZ, static_cast<int32>(h - 1));
    if (minX > maxX || minZ > maxZ)
        return;

    switch (source)
    {
    case Components::TerrainBaseSource::HeightmapAsset:
        if (heightmap != nullptr && !heightmap->IsEmpty())
        {
            // Global UV per destination sample -> bilinear source sample.
            // Purely coordinate-dependent, so region fills are exact subsets
            // of full fills (region-bake parity).
            const float32 invW = w > 1 ? 1.0f / static_cast<float32>(w - 1) : 0.0f;
            const float32 invH = h > 1 ? 1.0f / static_cast<float32>(h - 1) : 0.0f;
            for (int32 z = minZ; z <= maxZ; ++z)
                for (int32 x = minX; x <= maxX; ++x)
                    heightfield.SetSample(static_cast<uint32>(x), static_cast<uint32>(z),
                                          heightmap->SampleBilinear(static_cast<float32>(x) * invW,
                                                                    static_cast<float32>(z) * invH));
            return;
        }
        // Missing or undecodable source: flat, so the problem is visible.
        [[fallthrough]];

    case Components::TerrainBaseSource::Flat:
    default:
        for (int32 z = minZ; z <= maxZ; ++z)
            for (int32 x = minX; x <= maxX; ++x)
                heightfield.SetSample(static_cast<uint32>(x), static_cast<uint32>(z), 0.0f);
        return;
    }
}

Terrain::TerrainNoiseStack GeneratedNoiseStackOf(const Components::Terrain& terrain, const GeneratedNoiseRelief& relief)
{
    float32 gain = Terrain::TerrainNoiseStack::kRollingGain;
    switch (relief.Preset)
    {
    case Components::TerrainNoisePreset::Smooth: gain = Terrain::TerrainNoiseStack::kSmoothGain; break;
    case Components::TerrainNoisePreset::Rugged: gain = Terrain::TerrainNoiseStack::kRuggedGain; break;
    case Components::TerrainNoisePreset::Rolling:
    default: break;
    }
    Terrain::TerrainNoiseStack stack =
        Terrain::TerrainNoiseStack::ForTerrainSize(std::max(terrain.SizeX, terrain.SizeZ), gain);
    stack.Amplitude = relief.Weight;
    return stack;
}

float32 NominalHeightBandTop(const TiledTerrainConfig& config)
{
    if (!config.Base.Noise)
        return kNominalHeightBandTop;
    return config.Base.Noise->RealizedTop();
}

void FillTiledBaseRegion(Terrain::HeightfieldData& field, const TiledTerrainConfig& config,
                         float32 terrainOriginX, float32 terrainOriginZ,
                         float32 fieldOriginX, float32 fieldOriginZ,
                         float32 fieldSizeX, float32 fieldSizeZ,
                         int32 minX, int32 minZ, int32 maxX, int32 maxZ, float32 minResolvedCellMeters)
{
    const uint32 w = field.GetWidth();
    const uint32 h = field.GetHeight();
    if (w < 2 || h < 2)
        return;

    minX = std::max(minX, 0);
    minZ = std::max(minZ, 0);
    maxX = std::min(maxX, static_cast<int32>(w - 1));
    maxZ = std::min(maxZ, static_cast<int32>(h - 1));
    if (minX > maxX || minZ > maxZ)
        return;

    const TiledTerrainBase& base = config.Base;
    if (base.Noise)
    {
        field.FillRegionWithNoiseWorldSpace(*base.Noise, fieldOriginX, fieldOriginZ, fieldSizeX, fieldSizeZ,
                                            minX, minZ, maxX, maxZ, minResolvedCellMeters);
        return;
    }
    switch (base.Source)
    {
    case Components::TerrainBaseSource::HeightmapAsset:
        if (base.Heightmap != nullptr && !base.Heightmap->IsEmpty() && config.WorldSizeX > 0.0f &&
            config.WorldSizeZ > 0.0f)
        {
            // Footprint UV of each sample: its world position relative to the terrain's
            // corner over the authored size, the mapping an untiled terrain's sample
            // index / (dim - 1) resolves to. Clamped, because SampleBilinear reads out of
            // range past [0, 1] and the tile grid can overhang the footprint.
            const Terrain::HeightfieldData& heightmap = *base.Heightmap;
            const float32 spacingX = fieldSizeX / static_cast<float32>(w - 1);
            const float32 spacingZ = fieldSizeZ / static_cast<float32>(h - 1);
            for (int32 z = minZ; z <= maxZ; ++z)
            {
                const float32 worldZ = fieldOriginZ + static_cast<float32>(z) * spacingZ;
                const float32 v = Math::Clamp01((worldZ - terrainOriginZ) / config.WorldSizeZ);
                for (int32 x = minX; x <= maxX; ++x)
                {
                    const float32 worldX = fieldOriginX + static_cast<float32>(x) * spacingX;
                    const float32 u =
                        Math::Clamp01((worldX - terrainOriginX) / config.WorldSizeX);
                    field.SetSample(static_cast<uint32>(x), static_cast<uint32>(z),
                                    heightmap.SampleBilinear(u, v));
                }
            }
            return;
        }
        // Missing or undecodable source: flat, so the problem is visible.
        [[fallthrough]];

    case Components::TerrainBaseSource::Function:
        if (base.Source == Components::TerrainBaseSource::Function && base.HeightFunction != nullptr)
        {
            const float32 spacingX = fieldSizeX / static_cast<float32>(w - 1);
            const float32 spacingZ = fieldSizeZ / static_cast<float32>(h - 1);
            FillFunctionBaseRegion(field, *base.HeightFunction, base.Names, terrainOriginX, terrainOriginZ, fieldOriginX,
                                   fieldOriginZ, spacingX, spacingZ, minX, minZ, maxX, maxZ, minResolvedCellMeters,
                                   FunctionBaseHeightBand(config));
            return;
        }
        [[fallthrough]];

    case Components::TerrainBaseSource::Flat:
    default:
        for (int32 z = minZ; z <= maxZ; ++z)
            for (int32 x = minX; x <= maxX; ++x)
                field.SetSample(static_cast<uint32>(x), static_cast<uint32>(z), 0.0f);
        return;
    }
}

float32 FunctionBaseHeightBand(const TiledTerrainConfig& config)
{
    return config.HeightScale;
}

namespace
{

// One warning per terrain and function for the process's life, at the row's first non-finite sample.
void WarnNonFiniteFunctionHeight(const FunctionBaseNames& names, float32 localX, float32 localZ)
{
    static BakeWarningLatch warned;
    if (!warned.First(Hashing::HashCombine(HashStringId(names.Terrain), HashStringId(names.Function))))
        return;
    LOG_WARNING("Terrain '{}': height function '{}' returned a height that is not finite at ({}, {}) m from the "
                "terrain's corner. The page with that sample stays empty. Return finite meters from HeightRow.",
                names.Terrain, names.Function, localX, localZ);
}

} // namespace

FunctionBaseRowFill FillFunctionBaseRow(const RevocableTerrainHeightFunction& function, const FunctionBaseNames& names,
                                        float32 localZ, std::span<const float32> localX,
                                        float32 minResolvedWavelength, float32 heightBand,
                                        std::span<float32> outNormalized)
{
    assert(localX.size() == outNormalized.size());
    FunctionBaseRowFill fill = FunctionBaseRowFill::Filled;
    if (!function.HeightRow(localZ, localX, minResolvedWavelength, outNormalized))
        fill = FunctionBaseRowFill::Revoked;
    else if (!(heightBand > 0.0f))
        fill = FunctionBaseRowFill::NoHeightBand;
    else if (const auto bad = std::ranges::find_if_not(outNormalized, [](float32 h) { return std::isfinite(h); });
             bad != outNormalized.end())
    {
        fill = FunctionBaseRowFill::NonFinite;
        WarnNonFiniteFunctionHeight(names, localX[static_cast<std::size_t>(bad - outNormalized.begin())], localZ);
    }
    if (fill != FunctionBaseRowFill::Filled)
    {
        std::ranges::fill(outNormalized, 0.0f);
        return fill;
    }
    for (float32& sample : outNormalized)
        sample /= heightBand;
    return fill;
}

void FillAtlasCoarseBaseField(Terrain::HeightfieldData& field, const TiledTerrainData& tiled)
{
    const TiledRenderExtent extent = ComputeTiledRenderExtent(tiled.Config);
    const uint32 width = field.GetWidth();
    const uint32 height = field.GetHeight();
    if (width < 2 || height < 2)
        return;
    // The field carries no relief under two of its texels, the page provider's rule: a finer octave
    // sampled at the field's spacing shows relief that the ground does not have at that scale.
    const float32 spacingX = extent.WorldSizeX / static_cast<float32>(width - 1);
    const float32 spacingZ = extent.WorldSizeZ / static_cast<float32>(height - 1);
    const float32 minResolvedCellMeters = 2.0f * std::max(spacingX, spacingZ);
    FillTiledBaseRegion(field, tiled.Config, tiled.WorldOriginX, tiled.WorldOriginZ, tiled.WorldOriginX,
                        tiled.WorldOriginZ, extent.WorldSizeX, extent.WorldSizeZ, 0, 0, static_cast<int32>(width) - 1,
                        static_cast<int32>(height) - 1, minResolvedCellMeters);
}

TerrainHandle TerrainService::CreateTerrain(Terrain::TerrainConfig config)
{
    // C-#7: Config validation
    if (config.WorldSizeX <= 0.0f || config.WorldSizeZ <= 0.0f)
        return TerrainHandle{};
    if (config.HeightScale <= 0.0f)
        config.HeightScale = 1.0f;
    if (config.HeightmapWidth < 3)
        config.HeightmapWidth = 3;
    if (config.HeightmapHeight < 3)
        config.HeightmapHeight = 3;
    if (config.HeightmapWidth > Terrain::kMaxHeightmapDimension)
        config.HeightmapWidth = Terrain::kMaxHeightmapDimension;
    if (config.HeightmapHeight > Terrain::kMaxHeightmapDimension)
        config.HeightmapHeight = Terrain::kMaxHeightmapDimension;
    if (config.LODLevels == 0)
        return TerrainHandle{};
    config.LODLevels = std::min(config.LODLevels, Terrain::kMaxLODLevels);
    if (config.LODRangeScale < 0.1f) config.LODRangeScale = 0.1f;
    if (config.PatchGridSize < Terrain::kMinPatchSize) config.PatchGridSize = Terrain::kMinPatchSize;
    if (config.PatchGridSize > Terrain::kMaxPatchSize) config.PatchGridSize = Terrain::kMaxPatchSize;

    std::lock_guard<std::mutex> lock(m_Mutex);

    uint32 index;
    if (!m_FreeList.empty())
    {
        index = m_FreeList.back();
        m_FreeList.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_Slots.size());
        m_Slots.emplace_back();
    }

    auto& slot = m_Slots[index];
    slot.Generation++;
    slot.Active = true;
    slot.Data = std::make_shared<TerrainData>();
    slot.Data->Config = config;
    slot.Data->Heightfield.Resize(config.HeightmapWidth, config.HeightmapHeight, 0.0f);
    slot.Data->Quadtree.Build(slot.Data->Heightfield, config.LODLevels);
    slot.Data->MarkFullDirty();

    return TerrainHandle{index, slot.Generation};
}

TerrainHandle TerrainService::CreateTerrainFromBase(const Terrain::TerrainConfig& config,
                                                   const Components::Terrain& terrain)
{
    const TerrainHandle handle = CreateTerrain(config);
    TerrainData* data = GetTerrainData(handle);
    if (!data)
        return handle;

    std::shared_ptr<const Terrain::HeightfieldData> heightmap;
    if (terrain.BaseSource == Components::TerrainBaseSource::HeightmapAsset && !terrain.TerrainAssetGuid.IsNull())
        heightmap = ResolveHeightmapAsset(terrain.TerrainAssetGuid.ToGuid());

    Terrain::HeightfieldData& field = data->Heightfield;
    FillHeightfieldBaseRegion(field, terrain.BaseSource, heightmap.get(), 0, 0,
                              static_cast<int32>(field.GetWidth()) - 1, static_cast<int32>(field.GetHeight()) - 1);
    data->MarkFullDirty();
    RebuildQuadtree(handle);
    return handle;
}

void TerrainService::DestroyTerrain(TerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return;

    auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    slot.Active = false;
    slot.Data.reset();
    m_FreeList.push_back(handle.Index);
}

TerrainData* TerrainService::GetTerrainData(TerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return nullptr;

    auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    return slot.Data.get();
}

const TerrainData* TerrainService::GetTerrainData(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return nullptr;

    const auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    return slot.Data.get();
}

std::shared_ptr<const TerrainData> TerrainService::ShareTerrainData(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return nullptr;

    const auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    return slot.Data;
}

uint32 TerrainService::GetActiveTerrainCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    uint32 count = 0;
    for (const auto& slot : m_Slots)
    {
        if (slot.Active)
            ++count;
    }
    return count;
}

uint32 TerrainService::GetActiveTiledTerrainCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    uint32 count = 0;
    for (const auto& slot : m_TiledSlots)
    {
        if (slot.Active)
            ++count;
    }
    return count;
}

uint32 TerrainService::GetTerrainSlotCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<uint32>(m_Slots.size());
}

uint32 TerrainService::GetTiledTerrainSlotCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<uint32>(m_TiledSlots.size());
}

void TerrainService::RebuildQuadtree(TerrainHandle handle)
{
    // Hold mutex for the entire rebuild to prevent use-after-free
    // if DestroyTerrain is called concurrently.
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return;
    auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    slot.Data->Quadtree.Build(slot.Data->Heightfield, slot.Data->Config.LODLevels);
}

bool TerrainService::PatchQuadtreeRegion(TerrainHandle handle,
                                         int32 minSampleX, int32 minSampleZ,
                                         int32 maxSampleX, int32 maxSampleZ,
                                         float32& outMinH, float32& outMaxH)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return false;
    auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return false;

    auto& data = *slot.Data;
    if (!data.Quadtree.IsBuilt())
        data.Quadtree.Build(data.Heightfield, data.Config.LODLevels);
    else
        data.Quadtree.PatchRegion(data.Heightfield, minSampleX, minSampleZ, maxSampleX, maxSampleZ);

    return data.Quadtree.TryGetGlobalHeightRange(
        data.Heightfield.GetWidth(), data.Heightfield.GetHeight(), outMinH, outMaxH);
}

// ---- Tiled terrain implementation ----

TiledTerrainHandle TerrainService::CreateTiledTerrain(TiledTerrainConfig config)
{
    if (config.WorldSizeX <= 0.0f || config.WorldSizeZ <= 0.0f)
        return {};
    config.SamplesPerMeter = std::max(config.SamplesPerMeter, 0.01f);
    config.PatchGridSize = std::clamp(config.PatchGridSize, Terrain::kMinPatchSize, Terrain::kMaxPatchSize);

    // Tile geometry comes from the shared sizing derivation, so the grid built here is the same
    // grid the inspector predicts and the height-source rule is tested against. Tiles are square
    // and capped at the per-tile sample ceiling; larger tiles load unacceptably slowly on the
    // main thread. HeightScale is re-applied here only because it does not affect geometry.
    const TerrainSizingPlan plan =
        DeriveTerrainSizingPlan(config.WorldSizeX, config.WorldSizeZ, config.SamplesPerMeter,
                                config.PatchGridSize, config.LODRangeScale);

    config.TileConfig = Terrain::TerrainConfig::FromSamplesPerMeter(
        plan.TileWorldSize, plan.TileWorldSize, config.HeightScale,
        config.SamplesPerMeter, config.PatchGridSize, config.LODRangeScale);

    config.TileWorldSize = plan.TileWorldSize;
    config.TilesPerAxisX = plan.TilesPerAxisX;
    config.TilesPerAxisZ = plan.TilesPerAxisZ;

    // Auto-compute streaming radius (world metres). The resident window MUST stay
    // anchored in world space independent of texel density (SamplesPerMeter): a higher
    // SPM subdivides each tile into a finer grid, which shrinks tileWorld
    // (= (kMaxTileResolution-1)/spm). Deriving the window from the actual tileWorld
    // therefore HALVED it when SPM went 1->2 on a fixed-size terrain (e.g. 4096 m:
    // 3072 -> 1536 m), so previously-resident bands fell out of the window mid-session
    // and rendered as sky-through-terrain tears. Anchor the window to a REFERENCE tile
    // sized at kReferenceSamplesPerMeter so the coarsest-LOD-range derivation is a pure
    // function of world size, patch grid, and LOD range scale — identical for every SPM.
    // (LOD level count is already SPM-invariant for a multi-tile terrain, since each tile
    // targets the same ~kMaxTileResolution; recomputing it at the reference density keeps
    // the single-tile small-terrain case invariant too.) An explicitly user-set
    // StreamingRadius (> 0) stays authoritative and is never overridden here.
    if (config.StreamingRadius <= 0.0f)
    {
        static constexpr float32 kReferenceSamplesPerMeter = 1.0f;
        static constexpr float32 kStreamingRadiusMargin = 1.5f; // 50% beyond coarsest LOD
        const float32 referenceTileWorld = std::min(
            {config.WorldSizeX, config.WorldSizeZ,
             static_cast<float32>(Terrain::kMaxTileResolution - 1) / kReferenceSamplesPerMeter});
        const Terrain::TerrainConfig referenceTile = Terrain::TerrainConfig::FromSamplesPerMeter(
            referenceTileWorld, referenceTileWorld, config.HeightScale,
            kReferenceSamplesPerMeter, config.PatchGridSize, config.LODRangeScale);
        const uint32 finestNodes = 1u << (referenceTile.LODLevels - 1);
        const float32 patchSize = referenceTileWorld / static_cast<float32>(finestNodes);
        const float32 baseRange = patchSize * config.LODRangeScale;
        float32 maxRange = baseRange;
        for (uint32 i = 1; i < referenceTile.LODLevels; ++i)
            maxRange = Terrain::CDLODSelection::ComputeLODRange(i, baseRange, config.LODRangeScale);
        config.StreamingRadius = maxRange * kStreamingRadiusMargin;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);

    uint32 index;
    if (!m_TiledFreeList.empty())
    {
        index = m_TiledFreeList.back();
        m_TiledFreeList.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_TiledSlots.size());
        m_TiledSlots.emplace_back();
    }

    auto& slot = m_TiledSlots[index];
    slot.Generation++;
    slot.Active = true;
    slot.Data = std::make_unique<TiledTerrainData>();
    slot.Data->Config = config;
    slot.Data->CachedGlobalMaxH = NominalHeightBandTop(config);
    SeedFunctionHeightRange(*slot.Data);

    return TiledTerrainHandle{index, slot.Generation};
}

void TerrainService::DestroyTiledTerrain(TiledTerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    // Destroy the global GPU TerrainData slot before freeing the tiled data. Tiles no longer own per-tile GPU slots — the whole
    // terrain renders through one unified texture set on the render feature.
    if (slot.Data && (slot.Data->GlobalGpuHandleIndex != 0 || slot.Data->GlobalGpuHandleGeneration != 0))
    {
        TerrainHandle gpuH{slot.Data->GlobalGpuHandleIndex, slot.Data->GlobalGpuHandleGeneration};
        if (gpuH.Index < m_Slots.size())
        {
            auto& gpuSlot = m_Slots[gpuH.Index];
            if (gpuSlot.Active && gpuSlot.Generation == gpuH.Generation)
            {
                gpuSlot.Active = false;
                gpuSlot.Data.reset();
                m_FreeList.push_back(gpuH.Index);
            }
        }
    }

    slot.Active = false;
    slot.Data.reset();
    m_TiledFreeList.push_back(handle.Index);
}

TiledTerrainData* TerrainService::GetTiledTerrainData(TiledTerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return nullptr;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    return slot.Data.get();
}

const TiledTerrainData* TerrainService::GetTiledTerrainData(TiledTerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return nullptr;
    const auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    return slot.Data.get();
}

// ---- Per-page collider registry (design page-streaming D3) ----

TerrainService::PagePhysicsHandle TerrainService::AcquirePagePhysicsHandle(const SurfaceHeightQuery& query,
                                                                           uint32 pageX, uint32 pageZ)
{
    // Grid sample i of page p is lattice sample p x kPageOwnedSamples + i. A page whose first
    // sample is the footprint's last, or lies past it, owns no cell.
    const uint64 firstX = static_cast<uint64>(pageX) * PageStreaming::kPageOwnedSamples;
    const uint64 firstZ = static_cast<uint64>(pageZ) * PageStreaming::kPageOwnedSamples;
    if (firstX + 1u >= FootprintSamples(query.FootprintSizeX(), query.LatticeSpacingX(), query.LatticeSamplesX()) ||
        firstZ + 1u >= FootprintSamples(query.FootprintSizeZ(), query.LatticeSpacingZ(), query.LatticeSamplesZ()))
        return {};
    PageColliderData data;
    data.Samples.resize(static_cast<std::size_t>(kFinestPageGridSamples) * kFinestPageGridSamples);
    if (!ReadPageColliderSamples(query, pageX, pageZ, data.Samples))
        return {};
    data.Version = 1;
    constexpr int32 kGrid = static_cast<int32>(kFinestPageGridSamples);
    data.DirtyLog.Append(data.Version, DirtyRegionLog::Region{0, 0, kGrid, kGrid});
    data.ReadAt = query.PageStamp();
    data.OverlayVersion = query.Edits().OverlayVersion;

    std::lock_guard<std::mutex> lock(m_Mutex);
    const GenerationalVector::Handle slot = m_PagePhysicsSlots.Create(std::move(data));
    assert(slot.Index() < kPagePhysicsHandleBit && "page physics slots overflow the handle's index bits");
    return PagePhysicsHandle{slot.Index() | kPagePhysicsHandleBit, slot.Generation()};
}

void TerrainService::RefreshPagePhysicsHandle(const SurfaceHeightQuery& query, uint32 pageX, uint32 pageZ,
                                              PagePhysicsHandle handle)
{
    if ((handle.Index & kPagePhysicsHandleBit) == 0)
        return;
    PageColliderData* data = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        data = m_PagePhysicsSlots.Get(GenerationalVector::Handle(handle.Index & ~kPagePhysicsHandleBit, handle.Generation));
    }
    if (!data)
        return;
    const SurfaceHeightPageStamp stamp = query.PageStamp();
    const SurfaceHeightEdits& edits = query.Edits();
    const bool pagesMoved = stamp.Epoch != data->ReadAt.Epoch || stamp.Change != data->ReadAt.Change;
    const bool overlayMoved = edits.OverlayVersion != data->OverlayVersion;
    if (!pagesMoved && !overlayMoved)
        return;

    bool reread = pagesMoved && query.FinestPageChangedSince(data->ReadAt, pageX, pageZ);
    if (overlayMoved && !reread)
        reread = edits.OverlayChangedSince != data->OverlayVersion ||
                 OverlayRectReachesPage(edits.OverlayChanged, pageX, pageZ);
    DirtyRegionLog::Region changed;
    if (reread)
    {
        m_PageColliderScratch.resize(data->Samples.size());
        if (!ReadPageColliderSamples(query, pageX, pageZ, m_PageColliderScratch))
            return;
        changed = DifferingSamples(data->Samples, m_PageColliderScratch);
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!changed.IsEmpty())
    {
        std::copy(m_PageColliderScratch.begin(), m_PageColliderScratch.end(), data->Samples.begin());
        ++data->Version;
        data->DirtyLog.Append(data->Version, changed);
    }
    data->ReadAt = stamp;
    data->OverlayVersion = edits.OverlayVersion;
}

void TerrainService::ReleasePagePhysicsHandle(uint32 handleIndex, uint32 generation)
{
    if ((handleIndex & kPagePhysicsHandleBit) == 0)
        return;
    const GenerationalVector::Handle slot(handleIndex & ~kPagePhysicsHandleBit, generation);
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_PagePhysicsSlots.IsValid(slot))
        m_PagePhysicsSlots.Destroy(slot);
}

void TerrainService::ReleaseAllPagePhysicsHandles()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_PagePhysicsSlots.Clear();
}

const TerrainService::PageColliderData* TerrainService::ResolvePageCollider(uint32 handleIndex,
                                                                            uint32 generation) const
{
    if ((handleIndex & kPagePhysicsHandleBit) == 0)
        return nullptr;
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_PagePhysicsSlots.Get(GenerationalVector::Handle(handleIndex & ~kPagePhysicsHandleBit, generation));
}

TerrainService::PagePhysicsHandle TerrainService::AcquirePlanetPagePhysicsHandle(std::vector<float32>&& meters)
{
    assert(meters.size() == static_cast<std::size_t>(kFinestPageGridSamples) * kFinestPageGridSamples);
    PageColliderData data;
    data.Samples = std::move(meters);
    data.Version = 1;
    constexpr int32 kGrid = static_cast<int32>(kFinestPageGridSamples);
    data.DirtyLog.Append(data.Version, DirtyRegionLog::Region{0, 0, kGrid, kGrid});

    std::lock_guard<std::mutex> lock(m_Mutex);
    const GenerationalVector::Handle slot = m_PagePhysicsSlots.Create(std::move(data));
    assert(slot.Index() < kPagePhysicsHandleBit && "page physics slots overflow the handle's index bits");
    return PagePhysicsHandle{slot.Index() | kPagePhysicsHandleBit, slot.Generation()};
}

void TerrainService::RefillPlanetPagePhysicsHandle(PagePhysicsHandle handle, std::span<const float32> meters)
{
    if ((handle.Index & kPagePhysicsHandleBit) == 0)
        return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    PageColliderData* data =
        m_PagePhysicsSlots.Get(GenerationalVector::Handle(handle.Index & ~kPagePhysicsHandleBit, handle.Generation));
    if (!data || data->Samples.size() != meters.size())
        return;
    std::copy(meters.begin(), meters.end(), data->Samples.begin());
    ++data->Version;
    constexpr int32 kGrid = static_cast<int32>(kFinestPageGridSamples);
    data->DirtyLog.Append(data->Version, DirtyRegionLog::Region{0, 0, kGrid, kGrid});
}

// ---- Editable spherical sculpt (planet editing) ----

namespace
{
// Union an edit's (face-local) UV rect into a per-face accumulator.
void UnionPlanetSculptFaceRect(TerrainService::PlanetSculptFaceRect& acc, float32 minU,
                               float32 minV, float32 maxU, float32 maxV)
{
    if (!acc.Touched)
    {
        acc = TerrainService::PlanetSculptFaceRect{true, minU, minV, maxU, maxV};
        return;
    }
    acc.MinU = std::min(acc.MinU, minU);
    acc.MinV = std::min(acc.MinV, minV);
    acc.MaxU = std::max(acc.MaxU, maxU);
    acc.MaxV = std::max(acc.MaxV, maxV);
}

// True when the layer holds pages and was made for another pyramid than `levels`.
bool HoldsPagesOfAnotherPyramid(const TerrainService::PlanetPageOverlay& overlay,
                                std::span<const PageStreaming::PageStoreLevel> levels)
{
    return overlay.Layer && overlay.Layer->Bytes() != 0u && !overlay.Layer->FitsLevels(levels);
}

// True when a write must not change or replace the sculpt `overlay` holds: its height layer asset did
// not load, or its pages are of another pyramid. A save never writes over the file it came from.
bool SculptIsLocked(const TerrainService::PlanetPageOverlay* overlay,
                    std::span<const PageStreaming::PageStoreLevel> levels)
{
    return overlay && (!overlay->LayerLoadFailed.IsNull() || HoldsPagesOfAnotherPyramid(*overlay, levels));
}

// The whole-meter Planet Radius range whose PlanetFaceSamples at `level0Texel` is `faceSamples`: the
// layer stores its face samples, and LargestPlanetRadius finds each end of the range. False when no
// whole meter fits.
bool PlanetRadiusRangeFor(uint32 faceSamples, float32 level0Texel, uint64& outLow, uint64& outHigh)
{
    if (faceSamples < 2u)
        return false;
    outLow = LargestPlanetRadius(faceSamples - 1u, level0Texel) + 1u;
    outHigh = LargestPlanetRadius(faceSamples, level0Texel);
    return outLow <= outHigh && PlanetFaceSamples(static_cast<float64>(outLow), level0Texel) == faceSamples &&
           PlanetFaceSamples(static_cast<float64>(outHigh), level0Texel) == faceSamples;
}

// A no-break space keeps a number and its unit on one line.
constexpr const char* kNumberUnitSpace = "\xC2\xA0";

std::string DescribeLayerOfAnotherPyramid(uint32 layerSamples, float32 level0Texel)
{
    uint64 low = 0;
    uint64 high = 0;
    if (!PlanetRadiusRangeFor(layerSamples, level0Texel, low, high))
        return "The planet's sculpt does not fit this Planet Radius. The sculpt is kept on disk. Set Planet Radius "
               "back to the value the sculpt was saved with to show and edit it.";
    return std::format("The planet's sculpt was saved for a Planet Radius from {}{}m to {}{}m. The sculpt is kept on "
                       "disk. Set Planet Radius to a value in that range to show and edit the sculpt.",
                       FormatGroupedInteger(low), kNumberUnitSpace, FormatGroupedInteger(high), kNumberUnitSpace);
}
} // namespace

void TerrainService::ResetPlanetSculpt()
{
    std::lock_guard<std::mutex> lock(m_RenderSculptFacesMutex);
    m_RenderSculptFaces = {};
    m_PlanetPageOverlays.Clear();
}

TerrainService::PlanetPageOverlay& TerrainService::PlanetLayerFor(
    uint32 planet, std::span<const PageStreaming::PageStoreLevel> levels, bool& derived)
{
    PlanetPageOverlay& overlay = m_PlanetPageOverlays.GetOrInsert(planet);
    derived = overlay.Layer && overlay.Layer->FitsLevels(levels);
    if (!derived)
        overlay.Layer = std::make_shared<PageStreaming::HeightPageLayer>(PageStreaming::kPageStoreMaxFaces, levels);
    return overlay;
}

void TerrainService::RecordPlanetLayerChange(uint32 planet, PlanetPageOverlay& overlay, bool derived,
                                             std::span<const PageStreaming::PageFaceRect> rects)
{
    // The changes gather from the version the pages hold, so several writes between two page
    // requests rewrite each face's pages once.
    if (!derived)
    {
        overlay.Changed = {};
        overlay.ChangedSince = 0;
    }
    else if (m_TerrainPages && m_TerrainPages->PlanetOverlayVersion(planet) == overlay.Version)
    {
        overlay.Changed = {};
        overlay.ChangedSince = overlay.Version;
    }
    for (const PageStreaming::PageFaceRect& rect : rects)
        PageStreaming::IncludePageFaceRect(overlay.Changed, rect.Face, rect.Level0Rect);
    overlay.Version = ++m_PlanetPageOverlayVersion;
}

void TerrainService::RecordPlanetSculptChange(uint32 planet, PlanetPageOverlay& overlay, bool derived,
                                              std::span<const PageStreaming::PageFaceRect> rects)
{
    RecordPlanetLayerChange(planet, overlay, derived, rects);
    overlay.SculptVersion = overlay.Version;
}

void TerrainService::WritePlanetPageLayer(uint32 planet, std::span<const PageStreaming::PageStoreLevel> levels,
                                          uint8 face, const PageStreaming::PageSampleRect& rect,
                                          std::span<const float32> deltas)
{
    if (SculptIsLocked(m_PlanetPageOverlays.Find(planet), levels))
        return;
    bool derived = false;
    PlanetPageOverlay& overlay = PlanetLayerFor(planet, levels, derived);
    // The sculpt layer holds samples on the saved layer's lattice only, so a save keeps them bit for bit.
    std::vector<float32> onLattice(deltas.begin(), deltas.end());
    for (float32& delta : onLattice)
        delta = PageStreaming::QuantizeHeight(delta, PageStreaming::kHeightLayerStep);
    overlay.Layer->WriteLevel0(face, rect, onLattice);
    const PageStreaming::PageFaceRect changed{face, rect};
    RecordPlanetSculptChange(planet, overlay, derived, std::span(&changed, 1));
}

void TerrainService::ApplyPlanetSculptDab(uint32 planet, uint32 faceSamples, const PlanetSculptDab& dab)
{
    if (faceSamples == 0u)
        return;
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples);
    if (SculptIsLocked(m_PlanetPageOverlays.Find(planet), levels))
        return;
    bool derived = false;
    PlanetPageOverlay& overlay = PlanetLayerFor(planet, levels, derived);
    m_PlanetLayerRects.clear();
    dab.AddTo(*overlay.Layer, m_PlanetLayerRects);
    if (!m_PlanetLayerRects.empty() || !derived)
        RecordPlanetSculptChange(planet, overlay, derived, m_PlanetLayerRects);
}

void TerrainService::BeginPlanetSculptStroke(uint32 planet, uint32 faceSamples)
{
    if (faceSamples == 0u)
        return;
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples);
    if (SculptIsLocked(m_PlanetPageOverlays.Find(planet), levels))
        return;
    bool derived = false;
    PlanetPageOverlay& overlay = PlanetLayerFor(planet, levels, derived);
    if (!derived)
        RecordPlanetLayerChange(planet, overlay, false, {});
    overlay.Layer->BeginCapture();
}

std::string TerrainService::PlanetSculptRefusal(uint32 planet, uint32 faceSamples, float32 level0Texel) const
{
    const PlanetPageOverlay* overlay = m_PlanetPageOverlays.Find(planet);
    if (!overlay || faceSamples == 0u)
        return {};
    if (!overlay->LayerLoadFailed.IsNull())
        return overlay->LayerRefusal;
    if (!HoldsPagesOfAnotherPyramid(*overlay, PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples)))
        return {};
    return DescribeLayerOfAnotherPyramid(overlay->Layer->Levels()[0].SamplesX, level0Texel);
}

std::vector<PageStreaming::HeightPageLayerPage> TerrainService::TakePlanetSculptStroke(uint32 planet)
{
    PlanetPageOverlay* overlay = m_PlanetPageOverlays.Find(planet);
    if (!overlay || !overlay->Layer)
        return {};
    return overlay->Layer->TakeCapture();
}

std::vector<PageStreaming::HeightPageLayerPage> TerrainService::CopyPlanetSculptPages(
    uint32 planet, std::span<const PageStreaming::HeightPageLayerPage> keys) const
{
    const PlanetPageOverlay* overlay = m_PlanetPageOverlays.Find(planet);
    return overlay && overlay->Layer ? overlay->Layer->CopyPages(keys) : std::vector<PageStreaming::HeightPageLayerPage>();
}

void TerrainService::RestorePlanetSculptPages(uint32 planet, std::span<const PageStreaming::HeightPageLayerPage> pages)
{
    PlanetPageOverlay* overlay = m_PlanetPageOverlays.Find(planet);
    if (!overlay || !overlay->Layer || pages.empty())
        return;
    m_PlanetLayerRects.clear();
    overlay->Layer->RestorePages(pages, m_PlanetLayerRects);
    RecordPlanetSculptChange(planet, *overlay, true, m_PlanetLayerRects);
}

bool TerrainService::PlanetHeightLayerNeedsSave(uint32 planet) const
{
    const PlanetPageOverlay* overlay = m_PlanetPageOverlays.Find(planet);
    return overlay && overlay->SculptVersion != overlay->SavedSculptVersion;
}

bool TerrainService::AnyPlanetHeightLayerNeedsSave() const
{
    for (const auto& [planet, overlay] : m_PlanetPageOverlays)
        if (overlay.SculptVersion != overlay.SavedSculptVersion)
            return true;
    return false;
}

void TerrainService::MarkPlanetHeightLayerSaved(uint32 planet, const GUID& asset, uint64 sculptVersion)
{
    PlanetPageOverlay& overlay = m_PlanetPageOverlays.GetOrInsert(planet);
    overlay.LayerAsset = asset;
    overlay.LayerLoadFailed = GUID::Null();
    overlay.LayerRefusal.clear();
    overlay.SavedSculptVersion = sculptVersion;
}

void TerrainService::EnsurePlanetHeightLayerLoaded(uint32 planet, uint32 faceSamples, const GUID& asset)
{
    if (asset.IsNull())
        return;
    if (const PlanetPageOverlay* overlay = m_PlanetPageOverlays.Find(planet);
        overlay && (overlay->LayerAsset == asset || overlay->LayerLoadFailed == asset))
        return;
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;
    AssetMetadata meta{};
    if (!engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(asset, meta) || meta.Path.empty())
        return; // the registry may not know a new file yet

    std::shared_ptr<PageStreaming::HeightPageLayer> layer;
    std::string reason = PageStreaming::LoadHeightLayer(meta.Path, layer);
    if (reason.empty() && layer->FaceCount() != PageStreaming::kPageStoreMaxFaces)
        reason = std::format("{} is a height layer with a face count of {} and a planet's sculpt has a face count "
                             "of {}: the file is not a planet's sculpt. Restore the planet's own sculpt file.",
                             meta.Path.string(), layer->FaceCount(), PageStreaming::kPageStoreMaxFaces);
    PlanetPageOverlay& overlay = m_PlanetPageOverlays.GetOrInsert(planet);
    if (!reason.empty())
    {
        // The brush and the save do not change a sculpt whose file did not load (SculptIsLocked).
        overlay.LayerRefusal = "The planet's sculpt did not load, so the planet shows no sculpt and the brush "
                               "does not edit it: " + reason;
        Logger::Log::Warning("{}", overlay.LayerRefusal);
        overlay.LayerLoadFailed = asset;
        return;
    }
    overlay.Layer = std::move(layer);
    RecordPlanetLayerChange(planet, overlay, false, {});
    overlay.LayerAsset = asset;
    overlay.LayerLoadFailed = GUID::Null();
    overlay.LayerRefusal.clear();
    if (faceSamples != 0u &&
        HoldsPagesOfAnotherPyramid(overlay, PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples)))
        Logger::Log::Warning("{}",
                             DescribeLayerOfAnotherPyramid(overlay.Layer->Levels()[0].SamplesX, 0.0f));
    overlay.SculptVersion = overlay.Version;
    overlay.SavedSculptVersion = overlay.Version;
}

void TerrainService::ClearPlanetPageLayer(uint32 planet)
{
    PlanetPageOverlay* overlay = m_PlanetPageOverlays.Find(planet);
    if (!overlay || !overlay->Layer)
        return;
    overlay->Layer.reset();
    RecordPlanetSculptChange(planet, *overlay, false, {});
}

void TerrainService::WritePlanetModifierLayer(
    uint32 planet, uint32 faceSamples, bool fresh,
    FunctionRef<void(PageStreaming::HeightPageLayer&, std::vector<PageStreaming::PageFaceRect>&)> write)
{
    if (faceSamples == 0u)
        return;
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples);
    PlanetPageOverlay& overlay = m_PlanetPageOverlays.GetOrInsert(planet);
    const bool derived = !fresh && overlay.Modifiers && overlay.Modifiers->FitsLevels(levels);
    const bool replacesPages = !derived && overlay.Modifiers && overlay.Modifiers->Bytes() != 0u;
    if (!derived)
        overlay.Modifiers = std::make_shared<PageStreaming::HeightPageLayer>(PageStreaming::kPageStoreMaxFaces, levels);
    m_PlanetLayerRects.clear();
    write(*overlay.Modifiers, m_PlanetLayerRects);
    if (!m_PlanetLayerRects.empty() || replacesPages)
        RecordPlanetLayerChange(planet, overlay, derived, m_PlanetLayerRects);
}

const TerrainService::PlanetPageOverlay* TerrainService::FindPlanetPageOverlay(uint32 planet) const
{
    return m_PlanetPageOverlays.Find(planet);
}

void TerrainService::AccumulatePlanetPageDirtyRegions(const CBTTerrain::SphereEditRegions& regions)
{
    std::lock_guard<std::mutex> lock(m_RenderSculptFacesMutex);
    for (uint32 i = 0; i < regions.Count; ++i)
    {
        const CBTTerrain::SphereFaceUVRect& rect = regions.Rects[i];
        if (rect.Face < kPlanetSculptFaceCount)
            UnionPlanetSculptFaceRect(m_RenderSculptFaces[rect.Face], rect.MinU, rect.MinV, rect.MaxU, rect.MaxV);
    }
}

bool TerrainService::ConsumePlanetLayerDirtyRegions(CBTTerrain::SphereEditRegions& out)
{
    std::lock_guard<std::mutex> lock(m_RenderSculptFacesMutex);
    CBTTerrain::SphereEditRegions drained{};
    for (uint32 face = 0; face < kPlanetSculptFaceCount; ++face)
    {
        PlanetSculptFaceRect& r = m_RenderSculptFaces[face];
        if (!r.Touched)
            continue;
        drained.Rects[drained.Count++] =
            CBTTerrain::SphereFaceUVRect{face, r.MinU, r.MinV, r.MaxU, r.MaxV};
        r = PlanetSculptFaceRect{}; // clear-on-read
    }
    if (drained.Count == 0u)
        return false;
    out = drained;
    return true;
}

TerrainTileData* TerrainService::LoadTile(TiledTerrainHandle handle, TileCoord coord)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return nullptr;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    auto& tiled = *slot.Data;

    // Already loaded?
    auto it = tiled.Tiles.find(coord);
    if (it != tiled.Tiles.end())
        return it->second.get();

    // Out of bounds?
    if (coord.X < 0 || coord.Z < 0 ||
        coord.X >= static_cast<int32>(tiled.Config.TilesPerAxisX) ||
        coord.Z >= static_cast<int32>(tiled.Config.TilesPerAxisZ))
        return nullptr;

    auto tile = std::make_unique<TerrainTileData>();
    tile->Coord = coord;
    tile->Config = tiled.Config.TileConfig;
    tile->WorldOriginX = TileWorldOrigin(tiled.WorldOriginX, coord.X, tiled.Config.TileWorldSize);
    tile->WorldOriginZ = TileWorldOrigin(tiled.WorldOriginZ, coord.Z, tiled.Config.TileWorldSize);

    const float32 tileWorld = tiled.Config.TileWorldSize;
    tile->Heightfield.Resize(tile->Config.HeightmapWidth, tile->Config.HeightmapHeight, 0.0f);

    // Fill from the terrain's base. It resolves per world position, so adjacent
    // tiles produce identical values at their shared boundary.
    FillTiledBaseRegion(tile->Heightfield, tiled.Config, tiled.WorldOriginX, tiled.WorldOriginZ,
                        tile->WorldOriginX, tile->WorldOriginZ, tileWorld, tileWorld, 0, 0,
                        static_cast<int32>(tile->Config.HeightmapWidth) - 1,
                        static_cast<int32>(tile->Config.HeightmapHeight) - 1, 0.0f);

    tile->MarkFullDirty();

    // Size the splatmap to the tile and leave it unbaked; the modifier bake that
    // follows a tile's arrival composites the surface rules over it. Until then the
    // tile reads as channel 0 rather than as a guess about its own materials.
    ResetSplatmap(tile->Heightfield, tile->Splatmap,
                  tile->SplatmapWidth, tile->SplatmapHeight);
    tile->MarkSplatChanged();

    auto* ptr = tile.get();
    tiled.Tiles[coord] = std::move(tile);

    ++tiled.Revision;

    return ptr;
}

void TerrainService::UnloadTile(TiledTerrainHandle handle, TileCoord coord)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    if (slot.Data->Tiles.erase(coord) > 0)
        ++slot.Data->Revision;
}

bool TerrainService::IsTileLoaded(TiledTerrainHandle handle, TileCoord coord) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return false;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return false;

    return slot.Data->Tiles.count(coord) > 0;
}

// ---- Async tile API (used by TileStreamingManager) ----

TerrainTileData* TerrainService::CreateEmptyTile(TiledTerrainHandle handle, TileCoord coord)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return nullptr;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    auto& tiled = *slot.Data;

    // Already exists?
    auto it = tiled.Tiles.find(coord);
    if (it != tiled.Tiles.end())
        return it->second.get();

    // Out of bounds?
    if (coord.X < 0 || coord.Z < 0 ||
        coord.X >= static_cast<int32>(tiled.Config.TilesPerAxisX) ||
        coord.Z >= static_cast<int32>(tiled.Config.TilesPerAxisZ))
        return nullptr;

    auto tile = std::make_unique<TerrainTileData>();
    tile->Coord = coord;
    tile->Config = tiled.Config.TileConfig;
    tile->WorldOriginX = TileWorldOrigin(tiled.WorldOriginX, coord.X, tiled.Config.TileWorldSize);
    tile->WorldOriginZ = TileWorldOrigin(tiled.WorldOriginZ, coord.Z, tiled.Config.TileWorldSize);
    tile->LodState = TileLodState::Empty;
    tile->HeightfieldDirty = false; // No data yet

    auto* ptr = tile.get();
    tiled.Tiles[coord] = std::move(tile);
    ++tiled.Revision;
    return ptr;
}

void TerrainService::SetTileHeightfield(TiledTerrainHandle handle, TileCoord coord,
                                        Terrain::HeightfieldData&& heightfield,
                                        float32 minH, float32 maxH)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    auto it = slot.Data->Tiles.find(coord);
    if (it == slot.Data->Tiles.end() || !it->second)
        return;

    auto& tile = *it->second;
    const bool isFullRes = (heightfield.GetWidth() == tile.Config.HeightmapWidth &&
                            heightfield.GetHeight() == tile.Config.HeightmapHeight);

    // A Coarse tile turning Full keeps showing its coarse splat until its arrival bake ends.
    if (isFullRes && tile.LodState == TileLodState::Coarse && tile.SplatmapWidth >= 2 &&
        tile.SplatmapHeight == tile.SplatmapWidth)
    {
        tile.ArrivalSplatStandIn = std::move(tile.Splatmap);
        tile.ArrivalSplatStandInDim = tile.SplatmapWidth;
    }
    else
    {
        tile.ArrivalSplatStandIn = {};
        tile.ArrivalSplatStandInDim = 0;
    }
    tile.ArrivalSplatNextRow = 0;
    tile.ArrivalHeightsComposed = false;
    tile.Heightfield = std::move(heightfield);
    tile.CachedMinH = minH;
    tile.CachedMaxH = maxH;
    // Streamed content replaced the heights wholesale; the modifier system's
    // per-block height range grid (if any was built by an earlier edit) is now
    // stale. Invalidate it so the next region range refresh rebuilds it from these
    // heights instead of patching a stale grid.
    tile.HeightBlockDim = 0;
    tile.CoarseStandIn.reset();
    tile.LodState = isFullRes ? TileLodState::Full : TileLodState::Coarse;
    // Fresh streamed content carries the terrain's base only — the authored modifiers
    // must be (re)applied to it by the next modifier bake, scoped to this tile.
    tile.ModifiersApplied = false;
    tile.CoarseSplatBaked = false;
    tile.MarkFullDirty();
    ++slot.Data->Revision;
}

void TerrainService::SetTileMaps(TiledTerrainHandle handle, TileCoord coord,
                                 std::vector<uint8>&& splatmap, uint32 splatW, uint32 splatH,
                                 std::vector<uint8>&& normalmap, uint32 normW, uint32 normH)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    auto it = slot.Data->Tiles.find(coord);
    if (it == slot.Data->Tiles.end() || !it->second)
        return;

    auto& tile = *it->second;
    tile.Splatmap = std::move(splatmap);
    tile.SplatmapWidth = splatW;
    tile.SplatmapHeight = splatH;
    tile.MarkSplatChanged();

    tile.Normalmap = std::move(normalmap);
    tile.NormalmapWidth = normW;
    tile.NormalmapHeight = normH;
    // The cook-generated normal is valid for the tile's current heights, so record its version. The
    // extraction only re-generates the normal (an O(tileRes^2) cost) when a later modifier bake bumps
    // HeightfieldVersion past this — a freshly-streamed tile skips the re-gen entirely.
    tile.NormalmapVersion = tile.HeightfieldVersion;
    ++slot.Data->Revision;
}

} // namespace GameEngine::TerrainECS
