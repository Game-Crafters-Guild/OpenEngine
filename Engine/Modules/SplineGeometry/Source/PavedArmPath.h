#pragma once

#include "Mathematics/Vector2.h"
#include "SplineGeometry/PavedFootprint.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::SplineGeometry
{

// The cross-section of an arm at one place along it.
struct PavedArmSection
{
    Mathematics::Vector2 Left;
    Mathematics::Vector2 Right;
    float32 Distance = 0.0f;
    float32 Width = 0.0f;
};

// An arm resolved for paving: its stations from the node (a straight run from the node prepended
// when the first station is short of it), how far along the arm each lies, and their
// cross-sections, each square to the chord through its neighbors. On a closed road the stations'
// arc lengths are unwrapped, so they run on across the road's seam.
class PavedArmPath
{
  public:
    explicit PavedArmPath(const PavedArm& arm);

    // Squares the node's cross-section to the chord through both arms' second stations when
    // `partner` leaves the same node along the same road the other way (its arc length runs the
    // other way), so the two arms share their node cap the way two stations of one road share a
    // cross-section.
    void ShareNodeSection(const PavedArmPath& partner);

    std::size_t GetStationCount() const { return m_Stations.size(); }
    float32 GetLength() const { return m_Along.empty() ? 0.0f : m_Along.back(); }
    float32 GetAlong(std::size_t station) const { return m_Along[station]; }
    const PavedArmSection& GetSection(std::size_t station) const { return m_Sections[station]; }

    // The cross-section `along` meters from the node, square to the arm's chord there.
    PavedArmSection SectionAt(float32 along) const;

    // The approach stop when station `lastOverlap` is the last one whose cross-section overlaps
    // the region: the next station, advanced by the arm's width there, at most the arm's end.
    float32 StopAfter(std::size_t lastOverlap) const;

    // Whether the road goes on past the stop `along` meters from the node.
    bool GoesOnPast(float32 along) const { return !m_EndsRoad || along < GetLength() - kPavedTouchMeters; }

    // The road's length when it is a closed loop, else 0.
    float32 GetLoopMeters() const { return m_LoopMeters; }

    // An unwrapped arc length of the road as the road's own: in [0, loop) on a closed road.
    float32 RoadDistance(float32 unwrapped) const;

  private:
    std::vector<PavedArmStation> m_Stations;
    std::vector<float32> m_Along;
    std::vector<PavedArmSection> m_Sections;
    float32 m_LoopMeters = 0.0f;
    bool m_EndsRoad = false;
};

} // namespace GameEngine::SplineGeometry
