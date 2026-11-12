#include "PageStreaming/GeneratedHeightPageProvider.h"

#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"
#include "Terrain/Heightfield.h"

#include <algorithm>
#include <cassert>
#include <numbers>

namespace GameEngine::PageStreaming
{
namespace
{

// A tile axis's world coordinate of its sample `index`, as the tile fill computes it: the tile's
// corner (terrain corner + tile * tile size, TileStreamingManager's expression) plus the index
// times the tile's spacing (FillRegionWithNoiseWorldSpace's expression). Each operation is the
// fill's own, in its order, so the result is bit-identical.
float32 TileSampleWorld(float32 terrainOrigin, float32 tileWorldSize, uint32 tileSamples, uint32 tiles,
                        uint32 level0Index)
{
    const uint32 interval = tileSamples - 1u;
    const uint32 tile = std::min(level0Index / interval, tiles - 1u);
    const uint32 local = level0Index - tile * interval;
    const float32 tileOrigin = terrainOrigin + static_cast<float32>(static_cast<int32>(tile)) * tileWorldSize;
    const float32 spacing = tileWorldSize / static_cast<float32>(interval);
    return tileOrigin + static_cast<float32>(local) * spacing;
}

// The level-0 index of level `level`'s sample `index` (which may lie in the apron past either edge),
// clamped to the lattice.
uint32 Level0Index(int64 index, uint32 level, uint32 level0Samples)
{
    const int64 position = std::max<int64>(index, 0) << level;
    return static_cast<uint32>(std::min<int64>(position, static_cast<int64>(level0Samples) - 1));
}

// The gnomonic coordinate of level `level`'s sample `index` of a face of `level0Samples` a side.
float64 PlanetGnomonic(int64 index, uint32 level, uint32 level0Samples)
{
    return GnomonicFaceCoordinate(EqualAngleSampleCoordinate(static_cast<uint32>(index) << level, level0Samples));
}

// The sample steps from a page's region to its farthest apron sample: one along each axis.
constexpr float64 kApronSteps = 2.0;

} // namespace

GeneratedHeightPageProvider::GeneratedHeightPageProvider(GeneratedPlanetLattice planet)
    : m_Planet(std::move(planet))
{
    if (!m_Planet.HeightRow || !m_Planet.HeightRange || !PlanetFaceSamplesHalve(m_Planet.FaceSamples) || !(m_Planet.ReferenceRadius > 0.0))
    {
        m_Planet.HeightRow = nullptr;
        m_Planet.HeightRange = nullptr;
        return;
    }
    m_Levels = BuildPageStoreLevels(m_Planet.FaceSamples, m_Planet.FaceSamples);
}

float64 GeneratedHeightPageProvider::PlanetLevel0Arc() const
{
    // A face spans a quarter turn.
    return 0.5 * std::numbers::pi / static_cast<float64>(m_Levels.front().SamplesX - 1u);
}

float32 GeneratedHeightPageProvider::PlanetMinResolvedArc(uint32 level) const
{
    // A coarser level drops the relief under two of its sample intervals.
    return level == 0 ? 0.0f : static_cast<float32>(2.0 * PlanetLevel0Arc() * static_cast<float64>(1u << level));
}

PageHeightRange GeneratedHeightPageProvider::PlanetPageHeightRange(const PageAddress& address) const
{
    assert(m_Planet.HeightRange && address.Face < kPageStoreMaxFaces && address.Level < m_Levels.size() &&
           address.X < m_Levels[address.Level].PagesX && address.Z < m_Levels[address.Level].PagesZ);
    const uint32 last = m_Levels.front().SamplesX - 1u;
    const uint32 span = kPageOwnedSamples << address.Level;
    const uint32 x0 = std::min(address.X * span, last);
    const uint32 z0 = std::min(address.Z * span, last);
    const uint32 x1 = std::min(x0 + span, last);
    const uint32 z1 = std::min(z0 + span, last);
    const Mathematics::Vector3 corners[4] = {
        EqualAngleSampleDirection(address.Face, x0, z0, last + 1u), EqualAngleSampleDirection(address.Face, x1, z0, last + 1u),
        EqualAngleSampleDirection(address.Face, x1, z1, last + 1u), EqualAngleSampleDirection(address.Face, x0, z1, last + 1u)};
    const Mathematics::Vector3 center = (corners[0] + corners[1] + corners[2] + corners[3]).Normalize();
    // The page's sides are great circles, so its farthest point from the center is a corner. An
    // apron sample is at most two sample steps from the region (the diagonal corner), and one step
    // spans at most PlanetLevel0Arc() radians a level-0 sample.
    float32 chord = 0.0f;
    for (const Mathematics::Vector3& corner : corners)
        chord = std::max(chord, (corner - center).Length());
    chord += static_cast<float32>(kApronSteps * PlanetLevel0Arc() * static_cast<float64>(1u << address.Level));
    return m_Planet.HeightRange(center, chord, PlanetMinResolvedArc(address.Level));
}

bool GeneratedHeightPageProvider::FillPlanetWindow(const PageAddress& address, uint32 margin,
                                                   std::span<float32> outSamples,
                                                   std::span<Mathematics::Vector3> outDirections) const
{
    constexpr uint32 kMaxStride = kPageOwnedSamples + 2u * kPageSurfaceWindowMarginSamples;
    const uint32 stride = kPageOwnedSamples + 2u * margin;
    if (!m_Planet.HeightRow || margin > kPageSurfaceWindowMarginSamples || address.Face >= kPageStoreMaxFaces ||
        address.Level >= m_Levels.size() || address.X >= m_Levels[address.Level].PagesX ||
        address.Z >= m_Levels[address.Level].PagesZ || outSamples.size() != static_cast<std::size_t>(stride) * stride ||
        (!outDirections.empty() && outDirections.size() != outSamples.size()))
        return false;
    const uint32 level0Samples = m_Levels.front().SamplesX;
    const int64 last = static_cast<int64>(m_Levels[address.Level].SamplesX) - 1;
    const float32 minResolvedArc = PlanetMinResolvedArc(address.Level);
    const int64 firstX = static_cast<int64>(address.X) * kPageOwnedSamples - margin;
    const int64 firstZ = static_cast<int64>(address.Z) * kPageOwnedSamples - margin;
    // The gnomonic coordinate of each column and row on the page's own face, solved once a window.
    float64 columns[kMaxStride];
    float64 rows[kMaxStride];
    for (uint32 i = 0; i < stride; ++i)
    {
        columns[i] = PlanetGnomonic(std::clamp<int64>(firstX + i, 0, last), address.Level, level0Samples);
        rows[i] = PlanetGnomonic(std::clamp<int64>(firstZ + i, 0, last), address.Level, level0Samples);
    }
    Mathematics::Vector3 rowScratch[kMaxStride];
    for (uint32 row = 0; row < stride; ++row)
    {
        const std::size_t rowStart = static_cast<std::size_t>(row) * stride;
        const std::span<Mathematics::Vector3> directions =
            outDirections.empty() ? std::span<Mathematics::Vector3>(rowScratch, stride) : outDirections.subspan(rowStart, stride);
        const int64 z = firstZ + row;
        const bool outsideZ = z < 0 || z > last;
        for (uint32 i = 0; i < stride; ++i)
        {
            const int64 x = firstX + i;
            const bool outsideX = x < 0 || x > last;
            if (outsideX != outsideZ)
            {
                const PageFaceSample sample = PageCubeWindowSampleAt(address.Face, x, z, static_cast<uint32>(last + 1), margin);
                directions[i] = EqualAngleSampleDirection(sample.Face, sample.X << address.Level,
                                                          sample.Z << address.Level, level0Samples);
            }
            else
                directions[i] = GnomonicFaceDirection(address.Face, columns[i], rows[row]);
        }
        m_Planet.HeightRow(directions, minResolvedArc, outSamples.subspan(rowStart, stride));
    }
    return true;
}

GeneratedHeightPageProvider::GeneratedHeightPageProvider(const GeneratedHeightLattice& lattice)
    : m_Lattice(lattice)
{
    if (lattice.TileSamples < 2 || lattice.TilesX == 0 || lattice.TilesZ == 0 || !(lattice.TileWorldSize > 0.0f))
        return;
    const uint32 interval = lattice.TileSamples - 1u;
    m_Levels = BuildPageStoreLevels(lattice.TilesX * interval + 1u, lattice.TilesZ * interval + 1u);
    m_Level0Spacing = lattice.TileWorldSize / static_cast<float32>(interval);
}

float32 GeneratedHeightPageProvider::WorldX(uint32 level0Index) const
{
    return TileSampleWorld(m_Lattice.TerrainOriginX, m_Lattice.TileWorldSize, m_Lattice.TileSamples, m_Lattice.TilesX,
                           level0Index);
}

float32 GeneratedHeightPageProvider::WorldZ(uint32 level0Index) const
{
    return TileSampleWorld(m_Lattice.TerrainOriginZ, m_Lattice.TileWorldSize, m_Lattice.TileSamples, m_Lattice.TilesZ,
                           level0Index);
}

bool GeneratedHeightPageProvider::FillPage(const PageAddress& address, std::span<float32> outSamples) const
{
    if (m_Levels.empty() || address.Face >= FaceCount() || address.Level >= m_Levels.size() ||
        outSamples.size() != kPageSampleCount)
        return false;
    const PageStoreLevel& level = m_Levels[address.Level];
    if (address.X >= level.PagesX || address.Z >= level.PagesZ)
        return false;
    if (m_Planet.HeightRow)
        return FillPlanetWindow(address, kPageApronSamples, outSamples, {});

    const Terrain::HeightfieldData* heightmap = m_Lattice.Heightmap.get();
    const bool heightmapSource = m_Lattice.Source == GeneratedHeightSource::Heightmap;
    if (m_Lattice.Source == GeneratedHeightSource::Flat ||
        (m_Lattice.Source == GeneratedHeightSource::Function && !m_Lattice.FunctionRow) ||
        (heightmapSource && (!heightmap || heightmap->IsEmpty() || !(m_Lattice.AuthoredSizeX > 0.0f) ||
                             !(m_Lattice.AuthoredSizeZ > 0.0f))))
    {
        std::fill(outSamples.begin(), outSamples.end(), 0.0f);
        return true;
    }

    // Level 0 evaluates every octave, the tile fill's value; a coarser level drops the octaves its
    // spacing cannot carry (a lattice cell under two of its samples).
    const float32 minResolvedCell =
        address.Level == 0 ? 0.0f : 2.0f * m_Level0Spacing * static_cast<float32>(1u << address.Level);
    const uint32 level0SamplesX = m_Levels.front().SamplesX;
    const uint32 level0SamplesZ = m_Levels.front().SamplesZ;
    const int64 firstX = static_cast<int64>(address.X) * kPageOwnedSamples - kPageApronSamples;
    const int64 firstZ = static_cast<int64>(address.Z) * kPageOwnedSamples - kPageApronSamples;

    float32 rowWorldX[kPageStrideSamples];
    for (uint32 i = 0; i < kPageStrideSamples; ++i)
    {
        const int64 sample = std::min<int64>(firstX + i, static_cast<int64>(level.SamplesX) - 1);
        rowWorldX[i] = WorldX(Level0Index(sample, address.Level, level0SamplesX));
    }
    for (uint32 row = 0; row < kPageStrideSamples; ++row)
    {
        const int64 sample = std::min<int64>(firstZ + row, static_cast<int64>(level.SamplesZ) - 1);
        const float32 worldZ = WorldZ(Level0Index(sample, address.Level, level0SamplesZ));
        float32* out = outSamples.data() + static_cast<std::size_t>(row) * kPageStrideSamples;
        if (m_Lattice.Source == GeneratedHeightSource::Function)
        {
            // Terrain-local positions, formed as the tile fill forms them (TerrainECS FillTiledBaseRegion).
            float32 rowLocalX[kPageStrideSamples];
            for (uint32 i = 0; i < kPageStrideSamples; ++i)
                rowLocalX[i] = rowWorldX[i] - m_Lattice.TerrainOriginX;
            if (!m_Lattice.FunctionRow(worldZ - m_Lattice.TerrainOriginZ, rowLocalX, minResolvedCell,
                                       std::span<float32>(out, kPageStrideSamples)))
                return false;
            continue;
        }
        if (heightmapSource)
        {
            // The tile fill's footprint UV (TerrainECS FillTiledBaseRegion): the offset from the
            // terrain's corner over the authored size, clamped where the tile grid overhangs it.
            const float32 v = Math::Clamp01((worldZ - m_Lattice.TerrainOriginZ) / m_Lattice.AuthoredSizeZ);
            for (uint32 i = 0; i < kPageStrideSamples; ++i)
                out[i] = heightmap->SampleBilinear(
                    Math::Clamp01((rowWorldX[i] - m_Lattice.TerrainOriginX) / m_Lattice.AuthoredSizeX), v);
            continue;
        }
        for (uint32 i = 0; i < kPageStrideSamples; ++i)
            out[i] = Terrain::SampleWorldSpaceNoise(rowWorldX[i], worldZ, m_Lattice.Noise, minResolvedCell);
    }
    return true;
}

} // namespace GameEngine::PageStreaming
