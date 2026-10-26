#include "PavedStopSolver.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineGeometry
{

PavedStopSolver::PavedStopSolver(std::span<const PavedArm> arms, std::span<const uint8> armValid,
                                 std::vector<PavedTaggedRing> outlines)
    : m_Arms(arms), m_ArmValid(armValid), m_Outlines(std::move(outlines))
{
    m_Paths.reserve(arms.size());
    for (const PavedArm& arm : arms)
        m_Paths.emplace_back(arm);
    for (uint32 arm = 0; arm < arms.size(); ++arm)
    {
        for (uint32 other = 0; other < arms.size(); ++other)
        {
            if (other != arm && arms[other].Road == arms[arm].Road)
                m_Paths[arm].ShareNodeSection(m_Paths[other]);
        }
    }
    m_LastOverlap.assign(arms.size(), 0);
    m_Stops.assign(arms.size(), 0.0f);
    m_Ribbons.resize(arms.size());
    // The node's own station always counts: the arms meet there.
    for (uint32 arm = 0; arm < arms.size(); ++arm)
    {
        if (m_ArmValid[arm])
            SetStop(arm, m_Paths[arm].StopAfter(0));
    }
}

void PavedStopSolver::Solve()
{
    bool moved = true;
    while (moved)
    {
        moved = false;
        for (uint32 arm = 0; arm < m_Paths.size(); ++arm)
        {
            if (!m_ArmValid[arm])
                continue;
            std::size_t last = m_LastOverlap[arm];
            for (std::size_t station = last + 1; station < m_Paths[arm].GetStationCount(); ++station)
            {
                if (StationOverlaps(arm, station))
                    last = station;
            }
            if (last == m_LastOverlap[arm])
                continue;
            m_LastOverlap[arm] = last;
            SetStop(arm, m_Paths[arm].StopAfter(last));
            moved = true;
        }
    }
}

void PavedStopSolver::SetStop(uint32 arm, float32 stop)
{
    m_Stops[arm] = stop;
    m_Ribbons[arm] = BuildRibbon(m_Paths[arm], stop, arm);
}

bool PavedStopSolver::IsSameBand(uint32 arm, uint32 other, const PavedArmSection& section) const
{
    if (m_Arms[other].Road != m_Arms[arm].Road)
        return false;
    const float32 node = m_Paths[other].GetSection(0).Distance;
    const float32 stop = m_Paths[other].SectionAt(m_Stops[other]).Distance;
    // On a closed road the section's arc length is taken round the loop to the turn nearest the
    // other arm's node, each arm's being unwrapped from its own.
    const float32 loop = m_Paths[arm].GetLoopMeters();
    const float32 distance =
        loop > 0.0f ? section.Distance - loop * std::round((section.Distance - node) / loop) : section.Distance;
    return std::min(node, stop) <= distance + section.Width && std::max(node, stop) >= distance - section.Width;
}

bool PavedStopSolver::StationOverlaps(uint32 arm, std::size_t station) const
{
    const PavedArmSection& section = m_Paths[arm].GetSection(station);
    for (const PavedTaggedRing& outline : m_Outlines)
    {
        if (SegmentMeetsRing(section.Left, section.Right, outline))
            return true;
    }
    for (uint32 other = 0; other < m_Paths.size(); ++other)
    {
        if (other == arm || !m_ArmValid[other] || IsSameBand(arm, other, section))
            continue;
        if (SegmentMeetsRing(section.Left, section.Right, m_Ribbons[other]))
            return true;
    }
    return false;
}

} // namespace GameEngine::SplineGeometry
