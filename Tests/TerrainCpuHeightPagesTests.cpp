// The CPU height pages of a paged terrain (design page-streaming D3, as page-streaming-planets 3.11
// item 1 amends it) and the query the CPU readers sample them through (SurfaceHeightQuery): the
// windows keep the finest level, outside every window the answer is coarse and never unsampled, a
// RequireFinest caller treats a coarse answer as unsampled, the synchronous fetch holds its deadline,
// the CPU reads the bytes the GPU uploads, overlay included, and the pages the GPU fetched; a dynamic
// body far from every camera keeps its finest pages; a fetch of a restarted stream never lands in it,
// and a failed fetch leaves a coarser page answering until a retry lands; a skyline kept across
// frames (TerrainSkyline) samples again only where its eye, the sun or the pages moved.

#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/PlanarHeightfieldEditCursor.h"
#include "TerrainSkyline.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainService.h"

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/HeightStoreCache.h"
#include "PageStreaming/PageResidency.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "PageStreaming/PageStore/PageStoreReader.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Mathematics/Rect.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "Terrain/TerrainTypes.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using PageStreaming::PageAddress;

namespace
{

// 4 x 4 noise tiles of 257 samples at 1 m: 1,025 x 1,025 samples, a pyramid of 9 x 9, 5 x 5, 3 x 3,
// 2 x 2 and 1 pages (the last level-0 page owns the last sample column and row alone).
PageStreaming::GeneratedHeightLattice NoiseLattice()
{
    PageStreaming::GeneratedHeightLattice lattice;
    lattice.Source = PageStreaming::GeneratedHeightSource::Noise;
    lattice.TileWorldSize = 256.0f;
    lattice.TileSamples = 257;
    lattice.TilesX = 4;
    lattice.TilesZ = 4;
    lattice.Noise = {0.01f, 1.0f, 4u, 7u};
    return lattice;
}

constexpr uint32 kSamples = 1025;

PageRequestTerrain NoiseShape()
{
    PageRequestTerrain shape;
    shape.Level0TexelX = 1.0f;
    shape.Level0TexelZ = 1.0f;
    shape.SamplesX = kSamples;
    shape.SamplesZ = kSamples;
    shape.MinHeight = -2.0f;
    shape.MaxHeight = 2.0f;
    return shape;
}

PageStreaming::PageSource NoiseSource()
{
    PageStreaming::PageSource source;
    source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(NoiseLattice());
    return source;
}

// The 2 x 2 level-0 pages around world point (x, z), the page under it first.
std::vector<PageAddress> WindowAround(float32 x, float32 z)
{
    const uint32 px = static_cast<uint32>(x) / PageStreaming::kPageOwnedSamples;
    const uint32 pz = static_cast<uint32>(z) / PageStreaming::kPageOwnedSamples;
    return {PageAddress{0, 0, px, pz}, PageAddress{0, 0, px + 1u, pz}, PageAddress{0, 0, px, pz + 1u},
            PageAddress{0, 0, px + 1u, pz + 1u}};
}

constexpr uint32 kStream = 1;
// Level 2 and every coarser level are pinned.
constexpr uint32 kFirstPinned = 2;

constexpr uint32 kField = 0;

// A residency's budget: `bytes` of decoded pages, `syncPages` synchronous CPU window fetches a
// frame, `loadsInFlight` loads outstanding (the CPU's share is a quarter).
PageStreaming::PageResidencyBudget CpuBudget(uint64 bytes, uint32 syncPages, uint32 loadsInFlight = 64)
{
    PageStreaming::PageResidencyBudget budget;
    budget.CpuBytes = bytes;
    budget.CpuSyncFetchPages = syncPages;
    budget.LoadsInFlight = loadsInFlight;
    return budget;
}

// The CPU half of stream kStream of a residency with no GPU wants: its CPU windows, its pinned
// levels, and the query over them.
class CpuPages
{
public:
    explicit CpuPages(const PageStreaming::PageResidencyBudget& budget,
                      PageStreaming::PageFetchClock clock = &std::chrono::steady_clock::now)
        : Residency(nullptr, nullptr, budget, std::move(clock))
    {
        Residency.ConfigureField(kField, 64, 2, 0.0f);
    }

    void Configure(PageStreaming::PageSource source, std::vector<PageStreaming::PageStoreLevel> levels,
                   uint32 firstPinned)
    {
        Residency.Configure(kStream, kField, std::move(source));
        Levels = std::move(levels);
        FirstPinned = firstPinned;
        Residency.RequestCpu(kStream, FirstPinned, {});
    }

    void Want(std::span<const PageAddress> window) { Residency.RequestCpu(kStream, FirstPinned, window); }
    void Update(uint64 frame) { Residency.Update(frame, 1.0f / 60.0f, false); }
    SurfaceHeightQuery Query(const PageRequestTerrain& shape) const
    {
        return SurfaceHeightQuery(Residency.CpuPages(kStream), Levels, shape, 0.0f, 1.0f, nullptr, SurfaceHeightEdits{});
    }

    PageStreaming::PageResidencyManager Residency;
    std::vector<PageStreaming::PageStoreLevel> Levels;
    uint32 FirstPinned = 0;
};

struct CacheAndQuery
{
    explicit CacheAndQuery(const PageStreaming::PageResidencyBudget& budget,
                           PageStreaming::PageFetchClock clock = &std::chrono::steady_clock::now)
        : Cache(budget, std::move(clock))
    {
        Cache.Configure(NoiseSource(), PageStreaming::BuildPageStoreLevels(kSamples, kSamples), kFirstPinned);
        Query = Cache.Query(NoiseShape());
    }

    CpuPages Cache;
    SurfaceHeightQuery Query;
};

const PageStreaming::PageResidencyBudget kRoomyBudget = CpuBudget(64ull << 20, 4u);

// How many of `windows` answer at their center at level 0 (a RequireFinest answer).
uint32 CountFinestPages(const SurfaceHeightQuery& query, const std::vector<PageAddress>& windows)
{
    uint32 finest = 0;
    for (const PageAddress& page : windows)
    {
        const float32 x = (static_cast<float32>(page.X) + 0.5f) * PageStreaming::kPageOwnedSamples;
        const float32 z = (static_cast<float32>(page.Z) + 0.5f) * PageStreaming::kPageOwnedSamples;
        SurfaceHeightSample sample;
        finest += query.SampleHeight({x, 0.0f, z}, SurfaceHeightPolicy::RequireFinest, sample) ? 1u : 0u;
    }
    return finest;
}

// The query of `cpu`'s stream over NoiseShape whose heights stand at page source `source` and overlay
// version `version`, which changed level-0 samples 10..30 x 20..40 from version `since`.
SurfaceHeightQuery EditedQuery(const CpuPages& cpu, uint64 source, uint64 version, uint64 since)
{
    const SurfaceHeightEdits edits{source, version, PageStreaming::PageSampleRect{10, 20, 30, 40}, since};
    return SurfaceHeightQuery(cpu.Residency.CpuPages(kStream), cpu.Levels, NoiseShape(), 0.0f, 1.0f, nullptr, edits);
}

// Every page of `levels`: the state stream kStream's CPU page directory gives it against the
// residency's own samples of it.
void ExpectDirectoryMatchesResidency(const CpuPages& cpu, std::span<const PageStreaming::PageStoreLevel> levels,
                                     const char* when)
{
    const PageStreaming::CpuPageDirectory directory = cpu.Residency.CpuPages(kStream);
    ASSERT_EQ(directory.Entries.size(), levels.back().FirstEntry + 1u) << when;
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
            {
                const PageAddress address{0, static_cast<uint8>(level), x, z};
                std::span<const uint8> samples;
                const PageStreaming::CpuPageState state = directory.Page(address, samples);
                const std::vector<uint8>* held = cpu.Residency.Bytes(kStream, address);
                ASSERT_EQ(state == PageStreaming::CpuPageState::Resident, held != nullptr)
                    << when << ": level " << level << " page " << x << ", " << z;
                if (held)
                    EXPECT_EQ(samples.data(), held->data()) << when;
            }
}

} // namespace

TEST(CpuHeightPages, AWindowKeepsTheFinestLevelWhileThePagesItLeftAreDroppedOverTheBudget)
{
    // A budget of one page: only the pinned pages and the window's are kept past it.
    CacheAndQuery cpu(CpuBudget(static_cast<uint64>(PageStreaming::kPageSampleCount) * sizeof(float32), 4u));
    const std::vector<PageAddress> first = WindowAround(200.0f, 200.0f);
    uint64 frame = 1;
    for (; frame < 4; ++frame)
    {
        cpu.Cache.Want(first);
        cpu.Cache.Update(frame);
    }
    SurfaceHeightSample sample;
    ASSERT_TRUE(cpu.Query.SampleHeight({200.0f, 0.0f, 200.0f}, SurfaceHeightPolicy::RequireFinest, sample));
    EXPECT_EQ(sample.Level, 0u);
    EXPECT_EQ(sample.Detail, SurfaceHeightDetail::Finest);

    const std::vector<PageAddress> second = WindowAround(700.0f, 700.0f);
    for (; frame < 8; ++frame)
    {
        cpu.Cache.Want(second);
        cpu.Cache.Update(frame);
        ASSERT_TRUE(cpu.Query.SampleHeight({700.0f, 0.0f, 700.0f}, SurfaceHeightPolicy::AcceptCoarse, sample));
        EXPECT_EQ(sample.Level, 0u) << "the moved window's pages are fetched within the frame";
    }
    ASSERT_TRUE(cpu.Query.SampleHeight({200.0f, 0.0f, 200.0f}, SurfaceHeightPolicy::AcceptCoarse, sample));
    EXPECT_EQ(sample.Detail, SurfaceHeightDetail::Coarse) << "the left window's pages are dropped over the budget";
    EXPECT_EQ(sample.Level, kFirstPinned);
}

TEST(CpuHeightPages, TheCpuPageDirectoryHoldsEveryPageTheResidencyHoldsAfterLandingsAndTrims)
{
    // A budget of one page, so the second window's landings trim the first window's pages.
    CacheAndQuery cpu(CpuBudget(static_cast<uint64>(PageStreaming::kPageSampleCount) * sizeof(float32), 4u));
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(kSamples, kSamples);
    const PageAddress firstPage = WindowAround(200.0f, 200.0f).front();
    uint64 frame = 1;
    for (; frame < 4; ++frame)
    {
        cpu.Cache.Want(WindowAround(200.0f, 200.0f));
        cpu.Cache.Update(frame);
    }
    ExpectDirectoryMatchesResidency(cpu.Cache, levels, "after the first window landed");
    ASSERT_NE(cpu.Cache.Residency.Bytes(kStream, firstPage), nullptr);
    for (; frame < 8; ++frame)
    {
        cpu.Cache.Want(WindowAround(700.0f, 700.0f));
        cpu.Cache.Update(frame);
    }
    ExpectDirectoryMatchesResidency(cpu.Cache, levels, "after the trim");
    ASSERT_EQ(cpu.Cache.Residency.Bytes(kStream, firstPage), nullptr) << "the trim did not drop the first window";
}

TEST(CpuHeightPages, OutsideEveryWindowTheAnswerIsCoarseAndNeverUnsampledInsideTheFootprint)
{
    CacheAndQuery cpu(kRoomyBudget);
    cpu.Cache.Want(WindowAround(200.0f, 200.0f));
    cpu.Cache.Update(1);

    // The first Update holds the top page: every point of the footprint answers.
    uint32 coarse = 0;
    for (float32 z = 0.0f; z <= 1024.0f; z += 16.0f)
        for (float32 x = 0.0f; x <= 1024.0f; x += 16.0f)
        {
            SurfaceHeightSample sample;
            ASSERT_TRUE(cpu.Query.SampleHeight({x, 0.0f, z}, SurfaceHeightPolicy::AcceptCoarse, sample))
                << "unsampled at (" << x << ", " << z << ")";
            coarse += sample.Detail == SurfaceHeightDetail::Coarse ? 1u : 0u;
        }
    EXPECT_GT(coarse, 0u);
    SurfaceHeightSample far;
    ASSERT_TRUE(cpu.Query.SampleHeight({900.0f, 0.0f, 900.0f}, SurfaceHeightPolicy::AcceptCoarse, far));
    EXPECT_EQ(far.Detail, SurfaceHeightDetail::Coarse);
    EXPECT_EQ(far.Level, 4u) << "the top page, before the rest of the pinned levels land";

    cpu.Cache.Update(2);
    ASSERT_TRUE(cpu.Query.SampleHeight({900.0f, 0.0f, 900.0f}, SurfaceHeightPolicy::AcceptCoarse, far));
    EXPECT_EQ(far.Detail, SurfaceHeightDetail::Coarse);
    EXPECT_EQ(far.Level, kFirstPinned) << "the pinned level-2 pages land at the next Update";

    SurfaceHeightSample outside;
    EXPECT_FALSE(cpu.Query.SampleHeight({1100.0f, 0.0f, 500.0f}, SurfaceHeightPolicy::AcceptCoarse, outside))
        << "outside the footprint there is no ground";
}

TEST(CpuHeightPages, ARequireFinestCallerTreatsACoarseAnswerAsUnsampled)
{
    CacheAndQuery cpu(kRoomyBudget);
    cpu.Cache.Want(WindowAround(200.0f, 200.0f));
    cpu.Cache.Update(1);
    cpu.Cache.Update(2);

    SurfaceHeightSample sample{123.0f, 9u, SurfaceHeightDetail::Finest};
    EXPECT_FALSE(cpu.Query.SampleHeight({900.0f, 0.0f, 900.0f}, SurfaceHeightPolicy::RequireFinest, sample));
    EXPECT_EQ(sample.Height, 123.0f) << "an unsampled answer writes nothing";
    EXPECT_TRUE(cpu.Query.SampleHeight({900.0f, 0.0f, 900.0f}, SurfaceHeightPolicy::AcceptCoarse, sample));
    EXPECT_TRUE(cpu.Query.SampleHeight({200.0f, 0.0f, 200.0f}, SurfaceHeightPolicy::RequireFinest, sample));
    EXPECT_EQ(sample.Detail, SurfaceHeightDetail::Finest);
}

TEST(CpuHeightPages, TheSynchronousFetchStopsAtTheDeadlineAndTheRestLandAtTheNextUpdate)
{
    // Every clock read is 200 us after the last: the read at 200 us starts a fetch; at 400 us the
    // second would end at 600 us (the first cost 200 us), past the 500 us deadline.
    auto now = std::make_shared<std::chrono::steady_clock::time_point>();
    const PageStreaming::PageFetchClock clock = [now]() {
        *now += std::chrono::microseconds(200);
        return *now;
    };
    CacheAndQuery cpu(CpuBudget(64ull << 20, 8u), clock);
    std::vector<PageAddress> windows = WindowAround(200.0f, 200.0f);
    const std::vector<PageAddress> other = WindowAround(700.0f, 700.0f);
    windows.insert(windows.end(), other.begin(), other.end());
    cpu.Cache.Want(windows);
    cpu.Cache.Update(1);

    EXPECT_EQ(CountFinestPages(cpu.Query, windows), 1u) << "one window page fits before the deadline";

    cpu.Cache.Want(windows);
    cpu.Cache.Update(2);
    EXPECT_EQ(CountFinestPages(cpu.Query, windows), 8u) << "the rest were fetched on jobs and land at the next Update";
}

TEST(CpuHeightPages, AFetchThatCostsMoreThanTheDeadlineLeftStartsNoOtherInTheFrame)
{
    // The Update starts at 0; the first fetch starts at 10 us and costs 360 us (a generated page).
    // At 370 us a second fetch of the same cost would end at 730 us, past the 500 us deadline.
    auto reads = std::make_shared<uint32>(0u);
    const auto origin = std::chrono::steady_clock::now();
    const PageStreaming::PageFetchClock clock = [reads, origin]() {
        const uint32 read = (*reads)++;
        return origin + std::chrono::microseconds(read == 0u ? 0 : 10 + 360 * (read - 1u));
    };
    CacheAndQuery cpu(CpuBudget(64ull << 20, 4u), clock);
    const std::vector<PageAddress> windows = WindowAround(200.0f, 200.0f);
    cpu.Cache.Want(windows);
    cpu.Cache.Update(1);
    EXPECT_EQ(CountFinestPages(cpu.Query, windows), 1u) << "the second fetch would end past the deadline";
}

TEST(CpuHeightPages, TheSynchronousFetchStopsAtThePageCapWhenTheClockAllows)
{
    const auto still = std::chrono::steady_clock::now();
    CacheAndQuery cpu(CpuBudget(64ull << 20, 3u), [still]() { return still; });
    const std::vector<PageAddress> windows = WindowAround(200.0f, 200.0f);
    cpu.Cache.Want(windows);
    cpu.Cache.Update(1);
    EXPECT_EQ(CountFinestPages(cpu.Query, windows), 3u);
}

namespace
{

constexpr uint32 kTerrain = 5;
constexpr uint32 kGeneration = 2;

// The noise terrain paged whole (120 pages, within a quarter of 512 slots: every level pinned), with
// the overlay `overlay` at version `version`.
PagedTerrainRequest PinnedNoiseRequest(const PageStreaming::PageSource& source,
                                       std::shared_ptr<const PageStreaming::HeightPageOverlay> overlay, uint64 version)
{
    PagedTerrainRequest request;
    request.Terrain = kTerrain;
    request.TerrainGeneration = kGeneration;
    request.SourceIdentity = 41;
    request.Source = source;
    request.Shape = NoiseShape();
    request.OriginY = 0.0f;
    request.HeightScale = 1.0f;
    request.Overlay = std::move(overlay);
    request.OverlayVersion = version;
    return request;
}

std::shared_ptr<const PageStreaming::HeightPageOverlay> RaisedOverlay(const PageStreaming::PageSampleRect& rect,
                                                                      float32 delta)
{
    return std::make_shared<const PageStreaming::HeightPageOverlay>(
        PageStreaming::BuildPageStoreLevels(kSamples, kSamples), rect,
        std::vector<float32>(static_cast<std::size_t>(rect.Width()) * rect.Height(), delta));
}

// Runs `frames` frames of `request`, keeping the latest upload of every level-0 page.
void RunAndCollectLevel0Uploads(TerrainHeightPages& pages, const PagedTerrainRequest& request, uint64& frame,
                                uint64 frames, std::map<std::pair<uint32, uint32>, std::vector<float32>>& uploads)
{
    const Mathematics::Vector3 camera{200.0f, 10.0f, 200.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    for (const uint64 end = frame + frames; frame < end; ++frame)
    {
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        const PageStreaming::PageCache& cache = pages.Residency().FieldCache(kHeightPageField);
        for (const PageStreaming::PageUploadRequest& upload : cache.Uploads())
        {
            if (upload.Page.Address.Level != 0)
                continue;
            for (const HeightPageUpload& samples : pages.Uploads())
                if (samples.Slot == upload.Slot)
                {
                    const std::span<const float32> heights = PageStreaming::HeightSamples(samples.Bytes);
                    uploads[{upload.Page.Address.X, upload.Page.Address.Z}] = {heights.begin(), heights.end()};
                }
        }
    }
}

// Every level-0 sample the query answers is bit-equal to the GPU upload's sample (level-0 lattice
// points: the query's bilinear weights are zero there, so it returns the page's sample).
void ExpectQueryBitEqualToUploads(const TerrainHeightPages& pages,
                                  const std::map<std::pair<uint32, uint32>, std::vector<float32>>& uploads)
{
    const SurfaceHeightQuery query = pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration});
    ASSERT_TRUE(query.IsValid());
    ASSERT_EQ(uploads.size(), 81u) << "every level-0 page was uploaded";
    uint32 compared = 0;
    for (const auto& [page, samples] : uploads)
        for (uint32 sampleZ = 0; sampleZ < PageStreaming::kPageOwnedSamples; ++sampleZ)
            for (uint32 sampleX = 0; sampleX < PageStreaming::kPageOwnedSamples; ++sampleX)
            {
                const uint32 level0X = page.first * PageStreaming::kPageOwnedSamples + sampleX;
                const uint32 level0Z = page.second * PageStreaming::kPageOwnedSamples + sampleZ;
                if (level0X >= kSamples || level0Z >= kSamples)
                    continue;
                SurfaceHeightSample sample;
                ASSERT_TRUE(query.SampleHeight({static_cast<float32>(level0X), 0.0f, static_cast<float32>(level0Z)},
                                               SurfaceHeightPolicy::RequireFinest, sample));
                ASSERT_EQ(sample.Level, 0u);
                const float32 uploaded = samples[(sampleZ + PageStreaming::kPageApronSamples) * PageStreaming::kPageStrideSamples +
                                                 sampleX + PageStreaming::kPageApronSamples];
                ASSERT_EQ(std::bit_cast<uint32>(sample.Height), std::bit_cast<uint32>(uploaded))
                    << "level-0 sample (" << level0X << ", " << level0Z << ")";
                ++compared;
            }
    EXPECT_EQ(compared, kSamples * kSamples);
}

// A terrain entity of `terrain` at world (x, y, z).
ECS::EntityHandle AddTerrainEntity(ECS::World& world, const Components::Terrain& terrain, float32 x, float32 y,
                                   float32 z)
{
    Components::WorldTransform transform;
    transform.matrix[12] = x;
    transform.matrix[13] = y;
    transform.matrix[14] = z;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, terrain);
    world.AddComponentImmediate(entity, transform);
    return entity;
}

// A flat planar terrain of `size` metres with its height data, centered on world (x, 0, z).
ECS::EntityHandle AddPlanarTerrain(ECS::World& world, float32 size, float32 x, float32 z)
{
    Terrain::TerrainConfig config;
    config.HeightmapWidth = config.HeightmapHeight = 33;
    config.WorldSizeX = config.WorldSizeZ = size;
    const TerrainHandle handle = TerrainService::Get().CreateTerrain(config);
    Components::Terrain terrain;
    terrain.SizeX = terrain.SizeZ = size;
    terrain.BaseSource = Components::TerrainBaseSource::Flat;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    return AddTerrainEntity(world, terrain, x, 0.0f, z);
}

// A planet of `radius` metres, centered on the world origin as the renderer draws it.
ECS::EntityHandle AddPlanet(ECS::World& world, float32 radius)
{
    Components::Terrain terrain;
    terrain.Domain = Components::TerrainDomain::Spherical;
    terrain.PlanetRadius = radius;
    return AddTerrainEntity(world, terrain, 0.0f, 150.0f, -700.0f);
}

// A physics body of `motion` at world (x, 0, z).
void AddBody(ECS::World& world, float32 x, float32 z, Physics::MotionType motion)
{
    Components::PhysicsBody body;
    body.motionType = motion;
    Components::WorldTransform transform;
    transform.matrix[12] = x;
    transform.matrix[14] = z;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, body);
    world.AddComponentImmediate(entity, transform);
}

} // namespace

TEST(CpuHeightPages, TheCpuPagesHoldTheBytesTheGpuUploadsWithTheOverlayAndItsEdits)
{
    TerrainHeightPages pages(nullptr, nullptr, kDesktopHeightPages);
    const PageStreaming::PageSource source = NoiseSource();
    const PageStreaming::PageSampleRect rect{300, 300, 420, 380};
    std::map<std::pair<uint32, uint32>, std::vector<float32>> uploads;
    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    RunAndCollectLevel0Uploads(pages, PinnedNoiseRequest(source, RaisedOverlay(rect, 5.0f), 3), frame, 30, uploads);
    ExpectQueryBitEqualToUploads(pages, uploads);

    // An edit: a new overlay version over the same rect, derived from the one the pages hold.
    PagedTerrainRequest edited = PinnedNoiseRequest(source, RaisedOverlay(rect, 9.0f), 4);
    edited.OverlayChanged = rect;
    edited.OverlayChangedSince = 3;
    RunAndCollectLevel0Uploads(pages, edited, frame, 30, uploads);
    ExpectQueryBitEqualToUploads(pages, uploads);
    SurfaceHeightSample raised;
    ASSERT_TRUE(pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration})
                    .SampleHeight({350.0f, 0.0f, 350.0f}, SurfaceHeightPolicy::RequireFinest, raised));
    EXPECT_GT(raised.Height, 7.0f) << "the edit reached the CPU page";
}

TEST(CpuHeightPages, ADynamicBodyFarFromEveryCameraKeepsItsFinestPages)
{
    // 64 slots: the pyramid streams (only the top page is pinned), so level 0 near the body comes
    // from its window alone.
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords, 0, 64ull << 20, 4u});
    PagedTerrainRequest request = PinnedNoiseRequest(NoiseSource(), nullptr, 0);
    const Mathematics::Vector3 camera{100.0f, 10.0f, 100.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    // A dynamic body far from the camera, and a static one between them that opens no window.
    ECS::World world;
    AddBody(world, 900.0f, 900.0f, Physics::MotionType::Dynamic);
    AddBody(world, 500.0f, 900.0f, Physics::MotionType::Static);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    for (uint64 frame = 2; frame < 6; ++frame)
    {
        pages.GatherBodyWindows(world);
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    const SurfaceHeightQuery query = pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration});
    SurfaceHeightSample sample;
    ASSERT_TRUE(query.SampleHeight({900.0f, 0.0f, 900.0f}, SurfaceHeightPolicy::RequireFinest, sample));
    EXPECT_EQ(sample.Level, 0u);
    ASSERT_TRUE(query.SampleHeight({100.0f, 0.0f, 100.0f}, SurfaceHeightPolicy::RequireFinest, sample));
    EXPECT_EQ(sample.Level, 0u) << "and the camera keeps its own";
    ASSERT_TRUE(query.SampleHeight({500.0f, 0.0f, 900.0f}, SurfaceHeightPolicy::AcceptCoarse, sample));
    EXPECT_EQ(sample.Detail, SurfaceHeightDetail::Coarse) << "under the static body, the pinned level answers";
    EXPECT_FALSE(pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration + 1}).IsValid())
        << "a stale handle reads nothing";
}

// The narrow CBT arm, pending modifiers and a page table over the GPU cap keep the GPU on the
// terrain's texture (GpuPages false), and the CPU still reads the terrain's pages; no view at all
// (no camera) streams the CPU pages too.
TEST(CpuHeightPages, ATerrainTheGpuDrawsFromItsTextureStillHasCpuPagesAndPublishesNoTable)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords, 0, 64ull << 20, 4u});
    PagedTerrainRequest request = PinnedNoiseRequest(NoiseSource(), nullptr, 0);
    request.GpuPages = false;
    const Mathematics::Vector3 camera{300.0f, 10.0f, 300.0f};
    pages.Update(1, 1.0f / 60.0f, false, 2);
    for (uint64 frame = 2; frame < 6; ++frame)
    {
        pages.Request(request, {&camera, 1}, PageLevelView{});
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    const SurfaceHeightQuery query = pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration});
    SurfaceHeightSample sample;
    ASSERT_TRUE(query.SampleHeight({300.0f, 0.0f, 300.0f}, SurfaceHeightPolicy::RequireFinest, sample))
        << "the camera's window is on the CPU";
    EXPECT_EQ(sample.Level, 0u);
    ASSERT_TRUE(query.SampleHeight({900.0f, 0.0f, 100.0f}, SurfaceHeightPolicy::AcceptCoarse, sample))
        << "the pinned levels answer everywhere in the footprint";
    std::vector<uint32> drawn;
    pages.PagedTerrains(drawn);
    EXPECT_TRUE(drawn.empty()) << "the GPU draws its texture: no page table is published";
    uint64 version = 0;
    EXPECT_EQ(pages.TableWords(kTerrain, version), nullptr);
    EXPECT_TRUE(pages.DirtyRects().empty()) << "the drawn height never changed source";
    EXPECT_TRUE(pages.Uploads().empty());

    // The GPU starts drawing its pages: the whole terrain changes source, the CPU pages stay.
    request.GpuPages = true;
    pages.Request(request, {&camera, 1}, MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f));
    pages.Update(6, 1.0f / 60.0f, false, 2);
    pages.PagedTerrains(drawn);
    EXPECT_EQ(drawn.size(), 1u);
    EXPECT_FALSE(pages.DirtyRects().empty());
    ASSERT_TRUE(pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration})
                    .SampleHeight({300.0f, 0.0f, 300.0f}, SurfaceHeightPolicy::RequireFinest, sample));
}

// The CPU reads what the GPU fetched: a page the GPU wants beyond every CPU window answers finest.
TEST(CpuHeightPages, APageTheGpuFetchedAnswersFinestOutsideEveryCpuWindow)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{512, kDesktopHeightPages.DenseTableWords, 0, 64ull << 20, 4u});
    const PagedTerrainRequest request = PinnedNoiseRequest(NoiseSource(), nullptr, 0);
    const Mathematics::Vector3 camera{100.0f, 10.0f, 100.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    for (uint64 frame = 2; frame < 40; ++frame)
    {
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    // Level-0 page (3, 0) is two pages past the camera's window (pages 0 and 1): the GPU draws it at
    // level 0, so the CPU answers from the same fetch.
    const PageStreaming::PageCache& cache = pages.Residency().FieldCache(kHeightPageField);
    bool drawnFinest = false;
    for (const PageStreaming::CachedPage& page : cache.Resident())
        drawnFinest = drawnFinest || (page.Address == PageAddress{0, 0, 3, 0});
    ASSERT_TRUE(drawnFinest) << "the GPU wants level 0 at 3 pages from the camera";
    SurfaceHeightSample sample;
    ASSERT_TRUE(pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration})
                    .SampleHeight({450.0f, 0.0f, 60.0f}, SurfaceHeightPolicy::RequireFinest, sample));
    EXPECT_EQ(sample.Level, 0u);
}

TEST(CpuHeightPages, AFetchOfARestartedStreamsOldSourceNeverLands)
{
    PageStreaming::GeneratedHeightLattice flatLattice = NoiseLattice();
    flatLattice.Source = PageStreaming::GeneratedHeightSource::Flat;
    PageStreaming::PageSource flat;
    flat.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(flatLattice);
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(kSamples, kSamples);
    const std::vector<PageAddress> window = WindowAround(200.0f, 200.0f);

    // No synchronous window fetch, and room for the window's four fetches only: the old source's
    // fetches hold every slot until they are counted out. With no pool a fetch finishes inline, so
    // the flat pages wait in the completion queue when the stream restarts.
    CpuPages cache(CpuBudget(64ull << 20, 0u, 16u));
    cache.Configure(flat, levels, kFirstPinned);
    cache.Want(window);
    cache.Update(1);
    cache.Configure(NoiseSource(), levels, kFirstPinned); // a re-cook restarts the stream

    const SurfaceHeightQuery query = cache.Query(NoiseShape());
    const Mathematics::Vector3 point{200.0f, 0.0f, 200.0f};
    SurfaceHeightSample sample;
    for (uint64 frame = 2; frame < 5 && !query.SampleHeight(point, SurfaceHeightPolicy::RequireFinest, sample); ++frame)
    {
        cache.Want(window);
        cache.Update(frame);
    }
    ASSERT_TRUE(query.SampleHeight(point, SurfaceHeightPolicy::RequireFinest, sample))
        << "the restarted stream's own window fetches never landed";

    CacheAndQuery reference(kRoomyBudget);
    for (uint64 frame = 1; frame < 3; ++frame)
    {
        reference.Cache.Want(window);
        reference.Cache.Update(frame);
    }
    SurfaceHeightSample expected;
    ASSERT_TRUE(reference.Query.SampleHeight(point, SurfaceHeightPolicy::RequireFinest, expected));
    EXPECT_EQ(sample.Height, expected.Height) << "the old source's page landed in the restarted stream";
}

TEST(CpuHeightPages, AFailedFetchLeavesACoarserPageAnsweringUntilARetryLands)
{
    const TestUtils::ScopedTempDir dir(TestUtils::MakeUniqueTempDirectory("cpu_height_pages"));
    constexpr uint32 kWidth = 300;
    constexpr uint32 kHeight = 260;
    const std::filesystem::path heightmap = dir.Path() / "relief.r32";
    {
        std::vector<float32> heights(static_cast<std::size_t>(kWidth) * kHeight);
        for (std::size_t index = 0; index < heights.size(); ++index)
            heights[index] = static_cast<float32>(index % 97u);
        std::ofstream out(heightmap, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(heights.data()), static_cast<std::streamsize>(heights.size() * 4u));
    }
    const PageStreaming::HeightStoreCache stores(dir.Path() / "TerrainPages");
    const PageStreaming::HeightStoreCache::Result store =
        stores.Ensure(GUID::Generate(), heightmap, kWidth, kHeight, nullptr, nullptr);
    ASSERT_EQ(store.Error, "");
    auto reader = std::make_shared<PageStreaming::PageStoreReader>();
    ASSERT_EQ(reader->Open(store.Store, PageStreaming::PageFieldKind::Height), "");
    std::vector<char> bytes;
    {
        std::ifstream in(store.Store, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    const std::vector<PageStreaming::PageStoreLevel>& levels = reader->Layout().Levels;
    const uint32 top = static_cast<uint32>(levels.size()) - 1u;
    CpuPages cache(kRoomyBudget);
    cache.Configure(PageStreaming::PageSource{reader, nullptr}, levels, top);
    cache.Update(1); // the top page

    PageRequestTerrain shape;
    shape.Level0TexelX = 1.0f;
    shape.Level0TexelZ = 1.0f;
    shape.SamplesX = kWidth;
    shape.SamplesZ = kHeight;
    const SurfaceHeightQuery query = cache.Query(shape);
    const Mathematics::Vector3 point{100.0f, 0.0f, 100.0f};
    const std::vector<PageAddress> window = WindowAround(point.x, point.z);

    std::error_code error;
    std::filesystem::resize_file(store.Store, 64, error); // the reads fail (a store being rewritten)
    ASSERT_FALSE(error) << error.message();
    cache.Want(window);
    cache.Update(2);
    SurfaceHeightSample sample;
    ASSERT_TRUE(query.SampleHeight(point, SurfaceHeightPolicy::AcceptCoarse, sample));
    EXPECT_EQ(sample.Detail, SurfaceHeightDetail::Coarse);
    EXPECT_EQ(sample.Level, top);
    EXPECT_FALSE(query.SampleHeight(point, SurfaceHeightPolicy::RequireFinest, sample));

    {
        std::ofstream out(store.Store, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    for (uint64 frame = 3; frame < 200; ++frame)
    {
        cache.Want(window);
        cache.Update(frame);
    }
    ASSERT_TRUE(query.SampleHeight(point, SurfaceHeightPolicy::RequireFinest, sample))
        << "the store is whole again, but the failed window page was never fetched again";
    EXPECT_EQ(sample.Level, 0u);
}

TEST(CpuHeightPages, AWindowIsTwoPagesWideWithTheNeighborOnTheNearerSide)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords, 0, 64ull << 20, 4u});
    // The CPU pages alone: the GPU's level-0 pages around the camera would be on the CPU too.
    PagedTerrainRequest request = PinnedNoiseRequest(NoiseSource(), nullptr, 0);
    request.GpuPages = false;
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    // The camera stands 12 m into level-0 page (6, 6), which starts at 768 m: its window holds that
    // page and page 5 on each axis, the nearer neighbors, so 4 m back across the edge is finest.
    const Mathematics::Vector3 camera{780.0f, 10.0f, 780.0f};
    pages.Update(1, 1.0f / 60.0f, false, 2);
    for (uint64 frame = 2; frame < 6; ++frame)
    {
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    const SurfaceHeightQuery query = pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration});
    SurfaceHeightSample sample;
    for (const Mathematics::Vector3 point : {Mathematics::Vector3{780.0f, 0.0f, 780.0f}, Mathematics::Vector3{764.0f, 0.0f, 780.0f},
                                             Mathematics::Vector3{780.0f, 0.0f, 764.0f}, Mathematics::Vector3{764.0f, 0.0f, 764.0f}})
    {
        ASSERT_TRUE(query.SampleHeight(point, SurfaceHeightPolicy::RequireFinest, sample))
            << "(" << point.x << ", " << point.z << ") is in the window";
        EXPECT_EQ(sample.Level, 0u);
    }
    ASSERT_TRUE(query.SampleHeight({900.0f, 0.0f, 900.0f}, SurfaceHeightPolicy::AcceptCoarse, sample));
    EXPECT_EQ(sample.Detail, SurfaceHeightDetail::Coarse) << "page 7, on the farther side, is not in the window";
}

TEST(CpuHeightPages, AChangeOfFramesInFlightKeepsTheCpuPages)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords, 0, 64ull << 20, 4u});
    const PagedTerrainRequest request = PinnedNoiseRequest(NoiseSource(), nullptr, 0);
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    const Mathematics::Vector3 camera{200.0f, 10.0f, 200.0f};
    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    for (; frame < 6; ++frame)
    {
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    // The renderer moves to three frames in flight: the GPU cache restarts with the next Request.
    pages.Request(request, {&camera, 1}, view);
    pages.Update(frame++, 1.0f / 60.0f, false, 3);
    pages.Request(request, {&camera, 1}, view);
    SurfaceHeightSample sample;
    ASSERT_TRUE(pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration})
                    .SampleHeight(camera, SurfaceHeightPolicy::RequireFinest, sample))
        << "the window page under the camera survives the GPU cache's restart";
    EXPECT_EQ(sample.Level, 0u);
}

TEST(CpuHeightPages, BodyWindowsAreBoundedToTheBodiesNearestACamera)
{
    // A CPU budget of 16 pages: body windows may hold a quarter of it, one body's window.
    constexpr uint64 kSixteenPages = 16ull * PageStreaming::kPageSampleCount * sizeof(float32);
    TerrainHeightPages pages(nullptr, nullptr,
                             HeightPageProfile{64, kDesktopHeightPages.DenseTableWords, 0, kSixteenPages, 4u});
    const PagedTerrainRequest request = PinnedNoiseRequest(NoiseSource(), nullptr, 0);
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    const Mathematics::Vector3 camera{100.0f, 10.0f, 100.0f};
    ECS::World world;
    AddBody(world, 900.0f, 900.0f, Physics::MotionType::Dynamic);
    AddBody(world, 450.0f, 450.0f, Physics::MotionType::Dynamic);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    for (uint64 frame = 2; frame < 8; ++frame)
    {
        pages.GatherBodyWindows(world);
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    const SurfaceHeightQuery query = pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration});
    SurfaceHeightSample sample;
    EXPECT_TRUE(query.SampleHeight({450.0f, 0.0f, 450.0f}, SurfaceHeightPolicy::RequireFinest, sample))
        << "the body nearer the camera keeps its window";
    ASSERT_TRUE(query.SampleHeight({900.0f, 0.0f, 900.0f}, SurfaceHeightPolicy::AcceptCoarse, sample));
    EXPECT_EQ(sample.Detail, SurfaceHeightDetail::Coarse) << "the farther body is past the bound";
}

TEST(CpuHeightPages, AReaderWindowBringsTheFinestPagesAwayFromEveryCamera)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords, 0, 64ull << 20, 4u});
    PagedTerrainRequest request = PinnedNoiseRequest(NoiseSource(), nullptr, 0);
    request.GpuPages = false; // only the CPU windows want level 0
    const Mathematics::Vector3 camera{100.0f, 10.0f, 100.0f};
    const Mathematics::Vector3 reader{900.0f, 0.0f, 900.0f};
    pages.Update(1, 1.0f / 60.0f, false, 2);
    for (uint64 frame = 2; frame < 6; ++frame)
    {
        pages.Request(request, {&camera, 1}, PageLevelView{});
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    SurfaceHeightSample sample;
    ASSERT_TRUE(pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration})
                    .SampleHeight(reader, SurfaceHeightPolicy::AcceptCoarse, sample));
    ASSERT_EQ(sample.Detail, SurfaceHeightDetail::Coarse) << "the reader's point is outside every window";

    for (uint64 frame = 6; frame < 10; ++frame)
    {
        pages.RequestReaderWindow(reader);
        pages.Request(request, {&camera, 1}, PageLevelView{});
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    ASSERT_TRUE(pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration})
                    .SampleHeight(reader, SurfaceHeightPolicy::RequireFinest, sample))
        << "the reader's window is on the CPU";
    EXPECT_EQ(sample.Level, 0u);
}

TEST(CpuHeightPages, AFetchOfTheFinestPageAnswersFinestBeforeTheNextUpdate)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords, 0, 64ull << 20, 4u});
    PagedTerrainRequest request = PinnedNoiseRequest(NoiseSource(), nullptr, 0);
    request.GpuPages = false;
    const Mathematics::Vector3 camera{100.0f, 10.0f, 100.0f};
    const Mathematics::Vector3 pick{900.0f, 0.0f, 600.0f};
    pages.Update(1, 1.0f / 60.0f, false, 2);
    for (uint64 frame = 2; frame < 6; ++frame)
    {
        pages.Request(request, {&camera, 1}, PageLevelView{});
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    SurfaceHeightSample sample;
    ASSERT_FALSE(pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration})
                     .SampleHeight(pick, SurfaceHeightPolicy::RequireFinest, sample));

    ASSERT_TRUE(pages.FetchFinestPage(TerrainHandle{kTerrain, kGeneration}, pick));
    ASSERT_TRUE(pages.SurfaceQuery(TerrainHandle{kTerrain, kGeneration})
                    .SampleHeight(pick, SurfaceHeightPolicy::RequireFinest, sample))
        << "the fetched page answers at once";
    EXPECT_EQ(sample.Level, 0u);
    EXPECT_FALSE(pages.FetchFinestPage(TerrainHandle{kTerrain, kGeneration + 1}, pick)) << "another terrain";
}

TEST(CpuHeightPages, AnEditCursorReportsTheOverlayRectAndTheWholeFootprintForAMissedStepOrANewSource)
{
    CpuPages cpu(CpuBudget(64ull << 20, 4u));
    const Mathematics::Rect whole = Mathematics::Rect::FromEdges(0.0f, 0.0f, 1024.0f, 1024.0f);
    PlanarHeightfieldEditCursor cursor;
    std::vector<Mathematics::Rect> edits;

    cursor.Reset(EditedQuery(cpu, 0, 0, 0));
    cursor.CollectEdits(EditedQuery(cpu, 7, 3, 0), edits);
    EXPECT_TRUE(edits.empty()) << "a first source is the pages arriving, not an edit";
    cursor.CollectEdits(EditedQuery(cpu, 7, 3, 0), edits);
    EXPECT_TRUE(edits.empty());

    cursor.CollectEdits(EditedQuery(cpu, 7, 4, 3), edits);
    ASSERT_EQ(edits.size(), 1u);
    EXPECT_EQ(edits[0], Mathematics::Rect::FromEdges(9.0f, 19.0f, 31.0f, 41.0f))
        << "every lattice quad touching samples 10..30 x 20..40";

    edits.clear();
    cursor.CollectEdits(EditedQuery(cpu, 7, 6, 5), edits);
    ASSERT_EQ(edits.size(), 1u);
    EXPECT_EQ(edits[0], whole) << "the cursor missed version 5";

    edits.clear();
    cursor.CollectEdits(EditedQuery(cpu, 9, 6, 5), edits);
    ASSERT_EQ(edits.size(), 1u);
    EXPECT_EQ(edits[0], whole) << "a new source changes every height";
}

namespace
{

// A skyline over NoiseShape: 1,000 stations 1 m apart, kept within 5 mrad of the march's line.
constexpr float32 kSkylineDistance = 1000.0f;
constexpr float32 kSkylineStep = 1.0f;
constexpr float32 kSkylineTolerance = 0.005f;
constexpr uint32 kSkylineStations = 1000;

// Updates `cpu`'s stream, `window` wanted, from `frame` on until an Update changes no CPU page;
// returns the next frame.
uint64 SettlePages(CpuPages& cpu, std::span<const PageAddress> window, uint64 frame)
{
    for (uint32 attempt = 0; attempt < 16; ++attempt, ++frame)
    {
        const uint64 before = cpu.Residency.CpuPages(kStream).ChangesEnd();
        cpu.Want(window);
        cpu.Update(frame);
        if (cpu.Residency.CpuPages(kStream).ChangesEnd() == before)
            return frame + 1;
    }
    ADD_FAILURE() << "the CPU pages never settled";
    return frame;
}

float32 SkylineFrom(TerrainSkyline& skyline, const SurfaceHeightQuery& query, const Mathematics::Vector3& eye,
                    float32 dirX = 1.0f, float32 dirZ = 1.0f)
{
    return skyline.Tangent(query, eye, dirX, dirZ, kSkylineDistance, kSkylineStep, kSkylineTolerance);
}

// A skyline that keeps nothing: the march from scratch.
float32 FreshSkylineFrom(const SurfaceHeightQuery& query, const Mathematics::Vector3& eye)
{
    TerrainSkyline fresh;
    return SkylineFrom(fresh, query, eye);
}

} // namespace

TEST(CpuHeightPages, AnEditCursorOverANewWholeMapAtTheSameAddressStartsThere)
{
    TerrainData map;
    map.Heightfield = Terrain::HeightfieldData(kSamples, kSamples, 0.25f);
    PlanarHeightfieldEditCursor cursor;
    cursor.Reset(SurfaceHeightQuery(map, TerrainHandle{3, 1}, NoiseShape(), 0.0f, 1.0f));

    // The terrain is freed and a new one takes its slot and its address, and is edited.
    map.Heightfield = Terrain::HeightfieldData(kSamples, kSamples, 0.75f);
    map.MarkRegionDirty(4, 4, 8, 8);
    std::vector<Mathematics::Rect> edits;
    cursor.CollectEdits(SurfaceHeightQuery(map, TerrainHandle{3, 2}, NoiseShape(), 0.0f, 1.0f), edits);
    EXPECT_TRUE(edits.empty()) << "a new terrain at the same address read as an edit of the cursor's terrain";
}

TEST(CpuHeightPages, ASkylineSamplesNothingWhileItsEyeTheSunAndThePagesStayPut)
{
    CpuPages cpu(kRoomyBudget);
    cpu.Configure(NoiseSource(), PageStreaming::BuildPageStoreLevels(kSamples, kSamples), kFirstPinned);
    const std::vector<PageAddress> window = WindowAround(100.0f, 100.0f);
    uint64 frame = SettlePages(cpu, window, 1);

    TerrainSkyline skyline;
    const Mathematics::Vector3 eye{100.0f, 0.5f, 100.0f};
    const float32 first = SkylineFrom(skyline, cpu.Query(NoiseShape()), eye);
    EXPECT_EQ(skyline.LastSampleCount(), kSkylineStations) << "the first answer marches every station";
    ASSERT_NE(first, TerrainSkyline::kNoSkyline);

    cpu.Want(window);
    cpu.Update(frame++);
    EXPECT_EQ(SkylineFrom(skyline, cpu.Query(NoiseShape()), eye), first);
    EXPECT_EQ(skyline.LastSampleCount(), 0u) << "nothing moved and no page changed";

    const Mathematics::Vector3 raised{100.0f, 1.5f, 100.0f};
    const float32 fromRaised = SkylineFrom(skyline, cpu.Query(NoiseShape()), raised);
    EXPECT_EQ(skyline.LastSampleCount(), 0u) << "a height change of the eye costs no sample";
    EXPECT_LT(fromRaised, first);
    EXPECT_EQ(fromRaised, FreshSkylineFrom(cpu.Query(NoiseShape()), raised));
}

TEST(CpuHeightPages, APageThatLandsUnderASkylineResamplesOnlyTheStationsOverIt)
{
    CpuPages cpu(kRoomyBudget);
    cpu.Configure(NoiseSource(), PageStreaming::BuildPageStoreLevels(kSamples, kSamples), kFirstPinned);
    uint64 frame = SettlePages(cpu, WindowAround(100.0f, 100.0f), 1);

    TerrainSkyline skyline;
    const Mathematics::Vector3 eye{100.0f, 0.5f, 100.0f};
    SkylineFrom(skyline, cpu.Query(NoiseShape()), eye);

    // The finest pages around (500, 500), on the march's line, land.
    SettlePages(cpu, WindowAround(500.0f, 500.0f), frame);
    const float32 tangent = SkylineFrom(skyline, cpu.Query(NoiseShape()), eye);
    EXPECT_GT(skyline.LastSampleCount(), 0u) << "the landed pages' stations kept their coarse heights";
    EXPECT_LT(skyline.LastSampleCount(), kSkylineStations / 2u) << "stations away from the landed pages resampled";
    EXPECT_EQ(tangent, FreshSkylineFrom(cpu.Query(NoiseShape()), eye));
}

TEST(CpuHeightPages, ASkylineResamplesTheRingsItsEyeLeftAndEveryStationWhenTheSunTurns)
{
    CpuPages cpu(kRoomyBudget);
    cpu.Configure(NoiseSource(), PageStreaming::BuildPageStoreLevels(kSamples, kSamples), kFirstPinned);
    SettlePages(cpu, WindowAround(100.0f, 100.0f), 1);
    const SurfaceHeightQuery query = cpu.Query(NoiseShape());

    TerrainSkyline skyline;
    SkylineFrom(skyline, query, {100.0f, 0.5f, 100.0f});

    // 0.5 m is past the tolerance of rings 0 to 6 (nearest stations 1 to 64 m: 5 to 320 mm) and
    // inside it for rings 7 to 9 (nearest 128 m and farther: 640 mm and more).
    SkylineFrom(skyline, query, {100.5f, 0.5f, 100.0f});
    EXPECT_EQ(skyline.LastSampleCount(), 127u) << "the stations nearer than 128 m";
    SkylineFrom(skyline, query, {100.5f, 0.5f, 100.0f});
    EXPECT_EQ(skyline.LastSampleCount(), 0u);

    // The sun's azimuth turns from the diagonal by twice the tolerance.
    const float32 azimuth = std::atan2(1.0f, 1.0f) + 2.0f * kSkylineTolerance;
    SkylineFrom(skyline, query, {100.5f, 0.5f, 100.0f}, std::cos(azimuth), std::sin(azimuth));
    EXPECT_EQ(skyline.LastSampleCount(), kSkylineStations);

    SkylineFrom(skyline, query, {120.0f, 0.5f, 100.0f}, std::cos(azimuth), std::sin(azimuth));
    EXPECT_EQ(skyline.LastSampleCount(), kSkylineStations) << "20 m is past every ring's tolerance";
}

TEST(CpuHeightPages, ASkylineOverARestartedStreamResamplesEveryStation)
{
    CpuPages cpu(kRoomyBudget);
    cpu.Configure(NoiseSource(), PageStreaming::BuildPageStoreLevels(kSamples, kSamples), kFirstPinned);
    uint64 frame = SettlePages(cpu, WindowAround(100.0f, 100.0f), 1);
    TerrainSkyline skyline;
    const Mathematics::Vector3 eye{100.0f, 0.5f, 100.0f};
    SkylineFrom(skyline, cpu.Query(NoiseShape()), eye);

    std::vector<Mathematics::Rect> changed;
    const SurfaceHeightPageStamp before = cpu.Query(NoiseShape()).PageStamp();
    cpu.Configure(NoiseSource(), PageStreaming::BuildPageStoreLevels(kSamples, kSamples), kFirstPinned);
    SettlePages(cpu, WindowAround(100.0f, 100.0f), frame);
    EXPECT_FALSE(cpu.Query(NoiseShape()).CollectPageChanges(before, changed)) << "the record restarted with the stream";
    SkylineFrom(skyline, cpu.Query(NoiseShape()), eye);
    EXPECT_EQ(skyline.LastSampleCount(), kSkylineStations);
}

TEST(CpuHeightPages, ASkylineOverANewWholeMapAtTheSameAddressResamplesEveryStation)
{
    TerrainData map;
    map.Heightfield = Terrain::HeightfieldData(kSamples, kSamples, 0.25f);
    TerrainSkyline skyline;
    const Mathematics::Vector3 eye{100.0f, 0.5f, 100.0f};
    SkylineFrom(skyline, SurfaceHeightQuery(map, TerrainHandle{3, 1}, NoiseShape(), 0.0f, 1.0f), eye);

    // The terrain is freed and a new one takes its slot and its address, with other heights.
    map.Heightfield = Terrain::HeightfieldData(kSamples, kSamples, 0.75f);
    const SurfaceHeightQuery rebuilt(map, TerrainHandle{3, 2}, NoiseShape(), 0.0f, 1.0f);
    EXPECT_EQ(SkylineFrom(skyline, rebuilt, eye), FreshSkylineFrom(rebuilt, eye));
    EXPECT_EQ(skyline.LastSampleCount(), kSkylineStations) << "another terrain at the same address";
}

TEST(CpuHeightPages, AQueryHeldAcrossAFetchReportsTheFetchedPageAmongItsChanges)
{
    CpuPages cpu(kRoomyBudget);
    cpu.Configure(NoiseSource(), PageStreaming::BuildPageStoreLevels(kSamples, kSamples), kFirstPinned);
    SettlePages(cpu, WindowAround(100.0f, 100.0f), 1);
    const SurfaceHeightQuery query = cpu.Query(NoiseShape());
    const SurfaceHeightPageStamp before = query.PageStamp();

    // A pick far from every window fetches its finest page between two Updates.
    ASSERT_TRUE(cpu.Residency.FetchCpuPage(kStream, WindowAround(900.0f, 600.0f).front()));
    std::vector<Mathematics::Rect> changed;
    ASSERT_TRUE(query.CollectPageChanges(before, changed));
    EXPECT_GT(query.PageStamp().Change, before.Change) << "the held query's place did not move with the fetch";
    const bool covered = std::any_of(changed.begin(), changed.end(), [](const Mathematics::Rect& rect) {
        return rect.X <= 900.0f && rect.Right() >= 900.0f && rect.Y <= 600.0f && rect.Bottom() >= 600.0f;
    });
    EXPECT_TRUE(covered) << "the held query does not report the page the fetch landed";
}

TEST(CpuHeightPages, ASkylineWhosePageRecordNoLongerReachesBackResamplesEveryStation)
{
    // A budget of one page: each frame the window moves, its pages land and the pages it left are dropped.
    CpuPages cpu(CpuBudget(static_cast<uint64>(PageStreaming::kPageSampleCount) * sizeof(float32), 4u));
    cpu.Configure(NoiseSource(), PageStreaming::BuildPageStoreLevels(kSamples, kSamples), kFirstPinned);
    const std::vector<PageAddress> nearWindow = WindowAround(200.0f, 200.0f);
    const std::vector<PageAddress> farWindow = WindowAround(700.0f, 700.0f);
    uint64 frame = SettlePages(cpu, nearWindow, 1);
    TerrainSkyline skyline;
    const Mathematics::Vector3 eye{100.0f, 0.5f, 100.0f};
    SkylineFrom(skyline, cpu.Query(NoiseShape()), eye);
    const SurfaceHeightPageStamp stamp = cpu.Query(NoiseShape()).PageStamp();

    // The record keeps the latest changes only: move the window until it has dropped the skyline's place.
    const std::size_t record = cpu.Residency.CpuPages(kStream).Changes.size();
    ASSERT_GT(record, 0u);
    bool far = true;
    for (; cpu.Query(NoiseShape()).PageStamp().Change - stamp.Change <= record && frame < 4u * record; far = !far)
    {
        cpu.Want(far ? farWindow : nearWindow);
        cpu.Update(frame++);
    }
    std::vector<Mathematics::Rect> changed;
    EXPECT_FALSE(cpu.Query(NoiseShape()).CollectPageChanges(stamp, changed));
    EXPECT_EQ(SkylineFrom(skyline, cpu.Query(NoiseShape()), eye), FreshSkylineFrom(cpu.Query(NoiseShape()), eye));
    EXPECT_EQ(skyline.LastSampleCount(), kSkylineStations) << "a change it can no longer read may be under any station";

    // Past the wrap the record still names the latest changes: a move to a third window reports its
    // pages, and resamples only their stations and those of the window it left.
    const SurfaceHeightPageStamp beforeMove = cpu.Query(NoiseShape()).PageStamp();
    cpu.Want(WindowAround(500.0f, 500.0f));
    cpu.Update(frame++);
    changed.clear();
    ASSERT_TRUE(cpu.Query(NoiseShape()).CollectPageChanges(beforeMove, changed));
    EXPECT_TRUE(std::any_of(changed.begin(), changed.end(), [](const Mathematics::Rect& rect) {
        return rect.X <= 500.0f && rect.Right() >= 500.0f && rect.Y <= 500.0f && rect.Bottom() >= 500.0f;
    })) << "the record read past its wrap does not name the pages that landed";
    EXPECT_EQ(SkylineFrom(skyline, cpu.Query(NoiseShape()), eye), FreshSkylineFrom(cpu.Query(NoiseShape()), eye));
    EXPECT_GT(skyline.LastSampleCount(), 0u) << "the moved window's stations kept their old heights";
    EXPECT_LT(skyline.LastSampleCount(), kSkylineStations);
}

TEST(CpuHeightPages, ARecordExactlyFullReportsEveryChangeItHoldsAndNoReaderOneChangeOlder)
{
    CpuPages cpu(kRoomyBudget);
    cpu.Configure(NoiseSource(), PageStreaming::BuildPageStoreLevels(kSamples, kSamples), kFirstPinned);
    SettlePages(cpu, WindowAround(100.0f, 100.0f), 1);
    // A ring of four changes that has wrapped: changes 6 to 9 are in it, change 5 is overwritten.
    PageStreaming::CpuPageDirectory directory = cpu.Residency.CpuPages(kStream);
    const PageAddress page = WindowAround(700.0f, 700.0f).front();
    const std::array<PageAddress, 4> ring{page, page, page, page};
    const uint64 changeCount = 10u;
    directory.Changes = ring;
    directory.ChangeCount = &changeCount;
    const SurfaceHeightQuery query(directory, cpu.Levels, NoiseShape(), 0.0f, 1.0f, nullptr, SurfaceHeightEdits{});

    std::vector<Mathematics::Rect> changed;
    ASSERT_TRUE(query.CollectPageChanges({directory.Epoch, changeCount - ring.size()}, changed))
        << "a reader as far behind as the ring holds reads every change";
    EXPECT_EQ(changed.size(), ring.size());
    changed.clear();
    EXPECT_FALSE(query.CollectPageChanges({directory.Epoch, changeCount - ring.size() - 1u}, changed))
        << "a reader one change further behind would miss the overwritten change";
}

// The island scene's three terrains: a planet around the origin, a planar terrain away from the
// origin and the island at the origin. The query at a point on the island reads the island, in
// whatever order the world stores the terrains.
TEST(SurfaceHeightTerrain, ThePointPicksTheTerrainUnderItInEveryStorageOrder)
{
    if (!TerrainService::TryGet())
        TerrainService::Initialize();
    const Mathematics::Vector3 onIsland(10.0f, 0.0f, -20.0f);
    for (const bool islandFirst : {false, true})
    {
        ECS::World world;
        ECS::EntityHandle island;
        if (islandFirst)
            island = AddPlanarTerrain(world, 512.0f, 0.0f, 0.0f);
        AddPlanet(world, 2000.0f);
        AddPlanarTerrain(world, 256.0f, 700.0f, 0.0f);
        if (!islandFirst)
            island = AddPlanarTerrain(world, 512.0f, 0.0f, 0.0f);

        EXPECT_EQ(FindSurfaceHeightTerrain(world, onIsland).Terrain, world.GetComponent<Components::Terrain>(island))
            << "island first: " << islandFirst;
        const SurfaceHeightQuery query = ResolveSurfaceHeightQuery(world, onIsland);
        ASSERT_TRUE(query.IsValid()) << "island first: " << islandFirst;
        EXPECT_FLOAT_EQ(query.FootprintOriginX(), -256.0f);
        EXPECT_FLOAT_EQ(query.FootprintSizeX(), 512.0f);
    }
    TerrainService::Shutdown();
}
