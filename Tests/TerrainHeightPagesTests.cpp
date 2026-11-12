// The height source of a tiled terrain (TerrainHeightIsPaged) and the main-thread half of its height
// pages (TerrainPages): the predicate's arms, the GPU page-table words, the dirty rects that
// make the CBT re-evaluate the bisectors whose height changed source, a moved terrain's requests and
// the table versions; and the GPU side's latch (TerrainPageFeature): a table is readable only
// with the upload pass of its pages, a snapshot taken back from the pool holds the published words,
// the cache texture only from the latch after its clear, a surface page writes each plane into the
// same slot of its plane texture, and a terrain reads only the plane textures of its own planes.

#include "CBTTerrain/CBTLayout.h"
#include "TerrainECS/PageTable.h"
#include "TerrainECS/TerrainSurface/TerrainSurfacePageSource.h"
#include "TerrainECS/TerrainPageFeature.h"
#include "TerrainECS/TerrainPages.h"
#include "TerrainECS/TerrainSurface/TerrainSurfaceStore.h"
#include "TerrainECS/TerrainHeightSource.h"
#include "TerrainECS/TerrainDefaultSurfaceRules.h"
#include "TerrainPagedField.h"

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageResidency.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStore/PageStoreAccess.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "PageStreaming/PageTableEntry.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using GameEngine::TerrainECS::Test::FlatPlanetRequest;
using GameEngine::TerrainECS::Test::FlatTileRequest;

// The planets' dirty regions, which the height page driver drains.
struct GameEngine::TerrainECS::TerrainHeightPagesTestAccess
{
    static CBTTerrain::SphereEditRegions& PlanetDirtyRegions(TerrainPages& pages)
    {
        return pages.PlanetDirtyRegions();
    }
};

namespace
{

// A terrain whose modifiers are baked into its page overlay and whose page source exists on the
// tile lattice.
TerrainHeightSourceInputs BakedPageable()
{
    TerrainHeightSourceInputs inputs;
    inputs.ModifiersPending = false;
    inputs.GeneratedBase = false;
    inputs.CookedStoreOpen = true;
    inputs.PagesOnLattice = true;
    inputs.GpuReadsPages = true;
    return inputs;
}

bool CoversWholeTerrain(const HeightPageDirtyRect& rect)
{
    return rect.MinU == 0.0f && rect.MinV == 0.0f && rect.MaxU == 1.0f && rect.MaxV == 1.0f;
}

} // namespace

TEST(TerrainHeightSource, ATerrainWhoseModifiersAreNotBakedIntoItsPagesYetKeepsItsTexture)
{
    TerrainHeightSourceInputs inputs = BakedPageable();
    inputs.ModifiersPending = true;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs));
    inputs.GeneratedBase = true;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs)) << "pending modifiers keep even a generated base on its texture";
}

TEST(TerrainHeightSource, AHeightmapWithoutAnOpenStoreKeepsItsTexture)
{
    TerrainHeightSourceInputs inputs = BakedPageable();
    inputs.CookedStoreOpen = false;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs));
    inputs.CookedStoreOpen = true;
    inputs.PagesOnLattice = false;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs)) << "a pyramid off the tile lattice keeps the texture";
}

TEST(TerrainHeightSource, TheNarrowCbtArmKeepsTheTexture)
{
    TerrainHeightSourceInputs inputs = BakedPageable();
    inputs.GpuReadsPages = false;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs));
    inputs.CookedStoreOpen = false;
    inputs.GeneratedBase = true;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs)) << "nothing streams pages the narrow arm never reads";
}

TEST(TerrainHeightSource, AnOpenStoreOrAGeneratedBasePages)
{
    TerrainHeightSourceInputs inputs = BakedPageable();
    EXPECT_TRUE(TerrainHeightIsPaged(inputs));
    inputs.CookedStoreOpen = false;
    inputs.GeneratedBase = true;
    EXPECT_TRUE(TerrainHeightIsPaged(inputs));
}

TEST(TerrainHeightSource, EveryTerrainWithAPageSourceOnTheLatticeHasCpuPages)
{
    TerrainHeightSourceInputs inputs = BakedPageable();
    inputs.PagesOnLattice = true;
    EXPECT_TRUE(TerrainHeightHasCpuPages(inputs));
    inputs.GpuReadsPages = false;
    EXPECT_TRUE(TerrainHeightHasCpuPages(inputs)) << "the narrow arm draws the texture; the CPU reads the pages";
    inputs.ModifiersPending = true;
    EXPECT_TRUE(TerrainHeightHasCpuPages(inputs)) << "pending modifiers: the CPU pages wait for the overlay";
    inputs.CookedStoreOpen = false;
    EXPECT_FALSE(TerrainHeightHasCpuPages(inputs)) << "no page source";
    inputs.GeneratedBase = true;
    EXPECT_TRUE(TerrainHeightHasCpuPages(inputs));
    inputs.PagesOnLattice = false;
    EXPECT_FALSE(TerrainHeightHasCpuPages(inputs)) << "a generated pyramid off the lattice";
}

TEST(TerrainPages, PackedWordsCarryTheShapeFadesAndEntriesWhereTheResolveReadsThem)
{
    PageTable table;
    table.Reset(257, 257, 1u, 4, PageTableBudget{kDesktopHeightPages.DenseTableWords, 1u, 1u});
    table.SetEntry(PageStreaming::PageAddress{0, 0, 1, 2}, PageStreaming::PackPageEntry(2, 0));
    table.SetSlotFade(2, 0.25f);
    const PageCacheGeometry geometry = MakePageCacheGeometry(4);

    std::vector<uint32> words;
    PackPageTableWords(table, geometry, words);
    ASSERT_EQ(words.size(), CBTTerrain::kCBTPageFadeOffsetWords + 4u + table.Words().size());
    EXPECT_EQ(words[0], 257u);
    EXPECT_EQ(words[1], 257u);
    EXPECT_EQ(words[2], table.Levels().size());
    EXPECT_EQ(words[3], geometry.SlotsPerRow);
    EXPECT_EQ(words[4], 4u);
    const uint32 levelsAt = 8u;
    const uint32 fadesAt = levelsAt + 4u * 32u;
    for (uint32 level = 0; level < table.Levels().size(); ++level)
    {
        EXPECT_EQ(words[levelsAt + 4u * level], table.Levels()[level].FirstEntry);
        EXPECT_EQ(words[levelsAt + 4u * level + 1u], table.Levels()[level].PagesX);
        EXPECT_EQ(words[levelsAt + 4u * level + 2u], table.Levels()[level].PagesZ);
    }
    EXPECT_EQ(std::bit_cast<float32>(words[fadesAt + 2u]), 0.25f);
    const uint32 entriesAt = fadesAt + 4u;
    const uint32 pageAt = entriesAt + 2u * table.Levels()[0].PagesX + 1u;
    EXPECT_EQ(words[pageAt], PageStreaming::PackPageEntry(2, 0)) << "level 0, page (1, 2), row-major";
    EXPECT_EQ(words[entriesAt], PageStreaming::kNoPage);
}

TEST(TerrainPages, ATableThatFitsItsDenseBudgetPacksTheDenseWordsByteForByte)
{
    // The dense layout written out here from its definition: the shape, zeros in [5, 8), each level's
    // first entry and pages, the fades, then every entry level-major and row-major.
    constexpr uint32 kSlots = 64u;
    PageTable table;
    table.Reset(8193, 2049, 1u, kSlots, PageTableBudget{kDesktopHeightPages.DenseTableWords, 1u, 1u});
    ASSERT_EQ(table.FloorLevel(), 0u);
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(8193, 2049);
    std::vector<uint32> entries(levels.back().FirstEntry + levels.back().PagesX * levels.back().PagesZ,
                                PageStreaming::kNoPage);
    uint32 slot = 0;
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
                if ((x * 7u + z * 3u + level) % 5u == 0u && slot < kSlots)
                {
                    const uint32 entry = PageStreaming::PackPageEntry(slot++, level);
                    table.SetEntry(PageStreaming::PageAddress{0, static_cast<uint8>(level), x, z}, entry);
                    entries[levels[level].FirstEntry + z * levels[level].PagesX + x] = entry;
                }
    table.SetSlotFade(5, 0.5f);
    const PageCacheGeometry geometry = MakePageCacheGeometry(kSlots);

    std::vector<uint32> expected(CBTTerrain::kCBTPageFadeOffsetWords, 0u);
    expected[0] = 8193u;
    expected[1] = 2049u;
    expected[2] = static_cast<uint32>(levels.size());
    expected[3] = geometry.SlotsPerRow;
    expected[4] = kSlots;
    for (uint32 level = 0; level < levels.size(); ++level)
    {
        expected[CBTTerrain::kCBTPageHeaderWords + 4u * level] = static_cast<uint32>(levels[level].FirstEntry);
        expected[CBTTerrain::kCBTPageHeaderWords + 4u * level + 1u] = levels[level].PagesX;
        expected[CBTTerrain::kCBTPageHeaderWords + 4u * level + 2u] = levels[level].PagesZ;
    }
    for (uint32 fade = 0; fade < kSlots; ++fade)
        expected.push_back(std::bit_cast<uint32>(fade == 5u ? 0.5f : 1.0f));
    expected.insert(expected.end(), entries.begin(), entries.end());

    std::vector<uint32> words;
    PackPageTableWords(table, geometry, words);
    EXPECT_EQ(words, expected);
}

TEST(TerrainPages, ATerrainDirtiesItsRectWhenItsSourceSwitchesOrItsPagesChangeAndNothingOnceSettled)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    const PagedTerrainRequest request = FlatTileRequest(257);
    const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(257, 257);
    const uint32 pyramidPages = static_cast<uint32>(PageStreaming::DensePageCount(levels));

    pages.Update(1, 1.0f / 60.0f, false, 2); // configures the field's cache
    pages.DirtyRects().clear();

    // The terrain turns paged: its whole rect re-evaluates at once, then again while its pages
    // arrive and fade in (0.4 s); a settled parked terrain publishes nothing.
    uint32 uploads = 0;
    uint64 lastDirtyFrame = 0;
    bool wholeOnSwitch = false;
    for (uint64 frame = 2; frame < 62; ++frame)
    {
        ASSERT_EQ(pages.NeedsSource(request.Terrain, request.SourceIdentity), frame == 2);
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        uploads += static_cast<uint32>(pages.Uploads().size());
        for (const HeightPageDirtyRect& rect : pages.DirtyRects())
        {
            EXPECT_EQ(rect.Terrain, request.Terrain);
            EXPECT_EQ(rect.TerrainGeneration, request.TerrainGeneration);
            wholeOnSwitch = wholeOnSwitch || (frame == 2 && CoversWholeTerrain(rect));
            lastDirtyFrame = frame;
        }
        pages.DirtyRects().clear();
        pages.Uploads().clear();
    }
    EXPECT_TRUE(wholeOnSwitch);
    EXPECT_EQ(uploads, pyramidPages) << "every page of the pinned pyramid uploads once";
    EXPECT_GT(lastDirtyFrame, 2u + 20u) << "the arrival fades publish their pages for 0.4 s";
    EXPECT_LT(lastDirtyFrame, 2u + 40u) << "a settled parked terrain publishes nothing";
    ASSERT_NE(pages.PackedTable(request.Terrain), nullptr);

    // Not requested for a frame: it goes back to its texture, its whole rect re-evaluated.
    pages.Update(62, 1.0f / 60.0f, false, 2);
    ASSERT_EQ(pages.DirtyRects().size(), 1u);
    EXPECT_TRUE(CoversWholeTerrain(pages.DirtyRects().front()));
    EXPECT_EQ(pages.PackedTable(request.Terrain), nullptr);
}

TEST(TerrainPages, AnOverlayOfRebuiltModifierDataRewritesEveryPageTheOldOverlayReached)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(257, 257);
    const PageStreaming::PageSampleRect raised{240, 240, 256, 256}; // the far corner: nine pages read it
    PagedTerrainRequest modified = FlatTileRequest(257);
    auto layer = std::make_shared<PageStreaming::HeightPageLayer>(1u, levels);
    layer->WriteLevel0(0, raised, std::vector<float32>(raised.Width() * raised.Height(), 5.0f));
    modified.Overlay = layer;
    modified.OverlayVersion = 3;
    modified.OverlayChanged = raised;

    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    for (; frame < 62; ++frame)
    {
        pages.Request(modified, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }

    // A scene load rebuilt the terrain's data with no modifier: its first overlay version (none
    // built) is not derived from the one the resident pages hold.
    const PagedTerrainRequest rebuilt = FlatTileRequest(257);
    uint32 rewritten = 0;
    for (; frame < 72; ++frame)
    {
        pages.Request(rebuilt, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        for (const HeightPageUpload& upload : pages.Uploads())
        {
            ++rewritten;
            for (float32 sample : PageStreaming::HeightSamples(upload.Bytes))
                ASSERT_LT(sample, 1.0f) << "a rewritten page carries no overlay";
        }
    }
    uint32 reached = 0;
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
                reached += PageStreaming::PageReadsLevel0Rect(
                               PageStreaming::PageAddress{0, static_cast<uint8>(level), x, z}, raised,
                               PageStreaming::kPageApronSamples)
                               ? 1u
                               : 0u;
    EXPECT_EQ(rewritten, reached) << "every page of the pinned pyramid that read the old overlay, and no other";
}

namespace
{

// A flat 4097 x 4097 field (16 x 16 tiles of 257) in 64 slots: too many pages to pin whole, so its
// level 0 streams around the camera.
PagedTerrainRequest StreamedFieldRequest(float32 originX, uint64 identity)
{
    PageStreaming::GeneratedHeightLattice lattice;
    lattice.Source = PageStreaming::GeneratedHeightSource::Flat;
    lattice.TileWorldSize = 256.0f;
    lattice.TileSamples = 257;
    lattice.TilesX = 16;
    lattice.TilesZ = 16;
    PagedTerrainRequest request;
    request.Terrain = 0;
    request.TerrainGeneration = 1;
    request.SourceIdentity = identity;
    request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
    request.Shape.OriginX = originX;
    request.Shape.Level0TexelX = 1.0f;
    request.Shape.Level0TexelZ = 1.0f;
    request.Shape.SamplesX = 4097;
    request.Shape.SamplesZ = 4097;
    request.Shape.MaxHeight = 1.0f;
    return request;
}

// The published table entry of page (x, z) of `level` (the binding-22 layout the packing test pins).
uint32 PublishedEntry(const TerrainPages& pages, uint32 terrain, uint32 level, uint32 x, uint32 z)
{
    const PackedPageTable* table = pages.PackedTable(terrain);
    if (table == nullptr)
        return PageStreaming::kNoPage;
    const std::span<const uint32> words = table->Words();
    const uint32 entriesAt = CBTTerrain::kCBTPageFadeOffsetWords + words[4];
    const uint32* shape = words.data() + CBTTerrain::kCBTPageHeaderWords + 4u * level;
    return words[entriesAt + shape[0] + z * shape[1] + x];
}

// The published table entry of level-0 page (x, z).
uint32 Level0Entry(const TerrainPages& pages, uint32 terrain, uint32 x, uint32 z)
{
    return PublishedEntry(pages, terrain, 0u, x, z);
}

// The terrain-UV footprint of `address` on StreamedFieldRequest's 4097-sample field, with one
// sample of the page's level on each side for the bilinear tap: what a page that streams in,
// leaves or fades dirties.
HeightPageDirtyRect StreamedFieldFootprint(const PageStreaming::PageAddress& address)
{
    const float32 span = static_cast<float32>(PageStreaming::kPageOwnedSamples << address.Level);
    const float32 margin = static_cast<float32>(1u << address.Level);
    const float32 inv = 1.0f / 4096.0f;
    HeightPageDirtyRect rect;
    rect.MinU = std::max(0.0f, (static_cast<float32>(address.X) * span - margin) * inv);
    rect.MaxU = std::min(1.0f, (static_cast<float32>(address.X + 1u) * span + margin) * inv);
    rect.MinV = std::max(0.0f, (static_cast<float32>(address.Z) * span - margin) * inv);
    rect.MaxV = std::min(1.0f, (static_cast<float32>(address.Z + 1u) * span + margin) * inv);
    return rect;
}

} // namespace

TEST(TerrainPages, AMovedTerrainStreamsThePagesUnderTheCameraAtItsNewPlacement)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    const PagedTerrainRequest before = StreamedFieldRequest(0.0f, 77);
    const Mathematics::Vector3 cameraBefore{100.0f, 10.0f, 100.0f};
    for (; frame < 40; ++frame)
    {
        pages.Request(before, {&cameraBefore, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    ASSERT_NE(Level0Entry(pages, 0, 0, 0), PageStreaming::kNoPage) << "page (0, 0) streams in under the camera";
    pages.DirtyRects().clear();

    // The terrain moves 3 km east with the same source; the camera moves with it.
    const PagedTerrainRequest after = StreamedFieldRequest(3000.0f, 77);
    const Mathematics::Vector3 cameraAfter{3100.0f, 10.0f, 100.0f};
    pages.Request(after, {&cameraAfter, 1}, view);
    pages.Update(frame++, 1.0f / 60.0f, false, 2);
    ASSERT_FALSE(pages.DirtyRects().empty());
    EXPECT_TRUE(CoversWholeTerrain(pages.DirtyRects().front())) << "every corner moved with the terrain";
    for (; frame < 200; ++frame)
    {
        pages.Request(after, {&cameraAfter, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    EXPECT_NE(Level0Entry(pages, 0, 0, 0), PageStreaming::kNoPage) << "the level-0 page under the camera";
    EXPECT_EQ(Level0Entry(pages, 0, 23, 0), PageStreaming::kNoPage)
        << "the page 2.9 km away, under the camera's place at the old placement, is released";
}

namespace
{

// The union of the dirty rects `request`'s pinned pages publish when an edit raises level-0 samples
// `edit` by 5 m: the pages settle first (their arrival fades end), then the edit's rewrites land.
HeightPageDirtyRect EditDirtyUnion(PagedTerrainRequest request, const PageStreaming::PageSampleRect& edit)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    const float32 center = 0.5f * static_cast<float32>(request.Shape.SamplesX - 1u);
    const Mathematics::Vector3 camera{center, 10.0f, center};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    for (; frame < 62; ++frame)
    {
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    pages.DirtyRects().clear();
    pages.Uploads().clear();

    const std::vector<PageStreaming::PageStoreLevel> levels =
        PageStreaming::BuildPageStoreLevels(request.Shape.SamplesX, request.Shape.SamplesZ);
    auto layer = std::make_shared<PageStreaming::HeightPageLayer>(1u, levels);
    layer->WriteLevel0(0, edit, std::vector<float32>(edit.Width() * edit.Height(), 5.0f));
    request.Overlay = layer;
    request.OverlayVersion = 1;
    request.OverlayChangedSince = 0; // derived from the pages' overlay (none)
    request.OverlayChanged = edit;
    HeightPageDirtyRect dirty{request.Terrain, request.TerrainGeneration, 1.0f, 1.0f, 0.0f, 0.0f};
    uint32 rewrites = 0;
    for (; frame < 72; ++frame)
    {
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        rewrites += static_cast<uint32>(pages.Uploads().size());
        for (const HeightPageDirtyRect& rect : pages.DirtyRects())
        {
            dirty.MinU = std::min(dirty.MinU, rect.MinU);
            dirty.MinV = std::min(dirty.MinV, rect.MinV);
            dirty.MaxU = std::max(dirty.MaxU, rect.MaxU);
            dirty.MaxV = std::max(dirty.MaxV, rect.MaxV);
        }
        pages.DirtyRects().clear();
        pages.Uploads().clear();
    }
    EXPECT_GT(rewrites, 0u) << "the edit rewrote no page";
    return dirty;
}

} // namespace

TEST(TerrainPages, AnEditOnALevelZeroPageDirtiesTheEditAndOneSampleAroundItNotThePage)
{
    // One level-0 page holds the whole 128-sample terrain; the edit is samples 40 to 50. The
    // surface reads a sample through the bilinear tap of the cells on either side of it.
    const HeightPageDirtyRect dirty = EditDirtyUnion(FlatTileRequest(128), PageStreaming::PageSampleRect{40, 40, 50, 50});
    EXPECT_FLOAT_EQ(dirty.MinU, 39.0f / 127.0f);
    EXPECT_FLOAT_EQ(dirty.MinV, 39.0f / 127.0f);
    EXPECT_FLOAT_EQ(dirty.MaxU, 51.0f / 127.0f);
    EXPECT_FLOAT_EQ(dirty.MaxV, 51.0f / 127.0f);
}

TEST(TerrainPages, AnEditDirtiesWhatItReachesAtEachCoarseLevelNotTheCoarsePagesFootprints)
{
    // Samples 100 to 110 of the 257-sample terrain. The [1 2 1] tent reaches level-1 samples 50 to
    // 55 and level-2 samples 25 to 28 (level-0 samples 100 to 112); one level-2 sample on each
    // side for the bilinear tap gives level-0 samples 96 to 116, the widest reach, while the
    // level-2 page covers the whole terrain.
    const HeightPageDirtyRect dirty = EditDirtyUnion(FlatTileRequest(257), PageStreaming::PageSampleRect{100, 100, 110, 110});
    EXPECT_FLOAT_EQ(dirty.MinU, 96.0f / 256.0f);
    EXPECT_FLOAT_EQ(dirty.MinV, 96.0f / 256.0f);
    EXPECT_FLOAT_EQ(dirty.MaxU, 116.0f / 256.0f);
    EXPECT_FLOAT_EQ(dirty.MaxV, 116.0f / 256.0f);
}

TEST(TerrainPages, AStreamedInPageDirtiesItsWholeFootprint)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    const PagedTerrainRequest request = StreamedFieldRequest(0.0f, 77);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    const Mathematics::Vector3 before{100.0f, 10.0f, 100.0f};
    for (; frame < 40; ++frame)
    {
        pages.Request(request, {&before, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    ASSERT_EQ(Level0Entry(pages, 0, 8, 0), PageStreaming::kNoPage) << "page (8, 0) is 1 km from the camera";
    pages.DirtyRects().clear();

    // The camera moves 1 km east: level-0 page (8, 0), samples 1024 to 1151, streams in under it.
    const Mathematics::Vector3 after{1100.0f, 10.0f, 100.0f};
    bool arrived = false;
    for (; frame < 200 && !arrived; ++frame)
    {
        pages.Request(request, {&after, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        arrived = Level0Entry(pages, 0, 8, 0) != PageStreaming::kNoPage;
        if (arrived)
        {
            // Every page that arrived, left or faded this frame dirties its whole footprint.
            const PageStreaming::PageCache& cache = pages.Residency().FieldCache(kHeightPageField);
            std::vector<PageStreaming::CachedPage> changed = cache.Transitions();
            changed.insert(changed.end(), cache.FadeChanges().begin(), cache.FadeChanges().end());
            HeightPageDirtyRect expected = StreamedFieldFootprint(changed.front().Address);
            for (const PageStreaming::CachedPage& page : changed)
            {
                const HeightPageDirtyRect footprint = StreamedFieldFootprint(page.Address);
                expected.MinU = std::min(expected.MinU, footprint.MinU);
                expected.MinV = std::min(expected.MinV, footprint.MinV);
                expected.MaxU = std::max(expected.MaxU, footprint.MaxU);
                expected.MaxV = std::max(expected.MaxV, footprint.MaxV);
            }
            ASSERT_EQ(pages.DirtyRects().size(), 1u);
            const HeightPageDirtyRect& dirty = pages.DirtyRects().front();
            EXPECT_FLOAT_EQ(dirty.MinU, expected.MinU) << changed.size() << " pages changed";
            EXPECT_FLOAT_EQ(dirty.MinV, expected.MinV);
            EXPECT_FLOAT_EQ(dirty.MaxU, expected.MaxU);
            EXPECT_FLOAT_EQ(dirty.MaxV, expected.MaxV);
        }
        pages.DirtyRects().clear();
    }
    EXPECT_TRUE(arrived) << "page (8, 0) never streamed in";
}

namespace
{

// The pages per side of each level of FlatTileRequest(257).
constexpr std::array<uint32, 3> kFlatTilePagesPerSide{3u, 2u, 1u};

// A FlatTileRequest(257) terrain in 64 slots under one camera, a frame at a time. `Atlas` holds what each
// slot of the cache texture holds: every upload lands in it.
struct DrawnFlatTile
{
    static constexpr uint32 kSlots = 64;

    TerrainPages Pages{nullptr, nullptr, HeightPageProfile{kSlots, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile()};
    PagedTerrainRequest Request = FlatTileRequest(257);
    std::vector<std::vector<uint8>> Atlas = std::vector<std::vector<uint8>>(kSlots);
    uint64 Frame = 2;

    DrawnFlatTile() { Pages.Update(1, 1.0f / 60.0f, false, 2); }

    void Run(uint32 frames)
    {
        const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
        const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
        for (const uint64 end = Frame + frames; Frame < end; ++Frame)
        {
            Pages.Request(Request, {&camera, 1}, view);
            Pages.Update(Frame, 1.0f / 60.0f, false, 2);
            for (const HeightPageUpload& upload : Pages.Uploads())
                Atlas.at(upload.Slot) = upload.Bytes;
            Pages.Uploads().clear();
        }
    }

    bool EveryPageHasItsEntry() const
    {
        for (uint32 level = 0; level < kFlatTilePagesPerSide.size(); ++level)
            for (uint32 z = 0; z < kFlatTilePagesPerSide[level]; ++z)
                for (uint32 x = 0; x < kFlatTilePagesPerSide[level]; ++x)
                    if (PublishedEntry(Pages, Request.Terrain, level, x, z) == PageStreaming::kNoPage)
                        return false;
        return true;
    }

    // The height the GPU reads at level-0 sample (x, z): through the published table into the atlas.
    float32 DrawnHeight(uint32 x, uint32 z) const
    {
        const uint32 entry = Level0Entry(Pages, Request.Terrain, x / PageStreaming::kPageOwnedSamples,
                                         z / PageStreaming::kPageOwnedSamples);
        const std::vector<uint8>& texels = Atlas.at(PageStreaming::PageEntrySlot(entry));
        const uint32 column = x % PageStreaming::kPageOwnedSamples + PageStreaming::kPageApronSamples;
        const uint32 row = z % PageStreaming::kPageOwnedSamples + PageStreaming::kPageApronSamples;
        const std::span<const float32> heights = PageStreaming::HeightSamples(std::span<const uint8>(texels));
        return heights[row * PageStreaming::kPageStrideSamples + column];
    }
};

} // namespace

// A scene reopened while its terrain's modifiers bake: the terrain streams for the CPU only for a frame,
// then the GPU draws its pages again. Every page it holds must resolve, or its samples draw at height 0.
TEST(TerrainPages, ATerrainReopenedInTheSameSessionDrawsEveryPageItHolds)
{
    DrawnFlatTile tile;
    tile.Run(60);
    ASSERT_TRUE(tile.EveryPageHasItsEntry()) << "the whole pyramid streams in";

    tile.Request.TerrainGeneration += 1;
    tile.Request.GpuPages = false;
    tile.Run(1);
    tile.Request.GpuPages = true;
    tile.Run(1);

    EXPECT_TRUE(tile.EveryPageHasItsEntry()) << "the reopened terrain draws every page it holds";
}

// An edit that arrives while the terrain streams for the CPU only (its modifiers bake, or no view draws
// it) reaches the slots of the pages it covers: the GPU draws the edit when it draws the pages again.
TEST(TerrainPages, AnEditMadeWhileTheTerrainStreamsForTheCpuOnlyIsDrawnWhenTheGpuDrawsItsPagesAgain)
{
    DrawnFlatTile tile;
    tile.Run(60);
    ASSERT_NE(Level0Entry(tile.Pages, tile.Request.Terrain, 0, 0), PageStreaming::kNoPage);
    ASSERT_FLOAT_EQ(tile.DrawnHeight(105, 105), 0.0f) << "the tile is flat";

    tile.Request.GpuPages = false;
    tile.Run(1); // the switch to the texture dirties the whole terrain once
    tile.Pages.DirtyRects().clear();
    tile.Pages.ChangeRects().clear();
    const PageStreaming::PageSampleRect edit{100, 100, 110, 110};
    auto layer = std::make_shared<PageStreaming::HeightPageLayer>(
        1u, PageStreaming::BuildPageStoreLevels(tile.Request.Shape.SamplesX, tile.Request.Shape.SamplesZ));
    layer->WriteLevel0(0, edit, std::vector<float32>(edit.Width() * edit.Height(), 5.0f));
    tile.Request.Overlay = layer;
    tile.Request.OverlayVersion = 1;
    tile.Request.OverlayChangedSince = 0;
    tile.Request.OverlayChanged = edit;
    tile.Run(5);
    EXPECT_TRUE(tile.Pages.DirtyRects().empty()) << "no dirty rect reaches the renderer while it draws the texture";
    EXPECT_TRUE(tile.Pages.ChangeRects().empty()) << "no change rect reaches the renderer while it draws the texture";
    tile.Request.GpuPages = true;
    tile.Run(1);

    ASSERT_NE(Level0Entry(tile.Pages, tile.Request.Terrain, 0, 0), PageStreaming::kNoPage)
        << "the page over the edit resolves";
    EXPECT_FLOAT_EQ(tile.DrawnHeight(105, 105), 5.0f) << "the GPU draws the edit, not the height before it";
}

TEST(TerrainPages, ATerrainPagedAgainAtTheSameIndexNeverRepeatsATableVersion)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    const Mathematics::Vector3 camera{100.0f, 10.0f, 100.0f};
    std::vector<uint64> firstVersions;
    uint64 frame = 2;
    for (; frame < 6; ++frame)
    {
        pages.Request(StreamedFieldRequest(0.0f, 77), {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        const PackedPageTable* table = pages.PackedTable(0);
        ASSERT_NE(table, nullptr);
        firstVersions.push_back(table->Version());
    }
    pages.Update(frame++, 1.0f / 60.0f, false, 2); // not requested: forgotten
    pages.Request(StreamedFieldRequest(0.0f, 78), {&camera, 1}, view);
    pages.Update(frame, 1.0f / 60.0f, false, 2);
    const PackedPageTable* table = pages.PackedTable(0);
    ASSERT_NE(table, nullptr);
    const uint64 version = table->Version();
    for (uint64 seen : firstVersions)
        EXPECT_NE(version, seen) << "the CBT's per-ring-slot upload check would keep the old terrain's table";
}

TEST(TerrainPages, The60By15KmTerrainAtAQuarterMeterKeepsOneTierOnDesktopAndIsTieredOnTheDeck)
{
    // 60 x 15 km at 0.25 m is 240,001 x 60,001 samples: 1,174,877 dense words with the desktop's 512
    // slots, within its 2^21; 1,174,621 with the Deck's 256, past its 2^19, so the Deck tiers it.
    PageTable desktop;
    desktop.Reset(240001, 60001, 1u, kDesktopHeightPages.Slots,
                  PageTableBudget{kDesktopHeightPages.DenseTableWords, kDesktopHeightPages.TableMidBlocks,
                                  kDesktopHeightPages.TableLeafBlocks});
    EXPECT_EQ(desktop.FloorLevel(), 0u);
    EXPECT_EQ(desktop.TierCount(), 0u);
    EXPECT_EQ(CBTTerrain::kCBTPageFadeOffsetWords + kDesktopHeightPages.Slots + desktop.Words().size(), 1174877u);

    PageTable deck;
    deck.Reset(240001, 60001, 1u, kSteamDeckHeightPages.Slots,
               PageTableBudget{kSteamDeckHeightPages.DenseTableWords, kSteamDeckHeightPages.TableMidBlocks,
                               kSteamDeckHeightPages.TableLeafBlocks});
    EXPECT_GT(deck.FloorLevel(), 0u);
    ASSERT_EQ(deck.TierCount(), 1u) << "the floor is within six levels of level 0";
    EXPECT_LT(deck.Words().size(), 1174621u) << "the tiered table is smaller than the dense one";
}

TEST(TerrainPages, A1KmSquareModifierOnAQuarterMeterTerrainFitsEveryOverlayCapAndATerrainWideOneIsRefused)
{
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(240001, 60001);
    // 1 km x 1 km at 0.25 m: 4001 x 4001 samples and the bake's one-sample margin on each side.
    const PageStreaming::PageSampleRect square{100000, 20000, 104002, 24002};
    const uint64 local = PageStreaming::HeightPageLayerBytesOver(levels, std::span(&square, 1));
    EXPECT_GT(local, 4003ull * 4003ull * sizeof(float32)) << "the coarser levels hold samples too";
    EXPECT_EQ(OverlayRefusal("Outer", local, kDesktopHeightPages), "");
    EXPECT_EQ(OverlayRefusal("Outer", local, kSteamDeckHeightPages), "");
    // A modifier over the whole 60 x 15 km terrain reaches all of its 14.4 G samples.
    const PageStreaming::PageSampleRect whole{0, 0, 240000, 60000};
    const uint64 wide = PageStreaming::HeightPageLayerBytesOver(levels, std::span(&whole, 1));
    const std::string refusal = OverlayRefusal("Outer", wide, kDesktopHeightPages);
    EXPECT_NE(refusal.find("Outer does not stream its height"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find(std::format("needs {} bytes", wide)), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("cap of 536870912 bytes"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("Shrink the modifiers' bounds"), std::string::npos) << refusal;
}

// A power-of-two raw heightmap (the 0.25 m canyon, 16384 x 4096) is off its 16 x 4 tile lattice of
// 1025-sample tiles (16385 x 4097): it pages, but takes no modifier overlay, and the refusal names
// the lattice to import at.
TEST(TerrainPages, AStoreOffTheTileLatticeTakesNoModifierOverlayAndTheRefusalNamesTheLattice)
{
    const HeightPageOverlayAllowance off = PageOverlayAllowance(true, 16384, 4096, 16385, 4097, kDesktopHeightPages);
    EXPECT_EQ(off.ByteCap, kDesktopHeightPages.OverlayByteCap);
    EXPECT_TRUE(off.OffGrid);
    const HeightPageOverlayAllowance on = PageOverlayAllowance(true, 8193, 2049, 8193, 2049, kDesktopHeightPages);
    EXPECT_EQ(on.ByteCap, kDesktopHeightPages.OverlayByteCap);
    EXPECT_FALSE(on.OffGrid);
    const HeightPageOverlayAllowance texture = PageOverlayAllowance(false, 16384, 4096, 16385, 4097, kDesktopHeightPages);
    EXPECT_EQ(texture.ByteCap, 0u) << "a terrain that keeps its texture holds no overlay";

    const std::string refusal = OverlayGridRefusal("Canyon", 16384, 4096, 16385, 4097);
    EXPECT_NE(refusal.find("Canyon does not stream its height while height modifiers reach it"), std::string::npos)
        << refusal;
    EXPECT_NE(refusal.find("(16384 x 4096 samples)"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("Import the heightmap at 16385 x 4097 samples"), std::string::npos) << refusal;
}

TEST(TerrainPageFeature, ATableBecomesReadableOnlyWithTheUploadPassThatRecordsItsPages)
{
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";
    {
        TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                           DesktopSurfacePageProfile());
        const PagedTerrainRequest request = FlatTileRequest(257);
        const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
        const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
        pages.Update(1, 1.0f / 60.0f, false, 2);
        for (uint64 frame = 2; frame < 20 && pages.Uploads().empty(); ++frame)
        {
            pages.Request(request, {&camera, 1}, view);
            pages.Update(frame, 1.0f / 60.0f, false, 2);
        }
        ASSERT_FALSE(pages.Uploads().empty());
        const PackedPageTable* table = pages.PackedTable(request.Terrain);
        ASSERT_NE(table, nullptr);
        const uint64 published = table->Version();

        TerrainPageFeature feature;
        feature.Publish(pages);
        feature.Latch(*device, 1);
        ASSERT_TRUE(feature.HasStagedUploads());
        std::span<const uint32> words;
        uint64 version = 0;
        EXPECT_FALSE(feature.LatchedTable(request.Terrain, words, version))
            << "the table names slots whose upload pass is not declared yet";
        feature.Latch(*device, 2); // a frame whose graph declared no upload pass
        EXPECT_FALSE(feature.LatchedTable(request.Terrain, words, version));

        feature.CommitLatch(); // the upload pass that flushes the staged pages is declared
        ASSERT_TRUE(feature.LatchedTable(request.Terrain, words, version));
        EXPECT_EQ(version, published);
        auto list = device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
        list->Begin();
        feature.FlushUploads(*list);
        list->End();
        device->ExecuteCommandLists({list.get()});
        device->WaitForIdle();
        EXPECT_FALSE(feature.HasStagedUploads());
    }
    device->Shutdown();
}

// The arrival fades change the table version every frame, so each frame's Publish takes the snapshot
// that the previous commit released back to the pool.
TEST(TerrainPageFeature, ASnapshotTakenFromThePoolHoldsTheWordsOfThePublishedVersion)
{
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";
    {
        TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                           DesktopSurfacePageProfile());
        const PagedTerrainRequest request = FlatTileRequest(257);
        const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
        const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
        TerrainPageFeature feature;
        pages.Update(1, 1.0f / 60.0f, false, 2);
        constexpr uint64 kLastFrame = 14; // inside the 0.4 s arrival fade of the first pages
        uint64 previousVersion = 0;
        uint32 versionChanges = 0;
        for (uint64 frame = 2; frame <= kLastFrame; ++frame)
        {
            pages.Request(request, {&camera, 1}, view);
            pages.Update(frame, 1.0f / 60.0f, false, 2);
            const PackedPageTable* table = pages.PackedTable(request.Terrain);
            ASSERT_NE(table, nullptr);
            const std::vector<uint32> published(table->Words().begin(), table->Words().end());
            const uint64 publishedVersion = table->Version();
            if (previousVersion != 0 && publishedVersion != previousVersion)
                ++versionChanges;
            previousVersion = publishedVersion;

            feature.Publish(pages);
            feature.Latch(*device, frame);
            feature.CommitLatch();
            auto list = device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
            list->Begin();
            feature.FlushUploads(*list);
            list->End();
            device->ExecuteCommandLists({list.get()});
            device->WaitForIdle();

            std::span<const uint32> words;
            uint64 version = 0;
            ASSERT_TRUE(feature.LatchedTable(request.Terrain, words, version)) << "frame " << frame;
            EXPECT_EQ(version, publishedVersion) << "frame " << frame;
            EXPECT_TRUE(std::ranges::equal(words, published)) << "frame " << frame << ": the words of another version";
        }
        EXPECT_GE(versionChanges, 3u) << "too few new versions to take a snapshot back from the pool";
    }
    device->Shutdown();
}

TEST(TerrainPageFeature, TheCacheTextureIsReadableOnlyFromTheLatchAfterItsClear)
{
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";
    {
        TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                           DesktopSurfacePageProfile());
        const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
        const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
        pages.Update(1, 1.0f / 60.0f, false, 2);
        for (uint64 frame = 2; frame < 20 && pages.Uploads().empty(); ++frame)
        {
            pages.Request(FlatTileRequest(257), {&camera, 1}, view);
            pages.Update(frame, 1.0f / 60.0f, false, 2);
        }
        ASSERT_FALSE(pages.Uploads().empty());

        TerrainPageFeature feature;
        feature.Publish(pages);
        feature.Latch(*device, 1); // creates the cache and stages its first pages
        ASSERT_TRUE(feature.HasStagedUploads());
        EXPECT_FALSE(feature.CacheTexture().IsValid())
            << "a consumer of this frame could run before the upload pass clears the new cache";
        feature.Latch(*device, 2); // a frame whose upload pass never ran
        EXPECT_FALSE(feature.CacheTexture().IsValid()) << "the cache was never cleared";
        feature.CommitLatch();
        auto list = device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
        list->Begin();
        feature.FlushUploads(*list);
        list->End();
        device->ExecuteCommandLists({list.get()});
        device->WaitForIdle();
        EXPECT_FALSE(feature.CacheTexture().IsValid()) << "the clear is recorded in this frame";
        feature.Latch(*device, 3);
        EXPECT_TRUE(feature.CacheTexture().IsValid());
    }
    device->Shutdown();
}

namespace
{

// One render frame of the page feature over `pages`: request `cameras`, update, publish, latch,
// commit with a declared upload pass and record the uploads.
void StepPagesAndFeature(TerrainPages& pages, TerrainPageFeature& feature, Rendering::IDevice& device,
                         const PagedTerrainRequest& request, std::span<const Mathematics::Vector3> cameras,
                         uint64 frame)
{
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Request(request, cameras, view);
    pages.Update(frame, 1.0f / 60.0f, false, 2);
    feature.Publish(pages);
    feature.Latch(device, frame);
    feature.CommitLatch();
    auto list = device.CreateCommandList(Rendering::IDevice::QueueType::Graphics);
    list->Begin();
    feature.FlushUploads(*list);
    list->End();
    device.ExecuteCommandLists({list.get()});
    device.WaitForIdle();
}

// The terrain-UV rect of a grass cell window of +-48 m about world (x, z) on StreamedFieldRequest's
// 4096 m field.
Mathematics::Rect WindowAbout(float32 x, float32 z)
{
    constexpr float32 kField = 4096.0f;
    return Mathematics::Rect::FromEdges((x - 48.0f) / kField, (z - 48.0f) / kField, (x + 48.0f) / kField,
                                        (z + 48.0f) / kField);
}

uint64 SerialOver(const TerrainPageFeature& feature, const Mathematics::Rect& rect)
{
    return feature.ChangeSerialOver(0u, rect);
}

} // namespace

// A consumer that keeps what it read from the pages under one window (the grass placement) sees a
// change only when a page under that window changes: pages that stream in 1 km away advance the
// change serial over their own footprint and leave it over the window.
TEST(TerrainPageFeature, APageThatStreamsInAdvancesTheChangeSerialOverItsFootprintAndNotElsewhere)
{
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";
    {
        TerrainPages pages(nullptr, nullptr, HeightPageProfile{256, kDesktopHeightPages.DenseTableWords},
                           DesktopSurfacePageProfile());
        TerrainPageFeature feature;
        const PagedTerrainRequest request = StreamedFieldRequest(0.0f, 77);
        const Mathematics::Vector3 home{100.0f, 10.0f, 100.0f};
        const std::array<Mathematics::Vector3, 2> both = {home, Mathematics::Vector3{1100.0f, 10.0f, 100.0f}};
        pages.Update(1, 1.0f / 60.0f, false, 2);
        uint64 frame = 2;
        for (; frame < 150; ++frame) // the pages under the camera arrive and settle
            StepPagesAndFeature(pages, feature, *device, request, {&home, 1}, frame);
        ASSERT_NE(Level0Entry(pages, 0, 0, 0), PageStreaming::kNoPage) << "page (0, 0) streams in under the camera";
        ASSERT_EQ(Level0Entry(pages, 0, 8, 0), PageStreaming::kNoPage) << "page (8, 0) is 1 km from the camera";
        const Mathematics::Rect window = WindowAbout(home.x, home.z);
        const Mathematics::Rect east = WindowAbout(1100.0f, 100.0f);
        const uint64 windowBefore = SerialOver(feature, window);
        const uint64 eastBefore = SerialOver(feature, east);
        EXPECT_GT(windowBefore, 0u) << "the pages under the window committed no change";

        // A second camera 1 km east: level-0 page (8, 0) and its ancestors stream in under it.
        bool arrived = false;
        for (; frame < 400 && !arrived; ++frame)
        {
            StepPagesAndFeature(pages, feature, *device, request, both, frame);
            arrived = Level0Entry(pages, 0, 8, 0) != PageStreaming::kNoPage;
        }
        ASSERT_TRUE(arrived) << "page (8, 0) never streamed in";
        EXPECT_GT(SerialOver(feature, east), eastBefore) << "an upload under the east window did not change it";
        EXPECT_EQ(SerialOver(feature, window), windowBefore)
            << "pages that changed 1 km away changed the serial over the window about the camera";
    }
    device->Shutdown();
}

namespace
{

// One page of `plane` whose every byte is `seed` plus its index, so a copy at the wrong stride or offset
// reads different bytes.
std::vector<uint8> SurfacePlanePattern(PageStreaming::PageFieldKind plane, uint8 seed)
{
    std::vector<uint8> bytes(PageStreaming::PagePayloadBytes(SurfacePlaneFormat(plane)));
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8>(seed + i * 7u);
    return bytes;
}

// A surface source of `planes` (in PageFieldKind order) on a 257-sample lattice whose stores hold no page.
TerrainSurfacePageSource EmptySurfaceSource(std::span<const PageStreaming::PageFieldKind> planes)
{
    TerrainSurfacePageSource source;
    for (PageStreaming::PageFieldKind plane : planes)
    {
        PageStreaming::PageStoreHeader header;
        header.Format = SurfacePlaneFormat(plane);
        header.SamplesX = 257u;
        header.SamplesZ = 257u;
        source.Planes.push_back(std::make_shared<const PageStreaming::PageStoreAccess>(plane, header));
        const uint32 bit = static_cast<uint32>(plane) - static_cast<uint32>(PageStreaming::PageFieldKind::Splat);
        source.PlaneMask |= 1u << bit;
    }
    source.PinnedLevelsBaked = true;
    return source;
}

// Asks for the surface pages of render terrain `terrain` over `source`, seen from far above.
void RequestSurface(TerrainPages& pages, uint32 terrain, const TerrainSurfacePageSource& source)
{
    SurfacePagedRequest request;
    request.Terrain = terrain;
    request.Source = &source;
    request.Shape.Level0TexelX = 1.0f;
    request.Shape.Level0TexelZ = 1.0f;
    request.Shape.SamplesX = 257u;
    request.Shape.SamplesZ = 257u;
    request.Shape.MaxHeight = 1.0f;
    const PageRequestCamera camera{Mathematics::Vector3{0.0f, 1.0e6f, 0.0f}, {}, 0.0f, false};
    pages.Surface().Request(request, std::span(&camera, 1),
                            MakePageLevelView(kPageLevelReferenceRows, 1.0f, kSurfacePageTargetPixels));
}

// The bits (index in kSurfacePlanes) of the plane textures terrain `terrain` reads.
uint32 ReadablePlanes(const TerrainPageFeature& feature, uint32 terrain)
{
    uint32 planes = 0;
    for (std::size_t plane = 0; plane < kSurfacePlanes.size(); ++plane)
        planes |= feature.SurfacePlaneTexture(terrain, kSurfacePlanes[plane]).IsValid() ? 1u << plane : 0u;
    return planes;
}

} // namespace

// A surface page writes each of its planes into the same slot of that plane's cache texture, at the plane's
// own stride (136 for the BC5 normal, 130 for the others), and a plane no upload has is not created.
TEST(TerrainPageFeature, ASurfacePageWritesEachPlaneIntoTheSameSlotOfItsPlaneTexture)
{
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";
    {
        TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                           SurfacePageProfile{4u});
        const uint32 slotsPerRow = pages.Surface().Geometry().SlotsPerRow;
        ASSERT_EQ(slotsPerRow, 2u);
        // Grass control is absent; two uploads, so the second one's planes start past an odd byte count.
        const std::array<PageStreaming::PageFieldKind, 3> planes = {
            PageStreaming::PageFieldKind::Splat, PageStreaming::PageFieldKind::Normal, PageStreaming::PageFieldKind::Color};
        const TerrainSurfacePageSource source = EmptySurfaceSource(planes);
        const uint32 mask = source.PlaneMask;
        ASSERT_EQ(mask, 0b1011u);
        constexpr uint32 kTerrain = 1u;
        pages.Update(1, 1.0f / 60.0f, false, 2); // configures the fields
        RequestSurface(pages, kTerrain, source);
        pages.Update(2, 1.0f / 60.0f, false, 2);
        const std::array<uint32, 2> slots = {1u, 3u};
        for (uint32 slot : slots)
        {
            SurfacePageUpload upload{slot, mask, {}};
            for (PageStreaming::PageFieldKind plane : planes)
            {
                const std::vector<uint8> bytes = SurfacePlanePattern(plane, static_cast<uint8>(slot * 31u));
                upload.Bytes.insert(upload.Bytes.end(), bytes.begin(), bytes.end());
            }
            pages.Surface().Uploads().push_back(std::move(upload));
        }

        TerrainPageFeature feature;
        feature.Publish(pages);
        EXPECT_TRUE(pages.Surface().Uploads().empty()) << "the feature takes the surface uploads";
        feature.Latch(*device, 1);
        ASSERT_TRUE(feature.HasStagedUploads());
        EXPECT_FALSE(feature.SurfacePlaneTexture(kTerrain, PageStreaming::PageFieldKind::Splat).IsValid())
            << "a plane texture is readable only from the latch after its first upload";
        feature.CommitLatch();

        std::array<Rendering::BufferHandle, 2 * 3> readbacks{};
        auto list = device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
        list->Begin();
        feature.FlushUploads(*list);
        feature.Latch(*device, 2);
        for (std::size_t p = 0; p < planes.size(); ++p)
        {
            const Rendering::TextureHandle texture = feature.SurfacePlaneTexture(kTerrain, planes[p]);
            ASSERT_TRUE(texture.IsValid()) << SurfacePlaneName(planes[p]);
            const uint32 stride = PageStreaming::PageStrideTexels(SurfacePlaneFormat(planes[p]));
            list->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
                texture, Rendering::ResourceState::ShaderResource, Rendering::ResourceState::CopySource));
            for (std::size_t s = 0; s < slots.size(); ++s)
            {
                Rendering::BufferHandle& readback = readbacks[p * slots.size() + s];
                readback = device->CreateReadbackBuffer(PageStreaming::PagePayloadBytes(SurfacePlaneFormat(planes[p])));
                list->CopyTextureSubresourceToBuffer(texture, 0, 0, readback, stride, stride,
                                                     (slots[s] % slotsPerRow) * stride,
                                                     (slots[s] / slotsPerRow) * stride);
            }
        }
        EXPECT_FALSE(feature.SurfacePlaneTexture(kTerrain, PageStreaming::PageFieldKind::GrassControl).IsValid())
            << "a plane no upload has is not created";
        list->End();
        device->ExecuteCommandLists({list.get()});
        device->WaitForIdle();

        for (std::size_t p = 0; p < planes.size(); ++p)
            for (std::size_t s = 0; s < slots.size(); ++s)
            {
                const std::vector<uint8> expected = SurfacePlanePattern(planes[p], static_cast<uint8>(slots[s] * 31u));
                const Rendering::BufferHandle readback = readbacks[p * slots.size() + s];
                const uint8* mapped = static_cast<const uint8*>(device->MapBuffer(readback));
                ASSERT_NE(mapped, nullptr);
                const std::vector<uint8> actual(mapped, mapped + expected.size());
                device->UnmapBuffer(readback);
                EXPECT_EQ(actual, expected) << SurfacePlaneName(planes[p]) << " slot " << slots[s];
                device->DestroyBuffer(readback);
            }
    }
    device->Shutdown();
}

// A terrain reads only the plane textures of its own planes: not a plane that another terrain's pages
// wrote, and not a plane its surface no longer has once that plane's store closed.
TEST(TerrainPageFeature, ATerrainReadsOnlyThePlaneTexturesOfItsOwnPlanes)
{
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";
    {
        TerrainPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords},
                           SurfacePageProfile{4u});
        const std::array<PageStreaming::PageFieldKind, 2> splatNormal = {PageStreaming::PageFieldKind::Splat,
                                                                         PageStreaming::PageFieldKind::Normal};
        const TerrainSurfacePageSource twoPlanes = EmptySurfaceSource(splatNormal);
        const TerrainSurfacePageSource fourPlanes = EmptySurfaceSource(kSurfacePlanes);
        constexpr uint32 kTwoPlaneTerrain = 1u;
        constexpr uint32 kFourPlaneTerrain = 2u;
        pages.Update(1, 1.0f / 60.0f, false, 2); // configures the fields
        RequestSurface(pages, kTwoPlaneTerrain, twoPlanes);
        RequestSurface(pages, kFourPlaneTerrain, fourPlanes);
        pages.Update(2, 1.0f / 60.0f, false, 2);
        SurfacePageUpload upload{0u, fourPlanes.PlaneMask, {}};
        for (PageStreaming::PageFieldKind plane : kSurfacePlanes)
        {
            const std::vector<uint8> bytes = SurfacePlanePattern(plane, 5u);
            upload.Bytes.insert(upload.Bytes.end(), bytes.begin(), bytes.end());
        }
        pages.Surface().Uploads().push_back(std::move(upload));

        TerrainPageFeature feature;
        feature.Publish(pages);
        feature.Latch(*device, 1);
        feature.CommitLatch();
        auto list = device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
        list->Begin();
        feature.FlushUploads(*list);
        list->End();
        device->ExecuteCommandLists({list.get()});
        device->WaitForIdle();
        feature.Latch(*device, 2);
        EXPECT_EQ(ReadablePlanes(feature, kFourPlaneTerrain), 0b1111u) << "every plane texture exists";
        EXPECT_EQ(ReadablePlanes(feature, kTwoPlaneTerrain), 0b0011u)
            << "a terrain reads a plane texture that only another terrain's pages wrote";

        // The four-plane terrain's grass control and color stores close: its surface keeps two planes.
        TerrainSurfacePageSource closed = fourPlanes;
        closed.Planes.resize(2u);
        closed.PlaneMask = 0b0011u;
        RequestSurface(pages, kTwoPlaneTerrain, twoPlanes);
        RequestSurface(pages, kFourPlaneTerrain, closed);
        pages.Update(3, 1.0f / 60.0f, false, 2);
        feature.Publish(pages);
        feature.Latch(*device, 3);
        EXPECT_EQ(ReadablePlanes(feature, kFourPlaneTerrain), 0b0011u)
            << "a terrain reads the plane texture of a plane whose store closed";
    }
    device->Shutdown();
}

TEST(TerrainPages, APlanetTheGpuDoesNotDrawStreamsItsWantedPagesUploadsNoneAndIsForgottenWhenNotRequested)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{512, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    const PlanetPagedRequest request = FlatPlanetRequest(false);
    const uint32 faceSamples = request.Shape.FaceSamples;
    const Mathematics::Vector3 camera{2005.0f, 0.0f, 0.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);

    pages.Update(1, 1.0f / 60.0f, false, 2); // configures the field's cache
    for (uint64 frame = 2; frame < 40; ++frame)
    {
        EXPECT_EQ(pages.NeedsPlanetSource(request.Planet, request.SourceIdentity), frame == 2);
        pages.RequestPlanet(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        EXPECT_TRUE(pages.Uploads().empty()) << "the narrow arm draws no planet page";
        EXPECT_TRUE(pages.DirtyRects().empty());
        EXPECT_EQ(TerrainHeightPagesTestAccess::PlanetDirtyRegions(pages).Count, 0u);
    }
    std::vector<uint32> paged;
    pages.PagedTerrains(paged);
    EXPECT_TRUE(paged.empty());
    EXPECT_EQ(pages.PackedTable(request.RenderTerrain), nullptr);
    std::vector<PageStreamCounts> streams;
    pages.StreamCounts(streams);
    ASSERT_EQ(streams.size(), 1u);
    EXPECT_TRUE(streams[0].Planet);
    EXPECT_EQ(streams[0].Label, "planet");
    ASSERT_EQ(streams[0].Levels.size(), PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples).size());
    uint32 wanted = 0;
    for (const PageLevelCounts& level : streams[0].Levels)
    {
        EXPECT_EQ(level.Resident, level.Wanted);
        wanted += level.Wanted;
    }
    EXPECT_GT(streams[0].Levels.front().Wanted, 0u) << "the camera 5 m up needs level 0";
    EXPECT_GT(wanted, 6u);
    EXPECT_EQ(pages.Residency().FieldCache(kHeightPageField).ResidentCount(), wanted);

    pages.Update(40, 1.0f / 60.0f, false, 2);
    pages.StreamCounts(streams);
    EXPECT_TRUE(streams.empty());
}

TEST(TerrainPages, APlanetsResidentPagesReachItsTableWordsUploadsAndDirtyFacesUnderItsRenderTerrain)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{512, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    const PlanetPagedRequest request = FlatPlanetRequest(true);
    const Mathematics::Vector3 camera{2005.0f, 0.0f, 0.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);

    pages.Update(1, 1.0f / 60.0f, false, 2);
    std::vector<uint32> uploadedSlots;
    std::array<bool, PageStreaming::kPageStoreMaxFaces> dirtyFaces{};
    for (uint64 frame = 2; frame < 40; ++frame)
    {
        pages.RequestPlanet(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        for (const HeightPageUpload& upload : pages.Uploads())
        {
            EXPECT_EQ(upload.Bytes.size(), PageStreaming::kPageSampleCount * sizeof(float32));
            uploadedSlots.push_back(upload.Slot);
        }
        pages.Uploads().clear();
        const CBTTerrain::SphereEditRegions& dirty = TerrainHeightPagesTestAccess::PlanetDirtyRegions(pages);
        for (uint32 i = 0; i < dirty.Count; ++i)
        {
            const CBTTerrain::SphereFaceUVRect& rect = dirty.Rects[i];
            ASSERT_LT(rect.Face, PageStreaming::kPageStoreMaxFaces);
            EXPECT_FALSE(rect.IsEmpty());
            EXPECT_GE(rect.MinU, 0.0f);
            EXPECT_LE(rect.MaxU, 1.0f);
            dirtyFaces[rect.Face] = true;
        }
        TerrainHeightPagesTestAccess::PlanetDirtyRegions(pages) = {};
        EXPECT_TRUE(pages.DirtyRects().empty()) << "a planet's changes are face rects, not terrain-UV rects";
    }
    for (uint32 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
        EXPECT_TRUE(dirtyFaces[face]) << "face " << face << ": the switch to pages re-evaluates every face";

    std::vector<uint32> paged;
    pages.PagedTerrains(paged);
    ASSERT_EQ(paged, std::vector<uint32>{request.RenderTerrain});
    const PackedPageTable* table = pages.PackedTable(request.RenderTerrain);
    ASSERT_NE(table, nullptr);
    EXPECT_GT(table->Version(), 0u);
    const std::span<const uint32> words = table->Words();
    ASSERT_EQ(words[6], 0u) << "a 2 km planet at 2 m keeps a dense table";

    // Every resident page of the stream is its dense entry in the words, and its slot was uploaded;
    // no other entry is set.
    const PageStreaming::PageCache& cache = pages.Residency().FieldCache(kHeightPageField);
    const uint32 slots = words[4];
    const uint32 entriesOffset = CBTTerrain::kCBTPageFadeOffsetWords + slots;
    std::sort(uploadedSlots.begin(), uploadedSlots.end());
    uint32 resident = 0;
    for (const PageStreaming::CachedPage& page : cache.Resident())
    {
        const PageStreaming::PageAddress& address = page.Address;
        const uint32* shape = words.data() + CBTTerrain::kCBTPageHeaderWords + 4u * address.Level;
        const uint32 index = shape[0] + (address.Face * shape[2] + address.Z) * shape[1] + address.X;
        const uint32 slot = cache.SlotOf(page);
        EXPECT_EQ(words[entriesOffset + index], PageStreaming::PackPageEntry(slot, address.Level))
            << "face " << int(address.Face) << " L" << int(address.Level) << " (" << address.X << ","
            << address.Z << ")";
        EXPECT_TRUE(std::binary_search(uploadedSlots.begin(), uploadedSlots.end(), slot));
        ++resident;
    }
    uint32 set = 0;
    for (std::size_t i = entriesOffset; i < words.size(); ++i)
        set += words[i] != PageStreaming::kNoPage ? 1u : 0u;
    EXPECT_GT(resident, 6u);
    EXPECT_EQ(set, resident);
}

namespace
{

// True when every page of `level` on all six faces has an entry in a planet's dense table words.
bool EveryPageOfLevelHasItsEntry(std::span<const uint32> words, uint32 level)
{
    const uint32 entriesOffset = CBTTerrain::kCBTPageFadeOffsetWords + words[4];
    const uint32* shape = words.data() + CBTTerrain::kCBTPageHeaderWords + 4u * level;
    for (uint32 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
        for (uint32 z = 0; z < shape[2]; ++z)
            for (uint32 x = 0; x < shape[1]; ++x)
                if (words[entriesOffset + shape[0] + (face * shape[2] + z) * shape[1] + x] == PageStreaming::kNoPage)
                    return false;
    return true;
}

// The planet's table while it is published, or null.
const PackedPageTable* PublishedPlanetTable(const TerrainPages& pages, uint32 renderTerrain)
{
    std::vector<uint32> paged;
    pages.PagedTerrains(paged);
    const PackedPageTable* table = pages.PackedTable(renderTerrain);
    EXPECT_EQ(table != nullptr, paged == std::vector<uint32>{renderTerrain}) << "both lists publish the planet";
    return table;
}

// Runs `count` frames of the planet 5 m up and checks the published words before and after each update.
// Returns whether the planet is published after the last frame.
bool RunPlanetFrames(TerrainPages& pages, const PlanetPagedRequest& request, uint32 topLevel, uint64& frame,
                     uint64 count)
{
    const Mathematics::Vector3 camera{2005.0f, 0.0f, 0.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    bool published = false;
    for (const uint64 end = frame + count; frame < end; ++frame)
    {
        pages.RequestPlanet(request, {&camera, 1}, view);
        const PackedPageTable* table = PublishedPlanetTable(pages, request.RenderTerrain);
        EXPECT_TRUE(table == nullptr || EveryPageOfLevelHasItsEntry(table->Words(), topLevel))
            << "frame " << frame << ": a published planet resolves every sample";
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        table = PublishedPlanetTable(pages, request.RenderTerrain);
        EXPECT_TRUE(table == nullptr || EveryPageOfLevelHasItsEntry(table->Words(), topLevel)) << "frame " << frame;
        published = table != nullptr;
    }
    return published;
}

} // namespace

TEST(TerrainPages, APlanetIsPublishedOnlyOnceItsTopLevelIsResidentAndAgainAfterADeviceRestart)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{512, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    const PlanetPagedRequest request = FlatPlanetRequest(true);
    const uint32 topLevel = static_cast<uint32>(
        PageStreaming::BuildPageStoreLevels(request.Shape.FaceSamples, request.Shape.FaceSamples).size() - 1u);

    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    EXPECT_TRUE(RunPlanetFrames(pages, request, topLevel, frame, 38)) << "the pinned levels arrive";

    pages.RestartOnDeviceEpoch(1);
    EXPECT_EQ(PublishedPlanetTable(pages, request.RenderTerrain), nullptr) << "the restart empties the table";
    EXPECT_TRUE(RunPlanetFrames(pages, request, topLevel, frame, 38)) << "the pinned levels arrive again";
}

// An overlay with a 5 m bump on one face of a published planet: the resident pages under it are uploaded again
// with the bump, and the CBT re-evaluates around the bump on that face only, though no page changes its slot.
TEST(TerrainHeightPages, AnOverlayOnOneFaceOfAPlanetRewritesItsPagesWithTheBumpAndDirtiesThatFace)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{512, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    PlanetPagedRequest request = FlatPlanetRequest(true);
    const uint32 faceSamples = request.Shape.FaceSamples;
    const uint32 topLevel =
        static_cast<uint32>(PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples).size() - 1u);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    ASSERT_TRUE(RunPlanetFrames(pages, request, topLevel, frame, 38));
    pages.Uploads().clear();
    TerrainHeightPagesTestAccess::PlanetDirtyRegions(pages) = {};

    // The middle of the first resident level-0 page, and its slot.
    const PageStreaming::PageCache& cache = pages.Residency().FieldCache(kHeightPageField);
    const auto found = std::find_if(cache.Resident().begin(), cache.Resident().end(),
                                    [](const PageStreaming::CachedPage& p) { return p.Address.Level == 0u; });
    ASSERT_NE(found, cache.Resident().end());
    const PageStreaming::CachedPage page = *found;
    const PageStreaming::PageAddress address = page.Address;
    const uint32 slot = cache.SlotOf(page);
    const uint32 x = address.X * PageStreaming::kPageOwnedSamples + 64u;
    const uint32 z = address.Z * PageStreaming::kPageOwnedSamples + 64u;
    const PageStreaming::PageSampleRect bump{x, z, x, z};
    auto layer = std::make_shared<PageStreaming::HeightPageLayer>(
        PageStreaming::kPageStoreMaxFaces, PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples));
    layer->WriteLevel0(address.Face, bump, std::vector<float32>{5.0f});
    request.Overlay = layer;
    request.OverlayVersion = 1;
    request.OverlayChanged[address.Face] = bump;
    request.OverlayChangedSince = 0;

    float32 uploaded = 0.0f;
    std::array<bool, PageStreaming::kPageStoreMaxFaces> dirtyFaces{};
    float32 dirtyWidth = 0.0f; // the widest side of the bump face's dirty rect, in face UV
    const Mathematics::Vector3 camera{2005.0f, 0.0f, 0.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    for (const uint64 end = frame + 20; frame < end; ++frame)
    {
        pages.RequestPlanet(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        for (const HeightPageUpload& upload : pages.Uploads())
            if (upload.Slot == slot)
                uploaded = PageStreaming::HeightSamples(std::span<const uint8>(upload.Bytes))
                    [65u * PageStreaming::kPageStrideSamples + 65u];
    }
    const CBTTerrain::SphereEditRegions& dirty = TerrainHeightPagesTestAccess::PlanetDirtyRegions(pages);
    for (uint32 i = 0; i < dirty.Count; ++i)
    {
        const CBTTerrain::SphereFaceUVRect& rect = dirty.Rects[i];
        dirtyFaces[rect.Face] = true;
        if (rect.Face == address.Face)
            dirtyWidth = std::max(rect.MaxU - rect.MinU, rect.MaxV - rect.MinV);
    }
    EXPECT_EQ(cache.SlotOf(page), slot) << "the rewritten page keeps its slot";
    EXPECT_FLOAT_EQ(uploaded, 5.0f) << "the page holds the relief (0) plus the bump at its sample";
    EXPECT_TRUE(dirtyFaces[address.Face]) << "the bump's face is re-evaluated";
    // Each rewritten page dirties the bump's reach at its level, not its footprint: the top page spans the face.
    EXPECT_GT(dirtyWidth, 0.0f);
    EXPECT_LT(dirtyWidth, 0.05f) << "the bump's face is dirtied around the bump only";
    for (uint32 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
        EXPECT_TRUE(face == address.Face || !dirtyFaces[face]) << "face " << face << ": no edge is reached";
}

namespace
{

// The highest height of this frame's uploaded pages.
float32 LargestUploadedHeight(TerrainPages& pages)
{
    float32 largest = 0.0f;
    for (const HeightPageUpload& upload : pages.Uploads())
        for (const float32 height : PageStreaming::HeightSamples(std::span<const uint8>(upload.Bytes)))
            largest = std::max(largest, height);
    return largest;
}

} // namespace

// An overlay made for the planet's lattice, then a re-provision to a finer lattice with the same overlay: the
// restarted stream ignores it (its samples are the old lattice's), so every page is the relief alone.
TEST(TerrainHeightPages, APlanetIgnoresAnOverlayMadeForItsOldLattice)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{512, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    PlanetPagedRequest request = FlatPlanetRequest(true);
    const uint32 oldSamples = request.Shape.FaceSamples;
    const PageStreaming::PageSampleRect bump{oldSamples / 2u - 8u, oldSamples / 2u - 8u, oldSamples / 2u + 8u,
                                             oldSamples / 2u + 8u};
    auto layer = std::make_shared<PageStreaming::HeightPageLayer>(
        PageStreaming::kPageStoreMaxFaces, PageStreaming::BuildPageStoreLevels(oldSamples, oldSamples));
    layer->WriteLevel0(0, bump, std::vector<float32>(bump.Width() * bump.Height(), 5.0f));
    request.Overlay = layer;
    request.OverlayVersion = 1;
    request.OverlayChanged[0] = bump;
    const Mathematics::Vector3 camera{2005.0f, 0.0f, 0.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    float32 before = 0.0f;
    uint64 frame = 2;
    for (; frame < 40; ++frame)
    {
        pages.RequestPlanet(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        before = std::max(before, LargestUploadedHeight(pages));
    }
    EXPECT_GT(before, 0.0f) << "the overlay reaches the old lattice's pages";

    request.SourceIdentity = 2;
    request.Shape.FaceSamples = PlanetFaceSamples(request.Shape.ReferenceRadius, 1.0f);
    ASSERT_NE(request.Shape.FaceSamples, oldSamples);
    PageStreaming::GeneratedPlanetLattice lattice;
    lattice.ReferenceRadius = request.Shape.ReferenceRadius;
    lattice.FaceSamples = request.Shape.FaceSamples;
    lattice.HeightRow = [](std::span<const Mathematics::Vector3>, float32, std::span<float32> outHeights) {
        std::fill(outHeights.begin(), outHeights.end(), 0.0f);
    };
    lattice.HeightRange = [](const Mathematics::Vector3&, float32, float32) { return PageStreaming::PageHeightRange{}; };
    request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
    float32 after = 0.0f;
    uint32 uploads = 0;
    for (const uint64 end = frame + 40; frame < end; ++frame)
    {
        pages.RequestPlanet(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        after = std::max(after, LargestUploadedHeight(pages));
        uploads += static_cast<uint32>(pages.Uploads().size());
    }
    EXPECT_GT(uploads, 0u);
    EXPECT_EQ(after, 0.0f) << "a page of the new lattice took the old lattice's overlay";
}

namespace
{

// The wanted pages per level of planet 7 (a flat 2 km planet at 0.5 m, the camera 1.7 m up) after
// 40 frames on a cache of `slots`, with the narrow arm's fit or without it.
std::vector<PageLevelCounts> FlatPlanetCounts(uint32 slots, bool fitToSlots)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{slots, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    PageStreaming::GeneratedPlanetLattice lattice;
    lattice.ReferenceRadius = 2000.0;
    lattice.FaceSamples = PlanetFaceSamples(lattice.ReferenceRadius, 0.5f);
    lattice.HeightRow = [](std::span<const Mathematics::Vector3>, float32, std::span<float32> outHeights) {
        std::fill(outHeights.begin(), outHeights.end(), 0.0f);
    };
    lattice.HeightRange = [](const Mathematics::Vector3&, float32, float32) { return PageStreaming::PageHeightRange{}; };
    PlanetPagedRequest request;
    request.Planet = 7;
    request.SourceIdentity = 1;
    request.Name = "planet";
    request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
    request.Shape.ReferenceRadius = lattice.ReferenceRadius;
    request.Shape.FaceSamples = lattice.FaceSamples;
    request.FitToSlots = fitToSlots;
    const Mathematics::Vector3 camera{2001.7f, 0.0f, 0.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    for (uint64 frame = 2; frame < 40; ++frame)
    {
        pages.RequestPlanet(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    std::vector<PageStreamCounts> streams;
    pages.StreamCounts(streams);
    EXPECT_EQ(streams.size(), 1u);
    return streams.empty() ? std::vector<PageLevelCounts>{} : streams[0].Levels;
}

uint32 Wanted(const std::vector<PageLevelCounts>& levels)
{
    uint32 wanted = 0;
    for (const PageLevelCounts& level : levels)
        wanted += level.Wanted;
    return wanted;
}

} // namespace

// The planet's table as the GPU reads it: published once its pinned levels are in, every page of the finest
// pinned level on every face resolves at or below that level, and the entries count the table's pages.
TEST(TerrainPages, TheStatsNameEachPlanetFacesEntriesAndTheLevelEachPinnedPageResolvesTo)
{
    TerrainPages pages(nullptr, nullptr,
                       HeightPageProfile{kDesktopHeightPages.Slots, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    PageStreaming::GeneratedPlanetLattice lattice;
    lattice.ReferenceRadius = 2000.0;
    lattice.FaceSamples = PlanetFaceSamples(lattice.ReferenceRadius, 0.5f);
    lattice.HeightRow = [](std::span<const Mathematics::Vector3>, float32, std::span<float32> outHeights) {
        std::fill(outHeights.begin(), outHeights.end(), 0.0f);
    };
    lattice.HeightRange = [](const Mathematics::Vector3&, float32, float32) {
        return PageStreaming::PageHeightRange{};
    };
    PlanetPagedRequest request;
    request.Planet = 7;
    request.SourceIdentity = 1;
    request.Name = "planet";
    request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
    request.Shape.ReferenceRadius = lattice.ReferenceRadius;
    request.Shape.FaceSamples = lattice.FaceSamples;
    const Mathematics::Vector3 camera{2001.7f, 0.0f, 0.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    for (uint64 frame = 2; frame < 40; ++frame)
    {
        pages.RequestPlanet(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    std::vector<PageStreamCounts> streams;
    pages.StreamCounts(streams);
    ASSERT_EQ(streams.size(), 1u);
    const PageStreamCounts& planet = streams[0];
    EXPECT_TRUE(planet.Published);
    ASSERT_EQ(planet.Faces.size(), 6u);
    ASSERT_GT(planet.PinnedPagesX * planet.PinnedPagesZ, 0u);
    uint32 entries = 0;
    for (const HeightPageFaceTable& face : planet.Faces)
    {
        ASSERT_EQ(face.ResolvedLevel.size(), std::size_t(planet.PinnedPagesX) * planet.PinnedPagesZ);
        for (uint8 level : face.ResolvedLevel)
            EXPECT_LE(level, planet.PinnedLevel) << "a pinned page always has its entry";
        EXPECT_GE(face.EntriesPerLevel[planet.PinnedLevel], planet.PinnedPagesX * planet.PinnedPagesZ);
        for (uint32 count : face.EntriesPerLevel)
            entries += count;
    }
    uint32 resident = 0;
    for (const PageLevelCounts& level : planet.Levels)
        resident += level.Resident;
    EXPECT_GE(entries, resident) << "every wanted resident page has its entry";
    // The camera stands on face 0 (+X): level 0 answers under it, and on the far face (-X) no finer level.
    EXPECT_GT(planet.Faces[0].EntriesPerLevel[0], 0u);
    EXPECT_EQ(planet.Faces[1].EntriesPerLevel[0], 0u);
}

TEST(TerrainPages, TheNarrowArmFitsAPlanetToItsSlotsALevelCoarserAndChangesNothingThatFits)
{
    constexpr uint32 kSlots = 32u;
    const std::vector<PageLevelCounts> overflowing = FlatPlanetCounts(kSlots, false);
    ASSERT_GT(Wanted(overflowing), kSlots) << "the pose must pass the slots for the fit to act";
    uint32 overflowingResident = 0;
    for (const PageLevelCounts& level : overflowing)
        overflowingResident += level.Resident;
    EXPECT_LT(overflowingResident, Wanted(overflowing)) << "past its slots a wanted page is never resident";

    const std::vector<PageLevelCounts> fitted = FlatPlanetCounts(kSlots, true);
    EXPECT_LE(Wanted(fitted), kSlots);
    for (const PageLevelCounts& level : fitted)
        EXPECT_EQ(level.Resident, level.Wanted) << "every page the fitted request wants is resident";
    EXPECT_GT(fitted.front().Wanted, 0u) << "the camera 1.7 m up still needs level 0";
    EXPECT_LT(fitted.front().Wanted, overflowing.front().Wanted);
    RecordProperty("overflowing_wanted", static_cast<int>(Wanted(overflowing)));
    RecordProperty("overflowing_resident", static_cast<int>(overflowingResident));
    RecordProperty("fitted_wanted", static_cast<int>(Wanted(fitted)));

    const std::vector<PageLevelCounts> desktop = FlatPlanetCounts(kDesktopHeightPages.Slots, false);
    const std::vector<PageLevelCounts> narrow = FlatPlanetCounts(kDesktopHeightPages.Slots, true);
    ASSERT_EQ(desktop.size(), narrow.size());
    for (std::size_t level = 0; level < desktop.size(); ++level)
        EXPECT_EQ(narrow[level].Wanted, desktop[level].Wanted) << "level " << level;
}

namespace
{

// The surface pages of a planet: its height and surface requests from `altitude` meters over its
// surface at a pixel error of `targetPixels`, then an Update. Each upload's last bytes by slot go to
// `uploaded`; each rewrite of a surface page to `rewrites`.
void RunPlanetSurfaceFrames(TerrainPages& pages, const PlanetPagedRequest& height,
                            const PlanetSurfacePagedRequest& surface, uint64& frame, uint64 count,
                            FlatMap<uint32, std::vector<uint8>>& uploaded,
                            std::vector<PageStreaming::PageAddress>& rewrites, float32 altitude = 5.0f,
                            float32 targetPixels = 11.0f)
{
    const Mathematics::Vector3 camera{static_cast<float32>(height.Shape.ReferenceRadius) + altitude, 0.0f, 0.0f};
    const PageRequestCamera everyWay{camera, {}, 0.0f, false};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, targetPixels);
    for (const uint64 end = frame + count; frame < end; ++frame)
    {
        pages.RequestPlanet(height, {&camera, 1}, view);
        pages.RequestPlanetSurface(surface, {&everyWay, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        for (SurfacePageUpload& upload : pages.Surface().Uploads())
        {
            EXPECT_EQ(upload.PlaneMask, 3u) << "the splat and the normal";
            EXPECT_EQ(upload.Bytes.size(), PageStreaming::PagePayloadBytes(PageStreaming::PageFieldFormat::Rgba8) +
                                               PageStreaming::PagePayloadBytes(PageStreaming::PageFieldFormat::Bc5));
            uploaded.InsertOrAssign(upload.Slot, std::move(upload.Bytes));
        }
        for (const PageStreaming::PageRewrite& rewrite : pages.Residency().FieldCache(kSurfacePageField).Rewrites())
            rewrites.push_back(rewrite.Page.Address);
        pages.Surface().Uploads().clear();
        pages.Uploads().clear();
    }
}

} // namespace

// A planet's surface stream generates its pages from its height source and the rows on six faces, and
// its surface table resolves every sample at the top level.
TEST(TerrainPages, APlanetsSurfacePagesLoadOnSixFacesAndItsSurfaceTableResolvesTheTopLevel)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{512, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    const PlanetPagedRequest height = FlatPlanetRequest(true);
    const uint32 faceSamples = height.Shape.FaceSamples;
    const uint32 topLevel =
        static_cast<uint32>(PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples).size() - 1u);
    const Components::TerrainSurfaceRulesEffect rules = MakeDefaultPlanetSurfaceRules(-10.0f, 10.0f);
    PlanetSurfacePagedRequest surface;
    surface.Planet = height.Planet;
    surface.Rules = &rules;
    surface.RulesIdentity = 1;
    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    FlatMap<uint32, std::vector<uint8>> uploaded;
    std::vector<PageStreaming::PageAddress> rewrites;
    RunPlanetSurfaceFrames(pages, height, surface, frame, 40, uploaded, rewrites);

    uint32 planeMask = 0;
    const PackedPageTable* table = pages.Surface().PackedTable(height.RenderTerrain, planeMask);
    ASSERT_NE(table, nullptr) << "the planet has no surface table under its render terrain";
    EXPECT_EQ(planeMask, 3u);
    EXPECT_TRUE(EveryPageOfLevelHasItsEntry(table->Words(), topLevel));
    std::array<bool, PageStreaming::kPageStoreMaxFaces> topFaces{};
    for (const PageStreaming::CachedPage& page : pages.Residency().FieldCache(kSurfacePageField).Resident())
        if (page.Address.Level == topLevel)
            topFaces[page.Address.Face] = true;
    for (uint32 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
        EXPECT_TRUE(topFaces[face]) << "face " << face << " has no resident top surface page";
    EXPECT_GE(uploaded.Size(), PageStreaming::kPageStoreMaxFaces);
}

// The surface field holds the planar count until a planet comes and the planet count while it streams.
// A planet whose view needs more pages than that is requested a level coarser until it fits, and every
// page it then wants becomes resident.
TEST(TerrainPages, APlanetGrowsTheSurfaceFieldToItsSlotsAndFitsItsSurfaceRequestToThem)
{
    constexpr SurfacePageProfile kProfile{16u, 64u};
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{512, kDesktopHeightPages.DenseTableWords}, kProfile);
    EXPECT_EQ(pages.Surface().Geometry().SlotsPerRow, 4u) << "no planet: the planar slots";
    const PlanetPagedRequest height = FlatPlanetRequest(true);
    const Components::TerrainSurfaceRulesEffect rules = MakeDefaultPlanetSurfaceRules(-10.0f, 10.0f);
    PlanetSurfacePagedRequest surface;
    surface.Planet = height.Planet;
    surface.Rules = &rules;
    surface.RulesIdentity = 1;
    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    FlatMap<uint32, std::vector<uint8>> uploaded;
    std::vector<PageStreaming::PageAddress> rewrites;
    // 5 m up, the 2 km planet's horizon holds 16 pages at any pixel error. 500 m up at one texel a
    // pixel, it wants more pages than 64 slots hold.
    constexpr float32 kAltitude = 500.0f;
    constexpr float32 kTargetPixels = 1.0f;
    RunPlanetSurfaceFrames(pages, height, surface, frame, 60, uploaded, rewrites, kAltitude, kTargetPixels);
    EXPECT_EQ(pages.Surface().Geometry().SlotsPerRow, 8u) << "a planet: the planet slots";
    const PageSlotFit fit = pages.Surface().PlanetFit(height.Planet);
    EXPECT_GT(fit.FullDetailPages, kProfile.PlanetSlots) << "the pages the view wants before the fit";

    std::vector<PageStreamCounts> streams;
    pages.StreamCounts(streams);
    const auto stream = std::ranges::find_if(streams, [](const PageStreamCounts& counts) { return counts.Surface; });
    ASSERT_NE(stream, streams.end());
    EXPECT_GE(stream->FitPasses, 1u) << "the view needs more pages than the slots";
    EXPECT_EQ(stream->FullDetailPages, fit.FullDetailPages) << "the stats and the inspector count one number";
    EXPECT_GE(stream->PagesProduced, uploaded.Size()) << "every uploaded page was made by a load";
    uint32 wanted = 0;
    for (const PageLevelCounts& level : stream->Levels)
    {
        EXPECT_EQ(level.Resident, level.Wanted) << "every page the fitted request wants is resident";
        wanted += level.Wanted;
    }
    EXPECT_LE(wanted, kProfile.PlanetSlots);
    EXPECT_GT(wanted, 0u);
    RecordProperty("fit_passes", static_cast<int>(stream->FitPasses));
    RecordProperty("wanted", static_cast<int>(wanted));

    for (const uint64 end = frame + 3; frame < end; ++frame)
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    EXPECT_EQ(pages.Surface().Geometry().SlotsPerRow, 4u) << "the planet went: the planar slots again";
}

// A layer write three samples past a level-0 surface page's owned samples: the page's normal window
// reads it (kPageSurfaceWindowMarginSamples), so the page is rewritten in its slot, and every
// rewritten page reads the write.
TEST(TerrainPages, ALayerWriteRewritesTheSurfacePagesWhoseWindowReadsItPastTheirOwnedSamples)
{
    TerrainPages pages(nullptr, nullptr, HeightPageProfile{512, kDesktopHeightPages.DenseTableWords},
                       DesktopSurfacePageProfile());
    PlanetPagedRequest height = FlatPlanetRequest(true);
    const uint32 faceSamples = height.Shape.FaceSamples;
    const Components::TerrainSurfaceRulesEffect rules = MakeDefaultPlanetSurfaceRules(-10.0f, 10.0f);
    PlanetSurfacePagedRequest surface;
    surface.Planet = height.Planet;
    surface.Rules = &rules;
    surface.RulesIdentity = 1;
    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    FlatMap<uint32, std::vector<uint8>> uploaded;
    std::vector<PageStreaming::PageAddress> rewrites;
    RunPlanetSurfaceFrames(pages, height, surface, frame, 40, uploaded, rewrites);

    constexpr uint32 kPastOwned = 3u;
    const PageStreaming::PageCache& cache = pages.Residency().FieldCache(kSurfacePageField);
    const auto found = std::find_if(cache.Resident().begin(), cache.Resident().end(), [&](const auto& page) {
        return page.Address.Level == 0u && (page.Address.X + 1u) * PageStreaming::kPageOwnedSamples + kPastOwned < faceSamples;
    });
    ASSERT_NE(found, cache.Resident().end()) << "no level-0 surface page is resident";
    const PageStreaming::CachedPage page = *found;
    const uint32 slot = cache.SlotOf(page);
    ASSERT_NE(uploaded.Find(slot), nullptr);
    const std::vector<uint8> before = *uploaded.Find(slot);

    const uint32 x = (page.Address.X + 1u) * PageStreaming::kPageOwnedSamples + kPastOwned;
    const uint32 z = page.Address.Z * PageStreaming::kPageOwnedSamples + 64u;
    const PageStreaming::PageSampleRect bump{x, z, x, z};
    auto layer = std::make_shared<PageStreaming::HeightPageLayer>(
        PageStreaming::kPageStoreMaxFaces, PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples));
    layer->WriteLevel0(page.Address.Face, bump, std::vector<float32>{50.0f});
    height.Overlay = layer;
    height.OverlayVersion = 1;
    height.OverlayChanged[page.Address.Face] = bump;
    height.OverlayChangedSince = 0;
    surface.LayerChanged = height.OverlayChanged;
    surface.LayerChangedSince = 0;
    uploaded.Clear();
    rewrites.clear();
    RunPlanetSurfaceFrames(pages, height, surface, frame, 20, uploaded, rewrites);

    EXPECT_EQ(cache.SlotOf(page), slot) << "the rewritten page keeps its slot";
    EXPECT_NE(std::find(rewrites.begin(), rewrites.end(), page.Address), rewrites.end())
        << "the page whose window reads the write was not rewritten";
    ASSERT_NE(uploaded.Find(slot), nullptr);
    EXPECT_NE(*uploaded.Find(slot), before) << "the page's normal plane does not show the write";
    for (const PageStreaming::PageAddress& address : rewrites)
        EXPECT_TRUE(PageStreaming::PageReadsLevel0Rect(address, bump, PageStreaming::kPageSurfaceWindowMarginSamples))
            << "face " << int(address.Face) << " level " << int(address.Level) << " page (" << address.X << ", "
            << address.Z << ") was rewritten though its window does not read the write";
}
