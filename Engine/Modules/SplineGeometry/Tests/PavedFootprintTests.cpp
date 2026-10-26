// A paving region's footprint (BuildPavedFootprint): the arms grown to their approach stops, the
// regions they form with the outlines, the boundary's provenance and its runs.
//
// Every fixture is held to the same acceptance: no duplicate face (a 0.1 m grid of samples over
// the region: every sample inside the authored set, the outlines and the arms' ribbons to their
// stops minus the excludes, lies in exactly one piece and under exactly one lid triangle, and no
// sample outside does), no internal run (every run edge has the authored set on its left and not
// on its right), no blocked approach (every arm whose road goes on past its stop is Open), and
// every boundary edge tagged.

#include "SplineGeometry/AreaGround.h"
#include "SplineGeometry/PavedFootprint.h"

#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;
using Mathematics::DistanceToSegment;
using Mathematics::Vector2;

namespace
{

constexpr float kStationSpacing = 0.5f;
constexpr float kGridStep = 0.1f;
// The grid's offsets from the fixtures' round coordinates, unequal so no sample lands on an edge
// or on a diagonal through two round points.
constexpr float kGridOffsetX = 0.0137f;
constexpr float kGridOffsetZ = 0.0291f;
// A sample this close to the boundary is on it, not inside or outside: the pieces' boundary is the
// inputs' to a millimeter, the lid's to a centimeter (BuildAreaGround).
constexpr float kBoundaryMargin = 2.0f * kPavedTouchMeters;
constexpr float kLidBoundaryMargin = 1.0e-2f;
constexpr float kPi = std::numbers::pi_v<float>;

Vector2 Direction(float degrees)
{
    const float radians = degrees * kPi / 180.0f;
    return Vector2(std::cos(radians), std::sin(radians));
}

// A straight arm from `node` along `direction` (a unit vector), `length` meters, stations every
// kStationSpacing; the road's arc length runs from `nodeDistance` by `sign` per meter.
PavedArm StraightArm(uint32 road, const Vector2& node, const Vector2& direction, float length, float halfWidth,
                     float nodeDistance, float sign)
{
    PavedArm arm;
    arm.Road = road;
    arm.Node = node;
    const int steps = static_cast<int>(std::ceil(length / kStationSpacing));
    for (int i = 0; i <= steps; ++i)
    {
        const float along = length * static_cast<float>(i) / static_cast<float>(steps);
        arm.Stations.push_back(
            PavedArmStation{node + direction * along, halfWidth, halfWidth, nodeDistance + sign * along});
    }
    return arm;
}

// The two arms of a straight road through `node`, `length` meters each way; the road's arc length
// is `length` at the node.
void AddThroughRoad(std::vector<PavedArm>& arms, uint32 road, const Vector2& node, float degrees, float length,
                    float halfWidth)
{
    arms.push_back(StraightArm(road, node, Direction(degrees), length, halfWidth, length, 1.0f));
    arms.push_back(StraightArm(road, node, Direction(degrees) * -1.0f, length, halfWidth, length, -1.0f));
}

// An arm along a circle about `center` of `radius`, from `fromDegrees` to `toDegrees` in steps of
// `stepDegrees`; the arc length is measured from `fromDegrees`.
PavedArm ArcArm(uint32 road, const Vector2& center, float radius, float fromDegrees, float toDegrees,
                float stepDegrees, float halfWidth)
{
    PavedArm arm;
    arm.Road = road;
    const float sign = toDegrees > fromDegrees ? 1.0f : -1.0f;
    const int steps = static_cast<int>(std::round(std::abs(toDegrees - fromDegrees) / stepDegrees));
    float distance = radius * std::abs(fromDegrees) * kPi / 180.0f;
    for (int i = 0; i <= steps; ++i)
    {
        const Vector2 position = center + Direction(fromDegrees + sign * stepDegrees * static_cast<float>(i)) * radius;
        if (i > 0)
            distance += sign * (position - arm.Stations.back().Position).Length();
        arm.Stations.push_back(PavedArmStation{position, halfWidth, halfWidth, distance});
    }
    arm.Node = arm.Stations.front().Position;
    return arm;
}

// `arm` marked as running to its road's end.
PavedArm EndingTheRoad(PavedArm arm)
{
    arm.EndsRoad = true;
    return arm;
}

AreaRing Rectangle(float minX, float minZ, float maxX, float maxZ)
{
    return {{minX, minZ}, {maxX, minZ}, {maxX, maxZ}, {minX, maxZ}};
}

// The input of one fixture.
struct Fixture
{
    std::vector<AreaRing> Outlines;
    std::vector<PavedArm> Arms;
    std::vector<AreaRing> Excludes;

    PavedFootprintInput Input() const { return PavedFootprintInput{Outlines, Arms, Excludes}; }
};

// A station's cross-section, left end then right, square to `forward`.
std::pair<Vector2, Vector2> SectionOf(const PavedArmStation& station, const Vector2& forward)
{
    const Vector2 right(forward.y, -forward.x);
    return {station.Position - right * station.HalfWidthLeft, station.Position + right * station.HalfWidthRight};
}

// Whether the cross-section x meters along a road running +X, 2 m to each side, meets the band of a
// road 2 m to each side through the origin at 30 degrees: the band is |z cos 30 - x sin 30| <= 2.
bool MeetsThirtyDegreeBand(float x)
{
    const float sin30 = 0.5f;
    const float cos30 = std::sqrt(3.0f) * 0.5f;
    const float left = 2.0f * cos30 - x * sin30;
    const float right = -2.0f * cos30 - x * sin30;
    return left * right <= 0.0f || std::min(std::abs(left), std::abs(right)) <= 2.0f + kPavedTouchMeters;
}

// An arm's stations from its node: a station at the node prepended when the first is short of it.
std::vector<PavedArmStation> StationsFromNode(const PavedArm& arm)
{
    std::vector<PavedArmStation> stations;
    if ((arm.Stations.front().Position - arm.Node).Length() > kPavedTouchMeters)
    {
        stations.push_back(arm.Stations.front());
        stations.back().Position = arm.Node;
    }
    stations.insert(stations.end(), arm.Stations.begin(), arm.Stations.end());
    return stations;
}

// The road's other arm from the same node at the same arc length, its arc length running the
// other way, or null.
const PavedArm* PartnerOf(const Fixture& fixture, uint32 arm)
{
    const std::vector<PavedArmStation> own = StationsFromNode(fixture.Arms[arm]);
    for (uint32 other = 0; other < fixture.Arms.size(); ++other)
    {
        if (other == arm || fixture.Arms[other].Road != fixture.Arms[arm].Road)
            continue;
        const std::vector<PavedArmStation> theirs = StationsFromNode(fixture.Arms[other]);
        if ((theirs.front().Position - own.front().Position).Length() <= kPavedTouchMeters &&
            std::abs(theirs.front().Distance - own.front().Distance) <= kPavedTouchMeters && own.size() > 1 &&
            theirs.size() > 1 && (own[1].Distance - own[0].Distance) * (theirs[1].Distance - theirs[0].Distance) < 0.0f)
            return &fixture.Arms[other];
    }
    return nullptr;
}

// An arm's ribbon as the test models it: quads between its cross-sections up to its stop, each
// section square to the chord through its neighbors (the node's through the partner's second
// station, when the road has another arm from the node), the stop's square to its segment.
std::vector<AreaRing> RibbonQuads(const PavedArm& arm, const PavedArmStop& stop, const PavedArm* partner)
{
    const std::vector<PavedArmStation> stations = StationsFromNode(arm);
    std::vector<std::pair<Vector2, Vector2>> sections;
    for (std::size_t i = 0; i < stations.size(); ++i)
    {
        Vector2 chord = stations[std::min(i + 1, stations.size() - 1)].Position - stations[i == 0 ? 0 : i - 1].Position;
        if (i == 0 && partner && stations.size() > 1)
            chord = stations[1].Position - StationsFromNode(*partner)[1].Position;
        // The stop: the first segment past the extension whose arc length reaches it.
        if (i > 0 && stations[i].Distance != stations[i - 1].Distance &&
            (stations[i].Distance - stop.StopDistance) * (stations[i - 1].Distance - stop.StopDistance) <= 0.0f)
        {
            if (std::abs(stations[i - 1].Distance - stop.StopDistance) > kPavedTouchMeters)
                sections.emplace_back(stop.OpenLeft, stop.OpenRight);
            break;
        }
        sections.push_back(SectionOf(stations[i], chord * (1.0f / chord.Length())));
    }
    std::vector<AreaRing> quads;
    for (std::size_t i = 0; i + 1 < sections.size(); ++i)
        quads.push_back({sections[i].first, sections[i].second, sections[i + 1].second, sections[i + 1].first});
    return quads;
}

// The authored set of a region: its outlines and its arms' ribbons, minus every exclude.
struct AuthoredSet
{
    std::vector<AreaRing> Unions;
    std::vector<AreaRing> Cuts;

    bool Contains(const Vector2& point) const
    {
        const auto inside = [&](const AreaRing& ring) { return Mathematics::PointInPolygon(point, ring); };
        return std::any_of(Unions.begin(), Unions.end(), inside) && std::none_of(Cuts.begin(), Cuts.end(), inside);
    }
};

AuthoredSet AuthoredSetOf(const Fixture& fixture, const PavedFootprint& footprint, const PavedRegion& region)
{
    AuthoredSet set;
    for (const uint32 outline : region.Outlines)
        set.Unions.push_back(fixture.Outlines[outline]);
    for (const uint32 arm : region.Arms)
    {
        for (AreaRing& quad : RibbonQuads(fixture.Arms[arm], footprint.Arms[arm], PartnerOf(fixture, arm)))
            set.Unions.push_back(std::move(quad));
    }
    set.Cuts = fixture.Excludes;
    return set;
}

bool PieceContains(const AreaPiece& piece, const Vector2& point)
{
    if (!Mathematics::PointInPolygon(point, piece.Outer))
        return false;
    return std::none_of(piece.Holes.begin(), piece.Holes.end(),
                        [&](const AreaRing& hole) { return Mathematics::PointInPolygon(point, hole); });
}

// What the coverage sampler found over one region.
struct Coverage
{
    int Samples = 0;
    int Inside = 0;
    int PieceMismatches = 0;
    int LidMismatches = 0;
    // Where the first lid mismatch is, and how many triangles covered it.
    Vector2 FirstLidMismatch{};
    int FirstLidMismatchTriangles = 0;
};

// Items bucketed by the 1 m cells their bounds touch, so a sample tests only its own cell's.
class CellGrid
{
  public:
    CellGrid(const Vector2& min, const Vector2& max)
        : m_Origin(min - Vector2(1.0f, 1.0f)), m_Columns(static_cast<int>(max.x - m_Origin.x) + 2),
          m_Rows(static_cast<int>(max.y - m_Origin.y) + 2), m_Cells(static_cast<std::size_t>(m_Columns * m_Rows))
    {
    }

    void Add(const Vector2& low, const Vector2& high, uint32 item)
    {
        for (int z = Row(low.y); z <= Row(high.y); ++z)
        {
            for (int x = Column(low.x); x <= Column(high.x); ++x)
                m_Cells[static_cast<std::size_t>(z * m_Columns + x)].push_back(item);
        }
    }

    const std::vector<uint32>& At(const Vector2& point) const
    {
        return m_Cells[static_cast<std::size_t>(Row(point.y) * m_Columns + Column(point.x))];
    }

  private:
    int Column(float x) const { return std::clamp(static_cast<int>(x - m_Origin.x), 0, m_Columns - 1); }
    int Row(float z) const { return std::clamp(static_cast<int>(z - m_Origin.y), 0, m_Rows - 1); }

    Vector2 m_Origin;
    int m_Columns = 0;
    int m_Rows = 0;
    std::vector<std::vector<uint32>> m_Cells;
};

using Segment = std::pair<Vector2, Vector2>;

void AppendRingEdges(const AreaRing& ring, std::vector<Segment>& edges)
{
    for (std::size_t i = 0; i < ring.size(); ++i)
        edges.emplace_back(ring[i], ring[(i + 1) % ring.size()]);
}

// Whether `sample` lies strictly inside the lid's counter-clockwise triangle `t`.
bool LidTriangleContains(const AreaGround& lid, std::size_t t, const Vector2& sample)
{
    for (std::size_t k = 0; k < 3; ++k)
    {
        const auto& p = lid.LidPoints[lid.LidTriangles[t + k]];
        const auto& q = lid.LidPoints[lid.LidTriangles[t + (k + 1) % 3]];
        if (!((q.x - p.x) * (sample.y - p.z) - (q.z - p.z) * (sample.x - p.x) > 0.0f))
            return false;
    }
    return true;
}

// A 0.1 m grid of samples over `region`'s pieces: each sample farther than kBoundaryMargin from the
// boundary lies in as many pieces as the authored set holds it (one or none), and each farther than
// kLidBoundaryMargin under as many lid triangles.
Coverage SampleCoverage(const AuthoredSet& set, const PavedRegion& region)
{
    const AreaGround lid = BuildAreaGround(region.Pieces, AreaGroundSettings{}, [](const Vector2&) { return 0.0f; });
    std::vector<Segment> boundary;
    for (const AreaPiece& piece : region.Pieces)
    {
        AppendRingEdges(piece.Outer, boundary);
        for (const AreaRing& hole : piece.Holes)
            AppendRingEdges(hole, boundary);
    }
    Vector2 min(1.0e9f, 1.0e9f);
    Vector2 max(-1.0e9f, -1.0e9f);
    for (const AreaRing& ring : set.Unions)
    {
        for (const Vector2& point : ring)
        {
            min = Vector2(std::min(min.x, point.x), std::min(min.y, point.y));
            max = Vector2(std::max(max.x, point.x), std::max(max.y, point.y));
        }
    }
    CellGrid edgeCells(min, max);
    const Vector2 margin(kLidBoundaryMargin, kLidBoundaryMargin);
    for (uint32 e = 0; e < boundary.size(); ++e)
    {
        const auto& [a, b] = boundary[e];
        edgeCells.Add(Vector2(std::min(a.x, b.x), std::min(a.y, b.y)) - margin,
                      Vector2(std::max(a.x, b.x), std::max(a.y, b.y)) + margin, e);
    }
    CellGrid triangleCells(min, max);
    for (uint32 t = 0; t + 2 < lid.LidTriangles.size(); t += 3)
    {
        Vector2 low(1.0e9f, 1.0e9f);
        Vector2 high(-1.0e9f, -1.0e9f);
        for (uint32 corner = 0; corner < 3; ++corner)
        {
            const auto& p = lid.LidPoints[lid.LidTriangles[t + corner]];
            low = Vector2(std::min(low.x, p.x), std::min(low.y, p.z));
            high = Vector2(std::max(high.x, p.x), std::max(high.y, p.z));
        }
        triangleCells.Add(low, high, t);
    }

    Coverage coverage;
    for (float z = min.y - 1.0f + kGridOffsetZ; z < max.y + 1.0f; z += kGridStep)
    {
        for (float x = min.x - 1.0f + kGridOffsetX; x < max.x + 1.0f; x += kGridStep)
        {
            const Vector2 sample(x, z);
            float nearest = kLidBoundaryMargin;
            for (const uint32 e : edgeCells.At(sample))
                nearest = std::min(nearest, DistanceToSegment(sample, boundary[e].first, boundary[e].second));
            if (nearest <= kBoundaryMargin)
                continue;
            ++coverage.Samples;
            const int expected = set.Contains(sample) ? 1 : 0;
            coverage.Inside += expected;
            const int pieces = static_cast<int>(std::count_if(region.Pieces.begin(), region.Pieces.end(),
                                                              [&](const AreaPiece& piece) { return PieceContains(piece, sample); }));
            coverage.PieceMismatches += pieces != expected ? 1 : 0;
            if (nearest < kLidBoundaryMargin)
                continue;
            const std::vector<uint32>& candidates = triangleCells.At(sample);
            const int triangles = static_cast<int>(std::count_if(candidates.begin(), candidates.end(),
                                                                 [&](uint32 t) { return LidTriangleContains(lid, t, sample); }));
            if (triangles != expected && coverage.LidMismatches++ == 0)
            {
                coverage.FirstLidMismatch = sample;
                coverage.FirstLidMismatchTriangles = triangles;
            }
        }
    }
    return coverage;
}

// Every run edge has the authored set on its left (inside) and not on its right, 5 mm off its
// middle: no border inside the paving.
int InternalRunEdges(const AuthoredSet& set, const PavedRegion& region)
{
    constexpr float kProbe = 5.0e-3f;
    int internal = 0;
    for (const PavedBoundaryRun& run : region.Runs)
    {
        for (std::size_t i = 0; i < run.Edges.size(); ++i)
        {
            const Vector2& a = run.Points[i];
            const Vector2& b = run.Points[(i + 1) % run.Points.size()];
            const float length = (b - a).Length();
            if (length < 4.0f * kProbe)
                continue;
            const Vector2 forward = (b - a) * (1.0f / length);
            const Vector2 right(forward.y, -forward.x);
            const Vector2 middle = (a + b) * 0.5f;
            if (!set.Contains(middle - right * kProbe) || set.Contains(middle + right * kProbe))
                ++internal;
        }
    }
    return internal;
}

int UntaggedEdges(const PavedRegion& region)
{
    int untagged = 0;
    for (const PavedBoundaryRun& run : region.Runs)
        untagged += static_cast<int>(std::count_if(run.Edges.begin(), run.Edges.end(),
                                                   [](const PavedEdge& edge) { return edge.Source == PavedEdgeSource::New; }));
    return untagged;
}

// The paved arms whose road goes on past the stop (it does not end at the arm's last station, or
// the stop falls short of that station) and whose cap is not Open.
int BlockedApproaches(const Fixture& fixture, const PavedFootprint& footprint)
{
    int blocked = 0;
    for (const PavedRegion& region : footprint.Regions)
    {
        for (const uint32 arm : region.Arms)
        {
            const PavedArmStop& stop = footprint.Arms[arm];
            const float end = fixture.Arms[arm].Stations.back().Distance;
            const bool goesOn = !fixture.Arms[arm].EndsRoad || std::abs(stop.StopDistance - end) > kPavedTouchMeters;
            if (goesOn && !stop.Open)
                ++blocked;
        }
    }
    return blocked;
}

// The whole acceptance over every region of `footprint`.
void ExpectAccepted(const Fixture& fixture, const PavedFootprint& footprint)
{
    for (std::size_t r = 0; r < footprint.Regions.size(); ++r)
    {
        const PavedRegion& region = footprint.Regions[r];
        const AuthoredSet set = AuthoredSetOf(fixture, footprint, region);
        const Coverage coverage = SampleCoverage(set, region);
        EXPECT_GT(coverage.Inside, 0) << "region " << r;
        EXPECT_EQ(coverage.PieceMismatches, 0) << "region " << r << ": samples not in exactly the pieces they should be, of "
                                               << coverage.Samples;
        EXPECT_EQ(coverage.LidMismatches, 0) << "region " << r << ": samples not under exactly one lid triangle, of "
                                             << coverage.Samples << "; the first at (" << coverage.FirstLidMismatch.x << ", "
                                             << coverage.FirstLidMismatch.y << ") under "
                                             << coverage.FirstLidMismatchTriangles;
        EXPECT_EQ(InternalRunEdges(set, region), 0) << "region " << r << ": a run inside the paving";
        EXPECT_EQ(UntaggedEdges(region), 0) << "region " << r << ": a boundary edge on no input edge";
    }
    EXPECT_EQ(BlockedApproaches(fixture, footprint), 0) << "an approach without its Open edge";
}

int OpenArms(const PavedFootprint& footprint)
{
    return static_cast<int>(
        std::count_if(footprint.Arms.begin(), footprint.Arms.end(), [](const PavedArmStop& stop) { return stop.Open; }));
}

// A 30 m square plaza with six roads ending on its rim, two on the north and south sides and one on
// the east and west, and a seventh arm that carries the west road on into the plaza, ending inside.
Fixture SixEntryPlaza()
{
    Fixture fixture;
    fixture.Outlines.push_back(Rectangle(-15.0f, -15.0f, 15.0f, 15.0f));
    const float kOut = 30.0f;
    fixture.Arms.push_back(StraightArm(0, {-7.0f, 15.0f}, {0.0f, 1.0f}, kOut, 2.0f, 0.0f, 1.0f));
    fixture.Arms.push_back(StraightArm(1, {7.0f, 15.0f}, {0.0f, 1.0f}, kOut, 2.0f, 0.0f, 1.0f));
    fixture.Arms.push_back(StraightArm(2, {-7.0f, -15.0f}, {0.0f, -1.0f}, kOut, 2.0f, 0.0f, 1.0f));
    fixture.Arms.push_back(StraightArm(3, {7.0f, -15.0f}, {0.0f, -1.0f}, kOut, 2.0f, 0.0f, 1.0f));
    fixture.Arms.push_back(StraightArm(4, {15.0f, 0.0f}, {1.0f, 0.0f}, kOut, 2.5f, 0.0f, 1.0f));
    fixture.Arms.push_back(StraightArm(5, {-15.0f, 0.0f}, {-1.0f, 0.0f}, kOut, 1.5f, kOut, -1.0f));
    fixture.Arms.push_back(EndingTheRoad(StraightArm(5, {-15.0f, 0.0f}, {1.0f, 0.0f}, 8.0f, 1.5f, kOut, 1.0f)));
    return fixture;
}

} // namespace

// The six road ends of a six-entry plaza are its Open edges, one per road, and nothing else is: an
// arm that ends inside the plaza has no edge on the boundary at all. The plaza is one region, with
// no duplicate face, no internal run and every edge tagged.
TEST(PavedFootprint, ASixEntryPlazaIsOpenAtItsSixRoadEndsAndNowhereElse)
{
    const Fixture fixture = SixEntryPlaza();
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    ASSERT_EQ(footprint.Regions.size(), 1u);
    EXPECT_TRUE(footprint.Issues.empty());
    EXPECT_EQ(OpenArms(footprint), 6);
    EXPECT_FALSE(footprint.Arms[6].Open) << "the arm ending inside the plaza";
    for (const PavedBoundaryRun& run : footprint.Regions[0].Runs)
    {
        EXPECT_FALSE(run.Closed) << "the rim is cut at every Open edge";
        for (const PavedEdge& edge : run.Edges)
            EXPECT_FALSE(edge.Index == 6 && edge.Source != PavedEdgeSource::Outline) << "an edge of the inside arm";
    }
    EXPECT_EQ(footprint.Regions[0].Runs.size(), 6u) << "one run between each pair of road ends";
    // An approach stop on a road square to the rim: the next station past the rim, plus the width.
    EXPECT_NEAR(footprint.Arms[0].StopDistance, kStationSpacing + 4.0f, 1.0e-3f);
    EXPECT_NEAR(footprint.Arms[4].StopDistance, kStationSpacing + 5.0f, 1.0e-3f);
    ExpectAccepted(fixture, footprint);
}

// Every boundary edge names its source, the Open ones exactly the far caps of the arms that go on;
// a point the boolean made (New) is where the source changes, never inside a run of one source.
TEST(PavedFootprint, EveryBoundaryEdgeNamesItsSourceAndNewPointsAreIntersections)
{
    Fixture fixture;
    // Neither road's stations fall on the other's sides, so the notches are points the boolean made.
    AddThroughRoad(fixture.Arms, 0, {0.0f, 0.0f}, 0.0f, 30.0f, 2.75f);
    fixture.Arms.push_back(StraightArm(1, {0.0f, 0.0f}, {0.0f, 1.0f}, 30.0f, 1.8f, 0.0f, 1.0f));
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    ASSERT_EQ(footprint.Regions.size(), 1u);
    int newPoints = 0;
    for (const PavedBoundaryRun& run : footprint.Regions[0].Runs)
    {
        ASSERT_EQ(run.Points.size(), run.Edges.size() + (run.Closed ? 0u : 1u));
        for (std::size_t i = 0; i < run.Edges.size(); ++i)
        {
            EXPECT_NE(run.Edges[i].Source, PavedEdgeSource::New);
            EXPECT_NE(run.Edges[i].Source, PavedEdgeSource::Open) << "a run holds no Open edge";
        }
        for (std::size_t i = 1; i + 1 < run.Points.size(); ++i)
        {
            if (run.NewPoints[i] == 0)
                continue;
            ++newPoints;
            const PavedEdge& before = run.Edges[i - 1];
            const PavedEdge& after = run.Edges[i];
            EXPECT_TRUE(before.Source != after.Source || before.Index != after.Index)
                << "a New point inside a run of one source";
        }
    }
    EXPECT_EQ(newPoints, 2) << "the two notches where the T's sides meet the body's";
    EXPECT_EQ(OpenArms(footprint), 3);
    ExpectAccepted(fixture, footprint);
}

// The coverage sampler is a live instrument: with one piece of a two-piece region left out, the
// samples inside it are in no piece and the check counts them.
TEST(PavedFootprint, TheCoverageSamplerCountsTheSamplesOfAMissingPiece)
{
    Fixture fixture;
    fixture.Outlines.push_back(Rectangle(0.0f, 0.0f, 10.0f, 10.0f));
    fixture.Excludes.push_back(Rectangle(4.0f, -1.0f, 6.0f, 11.0f));
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    ASSERT_EQ(footprint.Regions.size(), 1u);
    ASSERT_EQ(footprint.Regions[0].Pieces.size(), 2u) << "the exclude cuts the plaza in two";
    ExpectAccepted(fixture, footprint);
    PavedRegion dropped = footprint.Regions[0];
    dropped.Pieces.pop_back();
    const Coverage coverage = SampleCoverage(AuthoredSetOf(fixture, footprint, footprint.Regions[0]), dropped);
    EXPECT_GT(coverage.PieceMismatches, 3000) << "40 m2 at 100 samples a square meter";
    EXPECT_GT(coverage.LidMismatches, 3000);
}

// Three roads through one point are three Crossings, twelve arms in all; their footprints are one
// region with six Open edges, one per direction, the arms of one direction sharing theirs.
TEST(PavedFootprint, ThreeRoadsThroughOnePointAreOneRegionOpenInSixDirections)
{
    Fixture fixture;
    // Road r runs at 60 r degrees; each pair of roads is one Crossing with all four of its arms.
    for (const auto& [first, second] : {std::pair{0u, 1u}, std::pair{0u, 2u}, std::pair{1u, 2u}})
    {
        AddThroughRoad(fixture.Arms, first, {0.0f, 0.0f}, 60.0f * static_cast<float>(first), 30.0f, 2.0f);
        AddThroughRoad(fixture.Arms, second, {0.0f, 0.0f}, 60.0f * static_cast<float>(second), 30.0f, 2.0f);
    }
    const auto start = std::chrono::steady_clock::now();
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
    RecordProperty("BuildPavedFootprintMilliseconds", std::to_string(elapsed.count()));
    ASSERT_EQ(footprint.Regions.size(), 1u);
    EXPECT_EQ(footprint.Regions[0].Arms.size(), 12u);
    EXPECT_EQ(OpenArms(footprint), 12) << "each direction's two arms share one Open edge";
    EXPECT_EQ(footprint.Regions[0].Runs.size(), 6u) << "one run between each pair of directions";
    ExpectAccepted(fixture, footprint);
}

// Two T junctions 8 m apart on one road have footprints that overlap: one region, built once, with
// the body's arms from both nodes stopping together past the far junction.
TEST(PavedFootprint, TwoCloseTJunctionsAreOneRegionBuiltOnce)
{
    Fixture fixture;
    // The body runs from x = -40 (arc length 0) to x = 40.
    for (const float x : {-4.0f, 4.0f})
    {
        fixture.Arms.push_back(StraightArm(0, {x, 0.0f}, {1.0f, 0.0f}, 40.0f - x, 2.0f, 40.0f + x, 1.0f));
        fixture.Arms.push_back(StraightArm(0, {x, 0.0f}, {-1.0f, 0.0f}, 40.0f + x, 2.0f, 40.0f + x, -1.0f));
        fixture.Arms.push_back(StraightArm(x < 0.0f ? 1u : 2u, {x, 0.0f}, {0.0f, 1.0f}, 30.0f, 1.5f, 0.0f, 1.0f));
    }
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    ASSERT_EQ(footprint.Regions.size(), 1u);
    EXPECT_EQ(footprint.Regions[0].Arms.size(), 6u);
    // Both eastward arms stop at the station past the far T's side (x = 5.5), plus the width.
    EXPECT_NEAR(footprint.Arms[0].StopDistance, footprint.Arms[3].StopDistance, 1.0e-3f);
    EXPECT_NEAR(footprint.Arms[0].StopDistance, 40.0f + 6.0f + 4.0f, 1.0e-3f);
    EXPECT_NEAR(footprint.Arms[1].StopDistance, 40.0f - 10.0f, 1.0e-3f);
    EXPECT_EQ(OpenArms(footprint), 6);
    EXPECT_EQ(footprint.Regions[0].Runs.size(), 4u) << "east, west and the two T's open ends cut four runs";
    ExpectAccepted(fixture, footprint);
}

// A plaza no road enters is paved whole: one region, its outline, bordered by one closed run; a
// concave plaza with a planter cut out of it is one piece with one hole and a closed run on each.
TEST(PavedFootprint, APlazaWithNoEntryIsPavedWholeAndItsPlanterIsAHole)
{
    Fixture fixture;
    fixture.Outlines.push_back({{0.0f, 0.0f}, {30.0f, 0.0f}, {30.0f, 12.0f}, {12.0f, 12.0f}, {12.0f, 30.0f}, {0.0f, 30.0f}});
    fixture.Excludes.push_back(Rectangle(3.0f, 3.0f, 8.0f, 8.0f));
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    ASSERT_EQ(footprint.Regions.size(), 1u);
    const PavedRegion& region = footprint.Regions[0];
    ASSERT_EQ(region.Pieces.size(), 1u);
    EXPECT_EQ(region.Pieces[0].Holes.size(), 1u);
    ASSERT_EQ(region.Runs.size(), 2u);
    for (const PavedBoundaryRun& run : region.Runs)
    {
        EXPECT_TRUE(run.Closed);
        const PavedEdgeSource source = run.Hole ? PavedEdgeSource::Exclude : PavedEdgeSource::Outline;
        for (const PavedEdge& edge : run.Edges)
            EXPECT_EQ(edge.Source, source);
        EXPECT_EQ(Mathematics::PolygonDoubledSignedArea(run.Points) < 0.0f, run.Hole) << "outer counter-clockwise, hole clockwise";
    }
    ExpectAccepted(fixture, footprint);
}

// Two curved roads whose ribbons touch along 50 degrees of arc are one region with no run along the
// line they share; the longer road's arms stop past the shorter road's ends, Open. Two roads whose
// ribbons meet end to end, each paved at a crossing of its own, are one region too.
TEST(PavedFootprint, RibbonsThatTouchAlongALengthOrEndToEndAreOneRegion)
{
    Fixture parallel;
    const float kStep = 1.5f;
    parallel.Arms.push_back(ArcArm(0, {0.0f, 0.0f}, 20.0f, 45.0f, 90.0f, kStep, 2.0f));
    parallel.Arms.push_back(ArcArm(0, {0.0f, 0.0f}, 20.0f, 45.0f, 0.0f, kStep, 2.0f));
    parallel.Arms.push_back(EndingTheRoad(ArcArm(1, {0.0f, 0.0f}, 24.5f, 45.0f, 69.0f, kStep, 2.5f)));
    parallel.Arms.push_back(EndingTheRoad(ArcArm(1, {0.0f, 0.0f}, 24.5f, 45.0f, 21.0f, kStep, 2.5f)));
    const PavedFootprint touching = BuildPavedFootprint(parallel.Input());
    ASSERT_EQ(touching.Regions.size(), 1u);
    EXPECT_TRUE(touching.Arms[0].Open);
    EXPECT_TRUE(touching.Arms[1].Open);
    EXPECT_FALSE(touching.Arms[2].Open) << "the shorter road ends inside its arm";
    ExpectAccepted(parallel, touching);

    Fixture collinear;
    AddThroughRoad(collinear.Arms, 0, {0.0f, 0.0f}, 90.0f, 20.0f, 2.0f);
    collinear.Arms.push_back(EndingTheRoad(StraightArm(1, {0.0f, 0.0f}, {1.0f, 0.0f}, 6.0f, 2.0f, 0.0f, 1.0f)));
    AddThroughRoad(collinear.Arms, 2, {12.0f, 0.0f}, 90.0f, 20.0f, 2.0f);
    collinear.Arms.push_back(EndingTheRoad(StraightArm(3, {12.0f, 0.0f}, {-1.0f, 0.0f}, 6.0f, 2.0f, 0.0f, 1.0f)));
    const PavedFootprint endToEnd = BuildPavedFootprint(collinear.Input());
    ASSERT_EQ(endToEnd.Regions.size(), 1u) << "the two caps at x = 6 touch";
    EXPECT_FALSE(endToEnd.Arms[2].Open);
    EXPECT_FALSE(endToEnd.Arms[5].Open);
    ExpectAccepted(collinear, endToEnd);
}

// The same two curved roads sampled at different steps: their touching sides share no point, and
// the weld still leaves no sliver between them.
TEST(PavedFootprint, RibbonsThatTouchAtDifferentStationsAreWeldedWithNoSliverBetween)
{
    Fixture fixture;
    fixture.Arms.push_back(ArcArm(0, {0.0f, 0.0f}, 20.0f, 45.0f, 90.0f, 1.5f, 2.0f));
    fixture.Arms.push_back(ArcArm(0, {0.0f, 0.0f}, 20.0f, 45.0f, 0.0f, 1.5f, 2.0f));
    fixture.Arms.push_back(EndingTheRoad(ArcArm(1, {0.0f, 0.0f}, 24.5f, 45.0f, 69.0f, 2.0f, 2.5f)));
    fixture.Arms.push_back(EndingTheRoad(ArcArm(1, {0.0f, 0.0f}, 24.5f, 45.0f, 21.0f, 2.0f, 2.5f)));
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    ASSERT_EQ(footprint.Regions.size(), 1u);
    ASSERT_EQ(footprint.Regions[0].Pieces.size(), 1u);
    EXPECT_TRUE(footprint.Regions[0].Pieces[0].Holes.empty()) << "slivers between the two ribbons";
    ExpectAccepted(fixture, footprint);
}

namespace
{
// A 20 m square with a road leaving its east side `gap` meters off the rim.
Fixture ArmOffTheRim(float gap)
{
    Fixture fixture;
    fixture.Outlines.push_back(Rectangle(-10.0f, -10.0f, 10.0f, 10.0f));
    fixture.Arms.push_back(StraightArm(0, {10.0f + gap, 0.0f}, {1.0f, 0.0f}, 20.0f, 2.0f, 0.0f, 1.0f));
    return fixture;
}
} // namespace

// An arm 0.9 mm off the rim touches it: one region, welded into one piece with no run across the
// gap. At 1.1 mm and 1.5 mm it is apart: two regions.
TEST(PavedFootprint, AnArmWithinTheTouchingToleranceOfTheRimIsWeldedAndApartBeyondIt)
{
    const Fixture touching = ArmOffTheRim(0.9e-3f);
    const PavedFootprint welded = BuildPavedFootprint(touching.Input());
    ASSERT_EQ(welded.Regions.size(), 1u);
    EXPECT_EQ(welded.Regions[0].Pieces.size(), 1u);
    EXPECT_TRUE(welded.Arms[0].Open);
    ExpectAccepted(touching, welded);
    for (const float gap : {1.1e-3f, 1.5e-3f})
    {
        const Fixture apart = ArmOffTheRim(gap);
        const PavedFootprint separate = BuildPavedFootprint(apart.Input());
        EXPECT_EQ(separate.Regions.size(), 2u) << "a gap of " << gap << " m";
        ExpectAccepted(apart, separate);
    }
}

// A road through a square is two Entry nodes of one road, four arms: the region returns the arc
// length under it for the road's strip to skip (each arm's node to its stop, together one
// interval), and the strip's two Open edges.
TEST(PavedFootprint, ARoadThroughASquareReturnsTheIntervalItsStripSkips)
{
    Fixture fixture;
    fixture.Outlines.push_back(Rectangle(-10.0f, -10.0f, 10.0f, 10.0f));
    // The road runs from x = -40 (arc length 0) to x = 40.
    fixture.Arms.push_back(StraightArm(0, {-10.0f, 0.0f}, {-1.0f, 0.0f}, 30.0f, 2.0f, 30.0f, -1.0f));
    fixture.Arms.push_back(StraightArm(0, {-10.0f, 0.0f}, {1.0f, 0.0f}, 50.0f, 2.0f, 30.0f, 1.0f));
    fixture.Arms.push_back(StraightArm(0, {10.0f, 0.0f}, {1.0f, 0.0f}, 30.0f, 2.0f, 50.0f, 1.0f));
    fixture.Arms.push_back(StraightArm(0, {10.0f, 0.0f}, {-1.0f, 0.0f}, 50.0f, 2.0f, 50.0f, -1.0f));
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    ASSERT_EQ(footprint.Regions.size(), 1u);
    float low = 1.0e9f;
    float high = -1.0e9f;
    for (const PavedArmStop& stop : footprint.Arms)
    {
        low = std::min({low, stop.NodeDistance, stop.StopDistance});
        high = std::max({high, stop.NodeDistance, stop.StopDistance});
    }
    EXPECT_NEAR(low, 30.0f - kStationSpacing - 4.0f, 1.0e-3f);
    EXPECT_NEAR(high, 50.0f + kStationSpacing + 4.0f, 1.0e-3f);
    EXPECT_TRUE(footprint.Arms[0].Open);
    EXPECT_TRUE(footprint.Arms[2].Open);
    EXPECT_EQ(footprint.Regions[0].Runs.size(), 2u) << "the north and the south halves of the rim";
    ExpectAccepted(fixture, footprint);
}

// Two roads crossing at 30 degrees: each arm stops one width past the station after the last one
// whose cross-section still meets the other road, so the acute corner's miter is cleared, and the
// border along the arm's side ends square to its Open edge.
TEST(PavedFootprint, AnAcuteArmStopsOneWidthPastItsLastOverlap)
{
    Fixture fixture;
    AddThroughRoad(fixture.Arms, 0, {0.0f, 0.0f}, 0.0f, 40.0f, 2.0f);
    AddThroughRoad(fixture.Arms, 1, {0.0f, 0.0f}, 30.0f, 40.0f, 2.0f);
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    ASSERT_EQ(footprint.Regions.size(), 1u);
    // Arm 0 runs +X from the node; its last overlapping station, sampled at the arms' spacing.
    float lastOverlap = 0.0f;
    for (float x = 0.0f; x <= 40.0f; x += kStationSpacing)
    {
        if (MeetsThirtyDegreeBand(x))
            lastOverlap = x;
    }
    const float stop = footprint.Arms[0].StopDistance - 40.0f;
    EXPECT_NEAR(stop, lastOverlap + kStationSpacing + kApproachStopAdvanceWidths * 4.0f, 1.0e-3f);
    EXPECT_FALSE(MeetsThirtyDegreeBand(stop - kApproachStopAdvanceWidths * 4.0f)) << "the miter is not cleared";
    // The run that ends at arm 0's Open edge ends along arm 0's side, square to the cap.
    const Vector2 cap = footprint.Arms[0].OpenRight - footprint.Arms[0].OpenLeft;
    int squareEnds = 0;
    for (const PavedBoundaryRun& run : footprint.Regions[0].Runs)
    {
        for (const std::size_t end : {std::size_t{0}, run.Edges.size() - 1})
        {
            if (run.Edges[end].Source != PavedEdgeSource::ArmSide || run.Edges[end].Index != 0)
                continue;
            const Vector2 side = run.Points[end + 1] - run.Points[end];
            EXPECT_NEAR((side.x * cap.x + side.y * cap.y) / (side.Length() * cap.Length()), 0.0f, 1.0e-3f);
            ++squareEnds;
        }
    }
    EXPECT_EQ(squareEnds, 2) << "arm 0's two sides end at its Open edge";
    ExpectAccepted(fixture, footprint);
}

// A road attached short of the rim (its end 3 m off it) has its ribbon carried straight to the node
// on the rim: the gap is paved, and the arm stops one width past its first station.
TEST(PavedFootprint, AnArmAttachedShortOfTheRimIsCarriedToTheNode)
{
    Fixture fixture;
    fixture.Outlines.push_back(Rectangle(-10.0f, -10.0f, 10.0f, 10.0f));
    PavedArm arm = StraightArm(0, {0.0f, -13.0f}, {0.0f, -1.0f}, 30.0f, 2.0f, 0.0f, 1.0f);
    arm.Node = Vector2(0.0f, -10.0f);
    fixture.Arms.push_back(arm);
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    ASSERT_EQ(footprint.Regions.size(), 1u);
    ASSERT_EQ(footprint.Regions[0].Pieces.size(), 1u);
    EXPECT_TRUE(PieceContains(footprint.Regions[0].Pieces[0], Vector2(0.0f, -11.5f))) << "the gap to the rim";
    EXPECT_NEAR(footprint.Arms[0].StopDistance, 4.0f, 1.0e-3f) << "the first station, plus the width";
    EXPECT_TRUE(footprint.Arms[0].Open);
    ExpectAccepted(fixture, footprint);
}

// An outline with no area, an exclude with none and an arm with no width are reported and left out:
// nothing is built from them.
TEST(PavedFootprint, ASliverInputIsReportedAndNotBuilt)
{
    Fixture fixture;
    fixture.Outlines.push_back({{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.001f}});
    fixture.Outlines.push_back(Rectangle(30.0f, 0.0f, 40.0f, 10.0f));
    fixture.Excludes.push_back({{32.0f, 2.0f}, {35.0f, 2.0f}, {38.0f, 2.0f}});
    fixture.Arms.push_back(StraightArm(0, {-20.0f, 0.0f}, {0.0f, 1.0f}, 10.0f, 0.0f, 0.0f, 1.0f));
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    ASSERT_EQ(footprint.Issues.size(), 3u);
    EXPECT_EQ(footprint.Issues[0].Kind, PavedFootprintIssueKind::SliverOutline);
    EXPECT_EQ(footprint.Issues[0].Index, 0u);
    EXPECT_EQ(footprint.Issues[1].Kind, PavedFootprintIssueKind::SliverArm);
    EXPECT_EQ(footprint.Issues[2].Kind, PavedFootprintIssueKind::SliverExclude);
    ASSERT_EQ(footprint.Regions.size(), 1u) << "the square alone";
    EXPECT_EQ(footprint.Regions[0].Outlines, std::vector<uint32>{1u});
    EXPECT_TRUE(footprint.Regions[0].Pieces[0].Holes.empty());
    ExpectAccepted(fixture, footprint);
}


// A sliver arm, and an arm with one station at its node (no length), are reported and in no
// region: their stops name kNoPavedRegion, and no region is left without pieces.
TEST(PavedFootprint, ASliverArmOrOneWithNoLengthIsInNoRegion)
{
    Fixture fixture;
    fixture.Outlines.push_back(Rectangle(-10.0f, -10.0f, 10.0f, 10.0f));
    fixture.Arms.push_back(StraightArm(0, {0.0f, 50.0f}, {1.0f, 0.0f}, 20.0f, 0.0f, 0.0f, 1.0f));
    PavedArm single;
    single.Road = 1;
    single.Node = Vector2(50.0f, 50.0f);
    single.Stations.push_back(PavedArmStation{single.Node, 2.0f, 2.0f, 0.0f});
    fixture.Arms.push_back(single);
    const PavedFootprint footprint = BuildPavedFootprint(fixture.Input());
    ASSERT_EQ(footprint.Issues.size(), 2u);
    EXPECT_EQ(footprint.Issues[1].Kind, PavedFootprintIssueKind::SliverArm);
    EXPECT_EQ(footprint.Issues[1].Index, 1u) << "the arm with no length";
    EXPECT_EQ(footprint.Arms[0].Region, kNoPavedRegion);
    EXPECT_EQ(footprint.Arms[1].Region, kNoPavedRegion);
    ASSERT_EQ(footprint.Regions.size(), 1u) << "the square alone";
    EXPECT_FALSE(footprint.Regions[0].Pieces.empty());
    ExpectAccepted(fixture, footprint);
}

// An arm the caller passed shorter than its stop (3 m, under the stop's 4.5 m) of a road that goes
// on is Open at its end, with no border across the road; the same arm marked as the road's end is
// a dead end, bordered across.
TEST(PavedFootprint, AnArmShorterThanItsStopIsOpenUnlessItEndsTheRoad)
{
    Fixture fixture;
    fixture.Outlines.push_back(Rectangle(-10.0f, -10.0f, 10.0f, 10.0f));
    fixture.Arms.push_back(StraightArm(0, {0.0f, 10.0f}, {0.0f, 1.0f}, 3.0f, 2.0f, 0.0f, 1.0f));
    const PavedFootprint goesOn = BuildPavedFootprint(fixture.Input());
    EXPECT_NEAR(goesOn.Arms[0].StopDistance, 3.0f, 1.0e-3f);
    EXPECT_TRUE(goesOn.Arms[0].Open) << "the road goes on past the 3 m it was passed";
    ExpectAccepted(fixture, goesOn);

    fixture.Arms[0].EndsRoad = true;
    const PavedFootprint deadEnd = BuildPavedFootprint(fixture.Input());
    EXPECT_FALSE(deadEnd.Arms[0].Open);
    ASSERT_EQ(deadEnd.Regions.size(), 1u);
    ASSERT_EQ(deadEnd.Regions[0].Runs.size(), 1u);
    EXPECT_TRUE(deadEnd.Regions[0].Runs[0].Closed) << "the border runs across the road's end";
    ExpectAccepted(fixture, deadEnd);
}

namespace
{
// Two T junctions at x = -4 and x = 4 on a road along z = 0 whose arc length is 40 + x. On a closed
// road of 80 m (`loop`) the arc length is taken modulo 80 after `seamShift` is added, so the
// seam falls at x = 40 - seamShift.
Fixture CloseTOnARoad(bool loop, float seamShift)
{
    Fixture fixture;
    for (const float x : {-4.0f, 4.0f})
    {
        for (const float sign : {1.0f, -1.0f})
        {
            PavedArm arm = StraightArm(0, {x, 0.0f}, {sign, 0.0f}, sign > 0.0f ? 40.0f - x : 40.0f + x, 2.0f, 40.0f + x, sign);
            if (loop)
            {
                arm.RoadLoopMeters = 80.0f;
                for (PavedArmStation& station : arm.Stations)
                    station.Distance = std::fmod(station.Distance + seamShift, 80.0f);
            }
            fixture.Arms.push_back(arm);
        }
        fixture.Arms.push_back(StraightArm(x < 0.0f ? 1u : 2u, {x, 0.0f}, {0.0f, 1.0f}, 30.0f, 1.5f, 0.0f, 1.0f));
    }
    return fixture;
}
} // namespace

// On a closed road whose seam falls anywhere under the paving, every arm stops where it stops on
// an open road, and its interval, from NodeDistance to StopDistance in its direction of travel
// modulo the loop, is the open road's moved by the seam.
TEST(PavedFootprint, AClosedRoadsIntervalCrossesItsSeamModuloTheLoop)
{
    const Fixture open = CloseTOnARoad(false, 0.0f);
    const PavedFootprint reference = BuildPavedFootprint(open.Input());
    for (float seamShift = 30.0f; seamShift <= 50.0f; seamShift += 0.35f)
    {
        const Fixture closed = CloseTOnARoad(true, seamShift);
        const PavedFootprint footprint = BuildPavedFootprint(closed.Input());
        ASSERT_EQ(footprint.Arms.size(), reference.Arms.size());
        for (std::size_t i = 0; i < footprint.Arms.size(); ++i)
        {
            const PavedArmStop& expected = reference.Arms[i];
            const PavedArmStop& stop = footprint.Arms[i];
            const Vector2 capExpected = (expected.OpenLeft + expected.OpenRight) * 0.5f;
            const Vector2 cap = (stop.OpenLeft + stop.OpenRight) * 0.5f;
            EXPECT_NEAR((cap - capExpected).Length(), 0.0f, 1.0e-3f) << "arm " << i << ", seam shift " << seamShift;
            if (closed.Arms[i].RoadLoopMeters <= 0.0f)
                continue;
            EXPECT_GE(stop.NodeDistance, 0.0f);
            EXPECT_LT(stop.StopDistance, 80.0f);
            EXPECT_NEAR(std::remainder(stop.NodeDistance - expected.NodeDistance - seamShift, 80.0f), 0.0f, 1.0e-3f)
                << "arm " << i << ", seam shift " << seamShift;
            EXPECT_NEAR(std::remainder(stop.StopDistance - expected.StopDistance - seamShift, 80.0f), 0.0f, 1.0e-3f)
                << "arm " << i << ", seam shift " << seamShift;
        }
    }
}
