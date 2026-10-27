#pragma once

#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <functional>
#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// What a generated height field holds.
enum class GeneratedHeightSource : uint8
{
    Noise = 0, ///< the terrain's base noise (Terrain/Heightfield.h, SampleWorldSpaceNoise)
    Flat = 1,  ///< zero everywhere
};

/// The base noise's parameters, as the terrain's tile fill uses them.
struct GeneratedHeightNoise
{
    float32 Frequency = 0.0f;
    float32 Amplitude = 0.0f;
    uint32 Octaves = 0;
    uint32 Seed = 0;
};

/// The lattice of a tiled terrain whose base is generated rather than cooked: its tile grid, whose
/// tiles share their edge samples, and the source. Level 0 is the tiles' own samples.
struct GeneratedHeightLattice
{
    GeneratedHeightSource Source = GeneratedHeightSource::Flat;
    float32 TerrainOriginX = 0.0f; ///< world position of tile (0, 0)'s corner
    float32 TerrainOriginZ = 0.0f;
    float32 TileWorldSize = 0.0f;  ///< meters per tile side
    uint32 TileSamples = 0;        ///< samples per tile side, shared edge included
    uint32 TilesX = 0;
    uint32 TilesZ = 0;
    GeneratedHeightNoise Noise;
};

/// The lowest and highest height over a region, in meters.
struct PageHeightRange
{
    float32 Min = 0.0f;
    float32 Max = 0.0f;
};

/// The lattice of a planet whose base is generated: six EqualAngleCube faces of FaceSamples a
/// side around the reference radius, and the height of the relief at a direction.
struct GeneratedPlanetLattice
{
    float64 ReferenceRadius = 0.0; ///< R, in meters
    /// Level-0 samples along a face's side: m x 2^k + 1 with m at most 128, as PlanetCookRequest.
    uint32 FaceSamples = 0;
    /// h = r - R in meters at a unit direction. Relief whose wavelength on the unit sphere is under
    /// `minResolvedArc` radians is replaced by its mean; 0 keeps all of it. Thread-safe.
    std::function<float32(const Mathematics::Vector3& direction, float32 minResolvedArc)> Height;
    /// The range of Height at every direction within `chord` (a straight-line distance) of a unit
    /// direction, at `minResolvedArc` and at 0. Conservative. Thread-safe.
    std::function<PageHeightRange(const Mathematics::Vector3& direction, float32 chord, float32 minResolvedArc)>
        HeightRange;
};

/// The page provider of a terrain whose base is noise or flat: a page is evaluated on demand, with
/// no store and no cook, the same in the editor and the Player.
///
/// Level 0 is byte-identical to the terrain's tile fill (TerrainECS FillTiledBaseRegion): a sample
/// is evaluated at the world position its owning tile computes for it (a tile owns its samples
/// but its last row and column, which the next tile owns; the grid's last tile owns its own). A
/// coarser level is evaluated at its own spacing with the octaves finer than twice that spacing
/// band-limited away, never filtered from level 0. Samples past the lattice's edge repeat the
/// edge position.
///
/// A planet's page holds h = r - R at each sample's direction (EqualAngleSampleDirection), band-limited
/// as on the plane. An apron sample past a face's side is the neighbor face's (PageFaceNeighbor), past
/// a cube corner the corner sample. Thread-safe: FillPage is a pure function of the lattice and address.
class GeneratedHeightPageProvider
{
public:
    explicit GeneratedHeightPageProvider(const GeneratedHeightLattice& lattice);
    /// A planet's provider; empty (every FillPage false) when the lattice is not valid.
    explicit GeneratedHeightPageProvider(GeneratedPlanetLattice planet);

    const GeneratedHeightLattice& Lattice() const { return m_Lattice; }
    /// Faces of the pyramid: six on a planet, else one.
    uint32 FaceCount() const { return m_Planet.Height ? kPageStoreMaxFaces : 1u; }
    /// The pyramid of the lattice, as a store of its level-0 size would have it.
    const std::vector<PageStoreLevel>& Levels() const { return m_Levels; }

    /// Fills a page's kPageSampleCount samples (row-major, apron included): normalized heights on
    /// the plane, meters above R on a planet.
    /// False when the address is outside the pyramid or the lattice is empty.
    bool FillPage(const PageAddress& address, std::span<float32> outSamples) const;

    /// A planet page's height range: every sample FillPage writes to it, apron included, and the
    /// relief at every level under it. Conservative. The address must be inside the pyramid.
    PageHeightRange PlanetPageHeightRange(const PageAddress& address) const;

private:
    float32 WorldX(uint32 level0Index) const;
    float32 WorldZ(uint32 level0Index) const;
    void FillPlanetPage(const PageAddress& address, std::span<float32> outSamples) const;
    float64 PlanetLevel0Arc() const;
    float32 PlanetMinResolvedArc(uint32 level) const;

    GeneratedHeightLattice m_Lattice;
    GeneratedPlanetLattice m_Planet;
    std::vector<PageStoreLevel> m_Levels;
    float32 m_Level0Spacing = 0.0f;
};

} // namespace GameEngine::PageStreaming
