#pragma once

#include "Mathematics/Vector2.h"
#include "SplineGeometry/AreaBoolean.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

// Two points, or a point and an edge, closer than this on the ground plane touch: the spline
// layout's touching tolerance, in meters.
inline constexpr float32 kPavedTouchMeters = 1.0e-3f;

// How far an arm's approach stop lies past the last station at which its ribbon overlaps the
// region, in the arm's own widths, so an acute corner's miter is cleared and the border ends
// square there.
inline constexpr float32 kApproachStopAdvanceWidths = 1.0f;

// One station of an arm's centerline on the ground plane (x, z).
struct PavedArmStation
{
    Mathematics::Vector2 Position{};
    // Meters to each side of travel outward from the node; right is +X when traveling +Z.
    float32 HalfWidthLeft = 0.0f;
    float32 HalfWidthRight = 0.0f;
    // The road's own arc length here, meters; it rises or falls along the arm, and on a closed road
    // wraps at the arm's RoadLoopMeters.
    float32 Distance = 0.0f;
};

// One arm of a node: a road's ribbon (its width band) from the node outward.
struct PavedArm
{
    // The road the arm belongs to. Two arms of one road over the same arc length are one band and
    // never overlap each other.
    uint32 Road = 0;
    // Where the node is. A first station farther than kPavedTouchMeters from it (a road attached
    // short of the body) is reached by a straight run at the first station's widths.
    Mathematics::Vector2 Node{};
    // From the node outward: to the road's end when EndsRoad, else as far as the caller passes the
    // road on (to its next node, or past where any region can reach).
    std::vector<PavedArmStation> Stations;
    // The last station is the road's end: a stop there is a dead end, bordered across. Otherwise
    // the road goes on past the last station, and the arm's cap at its stop is Open wherever the
    // stop falls.
    bool EndsRoad = false;
    // The road's length in meters when it is a closed loop, else 0: the stations' arc length then
    // wraps from RoadLoopMeters to 0, and arc lengths compare modulo it.
    float32 RoadLoopMeters = 0.0f;
};

// What a paving region is made of. Indices in the output name these spans' entries.
struct PavedFootprintInput
{
    // Closed outlines whose interiors are paved whole (an Area's outline).
    std::span<const AreaRing> Outlines;
    std::span<const PavedArm> Arms;
    // Closed rings cut out of the paving (a planting bed).
    std::span<const AreaRing> Excludes;
};

// Where a boundary edge comes from.
enum class PavedEdgeSource : uint8
{
    Outline = 0,
    // A side of an arm's ribbon.
    ArmSide,
    // An arm's end cap that gets a border: the node's cap, or the far cap of an arm that ends its
    // road (EndsRoad) at its stop.
    ArmEnd,
    // An arm's cap at its approach stop, where the road goes on: no border crosses it.
    Open,
    Exclude,
    // On no input edge: one the boolean made. None is expected; a test asserts it.
    New,
};

struct PavedEdge
{
    PavedEdgeSource Source = PavedEdgeSource::New;
    // The outline, arm or exclude, by its index in the input.
    uint32 Index = 0;
};

// One maximal chain of boundary edges that are not Open, in its ring's winding: an outer ring
// counter-clockwise from above, a hole clockwise, so the right of travel is out of the paving.
struct PavedBoundaryRun
{
    std::vector<Mathematics::Vector2> Points;
    // Edges[i] runs from Points[i] to Points[i + 1], and the last of a closed run back to Points[0].
    std::vector<PavedEdge> Edges;
    // 1 where a point is an intersection the boolean made (on no input point).
    std::vector<uint8> NewPoints;
    // A whole ring with no Open edge: a rim with no approach, or a bed's edge.
    bool Closed = false;
    bool Hole = false;
};

// The overlapping footprints built once: the pieces to pave and the runs to border.
struct PavedRegion
{
    std::vector<uint32> Outlines;
    std::vector<uint32> Arms;
    std::vector<AreaPiece> Pieces;
    std::vector<PavedBoundaryRun> Runs;
};

// The region of an arm that is in none: a sliver.
inline constexpr uint32 kNoPavedRegion = ~0u;

// An arm as the region paves it. The road's own strip skips the arc length from NodeDistance to
// StopDistance in the arm's direction of travel (modulo RoadLoopMeters on a closed road, so the
// interval can cross the road's seam) and, when Open, caps square at the Open edge.
struct PavedArmStop
{
    uint32 Region = kNoPavedRegion;
    float32 NodeDistance = 0.0f;
    float32 StopDistance = 0.0f;
    // The arm's cap at the stop reaches the region's boundary and the road goes on past it.
    bool Open = false;
    // The cap's two ends, left and right of travel outward.
    Mathematics::Vector2 OpenLeft{};
    Mathematics::Vector2 OpenRight{};
};

enum class PavedFootprintIssueKind : uint8
{
    // Under three points, or a mean width (twice the area over the perimeter) under 1 cm.
    SliverOutline = 0,
    // No station farther than kPavedTouchMeters from the node (no length), or under 1 cm wide at
    // every station.
    SliverArm,
    SliverExclude,
};

// An input left out of the paving, and why.
struct PavedFootprintIssue
{
    PavedFootprintIssueKind Kind = PavedFootprintIssueKind::SliverOutline;
    uint32 Index = 0;
};

struct PavedFootprint
{
    std::vector<PavedRegion> Regions;
    // One per input arm, in order; a sliver arm's is default, its Region kNoPavedRegion.
    std::vector<PavedArmStop> Arms;
    std::vector<PavedFootprintIssue> Issues;
};

// The paving of `input`. Each arm is trimmed at its approach stop: the station after the last one
// at which its cross-section overlaps another arm's trimmed ribbon or an outline, advanced by
// kApproachStopAdvanceWidths of its width, found by growing every arm from its node until no stop
// moves. The trimmed ribbons and the outlines that overlap or touch within kPavedTouchMeters form
// regions; each region is one CombineTouchingAreas of its ribbons and outlines minus the excludes,
// welded at kPavedTouchMeters with the 1 cm sliver bound, so what touches is one piece with no gap
// or sliver between. Every boundary edge is then matched to the input edge it lies on within
// kPavedTouchMeters, the Open tag winning where several match, and the boundary is cut into runs
// at the Open edges. An outline that no arm touches is a region of its own, bordered by a closed
// run.
PavedFootprint BuildPavedFootprint(const PavedFootprintInput& input);

} // namespace GameEngine::SplineGeometry
