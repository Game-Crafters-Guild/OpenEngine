// The streamed-tile height bake and the route query it spends its time in: the segment grid gives
// the scan's answer bit for bit, and the arrival bake's bytes are pinned for a scene with a route,
// a polygon area and noise across a tile border.

#include <gtest/gtest.h>

#include "TerrainTestHelpers.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainRoutePolyline.h"
#include "TerrainECS/TerrainService.h"
#include "Types/Fnv1a.h"

#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

constexpr float32 kTileWorld = 1024.0f;
constexpr float32 kHeightScale = 64.0f;
constexpr float32 kRouteRadius = 4.0f;
constexpr float32 kStationSpacing = 0.5f;

// A road that winds and turns back on itself, so segments far apart in index lie
// near each other in XZ. Crosses the border between tiles 0 and 1 at X = 1024.
const std::vector<Mathematics::Vector3> kRoutePoints = {
    {760.0f, 4.0f, 300.0f},  {900.0f, 9.0f, 380.0f},  {1060.0f, 15.0f, 340.0f},
    {1180.0f, 12.0f, 420.0f}, {1090.0f, 7.0f, 470.0f}, {960.0f, 3.0f, 430.0f},
};

// A closed area in tile 1.
const std::vector<Mathematics::Vector3> kAreaPoints = {
    {1500.0f, 0.0f, 560.0f}, {1620.0f, 0.0f, 590.0f}, {1600.0f, 0.0f, 700.0f}, {1480.0f, 0.0f, 660.0f},
};

SplineECS::SplineHandle MakeSpline(const std::vector<Mathematics::Vector3>& points, bool closed)
{
    auto& svc = SplineECS::SplineService::Get();
    const SplineECS::SplineHandle handle = svc.CreateSpline(Spline::SplineType::CatmullRom, closed);
    auto* data = svc.GetSplineData(handle);
    for (const auto& p : points)
        data->AddPoint(p, kRouteRadius);
    svc.RebuildCache(handle);
    return handle;
}

ECS::EntityHandle AddSplineVolume(ECS::World& world, const std::vector<Mathematics::Vector3>& points, bool closed,
                                  Components::TerrainVolumeShape shape, float32 falloff, float32 priority)
{
    const SplineECS::SplineHandle handle = MakeSpline(points, closed);
    auto e = world.CreateEntity();
    Components::SplineComponent comp{};
    comp.SplineDataIndex = handle.Index();
    comp.SplineDataGeneration = handle.Generation();
    world.AddComponentImmediate<Components::SplineComponent>(e, comp);
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    Components::TerrainModifierVolume volume{};
    volume.Shape = shape;
    volume.Falloff = falloff;
    volume.Priority = priority;
    volume.StationSpacing = kStationSpacing;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, volume);
    return e;
}

// The entities of the authored scene that a test edits.
struct AuthoredScene
{
    ECS::EntityHandle Area;
    ECS::EntityHandle Noise;
};

AuthoredScene AuthorScene(ECS::World& world, TiledTerrainHandle handle, float32 sizeZ = kTileWorld)
{
    auto terrainEntity = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = 2.0f * kTileWorld;
    terrain.SizeZ = sizeZ;
    terrain.HeightScale = kHeightScale;
    terrain.TiledTerrainHandle = handle.Index;
    terrain.TiledTerrainGeneration = handle.Generation;
    world.AddComponentImmediate<Components::Terrain>(terrainEntity, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(terrainEntity, Components::WorldTransform{});

    // The route: a pooled flatten that grades to the route's own heights.
    const auto road = AddSplineVolume(world, kRoutePoints, false, Components::TerrainVolumeShape::SplinePath,
                                      10.0f, 10.0f);
    Components::TerrainFlattenEffect roadFlatten{};
    roadFlatten.Blend = Components::TerrainModifierBlend::Average;
    roadFlatten.UseVolumeHeight = true;
    world.AddComponentImmediate(road, roadFlatten);

    // The polygon area.
    const auto area = AddSplineVolume(world, kAreaPoints, true, Components::TerrainVolumeShape::SplineArea, 12.0f,
                                      20.0f);
    Components::TerrainFlattenEffect areaFlatten{};
    areaFlatten.TargetHeight = 6.0f;
    world.AddComponentImmediate(area, areaFlatten);

    // Noise on a circle across the tile border.
    auto noiseEntity = world.CreateEntity();
    Components::TerrainModifierVolume noiseVolume{};
    noiseVolume.Shape = Components::TerrainVolumeShape::Circle;
    noiseVolume.Radius = 90.0f;
    noiseVolume.Falloff = 20.0f;
    noiseVolume.Priority = 5.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(noiseEntity, noiseVolume);
    Components::WorldTransform noiseTransform{};
    noiseTransform.Position.X = 1020.0f;
    noiseTransform.Position.Z = 600.0f;
    world.AddComponentImmediate<Components::WorldTransform>(noiseEntity, noiseTransform);
    Components::TerrainNoiseEffect noise{};
    noise.Amplitude = 5.0f;
    noise.Frequency = 8.0f;
    noise.Octaves = 3u;
    world.AddComponentImmediate(noiseEntity, noise);
    return AuthoredScene{area, noiseEntity};
}

// Streams tile `coord` in as Full with every sample at zero, not at the base noise. The arrival
// bake writes every sample, so a row it skips keeps zero and changes the pinned bytes.
void StreamInFullTile(TerrainService& svc, TiledTerrainHandle handle, TileCoord coord)
{
    const auto& tileConfig = svc.GetTiledTerrainData(handle)->Config.TileConfig;
    svc.CreateEmptyTile(handle, coord);
    Terrain::HeightfieldData heightfield;
    heightfield.Resize(tileConfig.HeightmapWidth, tileConfig.HeightmapHeight, 0.0f);
    svc.SetTileHeightfield(handle, coord, std::move(heightfield), 0.0f, 0.0f);
}

struct StreamedBake
{
    uint64 Tile0Hash = 0;
    uint64 Tile1Hash = 0;
    uint32 ArrivalHeightBands = 0;
};

uint64 HeightHash(const TerrainTileData& tile)
{
    return Hashing::Fnv1a64(tile.Heightfield.GetRawSamples(), tile.Heightfield.GetSampleCount() * sizeof(float32));
}

// Tile 0 resident and fully baked; then tile 1 streams in and takes the arrival bake.
StreamedBake BakeStreamedScene(JobSystem::WorkStealingThreadPool* pool)
{
    if (TerrainService::IsInitialized())
        TerrainService::Shutdown();
    TerrainService::Initialize();
    if (SplineECS::SplineService::IsInitialized())
        SplineECS::SplineService::Shutdown();
    SplineECS::SplineService::Initialize();
    auto& svc = TerrainService::Get();

    TiledTerrainConfig config{};
    config.Base = TerrainTests::GeneratedNoiseBase();
    config.WorldSizeX = 2.0f * kTileWorld;
    config.WorldSizeZ = kTileWorld;
    config.HeightScale = kHeightScale;
    config.SamplesPerMeter = 1.0f;
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(config);
    auto* tiled = svc.GetTiledTerrainData(handle);
    svc.LoadTile(handle, TileCoord{0, 0})->LodState = TileLodState::Full;

    ECS::World world;
    world.SetJobSystem(pool);
    EnableTerrainModifierLifecycleEvents(world);
    AuthorScene(world, handle);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    StreamInFullTile(svc, handle, TileCoord{1, 0});
    const TerrainTileData& arrival = *tiled->Tiles.at(TileCoord{1, 0});
    const uint32 maxFrames = arrival.Heightfield.GetHeight() / TerrainModifierSystem::kArrivalSplatRowsPerFrame + 4u;
    for (uint32 frame = 0; frame < maxFrames && !arrival.ModifiersApplied; ++frame)
        system.Update(world, 1.0f / 60.0f);

    StreamedBake result;
    EXPECT_TRUE(arrival.ModifiersApplied) << "the arrival bake must finish within the row budget's frames";
    result.Tile0Hash = HeightHash(*tiled->Tiles.at(TileCoord{0, 0}));
    result.Tile1Hash = HeightHash(arrival);
    result.ArrivalHeightBands = system.GetLastArrivalHeightBandCountForTests();

    SplineECS::SplineService::Shutdown();
    TerrainService::Shutdown();
    return result;
}

// How a batched load treats the gather while tiles stream in.
enum class StreamingGather
{
    AsTheGateDecides,
    // Each streaming frame writes the noise effect with its own values, so the gate gathers again.
    EveryFrame,
};

// Whether the area's spline moves in the frame the second batch streams in.
enum class SplineEdit
{
    None,
    WithSecondBatch,
};

struct BatchedLoad
{
    // Tiles (0,0), (1,0), (0,1), (1,1).
    std::vector<uint64> TileHashes;
    uint64 GathersWhileStreaming = 0;
    uint32 StreamingFrames = 0;
};

void TickAndSwap(ECS::World& world, TerrainModifierSystem& system)
{
    system.Update(world, 1.0f / 60.0f);
    world.SwapLifecycleEvents();
}

bool AllModifiersApplied(const TiledTerrainData& tiled, const std::vector<TileCoord>& coords)
{
    for (const TileCoord coord : coords)
    {
        if (!tiled.Tiles.at(coord)->ModifiersApplied)
            return false;
    }
    return true;
}

// A 2x2-tile terrain: tile (0,0) resident at the first update, (1,0) streams in, then (0,1) and (1,1).
// A second noise circle sits on the corner of all four tiles, so each batch has modifier work.
BatchedLoad LoadInBatches(StreamingGather mode, SplineEdit splineEdit)
{
    if (TerrainService::IsInitialized())
        TerrainService::Shutdown();
    TerrainService::Initialize();
    if (SplineECS::SplineService::IsInitialized())
        SplineECS::SplineService::Shutdown();
    SplineECS::SplineService::Initialize();
    auto& svc = TerrainService::Get();

    TiledTerrainConfig config{};
    config.Base = TerrainTests::GeneratedNoiseBase();
    config.WorldSizeX = 2.0f * kTileWorld;
    config.WorldSizeZ = 2.0f * kTileWorld;
    config.HeightScale = kHeightScale;
    config.SamplesPerMeter = 1.0f;
    const TiledTerrainHandle handle = svc.CreateTiledTerrain(config);
    auto* tiled = svc.GetTiledTerrainData(handle);
    svc.LoadTile(handle, TileCoord{0, 0})->LodState = TileLodState::Full;

    ECS::World world;
    EnableTerrainModifierLifecycleEvents(world);
    const AuthoredScene scene = AuthorScene(world, handle, 2.0f * kTileWorld);
    auto corner = world.CreateEntity();
    Components::TerrainModifierVolume cornerVolume{};
    cornerVolume.Shape = Components::TerrainVolumeShape::Circle;
    cornerVolume.Radius = 80.0f;
    cornerVolume.Falloff = 20.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(corner, cornerVolume);
    Components::WorldTransform cornerTransform{};
    cornerTransform.Position.X = 1000.0f;
    cornerTransform.Position.Z = 1040.0f;
    world.AddComponentImmediate<Components::WorldTransform>(corner, cornerTransform);
    Components::TerrainNoiseEffect cornerNoise{};
    cornerNoise.Amplitude = 4.0f;
    cornerNoise.Frequency = 6.0f;
    cornerNoise.Octaves = 2u;
    world.AddComponentImmediate(corner, cornerNoise);

    // The first ticks gather the authored set and consume its lifecycle events.
    TerrainModifierSystem system;
    for (int frame = 0; frame < 6; ++frame)
        TickAndSwap(world, system);
    BatchedLoad result;

    const uint64 beforeStreaming = TerrainModifierSystem::GetGatherCountForTests();
    const std::vector<std::vector<TileCoord>> batches = {{TileCoord{1, 0}}, {TileCoord{0, 1}, TileCoord{1, 1}}};
    for (std::size_t batch = 0; batch < batches.size(); ++batch)
    {
        for (const TileCoord coord : batches[batch])
            StreamInFullTile(svc, handle, coord);
        if (splineEdit == SplineEdit::WithSecondBatch && batch == 1)
        {
            const auto* splineComp = world.GetComponent<Components::SplineComponent>(scene.Area);
            const SplineECS::SplineHandle area(splineComp->SplineDataIndex, splineComp->SplineDataGeneration);
            // The area moves across Z = 1024 into tile (1,1), which streams in with this batch.
            for (auto& point : SplineECS::SplineService::Get().GetSplineData(area)->Points)
                point.Position.z += 420.0f;
            SplineECS::SplineService::Get().RebuildCache(area);
        }
        const uint32 tileFrames = static_cast<uint32>(kTileWorld) / TerrainModifierSystem::kArrivalSplatRowsPerFrame + 4u;
        const uint32 maxFrames = 4u * tileFrames;
        for (uint32 frame = 0; frame < maxFrames && !AllModifiersApplied(*tiled, batches[batch]); ++frame)
        {
            if (mode == StreamingGather::EveryFrame)
                world.GetComponentForWrite<Components::TerrainNoiseEffect>(scene.Noise);
            TickAndSwap(world, system);
            ++result.StreamingFrames;
        }
        EXPECT_TRUE(AllModifiersApplied(*tiled, batches[batch]))
            << "batch " << batch << " must finish its bake within the row budget's frames";
    }
    result.GathersWhileStreaming = TerrainModifierSystem::GetGatherCountForTests() - beforeStreaming;

    for (const TileCoord coord : {TileCoord{0, 0}, TileCoord{1, 0}, TileCoord{0, 1}, TileCoord{1, 1}})
        result.TileHashes.push_back(HeightHash(*tiled->Tiles.at(coord)));

    SplineECS::SplineService::Shutdown();
    TerrainService::Shutdown();
    return result;
}

// The bytes of the scene's heights as the station scan bakes them.
constexpr uint64 kPinnedTile0Hash = 2308198835280334469ull;
constexpr uint64 kPinnedTile1Hash = 17589719795089139055ull;
constexpr float32 kRouteFalloff = 10.0f;

RoutePolyline BuildFixtureRoute()
{
    SplineECS::SplineService::Initialize();
    const SplineECS::SplineHandle handle = MakeSpline(kRoutePoints, false);
    RoutePolyline polyline;
    BuildRoutePolyline(*SplineECS::SplineService::Get().GetSplineData(handle), kStationSpacing, kRouteFalloff,
                       polyline);
    SplineECS::SplineService::Shutdown();
    return polyline;
}

// The same stations with no segment grid: the query scans every segment.
RoutePolyline WithoutGrid(const RoutePolyline& polyline)
{
    RoutePolyline scanned = polyline;
    scanned.SegmentGrid = RouteSegmentGrid{};
    return scanned;
}

bool SameBits(const RouteSample& a, const RouteSample& b)
{
    return std::memcmp(&a.Distance, &b.Distance, sizeof(float32)) == 0 &&
           std::memcmp(&a.Height, &b.Height, sizeof(float32)) == 0 &&
           std::memcmp(&a.HalfWidth, &b.HalfWidth, sizeof(float32)) == 0;
}

uint64 HashSample(const RouteSample& sample, uint64 seed)
{
    const float32 values[3] = {sample.Distance, sample.Height, sample.HalfWidth};
    return Hashing::Fnv1a64(values, sizeof(values), seed);
}

} // namespace

// Every sample of a lattice over the route's footprint, the grid's cell borders and points far
// outside: the grid's answer has the scan's bits.
TEST(TerrainRouteSegmentGrid, TheGridGivesTheScansAnswerOnEverySample)
{
    if (SplineECS::SplineService::IsInitialized())
        SplineECS::SplineService::Shutdown();
    const RoutePolyline polyline = BuildFixtureRoute();
    ASSERT_GT(polyline.Stations.size(), 1000u);
    const RouteSegmentGrid& grid = polyline.SegmentGrid;
    ASSERT_GT(grid.CellsX * grid.CellsZ, 1u) << "a one-cell grid would not test the ring search";
    const RoutePolyline reference = WithoutGrid(polyline);

    std::vector<std::pair<float32, float32>> queries;
    for (float32 z = 250.0f; z <= 520.0f; z += 1.25f)
        for (float32 x = 700.0f; x <= 1240.0f; x += 1.25f)
            queries.emplace_back(x, z);
    for (uint32 i = 0; i <= grid.CellsX; ++i)
        for (uint32 k = 0; k <= grid.CellsZ; ++k)
            queries.emplace_back(grid.OriginX + static_cast<float32>(i) * grid.CellSize,
                                 grid.OriginZ + static_cast<float32>(k) * grid.CellSize);
    queries.emplace_back(-5000.0f, 300.0f);
    queries.emplace_back(4000.0f, -2500.0f);

    uint64 scanHash = Hashing::kFnv1a64OffsetBasis;
    uint64 gridHash = Hashing::kFnv1a64OffsetBasis;
    uint32 mismatches = 0;
    const auto scanStart = std::chrono::steady_clock::now();
    std::vector<RouteSample> scanned;
    scanned.reserve(queries.size());
    for (const auto& [x, z] : queries)
        scanned.push_back(ClosestStationXZ(reference, x, z));
    const auto gridStart = std::chrono::steady_clock::now();
    std::vector<RouteSample> gridded;
    gridded.reserve(queries.size());
    for (const auto& [x, z] : queries)
        gridded.push_back(ClosestStationXZ(polyline, x, z));
    const auto gridEnd = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < queries.size(); ++i)
    {
        scanHash = HashSample(scanned[i], scanHash);
        gridHash = HashSample(gridded[i], gridHash);
        if (!SameBits(scanned[i], gridded[i]) && ++mismatches <= 5u)
            ADD_FAILURE() << "query (" << queries[i].first << ", " << queries[i].second << "): scan distance "
                          << scanned[i].Distance << " height " << scanned[i].Height << ", grid distance "
                          << gridded[i].Distance << " height " << gridded[i].Height;
    }
    EXPECT_EQ(mismatches, 0u);
    EXPECT_EQ(gridHash, scanHash);

    // The bake queries only the swept band: time that subset on its own.
    std::vector<std::pair<float32, float32>> band;
    for (std::size_t i = 0; i < queries.size(); ++i)
        if (scanned[i].Distance <= kRouteRadius + kRouteFalloff)
            band.push_back(queries[i]);
    float32 sink = 0.0f;
    const auto bandScanStart = std::chrono::steady_clock::now();
    for (const auto& [x, z] : band)
        sink += ClosestStationXZ(reference, x, z).Height;
    const auto bandGridStart = std::chrono::steady_clock::now();
    for (const auto& [x, z] : band)
        sink -= ClosestStationXZ(polyline, x, z).Height;
    const auto bandGridEnd = std::chrono::steady_clock::now();
    RecordProperty("BandHeightResidual", std::to_string(sink));

    using Ms = std::chrono::duration<double, std::milli>;
    RecordProperty("BandQueries", static_cast<int>(band.size()));
    RecordProperty("BandScanMs", std::to_string(Ms(bandGridStart - bandScanStart).count()));
    RecordProperty("BandGridMs", std::to_string(Ms(bandGridEnd - bandGridStart).count()));
    RecordProperty("Queries", static_cast<int>(queries.size()));
    RecordProperty("Segments", static_cast<int>(polyline.Stations.size() - 1u));
    RecordProperty("ScanMs", std::to_string(Ms(gridStart - scanStart).count()));
    RecordProperty("GridMs", std::to_string(Ms(gridEnd - gridStart).count()));
}

// A hairpin: the sample midway between its legs is equally near a segment of each. The scan keeps
// the lower segment index, so the grid must too.
TEST(TerrainRouteSegmentGrid, ATieBetweenTwoSegmentsResolvesToTheLowerIndex)
{
    std::vector<RouteStation> stations;
    for (int32 i = 0; i <= 200; ++i)
        stations.push_back(RouteStation{0.5f * static_cast<float32>(i), 0.0f, 0.0f, 1.0f});
    for (int32 i = 200; i >= 0; --i)
        stations.push_back(RouteStation{0.5f * static_cast<float32>(i), 4.0f, 8.0f, 3.0f});
    RoutePolyline polyline;
    BuildRoutePolyline(std::move(stations), 0.0f, polyline);
    ASSERT_FALSE(polyline.SegmentGrid.CellStart.empty());

    const RouteSample scan = ClosestStationXZ(WithoutGrid(polyline), 50.25f, 2.0f);
    const RouteSample gridded = ClosestStationXZ(polyline, 50.25f, 2.0f);
    EXPECT_EQ(scan.Distance, 2.0f);
    EXPECT_EQ(scan.Height, 0.0f) << "the lower leg holds the lower segment index";
    EXPECT_TRUE(SameBits(scan, gridded)) << "grid height " << gridded.Height << ", width " << gridded.HalfWidth;
}

// The arrival bake of a scene with a route, a polygon area and noise across the tile border keeps
// the bytes the station scan baked.
TEST(TerrainStreamedBake, TheArrivalBakeKeepsItsPinnedBytes)
{
    const StreamedBake bake = BakeStreamedScene(nullptr);
    EXPECT_EQ(bake.Tile0Hash, kPinnedTile0Hash);
    EXPECT_EQ(bake.Tile1Hash, kPinnedTile1Hash);
}

// The arrival's height compose runs in row bands on the pool's workers, and its bytes do not depend
// on the worker count: one worker and many give the pinned bytes the whole-tile compose gives.
TEST(TerrainStreamedBake, TheArrivalComposeRunsOnThePoolWithTheSameBytesForAnyWorkerCount)
{
    const StreamedBake whole = BakeStreamedScene(nullptr);
    EXPECT_EQ(whole.ArrivalHeightBands, 0u) << "with no pool the compose runs whole on the calling thread";

    for (const uint32 workers : {1u, 8u})
    {
        JobSystem::WorkStealingThreadPool pool(workers);
        const StreamedBake banded = BakeStreamedScene(&pool);
        EXPECT_GT(banded.ArrivalHeightBands, 1u) << workers << " workers: the compose must fan out in row bands";
        EXPECT_EQ(banded.Tile0Hash, kPinnedTile0Hash) << workers << " workers";
        EXPECT_EQ(banded.Tile1Hash, kPinnedTile1Hash) << workers << " workers";
    }
}

// A load that streams its tiles in two batches gathers the modifiers once: the streamed tiles' bakes
// read that gather, because no modifier input changed since.
TEST(TerrainStreamedBake, ALoadThatStreamsTwoBatchesGathersTheModifiersOnce)
{
    const BatchedLoad load = LoadInBatches(StreamingGather::AsTheGateDecides, SplineEdit::None);
    EXPECT_GT(load.StreamingFrames, 2u) << "each batch must take its own frames";
    EXPECT_EQ(load.GathersWhileStreaming, 0u) << "a streamed tile's bake gathered the unchanged modifiers again";
}

// The streamed tiles bake the same bytes from the one gather as from a gather on every frame.
TEST(TerrainStreamedBake, TilesStreamedFromOneGatherHaveTheBytesOfAGatherEveryFrame)
{
    const BatchedLoad once = LoadInBatches(StreamingGather::AsTheGateDecides, SplineEdit::None);
    const BatchedLoad everyFrame = LoadInBatches(StreamingGather::EveryFrame, SplineEdit::None);
    EXPECT_GE(everyFrame.GathersWhileStreaming, everyFrame.StreamingFrames)
        << "the comparison arm must gather on every streaming frame";
    EXPECT_EQ(once.TileHashes, everyFrame.TileHashes);
}

// A spline edit in the frame a batch streams in gathers once, although the batch also waits for its bake.
// The edit moves the area into tile (1,1) of the batch. The batch bakes the moved spline: its bytes equal
// those of a gather on every frame with the same edit.
TEST(TerrainStreamedBake, ASplineEditWhileTilesStreamInGathersOnceAndBakesTheMovedSpline)
{
    const BatchedLoad load = LoadInBatches(StreamingGather::AsTheGateDecides, SplineEdit::WithSecondBatch);
    const BatchedLoad everyFrame = LoadInBatches(StreamingGather::EveryFrame, SplineEdit::WithSecondBatch);
    const BatchedLoad unedited = LoadInBatches(StreamingGather::AsTheGateDecides, SplineEdit::None);
    EXPECT_EQ(load.GathersWhileStreaming, 1u);
    EXPECT_NE(everyFrame.TileHashes[3], unedited.TileHashes[3]) << "the edit must reach tile (1,1) of the batch";
    EXPECT_EQ(load.TileHashes[3], everyFrame.TileHashes[3]) << "the batch must bake the moved spline into tile (1,1)";
    EXPECT_EQ(load.TileHashes, everyFrame.TileHashes) << "every tile must have the bytes of a gather on every frame";
}
