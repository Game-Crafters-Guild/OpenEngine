#pragma once

#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

struct AreaPiece;

// How far inside its rings an area's lid may run: a corner of the lid's outline leaves out the ring
// samples within this of the straight run past them. A border standing on the rings reaches this
// far in over the lid, so no gap opens at its foot.
inline constexpr float32 kAreaLidInsetMeters = 1.0e-2f;

// An area's ground: its rings sampled along the ground and the lid's points over the area, each
// at its ground height, with the lid's triangles.
struct AreaGround
{
    // The rings' ground samples in world meters, ring after ring (RingEnds), each ring's closing
    // point not repeated. Outer rings run counter-clockwise seen from above (+Y), holes clockwise,
    // so a wall wound out of its ring faces out of the area on both.
    std::vector<Mathematics::Vector3> Outline;
    // Outline on the ground plane (x, z), in the same order.
    std::vector<Mathematics::Vector2> OutlineXZ;
    // One past each ring's last sample in Outline, in order.
    std::vector<uint32> RingEnds;
    // The lid's points at their ground height, the rings' samples first; triangles index them,
    // counter-clockwise from above. Empty with no lid.
    std::vector<Mathematics::Vector3> LidPoints;
    std::vector<uint32> LidTriangles;
    // Each lid point's distance to the nearest ring on the ground plane, meters (0 on it).
    std::vector<float32> LidEdgeDistance;

    std::size_t GetRingCount() const { return RingEnds.size(); }
    std::span<const Mathematics::Vector3> GetRing(std::size_t ring) const;
    std::span<const Mathematics::Vector2> GetRingXZ(std::size_t ring) const;
    // Whether the ground-plane point (x, z) lies in the area: inside an odd number of its rings.
    bool Contains(const Mathematics::Vector2& xz) const;
};

// The ground height at a world (x, z), meters.
using GroundSampler = std::function<float32(const Mathematics::Vector2& xz)>;

// How finely an area's ground is sampled and its lid split.
struct AreaGroundSettings
{
    // The rings are sampled along the ground at most this far apart: the spline drape's step.
    float32 RingSpacingMeters = 0.5f;
    // The most samples one ring takes; the spacing widens past it. The extrude's 4096 m of
    // centerline at the drape's step.
    std::size_t MaxRingSamples = 8192;
    // The lid's triangles are split until no edge is longer than the larger of these two: a
    // length, and a fraction of the area's larger extent (0 for none).
    float32 MaxLidEdgeMeters = 1.0f;
    float32 MaxLidEdgeExtentFraction = 0.0f;
    // The split stops at this many lid triangles; the ring samples put back on the lid's edges
    // after it add at most one triangle each.
    std::size_t MaxLidTriangles = 16384;
};

// `samples` with consecutive samples closer than a millimeter on the ground plane merged, and,
// when `closed`, a repeated closing point dropped.
std::vector<Mathematics::Vector3> MergeGroundSamples(std::span<const Mathematics::Vector3> samples, bool closed);

// The ground of `pieces` (CombineAreas): every ring sampled along the ground at the settings'
// spacing, each sample at `groundAt`, and the lid over the pieces with their holes: the rings'
// corners ear-clipped (the rightmost hole bridged first), then split until no edge is longer than
// the settings allow, the rings' samples put back on its edges. A ring wound the other way (an
// outer ring clockwise, a hole counter-clockwise) is turned first, so Outline keeps the winding it
// documents. The lid's outline passes through every ring sample and stays within
// kAreaLidInsetMeters of the rings: a corner leaves out the samples within it of the straight run
// past them, and a split of that run stands on it. Interior lid points take `groundAt`. No pieces,
// no rings.
AreaGround BuildAreaGround(std::span<const AreaPiece> pieces, const AreaGroundSettings& settings,
                           const GroundSampler& groundAt);

// The ground of one simple ring already sampled along the ground (`ring`: world, y = the ground
// under each sample, counter-clockwise from above, merged by MergeGroundSamples): the ring as
// given and the lid over it, interior points at `groundAt`. The ring's spacing settings are not
// read.
AreaGround BuildRingGround(std::vector<Mathematics::Vector3> ring, const AreaGroundSettings& settings,
                           const GroundSampler& groundAt);

} // namespace GameEngine::SplineGeometry
