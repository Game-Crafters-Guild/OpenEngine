// The paged height source of a planar terrain as pure oracles: the level rule against the design
// note's numbers, the request set's closure under parents, the page cache's residency, and the CPU
// mirror of the paged resolve (parity, the fallback walk, ring continuity, parked quiescence).

#include "CBTTerrain/CBTLayout.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/PageTable.h"
#include "TerrainECS/PagedHeightSampler.h"
#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainPages.h"

#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "PageStreaming/PageTableEntry.h"
#include "Types/FlatMap.h"

#include <gtest/gtest.h>

#include "GlslShim.h"
#include "TerrainPagedField.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using GameEngine::PageStreaming::PageAddress;
using namespace GameEngine::TerrainECS::Test;

namespace
{

// A camera at `position` that looks every way.
std::array<PageRequestCamera, 1> DiscCamera(const Mathematics::Vector3& position)
{
    return {PageRequestCamera{position, {}, 0.0f, false}};
}

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

// The surface's level rule: one texel a pixel (lambda_s, design 3.3).
PageLevelView SurfaceView()
{
    PageLevelView view = NoteView();
    view.TargetPixels = 1.0f;
    return view;
}

// The pages `area` requests for `camera` on the user's terrain at the surface's level rule.
std::set<std::tuple<uint32, uint32, uint32>> SurfaceRequests(PageRequestArea area, const PageRequestCamera& camera)
{
    PageRequester requester;
    requester.Configure(UserTerrain(), area);
    std::vector<PageStreaming::PageWant> wants;
    requester.Build(std::span(&camera, 1), SurfaceView(), wants);
    std::set<std::tuple<uint32, uint32, uint32>> keys;
    for (const PageStreaming::PageWant& want : wants)
        keys.insert(Key(want.Address));
    return keys;
}


// A surface field's slots: the pinned levels of a planar wedge request take their quarter.
constexpr uint32 kSurfaceSlots = 2048u;

// The wedge model by bearing angles (the planets design 3.5): the footprint's corner bearings meet
// forward +- (half field + margin), or the footprint is within the foot pages, or the view has no
// bearing. `margin` is the angular gap's distance to the reach: small near a boundary.
bool BearingWedgeHolds(const PageRequestTerrain& terrain, const PageAddress& address,
                       const PageRequestCamera& camera, float32& margin)
{
    constexpr float32 kPi = std::numbers::pi_v<float32>;
    margin = kPi;
    const float32 reach = camera.HorizontalHalfFov + kPageWedgeMarginRadians;
    if (!camera.HasDirection || std::hypot(camera.Forward.x, camera.Forward.z) < 1e-3f || reach >= kPi)
        return true;
    const float32 span = static_cast<float32>(PageStreaming::kPageOwnedSamples << address.Level);
    const float32 minX = terrain.OriginX + static_cast<float32>(address.X) * span * terrain.Level0TexelX;
    const float32 minZ = terrain.OriginZ + static_cast<float32>(address.Z) * span * terrain.Level0TexelZ;
    const float32 maxX = minX + span * terrain.Level0TexelX;
    const float32 maxZ = minZ + span * terrain.Level0TexelZ;
    const float32 dx = std::max({minX - camera.Position.x, 0.0f, camera.Position.x - maxX});
    const float32 dz = std::max({minZ - camera.Position.z, 0.0f, camera.Position.z - maxZ});
    if (std::hypot(dx, dz) <= kPageWedgeFootPages * std::max(maxX - minX, maxZ - minZ))
        return true;
    const auto wrap = [](float32 angle) { return std::remainder(angle, 2.0f * kPi); };
    const float32 center =
        std::atan2(0.5f * (minX + maxX) - camera.Position.x, 0.5f * (minZ + maxZ) - camera.Position.z);
    float32 low = kPi;
    float32 high = -kPi;
    for (const float32 x : {minX, maxX})
        for (const float32 z : {minZ, maxZ})
        {
            const float32 corner = wrap(std::atan2(x - camera.Position.x, z - camera.Position.z) - center);
            low = std::min(low, corner);
            high = std::max(high, corner);
        }
    const float32 forward = wrap(std::atan2(camera.Forward.x, camera.Forward.z) - center);
    const float32 gap = forward < low ? std::min(low - forward, forward + 2.0f * kPi - high)
                        : forward > high ? std::min(forward - high, low + 2.0f * kPi - forward)
                                         : 0.0f;
    margin = std::abs(gap - reach);
    return gap <= reach;
}

// The request set by the documented rule with the bearing model; false when some page's wedge test
// is within 1e-5 rad of its boundary, where float rounding may decide either way.
bool BearingModelRequests(const PageRequestTerrain& terrain, std::span<const PageRequestCamera> cameras,
                          const PageLevelView& view, uint32 cacheSlots,
                          std::set<std::tuple<uint32, uint32, uint32>>& keys)
{
    const auto levels = PageStreaming::BuildPageStoreLevels(terrain.SamplesX, terrain.SamplesZ);
    const uint32 firstPinned = FirstPinnedPageLevel(levels, 1u, terrain.Level0TexelX, cacheSlots);
    bool clear = true;
    const auto distance = [&](const PageAddress& address, bool wedge) {
        const float32 span =
            static_cast<float32>(PageStreaming::kPageOwnedSamples << address.Level) * terrain.Level0TexelX;
        const float32 minX = terrain.OriginX + static_cast<float32>(address.X) * span;
        const float32 minZ = terrain.OriginZ + static_cast<float32>(address.Z) * span;
        float32 nearest = std::numeric_limits<float32>::max();
        for (const PageRequestCamera& camera : cameras)
        {
            float32 margin = std::numbers::pi_v<float32>;
            const bool holds = !wedge || BearingWedgeHolds(terrain, address, camera, margin);
            clear = clear && margin > 1e-5f;
            if (!holds)
                continue;
            const float32 dx = std::max({minX - camera.Position.x, 0.0f, camera.Position.x - (minX + span)});
            const float32 dz = std::max({minZ - camera.Position.z, 0.0f, camera.Position.z - (minZ + span)});
            const float32 dy =
                std::max({terrain.MinHeight - camera.Position.y, 0.0f, camera.Position.y - terrain.MaxHeight});
            nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        return nearest;
    };
    std::vector<PageAddress> frontier;
    for (uint32 level = static_cast<uint32>(levels.size()); level-- > firstPinned;)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
            {
                keys.insert({level, x, z});
                if (level == firstPinned)
                    frontier.push_back(PageAddress{0, uint8(level), x, z});
            }
    while (!frontier.empty())
    {
        const PageAddress page = frontier.back();
        frontier.pop_back();
        if (page.Level == 0 || GeometryPageLevel(distance(page, true), terrain.Level0TexelX, view) >= page.Level)
            continue;
        for (uint32 child = 0; child < 4u; ++child)
        {
            const PageAddress next{0, uint8(page.Level - 1u), 2u * page.X + (child & 1u),
                                   2u * page.Z + (child >> 1u)};
            if (next.X >= levels[next.Level].PagesX || next.Z >= levels[next.Level].PagesZ ||
                GeometryPageLevel(distance(next, true), terrain.Level0TexelX, view) >= page.Level)
                continue;
            keys.insert(Key(next));
            frontier.push_back(next);
        }
    }
    return clear;
}

} // namespace

// A camera without a direction looks every way: the wedge requests the disc's pages.
TEST(PageLevelRule, AWedgeCameraWithoutADirectionRequestsTheDisc)
{
    const PageRequestCamera camera{{30000.0f, 289.0f, 7500.0f}, {1.0f, 0.0f, 0.0f}, 0.8f, false};
    EXPECT_EQ(SurfaceRequests(PageRequestArea::Wedge, camera), SurfaceRequests(PageRequestArea::Disc, camera));
}

// A camera looking along +X: the wedge keeps the level-0 pages the disc requests in the field of
// view, and drops the ones behind the camera past two page widths of its foot.
TEST(PageLevelRule, TheWedgeRefinesInTheViewAndAtTheFootOnly)
{
    const PageRequestTerrain terrain = UserTerrain();
    const PageRequestCamera camera{{30000.0f, 289.0f, 7500.0f}, {1.0f, 0.0f, 0.0f}, 0.8f, true};
    const auto wedge = SurfaceRequests(PageRequestArea::Wedge, camera);
    const auto disc = SurfaceRequests(PageRequestArea::Disc, camera);
    const float32 page = static_cast<float32>(PageStreaming::kPageOwnedSamples) * terrain.Level0TexelX;
    uint32 behind = 0;
    for (const auto& key : disc)
    {
        const auto [level, x, z] = key;
        if (level != 0u)
            continue;
        const float32 centerX = terrain.OriginX + (static_cast<float32>(x) + 0.5f) * page - camera.Position.x;
        const float32 centerZ = terrain.OriginZ + (static_cast<float32>(z) + 0.5f) * page - camera.Position.z;
        if (centerX + 0.5f * page < -kPageWedgeFootPages * page)
        {
            ++behind;
            EXPECT_FALSE(wedge.count(key)) << "level-0 page " << x << "," << z << " behind the camera";
        }
        else if (centerX > 0.0f && std::abs(std::atan2(centerZ, centerX)) < camera.HorizontalHalfFov)
        {
            EXPECT_TRUE(wedge.count(key)) << "level-0 page " << x << "," << z << " in the view";
        }
    }
    EXPECT_GT(behind, 0u) << "the disc requests no level-0 page behind the camera: the test proves nothing";
    const std::size_t levels = PageStreaming::BuildPageStoreLevels(terrain.SamplesX, terrain.SamplesZ).size();
    for (const auto& key : wedge)
    {
        const auto [level, x, z] = key;
        if (level + 1u < levels)
        {
            EXPECT_TRUE(wedge.count(Key(PageStreaming::PageAddress{0, uint8(level), x, z}.Parent())))
                << "level " << level << " page without its parent";
        }
    }
}

// Random views on the user's terrain, some with two cameras, wide fields past a quarter turn, views
// straight down and views from off the terrain: the wedge requests the bearing model's pages.
TEST(PageLevelRule, TheWedgeRequestsTheBearingModelsPagesOnRandomViews)
{
    const PageRequestTerrain terrain = UserTerrain();
    std::mt19937 random(5);
    std::uniform_real_distribution<float32> x(-3000.0f, 63000.0f);
    std::uniform_real_distribution<float32> z(-3000.0f, 18000.0f);
    std::uniform_real_distribution<float32> height(180.0f, 2500.0f);
    std::uniform_real_distribution<float32> yaw(-std::numbers::pi_v<float32>, std::numbers::pi_v<float32>);
    std::uniform_real_distribution<float32> halfFov(0.2f, 1.6f);
    uint32 compared = 0;
    for (uint32 trial = 0; trial < 48u; ++trial)
    {
        std::vector<PageRequestCamera> cameras(trial % 3u == 0u ? 2u : 1u);
        for (PageRequestCamera& camera : cameras)
        {
            const float32 heading = yaw(random);
            camera.Position = {x(random), height(random), z(random)};
            camera.Forward = trial % 8u == 7u ? Mathematics::Vector3{0.0f, -1.0f, 0.0f}
                                              : Mathematics::Vector3{std::sin(heading), -0.2f, std::cos(heading)};
            camera.HorizontalHalfFov = halfFov(random);
            camera.HasDirection = true;
        }
        std::set<std::tuple<uint32, uint32, uint32>> model;
        if (!BearingModelRequests(terrain, cameras, SurfaceView(), kSurfaceSlots, model))
            continue; // a page on the wedge's boundary: rounding decides it
        PageRequestTerrain configured = terrain;
        configured.CacheSlots = kSurfaceSlots;
        PageRequester requester;
        requester.Configure(configured, PageRequestArea::Wedge);
        std::vector<PageStreaming::PageWant> wants;
        requester.Build(cameras, SurfaceView(), wants);
        std::set<std::tuple<uint32, uint32, uint32>> built;
        for (const PageStreaming::PageWant& want : wants)
            built.insert(Key(want.Address));
        EXPECT_EQ(built, model) << "trial " << trial << ": " << built.size() << " pages against " << model.size();
        ++compared;
    }
    EXPECT_GE(compared, 40u) << "most trials had a page on the wedge's boundary: the test proves little";
}

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
    const PageRequestTerrain terrain = UserTerrain();
    const float32 texel = std::max(terrain.Level0TexelX, terrain.Level0TexelZ);
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
        const uint32 first = FirstPinnedPageLevel(levels, 1u, texel, c.Slots);
        EXPECT_EQ(first, c.FirstPinned) << c.Slots << " slots";
        EXPECT_EQ(pinnedPages(first), c.Pages) << c.Slots << " slots";
    }

    // With no slots to spend the top level alone is pinned, so a fallback walk still ends on a page.
    EXPECT_EQ(FirstPinnedPageLevel(PageStreaming::BuildPageStoreLevels(300, 300), 1u, texel, 0u), 2u);
}

TEST(PageLevelRule, TheFarFieldIsRequestedByTheLevelRuleBelowThePinnedFloor)
{
    // Under the slot cap the pinned floor is 32 m; the 16 m level and every finer one the camera's
    // distance needs come from the walk. At each point of the terrain the page of the level its
    // distance needs (or the pinned floor, when that is finer) is wanted.
    PageRequestTerrain terrain = UserTerrain();
    terrain.CacheSlots = kDesktopHeightPages.Slots;
    PageRequester requester;
    requester.Configure(terrain, PageRequestArea::Disc);
    ASSERT_EQ(requester.FirstPinnedLevel(), 7u);
    const PageLevelView view{779.0f, 9.17f};
    const Mathematics::Vector3 camera{21000.0f, 330.0f, 7300.0f};
    std::vector<PageStreaming::PageWant> wants;
    requester.Build(DiscCamera(camera), view, wants);
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
        PageRequester requester;
        requester.Configure(terrain, PageRequestArea::Disc);
        EXPECT_EQ(requester.FirstPinnedLevel() == 0u, c.Whole) << c.Samples << " samples, " << c.Slots << " slots";

        // Whole: every page is requested and pinned, with the camera far away. Not whole: only the
        // texel rule's levels are pinned.
        const Mathematics::Vector3 far{1.0e6f, 1.0e6f, 1.0e6f};
        std::vector<PageStreaming::PageWant> wants;
        requester.Build(DiscCamera(far), NoteView(), wants);
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
    PageRequester requester;
    requester.Configure(terrain, PageRequestArea::Disc);
    requester.Build(DiscCamera(eye), view, requests);

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

// A parked camera's request is built once: Build skips the walk while its inputs stand, and a fresh
// requester gives the same set, so the request that stands is the one a walk would give.
TEST(PageLevelRule, AParkedCameraBuildsItsRequestOnce)
{
    const PageRequestTerrain terrain = UserTerrain();
    const PageRequestCamera camera{{12000.0f, 800.0f, 3000.0f}, {1.0f, 0.0f, 0.0f}, 0.6f, true};
    const std::span<const PageRequestCamera> cameras(&camera, 1);
    std::vector<PageStreaming::PageWant> first;
    std::vector<PageStreaming::PageWant> again;
    PageRequester requester;
    requester.Configure(terrain, PageRequestArea::Wedge);
    ASSERT_TRUE(requester.Build(cameras, NoteView(), first));
    EXPECT_FALSE(requester.Build(cameras, NoteView(), again)) << "a parked camera walked the pyramid again";
    EXPECT_TRUE(again.empty()) << "a skipped Build wrote its output";

    PageRequester fresh;
    fresh.Configure(terrain, PageRequestArea::Wedge);
    ASSERT_TRUE(fresh.Build(cameras, NoteView(), again));
    EXPECT_EQ(again, first);

    // Each input is part of the stamp: the forward, the view and the configuration.
    PageRequestCamera turned = camera;
    turned.Forward = {0.0f, 0.0f, 1.0f};
    EXPECT_TRUE(requester.Build(std::span(&turned, 1), NoteView(), again));
    PageLevelView coarser = NoteView();
    coarser.TargetPixels *= 2.0f;
    EXPECT_TRUE(requester.Build(std::span(&turned, 1), coarser, again));
    requester.Configure(terrain, PageRequestArea::Wedge);
    EXPECT_TRUE(requester.Build(std::span(&turned, 1), coarser, again));
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
    PageRequester requester;
    requester.Configure(terrain, PageRequestArea::Disc);
    uint64 frame = 0;
    const auto step = [&](float32 height) {
        const Mathematics::Vector3 eye{30000.0f, terrain.MaxHeight + height, 7500.0f};
        requester.Build(DiscCamera(eye), view, wants);
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
uint32 s_Unresolved = 0; ///< the walks that found no resident page since the test reset it

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

void CBT_PageUnresolved()
{
    ++s_Unresolved;
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

uint32 Entry(const PageTable& table, const PageAddress& page)
{
    s_Bound = Bound{&table, nullptr, 0u};
    CBTPageField shape{};
    shape.LevelCount = uint32(table.Levels().size());
    shape.FloorLevel = table.FloorLevel();
    shape.TierCount = table.TierCount();
    shape.DirectoryOffset = table.DirectoryOffset();
    return CBT_PageTableEntry(page.Level, page.Face, page.X, page.Z, shape);
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

// The shipped locate of the surface planes (cbt_page.glsl CBT_PageLocate, CBT_PageOwnedAt,
// CBT_PageSlotTexelX/Z) at (u, v, level), fetched bilinearly from `texels`: a plane cache of `stride`-texel
// slots whose pages hold their owned samples `apron` texels in. The fragment sums each plane the same way.
float32 LocateAndFetch(const PagedField& field, const std::vector<float32>& texels, uint32 stride, uint32 apron,
                       float32 u, float32 v, float32 level)
{
    using namespace PageResolveGlsl;
    s_Bound = Bound{&field.Table, nullptr, 0u};
    CBTPageField shape{};
    shape.Level0SamplesX = field.Table.Levels().front().SamplesX;
    shape.Level0SamplesZ = field.Table.Levels().front().SamplesZ;
    shape.LevelCount = uint32(field.Table.Levels().size());
    shape.SlotsPerRow = field.Geometry.SlotsPerRow;
    const CBTPagePoint point = CBT_PagePointAtUV(u, v, shape);
    const CBTPageLocation location = CBT_PageLocate(point, level, shape);
    const std::array<std::tuple<uint32, uint32, float32>, 4> pages = {{
        {location.Level0, location.Slot0, location.Weight0},
        {location.Level1, location.Slot1, location.Weight1},
        {location.Level2, location.Slot2, location.Weight2},
        {location.Level3, location.Slot3, location.Weight3},
    }};
    const uint32 dim = field.Geometry.SlotsPerRow * stride;
    float32 sum = 0.0f;
    for (uint32 i = 0; i < location.Count; ++i)
    {
        const auto [pageLevel, slot, weight] = pages[i];
        const CBTPageOwned owned = CBT_PageOwnedAt(point, pageLevel);
        sum += weight * SampleGridBilinearTexel(texels.data(), dim, dim,
                                                CBT_PageSlotTexelX(owned, slot, stride, apron, shape),
                                                CBT_PageSlotTexelZ(owned, slot, stride, apron, shape));
    }
    return sum;
}

// The field's resident pages as a plane cache of `stride`-texel slots, each page's owned samples `apron`
// texels in and its lattice edge-clamped past the field.
std::vector<float32> PlaneTexels(const PagedField& field, uint32 stride, uint32 apron)
{
    const uint32 dim = field.Geometry.SlotsPerRow * stride;
    std::vector<float32> texels(std::size_t(dim) * dim, 0.0f);
    for (const PageAddress& page : field.AllPages())
    {
        const uint32 slot = field.Cache.SlotOf(PagedField::Page(page));
        if (slot == PageStreaming::kNoResidentSlot)
            continue;
        const PageStreaming::PageStoreLevel& shape = field.Table.Levels()[page.Level];
        for (uint32 r = 0; r < stride; ++r)
            for (uint32 c = 0; c < stride; ++c)
            {
                const int64 j = std::clamp<int64>(int64(page.X) * 128 + c - apron, 0, shape.SamplesX - 1);
                const int64 k = std::clamp<int64>(int64(page.Z) * 128 + r - apron, 0, shape.SamplesZ - 1);
                texels[std::size_t((slot / field.Geometry.SlotsPerRow) * stride + r) * dim +
                       (slot % field.Geometry.SlotsPerRow) * stride + c] =
                    FieldHeight(float32(j << page.Level), float32(k << page.Level));
            }
    }
    return texels;
}

constexpr float32 kLocateTolerance = 1e-4f; // the locate sums (1 - w) and w where the mirror lerps

} // namespace

TEST(PagedSurfaceResolve, EachPlaneFetchesTheLocatedPagesAtItsStrideAndApron)
{
    // Every page resident and settled: a lattice plane (130, apron 1) and a block plane (136, apron 4, the BC5
    // normal) both read the height mirror's value at every level.
    PagedField field(64);
    const std::vector<PageAddress> all = field.AllPages();
    for (int frame = 0; frame < 40; ++frame)
        field.Step(all);
    const PagedHeightSampler mirror = field.Sampler();
    // The texels as the store lays them out; the fetch at the shader's strides and aprons.
    const std::vector<float32> lattice =
        PlaneTexels(field, PageStreaming::kPageStrideSamples, PageStreaming::kPageApronSamples);
    const std::vector<float32> block = PlaneTexels(
        field, PageStreaming::PageStrideTexels(PageStreaming::PageFieldFormat::Bc5), PageStreaming::kPageBlockTexels);
    using PageResolveGlsl::CBT_PAGE_APRON;
    using PageResolveGlsl::CBT_PAGE_BLOCK_APRON;
    using PageResolveGlsl::CBT_PAGE_BLOCK_STRIDE;
    using PageResolveGlsl::CBT_PAGE_STRIDE;
    for (float32 level : {0.0f, 0.5f, 0.9f, 1.3f, 2.95f, 40.0f})
        for (uint32 i = 0; i <= 32; ++i)
            for (uint32 k = 0; k <= 24; ++k)
            {
                const float32 u = float32(i) / 32.0f;
                const float32 v = float32(k) / 24.0f;
                const float32 expected = mirror.Sample(u, v, level);
                ASSERT_NEAR(LocateAndFetch(field, lattice, CBT_PAGE_STRIDE, CBT_PAGE_APRON, u, v, level), expected,
                            kLocateTolerance)
                    << "lattice plane, uv " << u << "," << v << " level " << level;
                ASSERT_NEAR(LocateAndFetch(field, block, CBT_PAGE_BLOCK_STRIDE, CBT_PAGE_BLOCK_APRON, u, v, level),
                            expected, kLocateTolerance)
                    << "block plane, uv " << u << "," << v << " level " << level;
            }
}

TEST(PagedSurfaceResolve, TheLocateWeighsThePagesTheHeightResolveReads)
{
    // A missing page (the fallback walk) and a page fading in over its settled parent: the locate's pages and
    // weights, fetched from the height cache, give the height resolve's value.
    PagedField field(64);
    std::vector<PageAddress> partial;
    for (const PageAddress& page : field.AllPages())
        if (!(page.Level == 0 && page.X == 1 && page.Z == 1) && !(page.Level == 0 && page.X == 3 && page.Z == 2))
            partial.push_back(page);
    for (int frame = 0; frame < 40; ++frame)
        field.Step(partial);
    partial.push_back(PageAddress{0, 0, 1, 1});
    field.Step(partial, 0.1f);
    ASSERT_LT(field.Cache.FadeOf(PagedField::Page(PageAddress{0, 0, 1, 1})), 1.0f);
    ASSERT_EQ(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 3, 2})), PageStreaming::kNoResidentSlot);

    const PagedHeightSampler mirror = field.Sampler();
    for (float32 level : {0.0f, 0.3f, 0.8f, 0.9375f, 1.0f, 1.5f, 1.9f, 2.6f, 40.0f})
        for (uint32 i = 0; i <= 64; ++i)
            for (uint32 k = 0; k <= 48; ++k)
            {
                const float32 u = float32(i) / 64.0f;
                const float32 v = float32(k) / 48.0f;
                ASSERT_NEAR(LocateAndFetch(field, field.Texels, PageResolveGlsl::CBT_PAGE_STRIDE,
                                           PageResolveGlsl::CBT_PAGE_APRON, u, v, level),
                            mirror.Sample(u, v, level), kLocateTolerance)
                    << "uv " << u << "," << v << " level " << level;
            }
}

TEST(PagedHeightResolve, AWalkThatFindsNoResidentPageUpToTheTopCountsOneUnresolvedSample)
{
    PagedField field(64);
    PageResolveGlsl::s_Unresolved = 0;
    EXPECT_EQ(PageResolveGlsl::Sample(field, 0.5f, 0.5f, 0.0f), 0.0f);
    EXPECT_EQ(PageResolveGlsl::s_Unresolved, 1u) << "no page is resident";

    const std::vector<PageAddress> all = field.AllPages();
    for (int frame = 0; frame < 40; ++frame)
        field.Step(all);
    PageResolveGlsl::s_Unresolved = 0;
    for (float32 level : {0.0f, 0.5f, 1.0f, 40.0f})
        for (uint32 i = 0; i <= 16; ++i)
            PageResolveGlsl::Sample(field, float32(i) / 16.0f, 1.0f - float32(i) / 16.0f, level);
    EXPECT_EQ(PageResolveGlsl::s_Unresolved, 0u) << "every page is resident";
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

TEST(PagedHeightResolve, TheSixFaceTableResolvesAsAOneFaceTablePerFaceOnRandomSets)
{
    // A planet's table holds six faces. Each face's entries must equal a one-face dense table that holds
    // that face's pages alone, on the CPU and through the GLSL walk, dense and tiered.
    constexpr uint32 kFaceSamples = 4097u; // six levels
    constexpr uint32 kSlots = 4096u;
    constexpr uint32 kFaces = PageStreaming::kPageStoreMaxFaces;
    PageTable denseSix;
    denseSix.Reset(kFaceSamples, kFaceSamples, kFaces, kSlots, PageTableBudget{1u << 21, 1u, 1u});
    PageTable tieredSix;
    // The floor holds more than one page a face, so the floor directory's face term is under test.
    tieredSix.Reset(kFaceSamples, kFaceSamples, kFaces, kSlots, PageTableBudget{1024u, kSlots, kSlots});
    ASSERT_EQ(denseSix.TierCount(), 0u);
    ASSERT_GT(tieredSix.TierCount(), 0u);
    ASSERT_GT(tieredSix.FloorLevel(), 0u);
    ASSERT_GT(tieredSix.Levels()[tieredSix.FloorLevel()].PagesX, 1u);
    std::array<PageTable, kFaces> perFace;
    for (PageTable& face : perFace)
        face.Reset(kFaceSamples, kFaceSamples, 1u, kSlots, PageTableBudget{1u << 21, 1u, 1u});

    std::vector<PageAddress> all;
    for (uint32 level = 0; level < perFace[0].Levels().size(); ++level)
        for (uint32 z = 0; z < perFace[0].Levels()[level].PagesZ; ++z)
            for (uint32 x = 0; x < perFace[0].Levels()[level].PagesX; ++x)
                all.push_back(PageAddress{0, uint8(level), x, z});
    std::mt19937 random(6060u);
    uint32 resident = 0;
    uint32 slot = 0;
    for (int round = 0; round < 40; ++round)
    {
        for (uint32 i = 0; i < 64u; ++i)
        {
            PageAddress page = all[random() % all.size()];
            const uint8 face = uint8(random() % kFaces);
            const bool set = random() % 3u != 0u;
            const uint32 entry =
                set ? PageStreaming::PackPageEntry(slot++ % kSlots, page.Level) : PageStreaming::kNoPage;
            perFace[face].SetEntry(page, entry);
            page.Face = face;
            denseSix.SetEntry(page, entry);
            tieredSix.SetEntry(page, entry);
        }
        for (uint8 face = 0; face < kFaces; ++face)
            for (PageAddress page : all)
            {
                const uint32 expected = perFace[face].Entry(page);
                page.Face = face;
                ASSERT_EQ(denseSix.Entry(page), expected) << "face " << int(face) << " L" << int(page.Level);
                ASSERT_EQ(tieredSix.Entry(page), expected) << "face " << int(face) << " L" << int(page.Level);
                ASSERT_EQ(PageResolveGlsl::Entry(denseSix, page), expected) << "the GLSL walk, dense";
                ASSERT_EQ(PageResolveGlsl::Entry(tieredSix, page), expected) << "the GLSL walk, tiered";
                resident += expected != PageStreaming::kNoPage ? 1u : 0u;
            }
    }
    EXPECT_GT(resident, 1000u) << "the sets left too few pages resident to compare";
    std::vector<PageAddress> sixResident;
    tieredSix.AppendResident(sixResident);
    std::vector<PageAddress> expectedResident;
    for (uint8 face = 0; face < kFaces; ++face)
    {
        std::vector<PageAddress> pages;
        perFace[face].AppendResident(pages);
        for (PageAddress page : pages)
        {
            page.Face = face;
            expectedResident.push_back(page);
        }
    }
    std::sort(sixResident.begin(), sixResident.end());
    std::sort(expectedResident.begin(), expectedResident.end());
    EXPECT_EQ(sixResident, expectedResident);
}

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
            const uint32 expected = dense.Table.Entry(page);
            ASSERT_EQ(tiered.Table.Entry(page), expected)
                << "frame " << frame << " L" << int(page.Level) << "(" << page.X << "," << page.Z << ")";
            ASSERT_EQ(PageResolveGlsl::Entry(tiered.Table, page), expected)
                << "the GLSL walk, frame " << frame << " L" << int(page.Level) << "(" << page.X << "," << page.Z << ")";
            resident += expected != PageStreaming::kNoPage ? 1u : 0u;
            ASSERT_EQ(PageResolveGlsl::Entry(crowded.Table, page), crowded.Table.Entry(page))
                << "the GLSL walk over crowded pools, frame " << frame << " L" << int(page.Level) << "(" << page.X
                << "," << page.Z << ")";
            const uint32 crowdedEntry = crowded.Table.Entry(page);
            ASSERT_TRUE(crowdedEntry == PageStreaming::kNoPage || crowdedEntry == expected)
                << "a crowded table holds fewer pages, never another entry";
            ASSERT_EQ(PageResolveGlsl::Entry(crowdedTwoTiers.Table, page), crowdedTwoTiers.Table.Entry(page))
                << "the GLSL walk over two crowded tiers, frame " << frame << " L" << int(page.Level) << "("
                << page.X << "," << page.Z << ")";
            // A page resident in the cache whose table entry reads kNoPage waits for a block.
            const bool cached = expected != PageStreaming::kNoPage;
            waited += cached && crowdedEntry == PageStreaming::kNoPage ? 1u : 0u;
            waitedTwoTiers +=
                cached && crowdedTwoTiers.Table.Entry(page) == PageStreaming::kNoPage ? 1u : 0u;
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
    table.Reset(kTieredX, kTieredZ, 1u, 8u, PageTableBudget{4u, 1u, 1u});
    const uint32 first = PageStreaming::PackPageEntry(1, 0);
    const uint32 second = PageStreaming::PackPageEntry(2, 0);
    table.SetEntry(PageAddress{0, 0, 0, 0}, first);
    table.SetEntry(PageAddress{0, 0, 100, 5}, second); // another level-1 root: the one leaf block is taken
    EXPECT_EQ(table.Entry(PageAddress{0, 0, 0, 0}), first);
    EXPECT_EQ(table.Entry(PageAddress{0, 0, 100, 5}), PageStreaming::kNoPage) << "it reads its ancestor while it waits";
    std::vector<PageAddress> late;
    table.TakeLateEntries(late);
    EXPECT_TRUE(late.empty()) << "a waiting page has no late entry yet";

    table.SetEntry(PageAddress{0, 0, 0, 0}, PageStreaming::kNoPage);
    EXPECT_EQ(table.Entry(PageAddress{0, 0, 100, 5}), second) << "the freed block takes the waiting page";
    table.TakeLateEntries(late);
    ASSERT_EQ(late.size(), 1u);
    EXPECT_EQ(late.front(), (PageAddress{0, 0, 100, 5}));

    table.SetEntry(PageAddress{0, 0, 100, 5}, PageStreaming::kNoPage);
    EXPECT_TRUE(ResidentOf(table).empty()) << "a block with nothing resident under it is freed";
}

namespace
{

// The target sizes of design large-planar-terrains, section 1, at 0.5 m: level-0 samples a side of a
// 250 km plane, of one face of a 600 km planet and of one Earth face.
constexpr uint32 kPlane250KmSamples = 500001u;
constexpr uint32 kPlanet600KmFaceSamples = 1900545u;
constexpr uint32 kEarthFaceSamples = 20185089u;
constexpr float32 kTargetTexelMeters = 0.5f;

// One target's field: a planar terrain has one face, a planet six.
struct TargetField
{
    const char* Name = "";
    uint32 Samples = 0;
    uint32 Faces = 1;
};

constexpr std::array<TargetField, 5> kTargetFields{{
    {"250 km plane", kPlane250KmSamples, 1u},
    {"600 km planet's face as a plane", kPlanet600KmFaceSamples, 1u},
    {"600 km planet", kPlanet600KmFaceSamples, PageStreaming::kPageStoreMaxFaces},
    {"Earth face as a plane", kEarthFaceSamples, 1u},
    {"Earth planet", kEarthFaceSamples, PageStreaming::kPageStoreMaxFaces},
}};

// The empty table of a target under a shipped profile, with the profile's slots and the budget
// TerrainPages gives it.
PageTable TargetTable(const TargetField& target, const HeightPageProfile& profile)
{
    PageTable table;
    table.Reset(target.Samples, target.Samples, target.Faces, profile.Slots,
                PageTableBudget{profile.DenseTableWords, profile.TableMidBlocks, profile.TableLeafBlocks});
    return table;
}

// A random page finer than the floor, under the floor page (rootX, rootZ) of face 0.
PageAddress RandomPageUnderFloorPage(const PageTable& table, uint32 rootX, uint32 rootZ, std::mt19937& random)
{
    const uint32 level = uint32(random() % table.FloorLevel());
    const uint32 below = table.FloorLevel() - level;
    const PageStreaming::PageStoreLevel& shape = table.Levels()[level];
    const uint32 x = std::min((rootX << below) + uint32(random() % (1u << below)), shape.PagesX - 1u);
    const uint32 z = std::min((rootZ << below) + uint32(random() % (1u << below)), shape.PagesZ - 1u);
    return PageAddress{0, uint8(level), x, z};
}

// A random page of face 0 on the floor level or coarser.
PageAddress RandomDensePage(const PageTable& table, std::mt19937& random)
{
    const uint32 denseLevels = uint32(table.Levels().size()) - table.FloorLevel();
    const uint32 level = table.FloorLevel() + uint32(random() % denseLevels);
    const PageStreaming::PageStoreLevel& shape = table.Levels()[level];
    return PageAddress{0, uint8(level), uint32(random() % shape.PagesX), uint32(random() % shape.PagesZ)};
}

// The indices of the words that differ between two packs of one table.
std::vector<uint32> ChangedWords(std::span<const uint32> before, std::span<const uint32> after)
{
    std::vector<uint32> changed;
    for (uint32 word = 0; word < after.size(); ++word)
        if (before[word] != after[word])
            changed.push_back(word);
    return changed;
}

} // namespace

TEST(PagedHeightResolve, TheShippedProfilesTableTheTargetSizesAtTheModelsWordsFloorsAndTiers)
{
    // The words at binding 22 (header, fades, table), the floor level and the tiers of blocks. A lookup
    // below the floor reads the floor directory, then one block for each tier. On desktop the six-face
    // Earth table has floor 10, and one Earth face as a plane has floor 8 and about twice the words.
    struct Expected
    {
        TargetField Target;
        const char* ProfileName = "";
        HeightPageProfile Profile;
        uint64 Words = 0;
        uint32 FloorLevel = 0;
        uint32 Tiers = 0;
    };
    const std::array<Expected, 10> cases{{
        {kTargetFields[0], "desktop", kDesktopHeightPages, 572634u, 3u, 1u},  // 2.18 MiB
        {kTargetFields[1], "desktop", kDesktopHeightPages, 724011u, 5u, 1u},  // 2.76 MiB
        {kTargetFields[2], "desktop", kDesktopHeightPages, 1636000u, 6u, 1u}, // 6.24 MiB
        {kTargetFields[3], "desktop", kDesktopHeightPages, 988444u, 8u, 2u},  // 3.77 MiB
        {kTargetFields[4], "desktop", kDesktopHeightPages, 487780u, 10u, 2u}, // 1.86 MiB
        {kTargetFields[0], "Deck", kSteamDeckHeightPages, 167921u, 4u, 1u},   // 0.64 MiB
        {kTargetFields[1], "Deck", kSteamDeckHeightPages, 564154u, 6u, 1u},   // 2.15 MiB
        {kTargetFields[2], "Deck", kSteamDeckHeightPages, 240914u, 7u, 2u},
        {kTargetFields[3], "Deck", kSteamDeckHeightPages, 278031u, 9u, 2u},
        {kTargetFields[4], "Deck", kSteamDeckHeightPages, 242668u, 11u, 2u},
    }};
    for (const Expected& expected : cases)
    {
        SCOPED_TRACE(std::string(expected.Target.Name) + ", " + expected.ProfileName);
        const PageTable table = TargetTable(expected.Target, expected.Profile);
        EXPECT_EQ(CBTTerrain::kCBTPageFadeOffsetWords + expected.Profile.Slots + table.Words().size(), expected.Words);
        EXPECT_EQ(table.FloorLevel(), expected.FloorLevel);
        EXPECT_EQ(table.TierCount(), expected.Tiers);
    }
}

TEST(PagedHeightResolve, TheTieredTableOfA250KmPlaneResolvesAsAMirrorOfItsResidentSetOnRandomSets)
{
    // The desktop table of 500,001 x 500,001 samples: levels 12 to 3 dense, one tier of blocks for
    // levels 2 to 0. The fine pages fall under at most 150 floor pages, within the 160 leaf blocks,
    // so no page waits and the table holds every page of the mirror.
    constexpr uint32 kFloorPagesInUse = 150u;
    const uint32 slots = kDesktopHeightPages.Slots;
    PageTable table = TargetTable(kTargetFields[0], kDesktopHeightPages);
    ASSERT_EQ(table.FloorLevel(), 3u);
    ASSERT_EQ(table.TierCount(), 1u);
    const PageStreaming::PageStoreLevel& floorShape = table.Levels()[table.FloorLevel()];
    std::mt19937 random(500001u);
    // The four corners (on the far edges a block's grid passes the level's last page), then random ones.
    std::vector<std::pair<uint32, uint32>> roots{{0u, 0u},
                                                 {floorShape.PagesX - 1u, 0u},
                                                 {0u, floorShape.PagesZ - 1u},
                                                 {floorShape.PagesX - 1u, floorShape.PagesZ - 1u}};
    while (roots.size() < kFloorPagesInUse)
        roots.emplace_back(uint32(random() % floorShape.PagesX), uint32(random() % floorShape.PagesZ));

    FlatMap<PageAddress, uint32> mirror;
    uint32 slot = 0;
    uint32 compared = 0;
    for (int round = 0; round < 40; ++round)
    {
        // Pages arrive and leave; the set stays within the cache's slots.
        for (uint32 i = 0; i < 64u; ++i)
        {
            if (!mirror.Empty() && (random() % 3u == 0u || mirror.Size() >= slots))
            {
                const PageAddress page = std::next(mirror.begin(), std::ptrdiff_t(random() % mirror.Size()))->first;
                table.SetEntry(page, PageStreaming::kNoPage);
                mirror.Erase(page);
                continue;
            }
            const std::pair<uint32, uint32> root = roots[random() % roots.size()];
            const PageAddress page = random() % 4u == 0u ? RandomDensePage(table, random)
                                                         : RandomPageUnderFloorPage(table, root.first, root.second, random);
            const uint32 entry = PageStreaming::PackPageEntry(slot++ % slots, page.Level);
            table.SetEntry(page, entry);
            mirror.InsertOrAssign(page, entry);
        }
        std::vector<PageAddress> mirrored;
        for (const auto& [page, entry] : mirror)
        {
            ASSERT_EQ(table.Entry(page), entry)
                << "round " << round << " L" << int(page.Level) << "(" << page.X << "," << page.Z << ")";
            ASSERT_EQ(PageResolveGlsl::Entry(table, page), entry)
                << "the GLSL walk, round " << round << " L" << int(page.Level) << "(" << page.X << "," << page.Z << ")";
            // Its neighbor along X reads its own entry or none: no write strays to it.
            const PageAddress neighbor{0, page.Level, page.X ^ 1u, page.Z};
            if (neighbor.X < table.Levels()[page.Level].PagesX)
            {
                const uint32* held = mirror.Find(neighbor);
                ASSERT_EQ(table.Entry(neighbor), held != nullptr ? *held : PageStreaming::kNoPage)
                    << "round " << round << " L" << int(neighbor.Level) << "(" << neighbor.X << "," << neighbor.Z << ")";
            }
            mirrored.push_back(page);
            ++compared;
        }
        ASSERT_EQ(ResidentOf(table), mirrored) << "round " << round;
        std::vector<PageAddress> late;
        table.TakeLateEntries(late);
        ASSERT_TRUE(late.empty()) << "no page waited for a block, round " << round;
    }
    EXPECT_GT(compared, 10000u) << "the sets left too few pages resident to compare";
}

TEST(PagedHeightResolve, ALevelSwapBelowTheFloorRewritesOnlyTheWordsOfOneBlock)
{
    // A level-1 page gives way to its four level-0 children: the children arrive, then the parent
    // leaves. On the plane both levels are in a block of the one tier; on the Earth face, in a block of
    // the second tier.
    for (const TargetField& target : {kTargetFields[0], kTargetFields[3]})
    {
        SCOPED_TRACE(target.Name);
        PageTable table = TargetTable(target, kDesktopHeightPages);
        ASSERT_GT(table.FloorLevel(), 1u);
        const PageStreaming::PageStoreLevel& level1 = table.Levels()[1];
        const PageAddress parent{0, 1, level1.PagesX / 2u, level1.PagesZ / 3u};
        // The parent's sibling shares its block; the level-0 page at a corner takes another block.
        table.SetEntry(parent, PageStreaming::PackPageEntry(1, 1));
        table.SetEntry(PageAddress{0, 1, parent.X ^ 1u, parent.Z}, PageStreaming::PackPageEntry(2, 1));
        table.SetEntry(PageAddress{0, 0, 3u, 5u}, PageStreaming::PackPageEntry(3, 0));
        const std::vector<uint32> before(table.Words().begin(), table.Words().end());

        for (uint32 child = 0; child < 4u; ++child)
            table.SetEntry(PageAddress{0, 0, parent.X * 2u + child % 2u, parent.Z * 2u + child / 2u},
                           PageStreaming::PackPageEntry(4u + child, 0));
        table.SetEntry(parent, PageStreaming::kNoPage);

        const std::vector<uint32> changed = ChangedWords(before, table.Words());
        ASSERT_EQ(changed.size(), 5u) << "the four children's entries and the parent's";
        // The last tier's descriptor: the block's words, then the pool's first word.
        const uint32 descriptor = (table.TierCount() - 1u) * PageResolveGlsl::CBT_PAGE_TIER_WORDS;
        const uint32 blockWords = table.Words()[descriptor + 2u];
        const uint32 poolOffset = table.Words()[descriptor + 3u];
        for (uint32 word : changed)
        {
            ASSERT_GE(word, poolOffset) << "word " << word << " is before the leaf pool";
            EXPECT_EQ((word - poolOffset) / blockWords, (changed.front() - poolOffset) / blockWords)
                << "word " << word << " is in another block";
        }
    }
}

TEST(PagedHeightResolve, ThePinnedLevelsOfEveryTargetStayInsideTheDenseTier)
{
    // The pins are the coarsest levels within a quarter of the slots (128 pages on desktop), never finer
    // than 16 m. A pinned page has a dense entry, so pinning takes no floor directory word and no block.
    for (const auto& [profile, profileName] :
         {std::pair{kDesktopHeightPages, "desktop"}, std::pair{kSteamDeckHeightPages, "Deck"}})
        for (const TargetField& target : kTargetFields)
        {
            SCOPED_TRACE(std::string(target.Name) + ", " + profileName);
            PageTable table = TargetTable(target, profile);
            const uint32 firstPinned =
                FirstPinnedPageLevel(table.Levels(), target.Faces, kTargetTexelMeters, profile.Slots);
            EXPECT_GE(firstPinned, table.FloorLevel());
            uint32 pinned = 0;
            for (uint32 level = firstPinned; level < table.Levels().size(); ++level)
                for (uint32 face = 0; face < target.Faces; ++face)
                    for (uint32 z = 0; z < table.Levels()[level].PagesZ; ++z)
                        for (uint32 x = 0; x < table.Levels()[level].PagesX; ++x)
                            table.SetEntry(PageAddress{uint8(face), uint8(level), x, z},
                                           PageStreaming::PackPageEntry(pinned++ % profile.Slots, level));
            EXPECT_LE(pinned, profile.Slots / 4u) << "the pins take at most a quarter of the slots";
            const std::span<const uint32> words = table.Words();
            EXPECT_TRUE(std::all_of(words.begin() + table.DirectoryOffset(), words.end(),
                                    [](uint32 word) { return word == PageStreaming::kNoPage; }))
                << "a pinned page took a floor directory word or a block";
        }
}

namespace
{

// True when some point of the level-`level` page (x, z) lies in the wedge of `camera`: within two page
// widths of its foot, or within its horizontal field plus the margin. Samples the footprint's edges,
// so it does not share the requester's corner-bearing test.
bool ModelPageInWedge(const PageRequestTerrain& terrain, uint32 level, uint32 x, uint32 z,
                      const PageRequestCamera& camera)
{
    constexpr uint32 kEdgeSamples = 32;
    const float32 span = float32(PageStreaming::kPageOwnedSamples << level) * terrain.Level0TexelX;
    const float32 minX = terrain.OriginX + float32(x) * span;
    const float32 minZ = terrain.OriginZ + float32(z) * span;
    const float32 dx = std::max({minX - camera.Position.x, 0.0f, camera.Position.x - minX - span});
    const float32 dz = std::max({minZ - camera.Position.z, 0.0f, camera.Position.z - minZ - span});
    if (std::sqrt(dx * dx + dz * dz) <= kPageWedgeFootPages * span)
        return true;
    const float32 forward = std::atan2(camera.Forward.x, camera.Forward.z);
    const float32 limit = camera.HorizontalHalfFov + kPageWedgeMarginRadians;
    for (uint32 i = 0; i <= kEdgeSamples; ++i)
    {
        const float32 along = span * float32(i) / float32(kEdgeSamples);
        const std::array<std::pair<float32, float32>, 4> points{{{minX + along, minZ},
                                                                 {minX + along, minZ + span},
                                                                 {minX, minZ + along},
                                                                 {minX + span, minZ + along}}};
        for (const auto& [px, pz] : points)
        {
            const float32 off = std::atan2(px - camera.Position.x, pz - camera.Position.z) - forward;
            if (std::abs(std::remainder(off, 2.0f * std::numbers::pi_v<float32>)) <= limit)
                return true;
        }
    }
    return false;
}

// The needed-plus-parents model (design page-streaming-planets 3.5): a page is needed where its own
// nearest point needs exactly its level, or when it is pinned; then every ancestor of a needed page.
// With `wedge`, a page outside that camera's wedge is needed only when pinned.
std::size_t NeededPlusParents(const PageRequestTerrain& terrain, uint32 firstPinned, const Mathematics::Vector3& camera,
                              const PageLevelView& view, const PageRequestCamera* wedge = nullptr)
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
                const bool inArea = wedge == nullptr || ModelPageInWedge(terrain, level, x, z, *wedge);
                if (level < firstPinned && (wants != level || !inArea))
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
    PageRequester requester;
    requester.Configure(terrain, PageRequestArea::Disc);
    for (float32 eye : {288.8f, 788.8f, 3288.8f})
    {
        const Mathematics::Vector3 camera{3493.0f, eye, 899.0f};
        std::vector<PageStreaming::PageWant> wants;
        requester.Build(DiscCamera(camera), view, wants);
        const std::size_t model = NeededPlusParents(terrain, requester.FirstPinnedLevel(), camera, view);
        EXPECT_LE(std::fabs(float64(wants.size()) - float64(model)), 0.25 * float64(model))
            << "eye " << eye << " m: requested " << wants.size() << ", needed with parents " << model;
    }
}

// The surface stream at the DEM eye pose: the wedge's request is the needed-plus-parents model inside
// the wedge, and well below the disc's request.
TEST(PageLevelRule, TheSurfaceRequestAtTheDemEyePoseIsTheNeededPagesInTheWedgeWithTheirParents)
{
    // The near terrain and viewport of the three-pose test, the surface's rule (one texel a pixel) and
    // the surface field's desktop slots. The eye looks along +X (yaw 0); a 16:9 view is assumed.
    PageRequestTerrain terrain;
    terrain.Level0TexelX = terrain.Level0TexelZ = 1.0f;
    terrain.SamplesX = 8193u;
    terrain.SamplesZ = 2049u;
    terrain.MinHeight = 236.0f;
    terrain.MaxHeight = 287.1f;
    terrain.CacheSlots = 2048u;
    const PageLevelView view{779.0f, 1.0f};
    const float32 halfFov = std::atan(std::tan(std::numbers::pi_v<float32> / 6.0f) * 16.0f / 9.0f);
    const PageRequestCamera camera{{3493.0f, 288.8f, 899.0f}, {1.0f, 0.0f, 0.0f}, halfFov, true};

    PageRequester wedge;
    wedge.Configure(terrain, PageRequestArea::Wedge);
    std::vector<PageStreaming::PageWant> wants;
    wedge.Build(std::span(&camera, 1), view, wants);
    const std::size_t model = NeededPlusParents(terrain, wedge.FirstPinnedLevel(), camera.Position, view, &camera);
    EXPECT_LE(std::fabs(float64(wants.size()) - float64(model)), 0.25 * float64(model))
        << "requested " << wants.size() << ", needed in the wedge with parents " << model;

    PageRequester disc;
    disc.Configure(terrain, PageRequestArea::Disc);
    std::vector<PageStreaming::PageWant> discWants;
    disc.Build(std::span(&camera, 1), view, discWants);
    EXPECT_LT(float64(wants.size()), 0.75 * float64(discWants.size()))
        << "the wedge requests " << wants.size() << " of the disc's " << discWants.size();
}
