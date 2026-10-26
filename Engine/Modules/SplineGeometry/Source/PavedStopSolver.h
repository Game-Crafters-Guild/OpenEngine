#pragma once

#include "PavedArmPath.h"
#include "PavedTaggedRing.h"

#include "SplineGeometry/PavedFootprint.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

// The arms grown to their approach stops, with the outlines they grow against.
class PavedStopSolver
{
  public:
    // `armValid` holds 1 for each arm that is not a sliver; a sliver is never grown or met.
    PavedStopSolver(std::span<const PavedArm> arms, std::span<const uint8> armValid,
                    std::vector<PavedTaggedRing> outlines);

    // Grows the stops until none moves. A stop moves only when a later station of its arm starts
    // to overlap, so the passes end within the stations' count.
    void Solve();

    const PavedArmPath& GetPath(uint32 arm) const { return m_Paths[arm]; }
    float32 GetStop(uint32 arm) const { return m_Stops[arm]; }
    const PavedTaggedRing& GetRibbon(uint32 arm) const { return m_Ribbons[arm]; }
    const PavedTaggedRing& GetOutline(uint32 outline) const { return m_Outlines[outline]; }
    // Item i is outline i below the outlines' count, else arm i less it; a sliver's ring is empty.
    const PavedTaggedRing& GetItem(uint32 item) const
    {
        return item < m_Outlines.size() ? m_Outlines[item] : m_Ribbons[item - m_Outlines.size()];
    }

  private:
    void SetStop(uint32 arm, float32 stop);

    // Whether another arm of the same road covers the road's arc length within the section's
    // width of `section`: the same band, which never overlaps itself.
    bool IsSameBand(uint32 arm, uint32 other, const PavedArmSection& section) const;

    // Whether the cross-section at `station` overlaps an outline or another arm's ribbon.
    bool StationOverlaps(uint32 arm, std::size_t station) const;

    std::span<const PavedArm> m_Arms;
    std::span<const uint8> m_ArmValid;
    std::vector<PavedTaggedRing> m_Outlines;
    std::vector<PavedArmPath> m_Paths;
    std::vector<std::size_t> m_LastOverlap;
    std::vector<float32> m_Stops;
    std::vector<PavedTaggedRing> m_Ribbons;
};

} // namespace GameEngine::SplineGeometry
