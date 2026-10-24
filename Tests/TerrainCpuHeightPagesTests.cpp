// The CPU height pages of a paged terrain (design page-streaming D3, as page-streaming-planets 3.11
// item 1 amends it) and the query the CPU readers sample them through (SurfaceHeightQuery): the
// windows keep the finest level, outside every window the answer is coarse and never unsampled, a
// RequireFinest caller treats a coarse answer as unsampled, the synchronous fetch holds its deadline,
// the CPU pages hold the bytes the GPU uploads, overlay included, and a dynamic body far from every
// camera keeps its finest pages; a fetch of a restarted stream never lands in it, and a failed fetch
// leaves a coarser page answering until a retry lands.

#include "TerrainECS/CpuHeightPageCache.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainService.h"

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/HeightStoreCache.h"
#include "PageStreaming/PageStoreFormat.h"
#include "PageStreaming/PageStoreReader.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
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

struct CacheAndQuery
{
    explicit CacheAndQuery(const CpuHeightPageBudget& budget, CpuHeightPageClock clock = &std::chrono::steady_clock::now)
        : Cache(nullptr, nullptr, budget, std::move(clock))
    {
        Cache.Configure(kStream, NoiseSource(), PageStreaming::BuildPageStoreLevels(kSamples, kSamples), kFirstPinned,
                        nullptr);
        Query = SurfaceHeightQuery(Cache, kStream, NoiseShape(), 0.0f, 1.0f);
    }

    CpuHeightPageCache Cache;
    SurfaceHeightQuery Query;
};

constexpr CpuHeightPageBudget kRoomyBudget{64ull << 20, 4u};

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

} // namespace

TEST(CpuHeightPages, AWindowKeepsTheFinestLevelWhileThePagesItLeftAreDroppedOverTheBudget)
{
    // A budget of one page: only the pinned pages and the window's are kept past it.
    CacheAndQuery cpu(CpuHeightPageBudget{static_cast<uint64>(PageStreaming::kPageSampleCount) * sizeof(float32), 4u});
    const std::vector<PageAddress> first = WindowAround(200.0f, 200.0f);
    uint64 frame = 1;
    for (; frame < 4; ++frame)
    {
        cpu.Cache.Want(kStream, first);
        cpu.Cache.Update(frame);
    }
    SurfaceHeightSample sample;
    ASSERT_TRUE(cpu.Query.SampleHeight({200.0f, 0.0f, 200.0f}, SurfaceHeightPolicy::RequireFinest, sample));
    EXPECT_EQ(sample.Level, 0u);
    EXPECT_EQ(sample.Detail, SurfaceHeightDetail::Finest);

    const std::vector<PageAddress> second = WindowAround(700.0f, 700.0f);
    for (; frame < 8; ++frame)
    {
        cpu.Cache.Want(kStream, second);
        cpu.Cache.Update(frame);
        ASSERT_TRUE(cpu.Query.SampleHeight({700.0f, 0.0f, 700.0f}, SurfaceHeightPolicy::AcceptCoarse, sample));
        EXPECT_EQ(sample.Level, 0u) << "the moved window's pages are fetched within the frame";
    }
    ASSERT_TRUE(cpu.Query.SampleHeight({200.0f, 0.0f, 200.0f}, SurfaceHeightPolicy::AcceptCoarse, sample));
    EXPECT_EQ(sample.Detail, SurfaceHeightDetail::Coarse) << "the left window's pages are dropped over the budget";
    EXPECT_EQ(sample.Level, kFirstPinned);
}

TEST(CpuHeightPages, OutsideEveryWindowTheAnswerIsCoarseAndNeverUnsampledInsideTheFootprint)
{
    CacheAndQuery cpu(kRoomyBudget);
    cpu.Cache.Want(kStream, WindowAround(200.0f, 200.0f));
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
    cpu.Cache.Want(kStream, WindowAround(200.0f, 200.0f));
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
    // Every clock read is 200 us after the last: the reads at 200 and 400 us start a fetch, the one
    // at 600 us is past the 500 us deadline.
    auto now = std::make_shared<std::chrono::steady_clock::time_point>();
    const CpuHeightPageClock clock = [now]() {
        *now += std::chrono::microseconds(200);
        return *now;
    };
    CacheAndQuery cpu(CpuHeightPageBudget{64ull << 20, 8u}, clock);
    std::vector<PageAddress> windows = WindowAround(200.0f, 200.0f);
    const std::vector<PageAddress> other = WindowAround(700.0f, 700.0f);
    windows.insert(windows.end(), other.begin(), other.end());
    cpu.Cache.Want(kStream, windows);
    cpu.Cache.Update(1);

    EXPECT_EQ(CountFinestPages(cpu.Query, windows), 2u) << "two window pages fit before the deadline";

    cpu.Cache.Want(kStream, windows);
    cpu.Cache.Update(2);
    EXPECT_EQ(CountFinestPages(cpu.Query, windows), 8u) << "the rest were fetched on jobs and land at the next Update";
}

TEST(CpuHeightPages, TheSynchronousFetchStopsAtThePageCapWhenTheClockAllows)
{
    const auto still = std::chrono::steady_clock::now();
    CacheAndQuery cpu(CpuHeightPageBudget{64ull << 20, 3u}, [still]() { return still; });
    const std::vector<PageAddress> windows = WindowAround(200.0f, 200.0f);
    cpu.Cache.Want(kStream, windows);
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
                    uploads[{upload.Page.Address.X, upload.Page.Address.Z}] = samples.Samples;
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
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.TableWordCap, 0, 64ull << 20, 4u});
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
    CpuHeightPageCache cache(nullptr, nullptr, CpuHeightPageBudget{64ull << 20, 0u, 4u});
    cache.Configure(kStream, flat, levels, kFirstPinned, nullptr);
    cache.Want(kStream, window);
    cache.Update(1);
    cache.Configure(kStream, NoiseSource(), levels, kFirstPinned, nullptr); // a re-cook restarts the stream

    const SurfaceHeightQuery query(cache, kStream, NoiseShape(), 0.0f, 1.0f);
    const Mathematics::Vector3 point{200.0f, 0.0f, 200.0f};
    SurfaceHeightSample sample;
    for (uint64 frame = 2; frame < 5 && !query.SampleHeight(point, SurfaceHeightPolicy::RequireFinest, sample); ++frame)
    {
        cache.Want(kStream, window);
        cache.Update(frame);
    }
    ASSERT_TRUE(query.SampleHeight(point, SurfaceHeightPolicy::RequireFinest, sample))
        << "the restarted stream's own window fetches never landed";

    CacheAndQuery reference(kRoomyBudget);
    for (uint64 frame = 1; frame < 3; ++frame)
    {
        reference.Cache.Want(kStream, window);
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
    ASSERT_EQ(reader->Open(store.Store), "");
    std::vector<char> bytes;
    {
        std::ifstream in(store.Store, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    const std::vector<PageStreaming::PageStoreLevel>& levels = reader->Layout().Levels;
    const uint32 top = static_cast<uint32>(levels.size()) - 1u;
    CpuHeightPageCache cache(nullptr, nullptr, kRoomyBudget);
    cache.Configure(kStream, PageStreaming::PageSource{reader, nullptr}, levels, top, nullptr);
    cache.Update(1); // the top page

    PageRequestTerrain shape;
    shape.Level0TexelX = 1.0f;
    shape.Level0TexelZ = 1.0f;
    shape.SamplesX = kWidth;
    shape.SamplesZ = kHeight;
    const SurfaceHeightQuery query(cache, kStream, shape, 0.0f, 1.0f);
    const Mathematics::Vector3 point{100.0f, 0.0f, 100.0f};
    const std::vector<PageAddress> window = WindowAround(point.x, point.z);

    std::error_code error;
    std::filesystem::resize_file(store.Store, 64, error); // the reads fail (a store being rewritten)
    ASSERT_FALSE(error) << error.message();
    cache.Want(kStream, window);
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
        cache.Want(kStream, window);
        cache.Update(frame);
    }
    ASSERT_TRUE(query.SampleHeight(point, SurfaceHeightPolicy::RequireFinest, sample))
        << "the store is whole again, but the failed window page was never fetched again";
    EXPECT_EQ(sample.Level, 0u);
}

TEST(CpuHeightPages, AWindowIsTwoPagesWideWithTheNeighborOnTheNearerSide)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.TableWordCap, 0, 64ull << 20, 4u});
    const PagedTerrainRequest request = PinnedNoiseRequest(NoiseSource(), nullptr, 0);
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
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.TableWordCap, 0, 64ull << 20, 4u});
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
                             HeightPageProfile{64, kDesktopHeightPages.TableWordCap, 0, kSixteenPages, 4u});
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
