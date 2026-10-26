#pragma once

#include "SplineGeometry/AreaGround.h"
#include "SplineGeometry/SplineCorner.h"
#include "SplineGeometry/SplineProfile.h"
#include "SplineGeometry/SplineStation.h"
#include "SplineGeometry/SplineStripBuilder.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

struct PavedBoundaryRun;

// The border a paved region's boundary runs carry.
struct PavedBorderSettings
{
    // Meters across the border, out from the boundary, and above the lid.
    float32 Width = 0.3f;
    float32 Height = 0.12f;
    // The turn at every point the boolean made and every corner of the boundary.
    SplineCornerStyle Corner = SplineCornerStyle::Mitre;
    // Stations along a run at most this far apart, so the border follows the ground: the drape's
    // step.
    float32 StationSpacingMeters = 0.5f;
};

// One run's border, ready for BuildSplineStrip with BuildPavedBorderProfile.
struct PavedBorderRun
{
    // In world meters. Each stands on the boundary at the ground's height there; the border runs
    // from kAreaLidInsetMeters to its left, over the lid's edge, to Width to the right of travel,
    // out of the paving (into the hole on a hole's run), and stands Height above it, its base sunk
    // where the ground falls away under it.
    std::vector<SplineStripStation> Stations;
    // U along the run in world meters from its first station; a closed run's faces each span a
    // whole number of tiles; an open run is capped at both ends, square to its last edges.
    SplineStripParams Strip;
    // The corners the sweep could not build as asked (ApplyCorners).
    std::vector<SplineCornerIssue> CornerIssues;
    bool Closed = false;
};

// The border's cross-section: a Rectangle `settings.Width` wide and `settings.Height` tall whose
// base points are Grounded, placed by the stations' per-side half widths (kAreaLidInsetMeters to
// the left, Width to the right).
SplineProfile BuildPavedBorderProfile(const PavedBorderSettings& settings);

// The border of each run (PavedFootprint): its points sampled at most StationSpacingMeters apart
// at `groundAt`'s height, the Corner style at every point the boolean made and every turn past
// 15 degrees with the corner module's miter limit, a closed run closed on itself (CloseStationLoop)
// from the middle of its longest edge with its U snapped to whole tiles, and every face's U along
// its own length (AccumulateFaceTurn).
std::vector<PavedBorderRun> BuildPavedBorder(std::span<const PavedBoundaryRun> runs,
                                             const PavedBorderSettings& settings, const GroundSampler& groundAt);

} // namespace GameEngine::SplineGeometry
