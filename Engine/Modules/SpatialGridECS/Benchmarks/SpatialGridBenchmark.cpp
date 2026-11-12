// The spatial grid's timings for the rule of the design note (spatial-grid.md, section 7). Each scene runs
// arm A (the grid), arm B (a scan with the same shape tests) and, in S1 to S3, arm C (physics box overlaps),
// interleaved, and prints one record line per scene and arm. SpatialGridBenchmarkPlanar builds this file with
// a grid keyed on X and Z (GE_SPATIAL_GRID_PLANAR_KEYS) and runs arm D in S1. Only a Release build gives numbers.
// build_ms is the time of the build in the tick; build_cpu_ms is the same build again, outside the tick.
#include "SpatialGrid/Grid.h"
#include "SpatialGrid/GridOverlap.h"
#include "Physics/PhysicsWorld.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "BuildInfo/BuildCommit.h"

namespace GameEngine::SpatialGrid
{

struct GridTestAccess
{
    // The key mode of the Grid code that this binary runs: two lone movers in different Y cell rows get
    // different tags only when the cell key has a Y axis.
    static const char* KeyMode()
    {
        return TagOfLoneMover(4.0) != TagOfLoneMover(0.0) ? "xyz" : "xz";
    }

    static uint32 TagOfLoneMover(float64 cellRowY)
    {
        constexpr float64 kCellSize = 16.0;
        const GridMover mover{{0.0, cellRowY * kCellSize, 0.0}, 0.0f, 1u, 0u};
        Grid grid(kCellSize);
        grid.Build({&mover, 1u});
        return grid.m_Tag.front();
    }
};

} // namespace GameEngine::SpatialGrid

namespace
{

using namespace GameEngine;
using namespace GameEngine::SpatialGrid;
using Mathematics::Vector3;
using Mathematics::WorldPosition;
using Clock = std::chrono::steady_clock;

enum class QueryKind : uint8
{
    Sphere,
    Cone,
    Nearest,
    Box,
    Segment,
    Count,
};

constexpr size_t kQueryKindCount = 6;
static_assert(static_cast<size_t>(QueryKind::Count) + 1 == kQueryKindCount, "QueryKind::Count is the last kind");
constexpr std::array<const char*, kQueryKindCount> kQueryKindNames{"sphere", "cone", "nearest",
                                                                    "box", "segment", "count"};

// The kinds of the note's CPU column. Box, Segment and Count are the mix: reported, with no threshold.
bool HasThreshold(QueryKind kind)
{
    return kind == QueryKind::Sphere || kind == QueryKind::Cone || kind == QueryKind::Nearest;
}

struct Query
{
    QueryKind Kind;
    WorldPosition Center;
    WorldPosition End;
    float64 Radius;
};

struct Scene
{
    const char* Name;
    float64 CellSize;
    std::vector<GridMover> Movers;
    std::vector<Vector3> Velocities;
    std::vector<Query> Queries;
};

struct Options
{
    uint32 Runs = 9;
    uint32 Ticks = 200;
    uint32 Warmup = 20;
    // Comma-separated scene names, run in this order; empty runs every scene.
    std::string Scenes;
    // Comma-separated scene names that run without arm B. Rule 1 still compares with the scan.
    std::string SkipScan;
    // Rotates the scene order and the arm order by this count, so no scene and no arm is always first.
    uint32 Rotate = 0;
};

#if defined(GE_SPATIAL_GRID_PLANAR_KEYS) && GE_SPATIAL_GRID_PLANAR_KEYS
constexpr char kGridArm = 'D';
#else
constexpr char kGridArm = 'A';
#endif

constexpr float32 kConeHalfAngle = 0.6f;
constexpr uint32 kNearestCount = 8;
constexpr float64 kStepSeconds = 1.0 / 30.0;
// Keeps the query results live in an optimized build.
volatile uint64 g_Sink = 0;

WorldPosition Uniform(std::mt19937& random, float64 extentX, float64 extentY, float64 extentZ)
{
    std::uniform_real_distribution<float64> unit(0.0, 1.0);
    return {unit(random) * extentX, unit(random) * extentY, unit(random) * extentZ};
}

// Adds `count` queries of `kind` centered on random movers, plus one tenth each of Box, Segment and Count.
void AddQueries(Scene& scene, std::mt19937& random, QueryKind kind, uint32 count, float64 radius)
{
    std::uniform_int_distribution<size_t> pick(0, scene.Movers.size() - 1);
    const auto add = [&scene, &random, &pick, radius](QueryKind queryKind, uint32 queryCount)
    {
        for (uint32 i = 0; i < queryCount; ++i)
        {
            const WorldPosition center = scene.Movers[pick(random)].Position;
            const WorldPosition end{center.X + radius, center.Y, center.Z + radius * 0.5};
            scene.Queries.push_back({queryKind, center, end, radius});
        }
    };
    add(kind, count);
    add(QueryKind::Box, count / 10);
    add(QueryKind::Segment, count / 10);
    add(QueryKind::Count, count / 10);
}

// The risky layouts of section 7: all movers in one cell, every mover on a cell edge, half in 5 % of the area.
enum class Spread : uint8
{
    Uniform,
    OneCell,
    OnEdges,
    Clustered,
};

WorldPosition Place(std::mt19937& random, Spread spread, uint32 index, float64 extent)
{
    switch (spread)
    {
    case Spread::Uniform: return Uniform(random, extent, 2.0, extent);
    case Spread::OneCell: return Uniform(random, 15.0, 2.0, 15.0);
    case Spread::OnEdges:
    {
        const WorldPosition p = Uniform(random, extent, 2.0, extent);
        return index % 2u == 0u ? WorldPosition{std::floor(p.X / 16.0) * 16.0, p.Y, p.Z}
                                : WorldPosition{p.X, p.Y, std::floor(p.Z / 16.0) * 16.0};
    }
    case Spread::Clustered:
    {
        // 5 % of the area is a square of side extent * sqrt(0.05).
        const float64 side = index % 2u == 0u ? extent * 0.2236 : extent;
        return Uniform(random, side, 2.0, side);
    }
    }
    return {};
}

Scene MakePlane(const char* name, uint32 movers, uint32 sphereQueries, float64 extent, float64 offset,
                uint32 staticMovers, Spread spread = Spread::Uniform)
{
    std::mt19937 random(1234u + movers);
    Scene scene{name, 16.0, {}, {}, {}};
    std::uniform_real_distribution<float32> speed(-3.0f, 3.0f);
    for (uint32 i = 0; i < movers; ++i)
    {
        WorldPosition p = Place(random, spread, i, extent);
        p.X += offset;
        p.Z += offset;
        scene.Movers.push_back({p, 0.5f, 1u, i});
        const bool moves = i >= staticMovers;
        scene.Velocities.push_back(moves ? Vector3{speed(random), 0.0f, speed(random)} : Vector3{0.0f, 0.0f, 0.0f});
    }
    if (movers == 2000u)
    {
        for (uint32 i = 0; i < 100; ++i)
        {
            WorldPosition p = Uniform(random, extent, 0.0, extent);
            p.X += offset;
            p.Z += offset;
            scene.Movers.push_back({p, 12.0f, 1u, movers + i});
            scene.Velocities.push_back({0.0f, 0.0f, 0.0f});
        }
    }
    AddQueries(scene, random, QueryKind::Sphere, sphereQueries, 10.2);
    return scene;
}

std::vector<Scene> MakeScenes()
{
    std::vector<Scene> scenes;
    scenes.push_back(MakePlane("S1", 2000, 500, 256.0, 0.0, 0));
    scenes.push_back(MakePlane("S1-1k", 1000, 250, 181.0, 0.0, 0));
    scenes.push_back(MakePlane("S1-10k", 10000, 2500, 573.0, 0.0, 0));
    scenes.push_back(MakePlane("S1-onecell", 2000, 500, 256.0, 0.0, 0, Spread::OneCell));
    scenes.push_back(MakePlane("S1-edges", 2000, 500, 256.0, 0.0, 0, Spread::OnEdges));
    scenes.push_back(MakePlane("S1-clustered", 2000, 500, 256.0, 0.0, 0, Spread::Clustered));

    std::mt19937 random(77u);
    Scene ecosystem{"S2", 64.0, {}, {}, {}};
    std::uniform_real_distribution<float64> herdSpread(-60.0, 60.0);
    std::uniform_real_distribution<float32> speed(-2.0f, 2.0f);
    for (uint32 herd = 0; herd < 100; ++herd)
    {
        const WorldPosition center = Uniform(random, 30000.0, 0.0, 30000.0);
        for (uint32 i = 0; i < 100; ++i)
        {
            ecosystem.Movers.push_back(
                {{center.X + herdSpread(random), 0.0, center.Z + herdSpread(random)}, 0.8f, 1u, herd * 100u + i});
            ecosystem.Velocities.push_back({speed(random), 0.0f, speed(random)});
        }
    }
    AddQueries(ecosystem, random, QueryKind::Sphere, 475, 50.0);
    scenes.push_back(std::move(ecosystem));

    Scene building{"S3", 16.0, {}, {}, {}};
    for (uint32 i = 0; i < 50; ++i)
    {
        WorldPosition p = Uniform(random, 60.0, 0.0, 60.0);
        p.Y = 4.0 * static_cast<float64>(i % 3);
        building.Movers.push_back({p, 0.4f, 1u, i});
        building.Velocities.push_back({speed(random), 0.0f, speed(random)});
    }
    AddQueries(building, random, QueryKind::Cone, 25, 30.0);
    scenes.push_back(std::move(building));

    // The flight preset: the cell is the 10 km query diameter rounded down to a power of two.
    Scene flight{"S4", 8192.0, {}, {}, {}};
    std::uniform_real_distribution<float32> flightSpeed(-200.0f, 200.0f);
    for (uint32 i = 0; i < 1000; ++i)
    {
        flight.Movers.push_back({Uniform(random, 100000.0, 100000.0, 100000.0), 10.0f, 1u, i});
        flight.Velocities.push_back({flightSpeed(random), flightSpeed(random), flightSpeed(random)});
    }
    AddQueries(flight, random, QueryKind::Sphere, 50, 5000.0);
    AddQueries(flight, random, QueryKind::Nearest, 50, 5000.0);
    scenes.push_back(std::move(flight));

    scenes.push_back(MakePlane("S5", 100000, 5000, 1810.0, 0.0, 0));
    scenes.push_back(MakePlane("S6", 2000, 500, 256.0, 300000.0, 0));
    scenes.push_back(MakePlane("S7", 10000, 2500, 573.0, 0.0, 6000));
    // Each kind's queries are contiguous, so one clock read pair times each kind.
    for (Scene& scene : scenes)
    {
        std::stable_sort(scene.Queries.begin(), scene.Queries.end(),
                         [](const Query& a, const Query& b) { return a.Kind < b.Kind; });
    }
    return scenes;
}

struct KindRange
{
    QueryKind Kind;
    size_t Begin;
    size_t End;
};

// The runs of one kind in a scene's queries, which MakeScenes groups by kind.
std::vector<KindRange> KindRangesOf(const Scene& scene)
{
    std::vector<KindRange> ranges;
    for (size_t i = 0; i < scene.Queries.size(); ++i)
    {
        if (ranges.empty() || ranges.back().Kind != scene.Queries[i].Kind)
            ranges.push_back({scene.Queries[i].Kind, i, i});
        ranges.back().End = i + 1;
    }
    return ranges;
}

void Move(Scene& scene)
{
    for (size_t i = 0; i < scene.Movers.size(); ++i)
    {
        scene.Movers[i].Position.X += scene.Velocities[i].x * kStepSeconds;
        scene.Movers[i].Position.Y += scene.Velocities[i].y * kStepSeconds;
        scene.Movers[i].Position.Z += scene.Velocities[i].z * kStepSeconds;
    }
}

uint32 RunGridQuery(const Grid& grid, const Query& query, std::span<GridHit> out)
{
    const GridFilter filter{};
    switch (query.Kind)
    {
    case QueryKind::Sphere: return grid.Sphere(query.Center, query.Radius, filter, out);
    case QueryKind::Cone:
        return grid.Cone(query.Center, Vector3{1.0f, 0.0f, 0.0f}, query.Radius, kConeHalfAngle, filter, out);
    case QueryKind::Nearest: return grid.Nearest(query.Center, query.Radius, filter, out.first(kNearestCount));
    case QueryKind::Box:
    {
        const float32 half = static_cast<float32>(query.Radius);
        return grid.Box(query.Center, Vector3{half, half, half}, filter, out);
    }
    case QueryKind::Segment: return grid.Segment(query.Center, query.End, 1.0, filter, out);
    case QueryKind::Count: return grid.Count(query.Center, query.Radius, filter);
    }
    return 0u;
}

// Arm B: every mover through the same shape test, then the same order rule.
uint32 RunScanQuery(std::span<const GridMover> movers, const Query& query, std::vector<GridHit>& scratch,
                    std::span<GridHit> out)
{
    scratch.clear();
    const float64 cosAngle = std::cos(static_cast<float64>(kConeHalfAngle));
    const float64 sinAngle = std::sin(static_cast<float64>(kConeHalfAngle));
    const float32 half = static_cast<float32>(query.Radius);
    bool byDistance = false;
    for (const GridMover& m : movers)
    {
        float64 key = DistanceSquared(query.Center, m.Position);
        bool hit = false;
        switch (query.Kind)
        {
        case QueryKind::Sphere:
        case QueryKind::Count: hit = OverlapsSphere(query.Center, query.Radius, m.Position, m.Radius); break;
        case QueryKind::Nearest:
            hit = OverlapsSphere(query.Center, query.Radius, m.Position, m.Radius);
            byDistance = true;
            break;
        case QueryKind::Cone:
            hit = OverlapsCone(query.Center, Vector3{1.0f, 0.0f, 0.0f}, query.Radius, cosAngle, sinAngle,
                               m.Position, m.Radius);
            break;
        case QueryKind::Box: hit = OverlapsBox(query.Center, Vector3{half, half, half}, m.Position, m.Radius); break;
        case QueryKind::Segment:
            hit = OverlapsSegment(query.Center, query.End, 1.0, m.Position, m.Radius, key);
            byDistance = true;
            break;
        }
        if (hit)
            scratch.push_back({m.Id, key});
    }
    if (query.Kind == QueryKind::Count)
        return static_cast<uint32>(scratch.size());
    std::sort(scratch.begin(), scratch.end(),
              [byDistance](const GridHit& a, const GridHit& b)
              {
                  if (byDistance && a.DistanceSquared != b.DistanceSquared)
                      return a.DistanceSquared < b.DistanceSquared;
                  return a.Id < b.Id;
              });
    const size_t limit = query.Kind == QueryKind::Nearest ? kNearestCount : out.size();
    std::copy_n(scratch.begin(), std::min(scratch.size(), limit), out.begin());
    return static_cast<uint32>(scratch.size());
}

// Rule 1: arm A and arm B give the same hits for every query of the scene.
bool SameResults(const Scene& scene, const Grid& grid)
{
    std::vector<GridHit> a(scene.Movers.size());
    std::vector<GridHit> b(scene.Movers.size());
    std::vector<GridHit> scratch;
    for (const Query& query : scene.Queries)
    {
        const uint32 countA = RunGridQuery(grid, query, a);
        const uint32 countB = RunScanQuery(scene.Movers, query, scratch, b);
        if (countA != countB)
            return false;
        const size_t listed = query.Kind == QueryKind::Count ? 0u
                              : query.Kind == QueryKind::Nearest ? std::min<size_t>(countA, kNearestCount)
                                                                 : countA;
        for (size_t i = 0; i < listed; ++i)
        {
            if (a[i].Id != b[i].Id || a[i].DistanceSquared != b[i].DistanceSquared)
                return false;
        }
    }
    return true;
}

// The bounding box of a query: arm C's query, and the Box query of arm A that rule 5 compares with it.
void QueryBox(const Query& query, WorldPosition& center, Vector3& half)
{
    if (query.Kind == QueryKind::Segment)
    {
        center = {(query.Center.X + query.End.X) * 0.5, (query.Center.Y + query.End.Y) * 0.5,
                  (query.Center.Z + query.End.Z) * 0.5};
        half = Vector3{static_cast<float32>(std::abs(query.End.X - query.Center.X) * 0.5 + 1.0),
                       static_cast<float32>(std::abs(query.End.Y - query.Center.Y) * 0.5 + 1.0),
                       static_cast<float32>(std::abs(query.End.Z - query.Center.Z) * 0.5 + 1.0)};
        return;
    }
    const float32 r = static_cast<float32>(query.Radius);
    center = query.Center;
    half = Vector3{r, r, r};
}

// Arm C: one kinematic sphere body per mover in a physics world.
class PhysicsArm
{
public:
    explicit PhysicsArm(const Scene& scene)
        : m_World(MakeSettings())
    {
        for (const GridMover& mover : scene.Movers)
        {
            Physics::SphereShapeDef sphere;
            sphere.radius = std::max(mover.Radius, 0.01f);
            Physics::BodySettings body;
            body.shape = m_World.CreateShape(sphere);
            body.position = mover.Position;
            body.motionType = Physics::MotionType::Kinematic;
            body.layer = Physics::Layers::Kinematic;
            body.userData = mover.Id;
            m_Bodies.push_back(m_World.CreateBody(body));
        }
        m_World.OptimizeBroadphase();
    }

    // The arm's build: each body teleports to its mover.
    void Move(const Scene& scene)
    {
        for (size_t i = 0; i < m_Bodies.size(); ++i)
            m_World.SetBodyTransform(m_Bodies[i], {scene.Movers[i].Position, Mathematics::Quaternion::Identity()},
                                     Physics::ActivationMode::DontActivate);
    }

    uint32 Run(const Query& query)
    {
        Physics::BoxOverlapQuery box;
        QueryBox(query, box.center, box.halfExtents);
        m_World.BoxOverlap(box, m_Hits, static_cast<uint32>(m_Bodies.size()));
        return static_cast<uint32>(m_Hits.size());
    }

private:
    static Physics::PhysicsWorldSettings MakeSettings()
    {
        Physics::PhysicsWorldSettings settings;
        settings.numThreads = 1;
        return settings;
    }

    Physics::PhysicsWorld m_World;
    std::vector<Physics::BodyHandle> m_Bodies;
    std::vector<Physics::OverlapResult> m_Hits;
};

bool HasPhysicsArm(const Scene& scene)
{
    const std::string_view name = scene.Name;
    return name == "S1" || name == "S2" || name == "S3";
}

// True when the grid arm runs the bounding boxes of rule 5 after the timed queries of each tick: a scene with
// a physics arm, and S6. The boxes change the processor state that the next tick's queries start from, so S6
// runs them as S1 does, and the two scenes differ in position only.
bool TimesBoxes(const Scene& scene)
{
    return HasPhysicsArm(scene) || std::string_view(scene.Name) == "S6";
}

struct TickTimes
{
    std::vector<float64> BuildMs;
    std::vector<float64> QueryMs;
    // Rule 5: the bounding boxes of the queries through Grid::Box, timed apart from the tick. The grid
    // arm only, in a scene that TimesBoxes names; else empty.
    std::vector<float64> BoxMs;
    // Per run, the mean ms per tick of each kind the scene makes; empty for a kind it does not make.
    std::array<std::vector<float64>, kQueryKindCount> KindMs;
    std::vector<float64> ThresholdQueryMs;
    std::vector<float64> MixQueryMs;
    // Per tick, the build plus the queries of the kinds with a threshold.
    std::vector<float64> ThresholdTickMs;
    // The grid arm only: the build again, outside the tick.
    std::vector<float64> BuildCpuMs;
};

float64 Milliseconds(Clock::duration duration)
{
    return std::chrono::duration<float64, std::milli>(duration).count();
}

float64 Median(std::vector<float64> values)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

float64 Percentile99(std::vector<float64> values)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, values.size() * 99 / 100)];
}

// One run of one arm: `warmup` ticks, then `ticks` timed ticks. Adds the mean ms per tick of the run to `times`.
// With `checkMoved`, returns rule 1 on the positions of the last tick; else returns true.
bool RunArm(char arm, Scene scene, const Options& options, TickTimes& times,
            std::vector<float64>& tickTotals, bool checkMoved)
{
    Grid grid(scene.CellSize);
    std::unique_ptr<PhysicsArm> physics = arm == 'C' ? std::make_unique<PhysicsArm>(scene) : nullptr;
    const bool usesGrid = arm == kGridArm;
    const bool timesBoxes = usesGrid && TimesBoxes(scene);
    const std::vector<KindRange> kinds = KindRangesOf(scene);
    std::vector<GridHit> out(scene.Movers.size());
    std::vector<GridHit> scratch;
    float64 buildSum = 0.0;
    float64 buildCpuSum = 0.0;
    float64 querySum = 0.0;
    float64 boxSum = 0.0;
    std::array<float64, kQueryKindCount> kindSum{};
    uint64 checksum = 0;
    for (uint32 tick = 0; tick < options.Warmup + options.Ticks; ++tick)
    {
        Move(scene);
        const Clock::time_point start = Clock::now();
        if (usesGrid)
            grid.Build(scene.Movers);
        else if (physics)
            physics->Move(scene);
        const Clock::time_point built = Clock::now();
        std::array<float64, kQueryKindCount> kindMs{};
        for (const KindRange& range : kinds)
        {
            const Clock::time_point kindStart = Clock::now();
            for (size_t i = range.Begin; i < range.End; ++i)
            {
                const Query& query = scene.Queries[i];
                checksum += usesGrid  ? RunGridQuery(grid, query, out)
                            : physics ? physics->Run(query)
                                      : RunScanQuery(scene.Movers, query, scratch, out);
            }
            kindMs[static_cast<size_t>(range.Kind)] = Milliseconds(Clock::now() - kindStart);
        }
        const Clock::time_point done = Clock::now();
        if (timesBoxes)
        {
            for (const Query& query : scene.Queries)
            {
                WorldPosition center;
                Vector3 half;
                QueryBox(query, center, half);
                checksum += grid.Box(center, half, GridFilter{}, out);
            }
        }
        const Clock::time_point boxed = Clock::now();
        if (tick < options.Warmup)
            continue;
        const float64 buildMs = Milliseconds(built - start);
        float64 thresholdMs = 0.0;
        for (const KindRange& range : kinds)
        {
            const size_t kind = static_cast<size_t>(range.Kind);
            kindSum[kind] += kindMs[kind];
            if (HasThreshold(range.Kind))
                thresholdMs += kindMs[kind];
        }
        buildSum += buildMs;
        querySum += Milliseconds(done - built);
        boxSum += Milliseconds(boxed - done);
        tickTotals.push_back(Milliseconds(done - start));
        times.ThresholdTickMs.push_back(buildMs + thresholdMs);
    }
    // The build again, after the timed ticks, so it does not touch them. The layout does not change.
    for (uint32 tick = 0; usesGrid && tick < options.Ticks; ++tick)
    {
        const Clock::time_point cpuStart = Clock::now();
        grid.Build(scene.Movers);
        buildCpuSum += Milliseconds(Clock::now() - cpuStart);
    }
    times.BuildMs.push_back(buildSum / options.Ticks);
    times.QueryMs.push_back(querySum / options.Ticks);
    if (timesBoxes)
        times.BoxMs.push_back(boxSum / options.Ticks);
    float64 thresholdSum = 0.0;
    float64 mixSum = 0.0;
    for (const KindRange& range : kinds)
    {
        const size_t kind = static_cast<size_t>(range.Kind);
        times.KindMs[kind].push_back(kindSum[kind] / options.Ticks);
        (HasThreshold(range.Kind) ? thresholdSum : mixSum) += kindSum[kind];
    }
    times.ThresholdQueryMs.push_back(thresholdSum / options.Ticks);
    times.MixQueryMs.push_back(mixSum / options.Ticks);
    if (usesGrid)
        times.BuildCpuMs.push_back(buildCpuSum / options.Ticks);
    g_Sink = g_Sink + checksum;
    // Rule 1 on moved positions: the grid holds the build of the last tick, and the scan reads the same movers.
    return !checkMoved || SameResults(scene, grid);
}

bool ParseOptions(std::span<char*> arguments, Options& options)
{
    for (size_t i = 1; i + 1 < arguments.size(); i += 2)
    {
        const std::string_view name = arguments[i];
        if (name == "--scenes")
        {
            options.Scenes = arguments[i + 1];
            continue;
        }
        if (name == "--skip-scan")
        {
            options.SkipScan = arguments[i + 1];
            continue;
        }
        const uint32 value = static_cast<uint32>(std::strtoul(arguments[i + 1], nullptr, 10));
        if (name == "--runs")
            options.Runs = value;
        else if (name == "--rotate")
            options.Rotate = value;
        else if (name == "--ticks")
            options.Ticks = value;
        else if (name == "--warmup")
            options.Warmup = value;
        else
            return false;
    }
    return options.Runs > 0u && options.Ticks > 0u && arguments.size() % 2 == 1;
}

bool IsListed(const std::string& list, const Scene& scene)
{
    const std::vector<std::string_view> names = Split(list, ',');
    return std::find(names.begin(), names.end(), std::string_view(scene.Name)) != names.end();
}

// The scenes in the order of --scenes, rotated by --rotate. False when --scenes names a scene that does not exist.
bool OrderScenes(const std::vector<Scene>& scenes, const Options& options, std::vector<const Scene*>& ordered)
{
    if (options.Scenes.empty())
    {
        for (const Scene& scene : scenes)
        {
            if (kGridArm == 'A' || std::string_view(scene.Name) == "S1")
                ordered.push_back(&scene);
        }
    }
    for (const std::string_view name : Split(options.Scenes, ','))
    {
        const auto found = std::find_if(scenes.begin(), scenes.end(),
                                        [name](const Scene& scene) { return name == scene.Name; });
        if (found == scenes.end())
        {
            std::printf("unknown scene %.*s\n", static_cast<int>(name.size()), name.data());
            return false;
        }
        ordered.push_back(&*found);
    }
    if (!ordered.empty())
        std::rotate(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(options.Rotate % ordered.size()),
                    ordered.end());
    return true;
}

// The instrument checks of section 7: the timer against a sleep of known length, and arm B's count against a
// second loop that shares no code with the shape tests.
void CheckInstrument(const Scene& scene)
{
    const Clock::time_point start = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::printf("instrument timer_ms=%.3f expected_ms=50 (at least)\n", Milliseconds(Clock::now() - start));
    std::vector<GridHit> scratch;
    std::vector<GridHit> out(scene.Movers.size());
    uint64 scanned = 0;
    uint64 counted = 0;
    for (const Query& query : scene.Queries)
    {
        if (query.Kind != QueryKind::Sphere)
            continue;
        scanned += RunScanQuery(scene.Movers, query, scratch, out);
        for (const GridMover& m : scene.Movers)
        {
            const float64 dx = m.Position.X - query.Center.X;
            const float64 dy = m.Position.Y - query.Center.Y;
            const float64 dz = m.Position.Z - query.Center.Z;
            const float64 reach = query.Radius + m.Radius;
            counted += dx * dx + dy * dy + dz * dz <= reach * reach ? 1u : 0u;
        }
    }
    std::printf("instrument scene=%s scan_count=%llu loop_count=%llu %s\n", scene.Name,
                static_cast<unsigned long long>(scanned), static_cast<unsigned long long>(counted),
                scanned == counted ? "PASS" : "FAIL");
}

const char* Verdict(bool passed)
{
    return passed ? "PASS" : "FAIL";
}

// The fields of every record; rule1 is rule 1 at tick 0, rule1_moved after the last tick of the grid arm.
void PrintRecordStart(const Scene& scene, char arm, const char* keyMode)
{
    std::printf("sha=%s config=%s key=%s scene=%s arm=%c movers=%zu queries=%zu", BuildInfo::kBuildCommit,
                GE_BUILD_CONFIG, keyMode, scene.Name, arm, scene.Movers.size(), scene.Queries.size());
}

void PrintRecord(const Scene& scene, char arm, const Options& options, const TickTimes& times,
                 const std::vector<float64>& totals, bool rule1, bool movedRule1, const char* keyMode)
{
    PrintRecordStart(scene, arm, keyMode);
    std::printf(" runs=%u ticks=%u build_ms=%.4f query_ms=%.4f total_ms=%.4f tick_p99_ms=%.4f", options.Runs,
                options.Ticks, Median(times.BuildMs), Median(times.QueryMs),
                Median(times.BuildMs) + Median(times.QueryMs), Percentile99(totals));
    if (!times.BoxMs.empty())
        std::printf(" box_ms=%.4f", Median(times.BoxMs));
    std::printf(" rule1=%s", Verdict(rule1));
    std::printf(" rule1_moved=%s threshold_query_ms=%.4f mix_query_ms=%.4f threshold_total_ms=%.4f "
                "threshold_tick_p99_ms=%.4f",
                Verdict(movedRule1), Median(times.ThresholdQueryMs), Median(times.MixQueryMs),
                Median(times.BuildMs) + Median(times.ThresholdQueryMs), Percentile99(times.ThresholdTickMs));
    if (!times.BuildCpuMs.empty())
        std::printf(" build_cpu_ms=%.4f", Median(times.BuildCpuMs));
    for (size_t kind = 0; kind < kQueryKindCount; ++kind)
    {
        if (!times.KindMs[kind].empty())
            std::printf(" %s_query_ms=%.4f", kQueryKindNames[kind], Median(times.KindMs[kind]));
    }
    std::printf("\n");
}

// A record of an arm that --skip-scan removed: no time field, so no zero reads as a time.
void PrintSkippedRecord(const Scene& scene, char arm, bool rule1, bool movedRule1, const char* keyMode)
{
    PrintRecordStart(scene, arm, keyMode);
    std::printf(" arm_status=skipped rule1=%s rule1_moved=%s\n", Verdict(rule1), Verdict(movedRule1));
}

bool SkipsScan(const Options& options, const Scene& scene)
{
    return kGridArm == 'A' && IsListed(options.SkipScan, scene);
}

// The arms of a scene: arm D runs alone in its own binary; arm C only where a physics world is plausible.
std::vector<char> ArmsOf(const Scene& scene, const Options& options)
{
    std::vector<char> arms{kGridArm};
    if (kGridArm == 'A' && !SkipsScan(options, scene))
        arms.push_back('B');
    if (kGridArm == 'A' && HasPhysicsArm(scene))
        arms.push_back('C');
    return arms;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseOptions({argv, static_cast<size_t>(argc)}, options))
    {
        std::printf("usage: SpatialGridBenchmark [--runs N] [--ticks N] [--warmup N] [--scenes S1,S2,...] "
                    "[--skip-scan S5,...] [--rotate N]\n");
        return 2;
    }
    const std::vector<Scene> scenes = MakeScenes();
    std::vector<const Scene*> ordered;
    if (!OrderScenes(scenes, options, ordered))
        return 2;
    CheckInstrument(scenes.front());
    const char* keyMode = GridTestAccess::KeyMode();
    for (const Scene* scene : ordered)
    {
        Grid check(scene->CellSize);
        check.Build(scene->Movers);
        const bool rule1 = SameResults(*scene, check);
        const std::vector<char> arms = ArmsOf(*scene, options);
        std::vector<TickTimes> times(arms.size());
        std::vector<std::vector<float64>> totals(arms.size());
        bool movedRule1 = true;
        for (uint32 run = 0; run < options.Runs; ++run)
        {
            // Interleaved: the arm that runs first rotates per run and with --rotate.
            for (size_t slot = 0; slot < arms.size(); ++slot)
            {
                const size_t arm = (run + slot + options.Rotate) % arms.size();
                const bool checkMoved = run == 0u && arms[arm] == kGridArm;
                const bool armRule1 = RunArm(arms[arm], *scene, options, times[arm], totals[arm], checkMoved);
                movedRule1 = movedRule1 && armRule1;
            }
        }
        for (size_t arm = 0; arm < arms.size(); ++arm)
            PrintRecord(*scene, arms[arm], options, times[arm], totals[arm], rule1, movedRule1, keyMode);
        if (SkipsScan(options, *scene))
            PrintSkippedRecord(*scene, 'B', rule1, movedRule1, keyMode);
    }
    return 0;
}
