#pragma once

#include "TerrainECS/PlanarHeightfieldEditCursor.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "Mathematics/Rect.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::TerrainECS
{

/// The terrain skyline of one eye along a horizontal azimuth: the highest elevation the terrain
/// reaches seen from the eye, as a TANGENT (rise over run), kept across frames.
///
/// This answers "is the sun behind a hill" independently of the frame. A screen-space depth probe
/// cannot: it is gated on the sun projecting inside the viewport, so a sun one degree outside the
/// frame edge is unoccludable by definition. The skyline is a property of the terrain and the eye,
/// so it holds on- and off-frame and past any shadow-cascade distance.
///
/// It marches stations outward from the eye and keeps each station's ground position and height,
/// taken as a coarse answer (AcceptCoarse): a far station reads the pinned level the GPU draws
/// there. The stations form rings of doubling distance (a ring's farthest station is at most twice
/// its nearest). A ring is sampled again when the eye moved sideways more than the angular
/// tolerance times the ring's nearest distance since the ring was sampled, or when the azimuth
/// turned more than the tolerance. One station is sampled again when the terrain under it changed:
/// a page landed or was dropped, or an edit. So each kept station lies within about the tolerance,
/// as an angle seen from the eye, of the line the march would take now. The tangent is taken from
/// the kept stations and the eye as it is now, so a height change of the eye costs no sample, and a
/// frame where nothing moved samples nothing. Main thread only, like the heights.
class TerrainSkyline
{
public:
    /// The value Tangent returns when there is no terrain to see: not "flat", but "this term has
    /// nothing to say", which a caller treats as fully visible.
    static constexpr float32 kNoSkyline = -1.0e30f;

    /// The skyline from `eye` along azimuth (dirX, dirZ) over `maxDistance` meters, with stations
    /// `stepMeters` apart (floored to the terrain's lattice: a finer step interpolates between the
    /// same two corners twice), each kept while it stays within `angularTolerance` radians of the
    /// march's line.
    float32 Tangent(const SurfaceHeightQuery& query, const Mathematics::Vector3& eye, float32 dirX, float32 dirZ,
                    float32 maxDistance, float32 stepMeters, float32 angularTolerance);

    /// The stations the last Tangent call sampled.
    uint32 LastSampleCount() const { return m_LastSampleCount; }

private:
    // Stations [First, First + Count), sampled from (AnchorX, AnchorZ) along (DirX, DirZ).
    struct Ring
    {
        uint32 First = 0;
        uint32 Count = 0;
        float32 AnchorX = 0.0f;
        float32 AnchorZ = 0.0f;
        float32 DirX = 0.0f;
        float32 DirZ = 0.0f;
        bool Sampled = false;
    };

    // What every kept height depends on besides the pages and the edits: another value means
    // another terrain, placement or march, and every station is sampled again.
    struct TerrainKey
    {
        uint64 WholeMap = 0; // SurfaceHeightQuery::WholeMapIdentity
        uint64 Source = 0;
        float32 OriginX = 0.0f;
        float32 OriginZ = 0.0f;
        float32 SizeX = 0.0f;
        float32 SizeZ = 0.0f;
        float32 OriginY = 0.0f;
        float32 HeightScale = 0.0f;
        float32 Step = 0.0f;
        float32 MaxDistance = 0.0f;

        bool operator==(const TerrainKey&) const = default;
    };

    void Clear();
    void Layout();
    void SampleStation(const SurfaceHeightQuery& query, uint32 station);
    void SampleRing(const SurfaceHeightQuery& query, uint32 ring, const Mathematics::Vector3& eye, float32 dirX,
                    float32 dirZ);
    uint32 SampleChangedStations(const SurfaceHeightQuery& query, const Ring& ring);
    float32 MaxTangent(const Mathematics::Vector3& eye) const;

    TerrainKey m_Key;
    std::vector<Ring> m_Rings;
    // Per station, every ring's stations in order: its ground position and its height
    // (kUnsampledHeight where the query had none).
    std::vector<float32> m_StationX;
    std::vector<float32> m_StationZ;
    std::vector<float32> m_Height;
    PlanarHeightfieldEditCursor m_Edits;
    SurfaceHeightPageStamp m_Pages;
    std::vector<Mathematics::Rect> m_Changed; // this call's changed rectangles (reused)
    Mathematics::Vector3 m_Eye;
    float32 m_Tangent = kNoSkyline;
    uint32 m_LastSampleCount = 0;
};

} // namespace GameEngine::TerrainECS
