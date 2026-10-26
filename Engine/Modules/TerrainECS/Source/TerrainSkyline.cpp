#include "TerrainSkyline.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainECS
{

namespace
{

// A station's height where the query had none (outside the footprint, or no page yet).
constexpr float32 kUnsampledHeight = -1.0e30f;

// Clamps the step from below so a degenerate lattice cannot spin the march.
constexpr float32 kMinimumStepMeters = 0.25f;

// How far two unit azimuths are apart, as the sine of the angle between them; 2 when they point
// more than a right angle apart.
float32 TurnBetween(float32 ax, float32 az, float32 bx, float32 bz)
{
    if (ax * bx + az * bz <= 0.0f)
        return 2.0f;
    return std::abs(ax * bz - az * bx);
}

} // namespace

void TerrainSkyline::Clear()
{
    m_Key = TerrainKey{};
    m_Rings.clear();
    m_StationX.clear();
    m_StationZ.clear();
    m_Height.clear();
}

void TerrainSkyline::Layout()
{
    // Station i sits at (i + 1) x step; ring k holds stations [2^k - 1, 2^(k+1) - 1), the distances
    // [step x 2^k, step x 2^(k+1)).
    const uint32 stations = static_cast<uint32>(std::floor(m_Key.MaxDistance / m_Key.Step));
    m_Rings.clear();
    for (uint32 first = 0; first < stations; first = first * 2u + 1u)
        m_Rings.push_back(Ring{first, std::min(first + 1u, stations - first)});
    m_StationX.assign(stations, 0.0f);
    m_StationZ.assign(stations, 0.0f);
    m_Height.assign(stations, kUnsampledHeight);
}

void TerrainSkyline::SampleStation(const SurfaceHeightQuery& query, uint32 station)
{
    SurfaceHeightSample sample;
    const bool sampled = query.SampleHeight(Mathematics::Vector3(m_StationX[station], 0.0f, m_StationZ[station]),
                                            SurfaceHeightPolicy::AcceptCoarse, sample);
    m_Height[station] = sampled ? sample.Height : kUnsampledHeight;
}

void TerrainSkyline::SampleRing(const SurfaceHeightQuery& query, uint32 ringIndex, const Mathematics::Vector3& eye,
                                float32 dirX, float32 dirZ)
{
    Ring& ring = m_Rings[ringIndex];
    ring.AnchorX = eye.x;
    ring.AnchorZ = eye.z;
    ring.DirX = dirX;
    ring.DirZ = dirZ;
    ring.Sampled = true;
    for (uint32 station = ring.First; station < ring.First + ring.Count; ++station)
    {
        const float32 distance = static_cast<float32>(station + 1u) * m_Key.Step;
        m_StationX[station] = eye.x + dirX * distance;
        m_StationZ[station] = eye.z + dirZ * distance;
        SampleStation(query, station);
    }
    m_LastSampleCount += ring.Count;
}

uint32 TerrainSkyline::SampleChangedStations(const SurfaceHeightQuery& query, const Ring& ring)
{
    uint32 sampled = 0;
    for (uint32 station = ring.First; station < ring.First + ring.Count; ++station)
    {
        const float32 x = m_StationX[station];
        const float32 z = m_StationZ[station];
        const bool changed = std::any_of(m_Changed.begin(), m_Changed.end(), [x, z](const Mathematics::Rect& rect) {
            return x >= rect.X && x <= rect.Right() && z >= rect.Y && z <= rect.Bottom();
        });
        if (!changed)
            continue;
        SampleStation(query, station);
        ++sampled;
    }
    return sampled;
}

float32 TerrainSkyline::MaxTangent(const Mathematics::Vector3& eye) const
{
    float32 best = kNoSkyline;
    bool sampledAny = false;
    const float32 nearest = 0.5f * m_Key.Step;
    for (std::size_t station = 0; station < m_Height.size(); ++station)
    {
        if (m_Height[station] == kUnsampledHeight)
            continue; // outside the footprint or no page yet: not ground, so the march learns nothing here
        const float32 dx = m_StationX[station] - eye.x;
        const float32 dz = m_StationZ[station] - eye.z;
        const float32 distance = std::max(std::sqrt(dx * dx + dz * dz), nearest);
        best = std::max(best, (m_Height[station] - eye.y) / distance);
        sampledAny = true;
    }
    return sampledAny ? best : kNoSkyline;
}

float32 TerrainSkyline::Tangent(const SurfaceHeightQuery& query, const Mathematics::Vector3& eye, float32 dirX,
                                float32 dirZ, float32 maxDistance, float32 stepMeters, float32 angularTolerance)
{
    m_LastSampleCount = 0;
    const float32 dirLength = std::sqrt(dirX * dirX + dirZ * dirZ);
    if (!query.IsValid() || maxDistance <= 0.0f || dirLength < 1e-6f)
    {
        Clear();
        return m_Tangent = kNoSkyline;
    }
    const float32 nx = dirX / dirLength;
    const float32 nz = dirZ / dirLength;

    const float32 lattice = std::min(query.LatticeSpacingX(), query.LatticeSpacingZ());
    const TerrainKey key{query.WholeMapIdentity(),
                         query.Edits().Source,
                         query.FootprintOriginX(),
                         query.FootprintOriginZ(),
                         query.FootprintSizeX(),
                         query.FootprintSizeZ(),
                         query.OriginY(),
                         query.HeightScale(),
                         std::max({stepMeters, lattice, kMinimumStepMeters}),
                         maxDistance};

    // Another terrain, placement or march, or a page record that no longer reaches back to the last
    // call: every station is sampled again. Otherwise the rectangles the terrain changed in since.
    m_Changed.clear();
    bool sampleAll = key != m_Key;
    if (sampleAll)
    {
        m_Key = key;
        Layout();
        m_Edits.Reset(query);
    }
    else
    {
        m_Edits.CollectEdits(query, m_Changed);
        sampleAll = !query.CollectPageChanges(m_Pages, m_Changed);
    }
    m_Pages = query.PageStamp();

    for (uint32 ringIndex = 0; ringIndex < m_Rings.size(); ++ringIndex)
    {
        const Ring& ring = m_Rings[ringIndex];
        const float32 allowed = angularTolerance * static_cast<float32>(ring.First + 1u) * m_Key.Step;
        const float32 movedX = eye.x - ring.AnchorX;
        const float32 movedZ = eye.z - ring.AnchorZ;
        const bool stale = sampleAll || !ring.Sampled || movedX * movedX + movedZ * movedZ > allowed * allowed ||
                           TurnBetween(nx, nz, ring.DirX, ring.DirZ) > angularTolerance;
        if (stale)
            SampleRing(query, ringIndex, eye, nx, nz);
        else if (!m_Changed.empty())
            m_LastSampleCount += SampleChangedStations(query, ring);
    }

    if (m_LastSampleCount == 0 && eye.x == m_Eye.x && eye.y == m_Eye.y && eye.z == m_Eye.z)
        return m_Tangent;
    m_Eye = eye;
    return m_Tangent = MaxTangent(eye);
}

} // namespace GameEngine::TerrainECS
