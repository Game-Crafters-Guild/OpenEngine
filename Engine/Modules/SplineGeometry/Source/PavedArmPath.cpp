#include "PavedArmPath.h"

#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineGeometry
{

using Mathematics::Vector2;

namespace
{

// The right of travel along `forward` on the ground plane: +X when traveling +Z.
Vector2 RightOf(const Vector2& forward) { return Vector2(forward.y, -forward.x); }

Vector2 DirectionOr(const Vector2& v, const Vector2& fallback)
{
    const float32 length = v.Length();
    return length > 0.0f ? v * (1.0f / length) : fallback;
}

PavedArmSection SectionOf(const PavedArmStation& station, const Vector2& forward)
{
    const Vector2 right = RightOf(forward);
    return PavedArmSection{station.Position - right * station.HalfWidthLeft,
                           station.Position + right * station.HalfWidthRight, station.Distance,
                           station.HalfWidthLeft + station.HalfWidthRight};
}

} // namespace

PavedArmPath::PavedArmPath(const PavedArm& arm) : m_LoopMeters(std::max(arm.RoadLoopMeters, 0.0f)), m_EndsRoad(arm.EndsRoad)
{
    if (!arm.Stations.empty() && (arm.Stations.front().Position - arm.Node).Length() > kPavedTouchMeters)
    {
        PavedArmStation node = arm.Stations.front();
        node.Position = arm.Node;
        m_Stations.push_back(node);
    }
    for (const PavedArmStation& station : arm.Stations)
    {
        if (m_Stations.empty() || (station.Position - m_Stations.back().Position).Length() > kPavedTouchMeters)
            m_Stations.push_back(station);
    }
    // A step of more than half the loop is the seam: the arc length runs on past it.
    for (std::size_t i = 1; m_LoopMeters > 0.0f && i < m_Stations.size(); ++i)
    {
        const float32 step = m_Stations[i].Distance - m_Stations[i - 1].Distance;
        m_Stations[i].Distance -= m_LoopMeters * std::round(step / m_LoopMeters);
    }
    m_Along.resize(m_Stations.size(), 0.0f);
    for (std::size_t i = 1; i < m_Stations.size(); ++i)
        m_Along[i] = m_Along[i - 1] + (m_Stations[i].Position - m_Stations[i - 1].Position).Length();
    Vector2 forward(0.0f, 1.0f);
    for (std::size_t i = 0; i < m_Stations.size(); ++i)
    {
        const Vector2& ahead = m_Stations[std::min(i + 1, m_Stations.size() - 1)].Position;
        const Vector2& behind = m_Stations[i == 0 ? 0 : i - 1].Position;
        forward = DirectionOr(ahead - behind, forward);
        m_Sections.push_back(SectionOf(m_Stations[i], forward));
    }
}

void PavedArmPath::ShareNodeSection(const PavedArmPath& partner)
{
    if (m_Stations.size() < 2 || partner.m_Stations.size() < 2 ||
        (m_Stations.front().Position - partner.m_Stations.front().Position).Length() > kPavedTouchMeters ||
        std::abs(m_Stations.front().Distance - partner.m_Stations.front().Distance) > kPavedTouchMeters ||
        (m_Stations[1].Distance - m_Stations[0].Distance) *
                (partner.m_Stations[1].Distance - partner.m_Stations[0].Distance) >= 0.0f)
        return;
    const Vector2 forward = DirectionOr(m_Stations[1].Position - partner.m_Stations[1].Position,
                                        m_Stations[1].Position - m_Stations[0].Position);
    m_Sections.front() = SectionOf(m_Stations.front(), DirectionOr(forward, Vector2(0.0f, 1.0f)));
}

PavedArmSection PavedArmPath::SectionAt(float32 along) const
{
    if (m_Stations.size() == 1 || along >= GetLength())
        return m_Sections.back();
    const std::size_t next =
        static_cast<std::size_t>(std::upper_bound(m_Along.begin(), m_Along.end(), along) - m_Along.begin());
    const std::size_t from = std::max<std::size_t>(next, 1) - 1;
    const std::size_t to = std::min(from + 1, m_Stations.size() - 1);
    const float32 span = m_Along[to] - m_Along[from];
    const float32 t = span > 0.0f ? (along - m_Along[from]) / span : 0.0f;
    const PavedArmStation& a = m_Stations[from];
    const PavedArmStation& b = m_Stations[to];
    PavedArmStation station;
    station.Position = a.Position + (b.Position - a.Position) * t;
    station.HalfWidthLeft = Math::Lerp(a.HalfWidthLeft, b.HalfWidthLeft, t);
    station.HalfWidthRight = Math::Lerp(a.HalfWidthRight, b.HalfWidthRight, t);
    station.Distance = Math::Lerp(a.Distance, b.Distance, t);
    return SectionOf(station, DirectionOr(b.Position - a.Position, Vector2(0.0f, 1.0f)));
}

float32 PavedArmPath::RoadDistance(float32 unwrapped) const
{
    if (m_LoopMeters <= 0.0f)
        return unwrapped;
    const float32 wrapped = std::fmod(unwrapped, m_LoopMeters);
    return wrapped < 0.0f ? wrapped + m_LoopMeters : wrapped;
}

float32 PavedArmPath::StopAfter(std::size_t lastOverlap) const
{
    const std::size_t next = lastOverlap + 1;
    if (next >= m_Stations.size())
        return GetLength();
    return std::min(GetLength(), m_Along[next] + kApproachStopAdvanceWidths * m_Sections[next].Width);
}

} // namespace GameEngine::SplineGeometry
