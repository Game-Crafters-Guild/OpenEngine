// The paged height source of a planar terrain as pure oracles: the level rule against the design
// note's numbers, the request set's closure under parents, the page cache's residency, and the CPU
// mirror of the paged resolve (parity, the fallback walk, ring continuity, parked quiescence).

#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/PagedHeightSampler.h"
#include "TerrainECS/TerrainAtlas.h"

#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageStoreFormat.h"
#include "PageStreaming/PageTableEntry.h"

#include <gtest/gtest.h>

#include "GlslShim.h"
#include "TerrainPagedField.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <tuple>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using GameEngine::PageStreaming::PageAddress;
using namespace GameEngine::TerrainECS::Test;

namespace
{

// The design note's measured viewport: 900 rows, 60 degree vertical field, TargetPixelError 11.
PageLevelView NoteView()
{
    return MakePageLevelView(900, std::numbers::pi_v<float32> / 3.0f, 11.0f);
}

// The user's 60 x 15 km terrain at 0.25 m, ground between 171.6 and 304.3 m.
PageRequestTerrain UserTerrain()
{
    PageRequestTerrain terrain;
    terrain.Level0TexelX = 0.25f;
    terrain.Level0TexelZ = 0.25f;
    terrain.SamplesX = 240000;
    terrain.SamplesZ = 60000;
    terrain.MinHeight = 171.6f;
    terrain.MaxHeight = 304.3f;
    return terrain;
}

std::tuple<uint32, uint32, uint32> Key(const PageAddress& a)
{
    return {a.Level, a.X, a.Z};
}

} // namespace

TEST(PageLevelRule, GeometryLevelsStartAtTwentyOneMetersAndDoubleAsTheNoteComputes)
{
    const PageLevelView view = NoteView();
    EXPECT_NEAR(view.FocalPixels, 779.4f, 0.1f);
    EXPECT_NEAR(view.TargetPixels, 9.17f, 0.01f);
    // Level 0 ends at about 21 m for a 0.25 m field, and each level's ring is twice the one before.
    EXPECT_NEAR(GeometryPageLevel(21.25f, 0.25f, view), 0.0f, 0.01f);
    for (uint32 level = 1; level < 11; ++level)
        EXPECT_NEAR(GeometryPageLevel(21.25f * static_cast<float32>(1u << level), 0.25f, view),
                    static_cast<float32>(level), 0.01f);
}

TEST(PageLevelRule, ParentBlendRampsOverTheLastQuarterOfEachRingOnly)
{
    for (float32 base : {0.0f, 3.0f, 7.0f})
    {
        EXPECT_EQ(PageParentBlend(base), 0.0f);
        EXPECT_EQ(PageParentBlend(base + 0.5f), 0.0f);
        EXPECT_EQ(PageParentBlend(base + 0.75f), 0.0f);
        EXPECT_FLOAT_EQ(PageParentBlend(base + 0.875f), 0.5f);
        EXPECT_GT(PageParentBlend(base + 0.999f), 0.99f);
    }
}

TEST(PageLevelRule, PinnedLevelsAreTheCoarsestWithinAQuarterOfTheSlotsAndNeverFinerThanSixteenMeters)
{
    // The 60 x 15 km terrain at 0.25 m: levels 11 to 6 hold 1, 2, 4, 16, 60 and 240 pages; level 6 is
    // the first 16 m texel.
    PageRequestTerrain terrain = UserTerrain();
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(240000, 60000);
    const auto pinnedPages = [&levels](uint32 first) {
        uint32 pages = 0;
        for (uint32 level = first; level < levels.size(); ++level)
            pages += levels[level].PagesX * levels[level].PagesZ;
        return pages;
    };
    struct Case
    {
        uint32 Slots;
        uint32 FirstPinned;
        uint32 Pages;
    };
    // Desktop: 83 pages at a 32 m floor (the design's number), not every 16 m level's 323. The Deck:
    // 23 at 64 m. 2,048 slots: every 16 m level. 8,192 slots: still no finer than 16 m, though the 8 m
    // level's 885 more pages (1,208 in all) would fit the cap of 2,048.
    for (const Case& c :
         {Case{512u, 7u, 83u}, Case{256u, 8u, 23u}, Case{2048u, 6u, 323u}, Case{8192u, 6u, 323u}})
    {
        terrain.CacheSlots = c.Slots;
        const uint32 first = FirstPinnedPageLevel(terrain, levels);
        EXPECT_EQ(first, c.FirstPinned) << c.Slots << " slots";
        EXPECT_EQ(pinnedPages(first), c.Pages) << c.Slots << " slots";
    }

    // With no slots to spend the top level alone is pinned, so a fallback walk still ends on a page.
    PageRequestTerrain small = terrain;
    small.SamplesX = small.SamplesZ = 300;
    small.CacheSlots = 0;
    EXPECT_EQ(FirstPinnedPageLevel(small, PageStreaming::BuildPageStoreLevels(300, 300)), 2u);
}

TEST(PageLevelRule, TheFarFieldIsRequestedByTheLevelRuleBelowThePinnedFloor)
{
    // Under the slot cap the pinned floor is 32 m; the 16 m level and every finer one the camera's
    // distance needs come from the walk. At each point of the terrain the page of the level its
    // distance needs (or the pinned floor, when that is finer) is wanted.
    PageRequestTerrain terrain = UserTerrain();
    terrain.CacheSlots = kDesktopHeightPages.Slots;
    HeightPageRequester requester;
    requester.Configure(terrain);
    ASSERT_EQ(requester.FirstPinnedLevel(), 7u);
    const PageLevelView view{779.0f, 9.17f};
    const Mathematics::Vector3 camera{21000.0f, 330.0f, 7300.0f};
    std::vector<PageStreaming::PageWant> wants;
    requester.Build({&camera, 1}, view, wants);
    std::set<std::tuple<uint32, uint32, uint32>> wanted;
    for (const PageStreaming::PageWant& want : wants)
        wanted.insert(Key(want.Address));

    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(240000, 60000);
    uint32 checked = 0;
    for (float32 x = 10.0f; x < 60000.0f; x += 250.0f)
        for (float32 z = 10.0f; z < 15000.0f; z += 250.0f)
        {
            const float32 dy = std::max({terrain.MinHeight - camera.y, 0.0f, camera.y - terrain.MaxHeight});
            const float32 distance = std::sqrt((x - camera.x) * (x - camera.x) + dy * dy + (z - camera.z) * (z - camera.z));
            const float32 lambda = GeometryPageLevel(distance, 0.25f, view);
            const uint32 level = std::min(uint32(std::max(0.0f, std::floor(lambda))), requester.FirstPinnedLevel());
            const uint32 span = PageStreaming::kPageOwnedSamples << level;
            const PageAddress page{0, uint8(level), std::min(uint32(x / 0.25f) / span, levels[level].PagesX - 1u),
                                   std::min(uint32(z / 0.25f) / span, levels[level].PagesZ - 1u)};
            EXPECT_TRUE(wanted.count(Key(page))) << "(" << x << ", " << z << ") needs L" << level << "(" << page.X
                                                 << "," << page.Z << ")";
            ++checked;
        }
    EXPECT_GT(checked, 10000u);
}

// The owner's card 93, option B: a small pyramid is pinned whole, the cap a quarter of the
// profile's height cache. A 1 km terrain at 1, 2 and 4 samples per meter has 85, 341 and 1,365 pages.
TEST(PageLevelRule, ASmallPyramidIsPinnedWholeWithinAQuarterOfTheCache)
{
    PageRequestTerrain terrain;
    terrain.MaxHeight = 100.0f;
    const auto pagesOf = [](uint32 samples) {
        const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(samples, samples);
        return static_cast<uint32>(PageStreaming::DensePageCount(levels));
    };
    EXPECT_EQ(pagesOf(1001), 85u);
    EXPECT_EQ(pagesOf(2001), 341u);
    EXPECT_EQ(pagesOf(4001), 1365u);
    struct Case
    {
        uint32 Samples;
        float32 Texel;
        uint32 Slots;
        bool Whole;
    };
    // Desktop 512 height slots, Steam Deck 256, web 128, high detail 2,048.
    for (const Case& c : {Case{1001, 1.0f, 512, true}, Case{1001, 1.0f, 256, false}, Case{1001, 1.0f, 128, false},
                          Case{2001, 0.5f, 512, false}, Case{2001, 0.5f, 2048, true}, Case{4001, 0.25f, 2048, false}})
    {
        terrain.SamplesX = terrain.SamplesZ = c.Samples;
        terrain.Level0TexelX = terrain.Level0TexelZ = c.Texel;
        terrain.CacheSlots = c.Slots;
        HeightPageRequester requester;
        requester.Configure(terrain);
        EXPECT_EQ(requester.FirstPinnedLevel() == 0u, c.Whole) << c.Samples << " samples, " << c.Slots << " slots";

        // Whole: every page is requested and pinned, with the camera far away. Not whole: only the
        // texel rule's levels are pinned.
        const Mathematics::Vector3 far{1.0e6f, 1.0e6f, 1.0e6f};
        std::vector<PageStreaming::PageWant> wants;
        requester.Build(std::span(&far, 1), NoteView(), wants);
        const bool everyPagePinned =
            wants.size() == pagesOf(c.Samples) &&
            std::all_of(wants.begin(), wants.end(), [](const PageStreaming::PageWant& w) { return w.Pinned; });
        EXPECT_EQ(everyPagePinned, c.Whole) << c.Samples << " samples, " << c.Slots << " slots";
        // Not whole: the texel rule's levels, or the coarsest when no level reaches 16 m texels.
        const uint32 top = uint32(PageStreaming::BuildPageStoreLevels(c.Samples, c.Samples).size()) - 1u;
        if (!c.Whole)
        {
            for (const PageStreaming::PageWant& want : wants)
                EXPECT_TRUE(c.Texel * float32(1u << want.Address.Level) >= kPinnedPageTexelMeters ||
                            want.Address.Level == top);
        }
    }
}

TEST(PageLevelRule, RequestsAreClosedUnderParentsAndFineOnlyNearTheCamera)
{
    const PageRequestTerrain terrain = UserTerrain();
    const PageLevelView view = NoteView();
    const Mathematics::Vector3 eye{30000.0f, 289.0f, 7500.0f}; // 1.7 m above the ridge
    std::vector<PageStreaming::PageWant> requests;
    HeightPageRequester requester;
    requester.Configure(terrain);
    requester.Build(std::span(&eye, 1), view, requests);

    std::set<std::tuple<uint32, uint32, uint32>> seen;
    std::map<std::tuple<uint32, uint32, uint32>, float32> distance;
    for (const PageStreaming::PageWant& request : requests)
    {
        EXPECT_TRUE(seen.insert(Key(request.Address)).second) << "a page requested twice";
        distance[Key(request.Address)] = request.Distance;
    }
    const uint32 top = static_cast<uint32>(PageStreaming::BuildPageStoreLevels(240000, 60000).size()) - 1u;
    uint32 level0 = 0;
    for (const PageStreaming::PageWant& request : requests)
    {
        if (request.Address.Level < top)
        {
            EXPECT_TRUE(seen.count(Key(request.Address.Parent())))
                << "level " << int(request.Address.Level) << " page without its parent";
        }
        if (request.Address.Level == 0)
        {
            ++level0;
            // Level-0 pages come as the four children of a level-1 page some point of which needs
            // finer than level 1.
            EXPECT_LT(GeometryPageLevel(distance[Key(request.Address.Parent())], 0.25f, view), 1.0f);
        }
    }
    // The eye pose needs a handful of level-0 pages around the camera, not the terrain's 879,375.
    EXPECT_GE(level0, 4u);
    EXPECT_LE(level0, 16u);
    EXPECT_LT(requests.size(), 2000u);
}

TEST(PageLevelRule, AParkedCameraRequestsTheSameSetEveryFrame)
{
    const PageRequestTerrain terrain = UserTerrain();
    const Mathematics::Vector3 eye{12000.0f, 800.0f, 3000.0f};
    std::vector<PageStreaming::PageWant> first;
    std::vector<PageStreaming::PageWant> second;
    HeightPageRequester requester;
    requester.Configure(terrain);
    requester.Build(std::span(&eye, 1), NoteView(), first);
    requester.Build(std::span(&eye, 1), NoteView(), second);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i)
        EXPECT_EQ(first[i].Address, second[i].Address);
}

// ---- The page cache and the paged resolve -------------------------------------------------------

namespace
{

// Level L's lattice sampled bilinearly at terrain UV, from the field function directly: what the
// paged resolve must reproduce at a resident, settled level.
float32 DirectLevelSample(uint32 level, float32 u, float32 v)
{
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(kFieldX, kFieldZ);
    const float32 cx = u * float32(kFieldX - 1) / float32(1u << level);
    const float32 cz = v * float32(kFieldZ - 1) / float32(1u << level);
    const float32 x0 = std::floor(cx);
    const float32 z0 = std::floor(cz);
    const auto at = [&](float32 j, float32 k) {
        j = std::min(j, float32(levels[level].SamplesX - 1));
        k = std::min(k, float32(levels[level].SamplesZ - 1));
        return FieldHeight(j * float32(1u << level), k * float32(1u << level));
    };
    const float32 fx = cx - x0;
    const float32 fz = cz - z0;
    const float32 a = at(x0, z0) + (at(x0 + 1, z0) - at(x0, z0)) * fx;
    const float32 b = at(x0, z0 + 1) + (at(x0 + 1, z0 + 1) - at(x0, z0 + 1)) * fx;
    return a + (b - a) * fz;
}

} // namespace

TEST(PagedHeightResolve, AResidentSettledLevelIsItsLatticeBilinearly)
{
    PagedField field(64);
    const std::vector<PageAddress> all = field.AllPages();
    for (int frame = 0; frame < 40; ++frame)
        field.Step(all);
    ASSERT_EQ(field.Cache.ResidentCount(), all.size());
    const PagedHeightSampler sampler = field.Sampler();
    for (uint32 level = 0; level <= field.TopLevel(); ++level)
        for (float32 u : {0.0f, 0.13f, 0.2133f, 0.5f, 0.77f, 1.0f})
            for (float32 v : {0.0f, 0.31f, 0.64f, 0.999f, 1.0f})
                EXPECT_NEAR(sampler.Sample(u, v, float32(level)), DirectLevelSample(level, u, v), 2e-4f)
                    << "level " << level << " uv " << u << "," << v;
}

TEST(PagedHeightResolve, AMissingPageResolvesToItsFirstResidentAncestorAlone)
{
    PagedField field(64);
    std::vector<PageAddress> wanted;
    for (const PageAddress& page : field.AllPages())
    {
        const bool missingLevel0 = page.Level == 0 && page.X == 1 && page.Z == 1;
        const bool missingLevel1 = page.Level == 1 && page.X == 0 && page.Z == 0;
        if (!missingLevel0 && !missingLevel1)
            wanted.push_back(page);
    }
    for (int frame = 0; frame < 40; ++frame)
        field.Step(wanted);
    // Parent first: level-0 (0, 0) is wanted but its parent is not resident, so it stays out too.
    EXPECT_EQ(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 0, 0})), PageStreaming::kNoResidentSlot);
    const PagedHeightSampler sampler = field.Sampler();
    // Inside level-0 page (1, 1) and level-1 page (0, 0): level 2 answers alone.
    // Off every coarser lattice, so each level of the walk answers differently.
    const float32 u = 203.0f / float32(kFieldX - 1);
    const float32 v = 203.0f / float32(kFieldZ - 1);
    ASSERT_GT(std::fabs(DirectLevelSample(2, u, v) - DirectLevelSample(3, u, v)), 0.01f);
    uint32 resolved = 0;
    const float32 ancestor = sampler.SampleLevel(u, v, 2, resolved);
    EXPECT_EQ(resolved, 2u);
    EXPECT_EQ(sampler.Sample(u, v, 0.0f), ancestor);
    EXPECT_NEAR(ancestor, DirectLevelSample(2, u, v), 2e-4f);
    // Inside a resident level-0 page the level-0 data answers.
    const float32 u2 = 520.0f / float32(kFieldX - 1);
    EXPECT_NEAR(sampler.Sample(u2, v, 0.0f), DirectLevelSample(0, u2, v), 2e-4f);
}

TEST(PagedHeightResolve, HeightIsContinuousAcrossEveryLevelRing)
{
    PagedField field(64);
    const std::vector<PageAddress> all = field.AllPages();
    for (int frame = 0; frame < 40; ++frame)
        field.Step(all);
    const PagedHeightSampler sampler = field.Sampler();
    const float32 v = 0.61f;
    for (float32 u : {0.137f, 0.4f, 0.9f})
    {
        for (uint32 ring = 1; ring <= field.TopLevel(); ++ring)
        {
            // Just inside a ring's end and at the next level's start: the same height, the parent's.
            const float32 before = sampler.Sample(u, v, float32(ring) - 1e-4f);
            const float32 after = sampler.Sample(u, v, float32(ring));
            EXPECT_NEAR(before, after, 1e-2f) << "a pop at ring " << ring << " (u " << u << ")";
        }
    }
}

TEST(PagedHeightResolve, APageArrivingFadesInFromItsParentWithoutAPop)
{
    PagedField field(64);
    std::vector<PageAddress> withoutPage;
    for (const PageAddress& page : field.AllPages())
        if (!(page.Level == 0 && page.X == 2 && page.Z == 1))
            withoutPage.push_back(page);
    for (int frame = 0; frame < 40; ++frame)
        field.Step(withoutPage);
    const PagedHeightSampler sampler = field.Sampler();
    // An odd level-0 sample: not on the parent's lattice, so the two levels differ there.
    const float32 u = 301.0f / float32(kFieldX - 1);
    const float32 v = 191.0f / float32(kFieldZ - 1);
    ASSERT_GT(std::fabs(DirectLevelSample(0, u, v) - DirectLevelSample(1, u, v)), 0.01f);
    const float32 before = sampler.Sample(u, v, 0.0f);
    field.Step(field.AllPages()); // the page arrives
    EXPECT_NE(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 2, 1})), PageStreaming::kNoResidentSlot);
    EXPECT_EQ(sampler.Sample(u, v, 0.0f), before) << "the arrival frame must show the parent";
    for (int frame = 0; frame < 30; ++frame)
        field.Step(field.AllPages());
    EXPECT_NEAR(sampler.Sample(u, v, 0.0f), DirectLevelSample(0, u, v), 2e-4f);
}

TEST(PageCacheResidency, AParkedCameraSettlesToZeroWork)
{
    PagedField field(64);
    const std::vector<PageAddress> all = field.AllPages();
    field.Step(all);
    EXPECT_GT(field.Cache.AssignsThisFrame(), 0u);
    for (int frame = 0; frame < 40; ++frame) // past the 0.4 s fades
        field.Step(all);
    for (int frame = 0; frame < 10; ++frame)
    {
        field.Step(all);
        EXPECT_EQ(field.Cache.AssignsThisFrame(), 0u);
        EXPECT_EQ(field.Cache.ReleasesThisFrame(), 0u);
        EXPECT_TRUE(field.Cache.Transitions().empty());
        EXPECT_EQ(field.Cache.ActiveFadesThisFrame(), 0u);
        EXPECT_EQ(field.LastTableWrites, 0u);
    }
}

TEST(PageCacheResidency, PagesArriveParentFirstAndAFullCacheDegradesByLevel)
{
    PagedField field(5); // the top two levels' 3 pages and two more
    const std::vector<PageAddress> all = field.AllPages();
    field.Step(all, 1.0f / 60.0f, 1); // one assign a frame: the top page first
    EXPECT_NE(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 3, 0, 0})), PageStreaming::kNoResidentSlot);
    for (int frame = 0; frame < 20; ++frame)
        field.Step(all);
    EXPECT_EQ(field.Cache.ResidentCount(), 5u);
    for (const PageStreaming::CachedPage& page : field.Cache.Resident())
        if (page.Address.Level < field.TopLevel())
        {
            EXPECT_NE(field.Cache.SlotOf(page.Parent()), PageStreaming::kNoResidentSlot) << "a page without its parent";
        }
    for (const PageAddress& page : all)
        if (page.Level >= 2)
        {
            EXPECT_NE(field.Cache.SlotOf(PagedField::Page(page)), PageStreaming::kNoResidentSlot) << "a coarse page left out";
        }
    EXPECT_GT(field.Cache.DeferredThisFrame(), 0u);
}

// ---- Review fold (PR #3072): parent first on release, hysteresis, pressure, page corners ---------------

TEST(PageCacheResidency, AParentIsNeverReleasedUnderAResidentChildPastTheHysteresis)
{
    PagedField field(64);
    const std::vector<PageAddress> all = field.AllPages();
    for (int frame = 0; frame < 40; ++frame)
        field.Step(all);
    std::vector<PageAddress> withoutParent;
    for (const PageAddress& page : all)
        if (!(page.Level == 1 && page.X == 0 && page.Z == 0))
            withoutParent.push_back(page);
    // Past the release hysteresis, so only the resident-child rule keeps the parent.
    for (int frame = 0; frame < 2 * int(PageStreaming::kPageReleaseHysteresisFrames); ++frame)
        field.Step(withoutParent);
    const bool childResident = field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 0, 0})) != PageStreaming::kNoResidentSlot;
    const bool parentResident = field.Cache.SlotOf(PagedField::Page(PageAddress{0, 1, 0, 0})) != PageStreaming::kNoResidentSlot;
    ASSERT_TRUE(childResident) << "the test needs the child resident";
    EXPECT_TRUE(parentResident) << "the cache released level-1 (0,0) under its resident child level-0 (0,0)";
}

TEST(PagedHeightResolve, PageCornersResolveToTheLattice)
{
    PagedField field(64);
    const std::vector<PageAddress> all = field.AllPages();
    for (int frame = 0; frame < 40; ++frame)
        field.Step(all);
    const PagedHeightSampler sampler = field.Sampler();
    for (uint32 level = 0; level < 2; ++level)
        for (float32 c : {127.0f, 127.5f, 127.99f, 128.0f, 128.5f, 255.5f, 256.0f, 256.5f})
        {
            const float32 u = c * float32(1u << level) / float32(kFieldX - 1);
            const float32 v = c * float32(1u << level) / float32(kFieldZ - 1);
            if (u > 1.0f || v > 1.0f)
                continue;
            EXPECT_NEAR(sampler.Sample(u, v, float32(level)), DirectLevelSample(level, u, v), 2e-4f)
                << "level " << level << " lattice corner " << c;
        }
}

TEST(PageCacheResidency, AFullCacheGivesANearerWantedPageASlotOverAFartherOne)
{
    PageStreaming::PageCache cache;
    cache.Configure(5, 2, 0.0f);
    constexpr uint32 kTop = 3;
    const auto want = [](uint8 level, uint32 x, uint32 z, float32 distance) {
        return PageStreaming::PageWant{PageAddress{0, level, x, z}, distance, false, 0u, level >= kTop};
    };
    std::vector<PageStreaming::PageWant> wants = {want(3, 0, 0, 0.0f), want(2, 0, 0, 0.0f), want(2, 1, 0, 0.0f),
                                                  want(1, 0, 0, 900.0f), want(1, 1, 0, 1000.0f)};
    uint64 frame = 0;
    for (int i = 0; i < 5; ++i)
        cache.Update(++frame, 1.0f / 60.0f, wants, 64);
    ASSERT_EQ(cache.ResidentCount(), 5u);
    wants.push_back(want(1, 2, 0, 1.0f)); // the camera arrives beside level-1 (2,0); its parent is resident
    for (int i = 0; i < 10; ++i)
        cache.Update(++frame, 1.0f / 60.0f, wants, 64);
    EXPECT_NE(cache.SlotOf(PageStreaming::CachedPage{0u, PageAddress{0, 1, 2, 0}}), PageStreaming::kNoResidentSlot)
        << "the page beside the camera stays without a slot while pages 900 and 1000 m away keep theirs";
    EXPECT_EQ(cache.SlotOf(PageStreaming::CachedPage{0u, PageAddress{0, 1, 1, 0}}), PageStreaming::kNoResidentSlot)
        << "the farthest settled page is the one that gives its slot up";
}

TEST(PageCacheResidency, ACameraHoveringAtARingBoundaryDoesNotChurnTheCache)
{
    const PageRequestTerrain terrain = UserTerrain();
    const PageLevelView view = NoteView();
    // Directly above the field's top at the height where level-0 pages stop being wanted.
    const float32 boundary = 2.0f * view.FocalPixels * terrain.Level0TexelX / view.TargetPixels;
    const uint32 top = static_cast<uint32>(PageStreaming::BuildPageStoreLevels(240000, 60000).size()) - 1u;
    PageStreaming::PageCache cache;
    cache.Configure(4096, 2, 0.4f);
    std::vector<PageStreaming::PageWant> wants;
    HeightPageRequester requester;
    requester.Configure(terrain);
    uint64 frame = 0;
    const auto step = [&](float32 height) {
        const Mathematics::Vector3 eye{30000.0f, terrain.MaxHeight + height, 7500.0f};
        requester.Build(std::span(&eye, 1), view, wants);
        for (PageStreaming::PageWant& want : wants)
            want.Top = want.Address.Level >= top;
        cache.Update(++frame, 1.0f / 60.0f, wants, 4096);
    };
    for (int i = 0; i < 60; ++i)
        step(boundary - 0.05f);
    uint32 releases = 0;
    uint32 assigns = 0;
    for (int i = 0; i < 20; ++i)
    {
        step(i % 2 == 0 ? boundary + 0.05f : boundary - 0.05f); // 10 cm of camera bob
        releases += cache.ReleasesThisFrame();
        assigns += cache.AssignsThisFrame();
    }
    EXPECT_EQ(releases, 0u) << "assigns " << assigns << " over 20 frames of a 10 cm bob at " << boundary << " m";

    // Leaving for good, the pages do go once the hysteresis has run out.
    for (int i = 0; i < 2 * int(PageStreaming::kPageReleaseHysteresisFrames); ++i)
        step(4.0f * boundary);
    EXPECT_EQ(cache.SlotOf(PageStreaming::CachedPage{0u, PageAddress{0, 0, 30000u * 4u / 128u, 7500u * 4u / 128u}}),
              PageStreaming::kNoResidentSlot);
}

TEST(PageCacheResidency, PressureNeverEvictsAPageWithAResidentChild)
{
    PageStreaming::PageCache cache;
    cache.Configure(4, 2, 0.0f);
    constexpr uint32 kTop = 3;
    const auto want = [](uint8 level, uint32 x, uint32 z, float32 distance) {
        return PageStreaming::PageWant{PageAddress{0, level, x, z}, distance, false, 0u, level >= kTop};
    };
    // A parent farther than its child (an unwanted parent kept by its child counts as infinitely far).
    std::vector<PageStreaming::PageWant> wants = {want(3, 0, 0, 0.0f), want(3, 1, 0, 0.0f), want(2, 0, 0, 6000.0f),
                                                  want(1, 0, 0, 5000.0f)};
    uint64 frame = 0;
    for (int i = 0; i < 5; ++i)
        cache.Update(++frame, 1.0f / 60.0f, wants, 64);
    ASSERT_EQ(cache.ResidentCount(), 4u);
    wants.push_back(want(2, 2, 0, 1.0f)); // parent level-3 (1,0) is resident
    for (int i = 0; i < 10; ++i)
    {
        cache.Update(++frame, 1.0f / 60.0f, wants, 64);
        for (const PageStreaming::CachedPage& page : cache.Resident())
            if (page.Address.Level < kTop)
            {
                EXPECT_NE(cache.SlotOf(page.Parent()), PageStreaming::kNoResidentSlot)
                    << "level " << int(page.Address.Level) << " (" << page.Address.X << "," << page.Address.Z
                    << ") without its parent";
            }
    }
}

// The shipped GLSL resolve (cbt_page.glsl, between its GE_SHARED_PAGE markers), compiled as C++
// over the same table and texels as the mirror: the taps the consumer defines read them directly.
namespace PageResolveGlsl
{
// Declarations, not a using-directive: they hide <cmath>'s global floor and friends.
using GameEngine::GlslShim::clamp;
using GameEngine::GlslShim::floor;
using GameEngine::GlslShim::min;
using GameEngine::GlslShim::uint;

struct Bound
{
    const PageTable* Table = nullptr;
    const float32* Texels = nullptr;
    uint32 Dim = 0;
};
Bound s_Bound;

#include "PageResolveTypesExtracted.h"

// A read past the words answers a resident-looking entry, so a walk that strays shows as a wrong
// height, never as a crash.
uint CBT_PageTableWord(uint index)
{
    const std::span<const uint32> words = s_Bound.Table->Words();
    return index < words.size() ? words[index] : 0u;
}

float CBT_PageSlotFade(uint slot)
{
    return s_Bound.Table->SlotFades()[slot];
}

CBTPageLevel CBT_PageLevelShape(uint level)
{
    const PageStreaming::PageStoreLevel& shape = s_Bound.Table->Levels()[level];
    return CBTPageLevel{s_Bound.Table->LevelOffsets()[level], shape.PagesX, shape.PagesZ, 0u};
}

float CBT_PageCacheBilinear(float texelX, float texelZ)
{
    return SampleGridBilinearTexel(s_Bound.Texels, s_Bound.Dim, s_Bound.Dim, texelX, texelZ);
}

#include "PageResolveExtracted.h"

float32 Sample(const PagedField& field, float32 u, float32 v, float32 level)
{
    s_Bound = Bound{&field.Table, field.Texels.data(), field.Geometry.CacheDim};
    CBTPageField shape{};
    shape.Level0SamplesX = field.Table.Levels().front().SamplesX;
    shape.Level0SamplesZ = field.Table.Levels().front().SamplesZ;
    shape.LevelCount = uint32(field.Table.Levels().size());
    shape.SlotsPerRow = field.Geometry.SlotsPerRow;
    shape.FloorLevel = field.Table.FloorLevel();
    shape.TierCount = field.Table.TierCount();
    shape.DirectoryOffset = field.Table.DirectoryOffset();
    return CBT_PageSample(u, v, level, shape);
}

uint32 Entry(const PagedField& field, uint32 level, uint32 pageX, uint32 pageZ)
{
    s_Bound = Bound{&field.Table, field.Texels.data(), field.Geometry.CacheDim};
    CBTPageField shape{};
    shape.LevelCount = uint32(field.Table.Levels().size());
    shape.FloorLevel = field.Table.FloorLevel();
    shape.TierCount = field.Table.TierCount();
    shape.DirectoryOffset = field.Table.DirectoryOffset();
    return CBT_PageTableEntry(level, pageX, pageZ, shape);
}

} // namespace PageResolveGlsl

TEST(PagedHeightResolve, TheGlslResolveIsTheMirrorBitForBit)
{
    // Every state the walk can meet: a missing level-0 page and a missing level-1 page (the
    // fallback walk), and a page and its parent both mid-fade (the fade chain).
    PagedField field(64);
    std::vector<PageAddress> partial;
    for (const PageAddress& page : field.AllPages())
    {
        const bool held = (page.Level == 0 && page.X == 1 && page.Z == 1) ||
                          (page.Level == 1 && page.X == 0 && page.Z == 0) ||
                          (page.Level == 0 && page.X == 3 && page.Z == 2) ||
                          (page.Level == 1 && page.X == 1 && page.Z == 1);
        if (!held)
            partial.push_back(page);
    }
    for (int frame = 0; frame < 40; ++frame)
        field.Step(partial);
    std::vector<PageAddress> arriving = partial;
    arriving.push_back(PageAddress{0, 1, 1, 1});
    field.Step(arriving, 0.1f); // level-1 (1, 1) arrives and fades
    arriving.push_back(PageAddress{0, 0, 3, 2});
    field.Step(arriving, 0.1f); // its child arrives under it while it still fades
    ASSERT_NE(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 3, 2})), PageStreaming::kNoResidentSlot);
    ASSERT_LT(field.Cache.FadeOf(PagedField::Page(PageAddress{0, 1, 1, 1})), 1.0f);

    const PagedHeightSampler mirror = field.Sampler();
    uint32 compared = 0;
    for (float32 level : {0.0f, 0.3f, 0.8f, 0.9375f, 1.0f, 1.5f, 1.9f, 2.6f, 40.0f})
        for (uint32 i = 0; i <= 64; ++i)
            for (uint32 k = 0; k <= 48; ++k)
            {
                const float32 u = float32(i) / 64.0f;
                const float32 v = float32(k) / 48.0f;
                ASSERT_EQ(PageResolveGlsl::Sample(field, u, v, level), mirror.Sample(u, v, level))
                    << "uv " << u << "," << v << " level " << level;
                ++compared;
            }
    EXPECT_EQ(compared, 9u * 65u * 49u);
}

namespace
{

// 16,385 x 1,025 samples: nine levels. A dense budget of 4 words keeps only the top level dense, so
// the blocks take two tiers: six levels from the mid pool, then the last two from the leaf pool.
constexpr uint32 kTieredX = 16385u;
constexpr uint32 kTieredZ = 1025u;
constexpr uint32 kTieredSlots = 96u;
constexpr PageTableBudget kTwoTiers{4u, kTieredSlots, kTieredSlots};

std::vector<PageAddress> ResidentOf(const PageTable& table)
{
    std::vector<PageAddress> pages;
    table.AppendResident(pages);
    std::sort(pages.begin(), pages.end());
    return pages;
}

} // namespace

TEST(PagedHeightResolve, TheTieredTableResolvesAsTheDenseTableOnRandomSets)
{
    PagedField dense(kTieredSlots, 0.4f, kTieredX, kTieredZ);
    PagedField tiered(kTieredSlots, 0.4f, kTieredX, kTieredZ, kTwoTiers);
    // Levels 8 to 5 dense (five floor pages), one tier of blocks below, and a pool far smaller than
    // the cache: pages wait, blocks are taken again and again, and a floor page often has none.
    PagedField crowded(kTieredSlots, 0.4f, kTieredX, kTieredZ, PageTableBudget{64u, 3u, 3u});
    // Levels 8 and 7 dense (two floor pages), two tiers below, and one mid block: the second floor page
    // waits, so its directory word stays empty while pages under it are wanted.
    PagedField crowdedTwoTiers(kTieredSlots, 0.4f, kTieredX, kTieredZ, PageTableBudget{12u, 1u, 2u});
    ASSERT_EQ(tiered.Table.Levels().size(), 9u);
    ASSERT_EQ(tiered.Table.FloorLevel(), 8u);
    ASSERT_EQ(tiered.Table.TierCount(), 2u);
    ASSERT_EQ(crowded.Table.FloorLevel(), 5u);
    ASSERT_EQ(crowded.Table.TierCount(), 1u);
    ASSERT_EQ(crowdedTwoTiers.Table.FloorLevel(), 7u);
    ASSERT_EQ(crowdedTwoTiers.Table.TierCount(), 2u);
    const std::vector<PageAddress> all = dense.AllPages();
    std::mt19937 random(4242u);
    uint32 resident = 0;
    uint32 waited = 0;
    uint32 waitedTwoTiers = 0;
    for (int frame = 0; frame < 60; ++frame)
    {
        // A random set each frame, larger than the cache now and then, so pages arrive, stay and leave
        // and blocks are taken, freed and taken again under another root.
        // Each with its ancestors: the cache takes a page only under a resident parent.
        std::vector<PageAddress> wanted;
        const uint32 count = 4u + random() % 24u;
        for (uint32 i = 0; i < count; ++i)
            for (PageAddress page = all[random() % all.size()]; page.Level < tiered.TopLevel(); page = page.Parent())
                wanted.push_back(page);
        wanted.push_back(PageAddress{0, uint8(tiered.TopLevel()), 0, 0});
        dense.Step(wanted, 0.1f);
        tiered.Step(wanted, 0.1f);
        crowded.Step(wanted, 0.1f);
        crowdedTwoTiers.Step(wanted, 0.1f);
        for (const PageAddress& page : all)
        {
            const uint32 expected = dense.Table.Entry(page.Level, page.X, page.Z);
            ASSERT_EQ(tiered.Table.Entry(page.Level, page.X, page.Z), expected)
                << "frame " << frame << " L" << int(page.Level) << "(" << page.X << "," << page.Z << ")";
            ASSERT_EQ(PageResolveGlsl::Entry(tiered, page.Level, page.X, page.Z), expected)
                << "the GLSL walk, frame " << frame << " L" << int(page.Level) << "(" << page.X << "," << page.Z << ")";
            resident += expected != PageStreaming::kNoPage ? 1u : 0u;
            ASSERT_EQ(PageResolveGlsl::Entry(crowded, page.Level, page.X, page.Z),
                      crowded.Table.Entry(page.Level, page.X, page.Z))
                << "the GLSL walk over crowded pools, frame " << frame << " L" << int(page.Level) << "(" << page.X
                << "," << page.Z << ")";
            const uint32 crowdedEntry = crowded.Table.Entry(page.Level, page.X, page.Z);
            ASSERT_TRUE(crowdedEntry == PageStreaming::kNoPage || crowdedEntry == expected)
                << "a crowded table holds fewer pages, never another entry";
            ASSERT_EQ(PageResolveGlsl::Entry(crowdedTwoTiers, page.Level, page.X, page.Z),
                      crowdedTwoTiers.Table.Entry(page.Level, page.X, page.Z))
                << "the GLSL walk over two crowded tiers, frame " << frame << " L" << int(page.Level) << "("
                << page.X << "," << page.Z << ")";
            // A page resident in the cache whose table entry reads kNoPage waits for a block.
            const bool cached = expected != PageStreaming::kNoPage;
            waited += cached && crowdedEntry == PageStreaming::kNoPage ? 1u : 0u;
            waitedTwoTiers +=
                cached && crowdedTwoTiers.Table.Entry(page.Level, page.X, page.Z) == PageStreaming::kNoPage ? 1u : 0u;
        }
        ASSERT_EQ(ResidentOf(tiered.Table), ResidentOf(dense.Table)) << "frame " << frame;
    }
    EXPECT_GT(resident, 1000u) << "the sets left too few pages resident to compare";
    EXPECT_GT(waited, 0u) << "the crowded pools never filled";
    EXPECT_GT(waitedTwoTiers, 0u) << "the crowded two-tier pools never filled";

    const PagedHeightSampler mirror = dense.Sampler();
    const PagedHeightSampler tieredMirror = tiered.Sampler();
    for (float32 level : {0.0f, 0.9f, 2.5f, 5.95f, 7.0f})
        for (uint32 i = 0; i <= 256; ++i)
            for (uint32 k = 0; k <= 16; ++k)
            {
                const float32 u = float32(i) / 256.0f;
                const float32 v = float32(k) / 16.0f;
                const float32 expected = mirror.Sample(u, v, level);
                ASSERT_EQ(tieredMirror.Sample(u, v, level), expected) << "uv " << u << "," << v << " level " << level;
                ASSERT_EQ(PageResolveGlsl::Sample(tiered, u, v, level), expected)
                    << "uv " << u << "," << v << " level " << level;
            }
}

TEST(PagedHeightResolve, APageWhoseBlockPoolIsFullWaitsForABlockThenTakesItsEntry)
{
    PageTable table;
    table.Reset(kTieredX, kTieredZ, 8u, PageTableBudget{4u, 1u, 1u});
    const uint32 first = PageStreaming::PackPageEntry(1, 0);
    const uint32 second = PageStreaming::PackPageEntry(2, 0);
    table.SetEntry(0, 0, 0, first);
    table.SetEntry(0, 100, 5, second); // another level-1 root: the one leaf block is taken
    EXPECT_EQ(table.Entry(0, 0, 0), first);
    EXPECT_EQ(table.Entry(0, 100, 5), PageStreaming::kNoPage) << "it reads its ancestor while it waits";
    std::vector<PageAddress> late;
    table.TakeLateEntries(late);
    EXPECT_TRUE(late.empty()) << "a waiting page has no late entry yet";

    table.SetEntry(0, 0, 0, PageStreaming::kNoPage);
    EXPECT_EQ(table.Entry(0, 100, 5), second) << "the freed block takes the waiting page";
    table.TakeLateEntries(late);
    ASSERT_EQ(late.size(), 1u);
    EXPECT_EQ(late.front(), (PageAddress{0, 0, 100, 5}));

    table.SetEntry(0, 100, 5, PageStreaming::kNoPage);
    EXPECT_TRUE(ResidentOf(table).empty()) << "a block with nothing resident under it is freed";
}

namespace
{

// The needed-plus-parents model (design page-streaming-planets 3.5): a page is needed where its own
// nearest point needs exactly its level, or when it is pinned; then every ancestor of a needed page.
std::size_t NeededPlusParents(const PageRequestTerrain& terrain, uint32 firstPinned, const Mathematics::Vector3& camera,
                              const PageLevelView& view)
{
    const std::vector<PageStreaming::PageStoreLevel> levels =
        PageStreaming::BuildPageStoreLevels(terrain.SamplesX, terrain.SamplesZ);
    const uint32 top = uint32(levels.size()) - 1u;
    std::set<std::tuple<uint32, uint32, uint32>> needed;
    for (uint32 level = 0; level <= top; ++level)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
            {
                const float32 span = float32(PageStreaming::kPageOwnedSamples << level) * terrain.Level0TexelX;
                const float32 dx = std::max({float32(x) * span - camera.x, 0.0f, camera.x - float32(x + 1u) * span});
                const float32 dz = std::max({float32(z) * span - camera.z, 0.0f, camera.z - float32(z + 1u) * span});
                const float32 dy = std::max({terrain.MinHeight - camera.y, 0.0f, camera.y - terrain.MaxHeight});
                const float32 lambda = GeometryPageLevel(std::sqrt(dx * dx + dy * dy + dz * dz), terrain.Level0TexelX, view);
                const uint32 wants = std::min(uint32(std::max(0.0f, std::floor(lambda))), top);
                if (level < firstPinned && wants != level)
                    continue;
                for (PageAddress page{0, uint8(level), x, z}; page.Level <= top; page = page.Parent())
                    needed.insert(Key(page));
            }
    return needed.size();
}

} // namespace

TEST(PageLevelRule, TheRequestAtTheThreeDemPosesIsTheNeededPagesWithTheirParents)
{
    // The DEM repro project's near terrain: 8 x 2 km at 1 m (8,193 x 2,049 samples), the ridge pose
    // its highest sample, at terrain-local (3,493, 899) and 287.1 m, the eye 1.7 m above it, then 500 m
    // and 3 km up.
    // The relief's lowest point (236 m) is assumed. The measured viewport: f 779 px, t 9.17 px.
    PageRequestTerrain terrain;
    terrain.Level0TexelX = terrain.Level0TexelZ = 1.0f;
    terrain.SamplesX = 8193u;
    terrain.SamplesZ = 2049u;
    terrain.MinHeight = 236.0f;
    terrain.MaxHeight = 287.1f;
    terrain.CacheSlots = kDesktopHeightPages.Slots;
    const PageLevelView view{779.0f, 9.17f};
    HeightPageRequester requester;
    requester.Configure(terrain);
    for (float32 eye : {288.8f, 788.8f, 3288.8f})
    {
        const Mathematics::Vector3 camera{3493.0f, eye, 899.0f};
        std::vector<PageStreaming::PageWant> wants;
        requester.Build({&camera, 1}, view, wants);
        const std::size_t model = NeededPlusParents(terrain, requester.FirstPinnedLevel(), camera, view);
        EXPECT_LE(std::fabs(float64(wants.size()) - float64(model)), 0.25 * float64(model))
            << "eye " << eye << " m: requested " << wants.size() << ", needed with parents " << model;
    }
}
