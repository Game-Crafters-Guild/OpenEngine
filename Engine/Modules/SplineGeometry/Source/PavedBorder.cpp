#include "SplineGeometry/PavedBorder.h"

#include "Mathematics/Angle.h"
#include "SplineGeometry/PavedFootprint.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::SplineGeometry
{

using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

const Vector3 kUp(0.0f, 1.0f, 0.0f);
// A boundary point whose run turns by more than this is a corner (SplineCornerStyle), as every
// point the boolean made is: a border swept round a sharper turn without one narrows by more than
// 1 % (cos(7.5 degrees) = 0.991).
constexpr float32 kCornerTurnDegrees = 15.0f;
// The finest station spacing a run takes, so a zero spacing cannot sample it without end.
constexpr float32 kMinBorderSpacingMeters = 0.01f;

Vector3 NormalizedOr(const Vector3& v, const Vector3& fallback)
{
    const float32 length = v.Length();
    return length > 0.0f ? v * (1.0f / length) : fallback;
}

// Whether the run turns at point `point` by more than kCornerTurnDegrees; an open run's
// ends never do.
bool TurnsSharply(std::span<const Vector2> points, std::size_t point, bool closed)
{
    const std::size_t count = points.size();
    if (!closed && (point == 0 || point + 1 >= count))
        return false;
    const Vector2 in = points[point] - points[(point + count - 1) % count];
    const Vector2 out = points[(point + 1) % count] - points[point];
    const float32 lengths = in.Length() * out.Length();
    if (!(lengths > 0.0f))
        return false;
    const float32 cosine = std::clamp(Vector2::Dot(in, out) / lengths, -1.0f, 1.0f);
    return std::acos(cosine) > Mathematics::ToRadians(kCornerTurnDegrees);
}

// A closed run started at the middle of its longest edge, so the loop's seam lies on a straight
// stretch rather than at a corner, whose rings the seam would split.
PavedBoundaryRun SeamOnLongestEdge(const PavedBoundaryRun& run)
{
    const std::size_t count = run.Points.size();
    std::size_t longest = 0;
    float32 longestSquared = -1.0f;
    for (std::size_t i = 0; i < count; ++i)
    {
        const Vector2 edge = run.Points[(i + 1) % count] - run.Points[i];
        const float32 lengthSquared = Vector2::Dot(edge, edge);
        if (lengthSquared > longestSquared)
        {
            longestSquared = lengthSquared;
            longest = i;
        }
    }
    PavedBoundaryRun seamed;
    seamed.Closed = true;
    seamed.Hole = run.Hole;
    seamed.Points.push_back((run.Points[longest] + run.Points[(longest + 1) % count]) * 0.5f);
    seamed.NewPoints.push_back(0u);
    seamed.Edges.push_back(run.Edges[longest]);
    for (std::size_t step = 1; step <= count; ++step)
    {
        const std::size_t i = (longest + step) % count;
        seamed.Points.push_back(run.Points[i]);
        seamed.NewPoints.push_back(run.NewPoints[i]);
        seamed.Edges.push_back(run.Edges[i]);
    }
    return seamed;
}

void AppendStation(std::vector<SplineStripStation>& stations, const Vector2& xz, const GroundSampler& groundAt)
{
    stations.emplace_back().Position = Vector3(xz.x, groundAt(xz), xz.y);
}

// The stations along `run`'s points at most `spacing` apart, a closed run's start reached again at
// its end, and the corner sites at its New and sharply turning points.
void SampleRun(const PavedBoundaryRun& run, float32 spacing, const GroundSampler& groundAt,
               std::vector<SplineStripStation>& stations, std::vector<SplineCornerSite>& sites)
{
    const std::span<const Vector2> points(run.Points);
    const std::size_t count = points.size();
    const std::size_t legs = run.Closed ? count : count - 1;
    for (std::size_t leg = 0; leg < legs; ++leg)
    {
        const Vector2& from = points[leg];
        const Vector2& to = points[(leg + 1) % count];
        const Vector2 along = to - from;
        const float32 length = along.Length();
        const uint32 steps = std::max(1u, static_cast<uint32>(std::ceil(length / spacing)));
        if (run.NewPoints[leg] != 0 || TurnsSharply(points, leg, run.Closed))
            sites.push_back(SplineCornerSite{static_cast<uint32>(stations.size()), static_cast<uint32>(leg)});
        for (uint32 step = 0; step < steps; ++step)
            AppendStation(stations, from + along * (static_cast<float32>(step) / static_cast<float32>(steps)), groundAt);
    }
    AppendStation(stations, points[run.Closed ? 0 : count - 1], groundAt);
}

// Gives each station its frame (forward along the chord through its neighbors, right of travel
// level, up between them), its half widths (kAreaLidInsetMeters in over the lid, `width` out) and
// its distance along the run.
void FrameStations(std::vector<SplineStripStation>& stations, float32 width)
{
    float32 distance = 0.0f;
    Vector3 forward(0.0f, 0.0f, 1.0f);
    for (std::size_t i = 0; i < stations.size(); ++i)
    {
        if (i > 0)
            distance += (stations[i].Position - stations[i - 1].Position).Length();
        const Vector3& ahead = stations[std::min(i + 1, stations.size() - 1)].Position;
        const Vector3& behind = stations[i == 0 ? 0 : i - 1].Position;
        forward = NormalizedOr(ahead - behind, forward);
        SplineStripStation& station = stations[i];
        station.Forward = forward;
        station.Right = NormalizedOr(Vector3::Cross(kUp, Vector3(forward.x, 0.0f, forward.z)), Vector3(1.0f, 0.0f, 0.0f));
        station.Up = Vector3::Cross(forward, station.Right);
        station.HalfWidthLeft = kAreaLidInsetMeters;
        station.HalfWidthRight = width;
        station.Distance = distance;
    }
}

// Sinks each station's grounded base by the ground's fall from the station to the border's outer
// base edge (along MitreRight on a mitered side), so a cross slope never floats it.
void SinkGroundedBase(std::vector<SplineStripStation>& stations, const GroundSampler& groundAt)
{
    for (SplineStripStation& station : stations)
    {
        const Vector3 axis =
            (station.MitreSides & kSplineMitreRightSide) != 0u ? station.MitreRight : station.Right;
        const Vector3 edge = station.Position + axis * station.HalfWidthRight;
        const float32 fall = station.Position.y - groundAt(Vector2(edge.x, edge.z));
        station.GroundOffset = -std::max(0.0f, fall);
    }
}

SplineStripParams StripParams(const std::vector<SplineStripStation>& stations, bool closed)
{
    SplineStripParams params;
    params.WidthScale = SplineProfileScale::LateralOnly;
    params.GenerateCaps = !closed;
    if (stations.empty())
        return params;
    params.UOriginMetres = stations.front().Distance;
    if (closed)
    {
        const SplineStripStation& last = stations.back();
        params.LoopLengthMetres = last.Distance - stations.front().Distance;
        params.LoopFaceTurnLeft = last.FaceTurnLeft;
        params.LoopFaceTurnRight = last.FaceTurnRight;
        params.LoopTopArcAllowance = last.TopArcAllowance;
    }
    return params;
}

PavedBorderRun BuildRun(const PavedBoundaryRun& run, const PavedBorderSettings& settings, const GroundSampler& groundAt)
{
    PavedBorderRun border;
    border.Closed = run.Closed;
    if (run.Points.size() < (run.Closed ? 3u : 2u))
        return border;
    std::vector<SplineCornerSite> sites;
    const float32 spacing = std::max(settings.StationSpacingMeters, kMinBorderSpacingMeters);
    SampleRun(run.Closed ? SeamOnLongestEdge(run) : run, spacing, groundAt, border.Stations, sites);
    FrameStations(border.Stations, std::max(settings.Width, 0.0f));
    if (run.Closed)
        CloseStationLoop(border.Stations, kUp);
    if (!sites.empty() && settings.Corner != SplineCornerStyle::None)
        border.CornerIssues = ApplyCorners(border.Stations, sites, settings.Corner, kUp, run.Closed);
    AccumulateFaceTurn(border.Stations, kUp);
    SinkGroundedBase(border.Stations, groundAt);
    border.Strip = StripParams(border.Stations, run.Closed);
    return border;
}

} // namespace

SplineProfile BuildPavedBorderProfile(const PavedBorderSettings& settings)
{
    SplineProfileParams params;
    params.Shape = SplineProfileShape::Rectangle;
    params.Width = settings.Width;
    params.Height = settings.Height;
    return BuildProfile(params);
}

std::vector<PavedBorderRun> BuildPavedBorder(std::span<const PavedBoundaryRun> runs,
                                             const PavedBorderSettings& settings, const GroundSampler& groundAt)
{
    std::vector<PavedBorderRun> borders;
    borders.reserve(runs.size());
    for (const PavedBoundaryRun& run : runs)
        borders.push_back(BuildRun(run, settings, groundAt));
    return borders;
}

} // namespace GameEngine::SplineGeometry
