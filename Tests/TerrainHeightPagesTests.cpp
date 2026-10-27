// The height source of a tiled terrain (TerrainHeightIsPaged) and the main-thread half of its height
// pages (TerrainHeightPages): the predicate's arms, the GPU page-table words, the dirty rects that
// make the CBT re-evaluate the bisectors whose height changed source, a moved terrain's requests and
// the table versions; and the GPU side's latch (TerrainHeightPageFeature): a table is readable only
// with the upload pass of its pages, and the cache texture only from the latch after its clear.

#include "CBTTerrain/CBTLayout.h"
#include "TerrainECS/PageTable.h"
#include "TerrainECS/TerrainHeightPageFeature.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainHeightSource.h"
#include "TerrainPagedField.h"

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/PageResidency.h"
#include "PageStreaming/PageSampleRect.h"
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
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using GameEngine::TerrainECS::Test::FlatTileRequest;

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

TEST(TerrainHeightPages, PackedWordsCarryTheShapeFadesAndEntriesWhereTheResolveReadsThem)
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

TEST(TerrainHeightPages, ATableThatFitsItsDenseBudgetPacksTheDenseWordsByteForByte)
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

TEST(TerrainHeightPages, ATerrainDirtiesItsRectWhenItsSourceSwitchesOrItsPagesChangeAndNothingOnceSettled)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords});
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
    uint64 version = 0;
    ASSERT_NE(pages.TableWords(request.Terrain, version), nullptr);

    // Not requested for a frame: it goes back to its texture, its whole rect re-evaluated.
    pages.Update(62, 1.0f / 60.0f, false, 2);
    ASSERT_EQ(pages.DirtyRects().size(), 1u);
    EXPECT_TRUE(CoversWholeTerrain(pages.DirtyRects().front()));
    EXPECT_EQ(pages.TableWords(request.Terrain, version), nullptr);
}

TEST(TerrainHeightPages, AnOverlayOfRebuiltModifierDataRewritesEveryPageTheOldOverlayReached)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords});
    const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(257, 257);
    const PageStreaming::PageSampleRect raised{240, 240, 256, 256}; // the far corner: nine pages read it
    PagedTerrainRequest modified = FlatTileRequest(257);
    modified.Overlay = std::make_shared<const PageStreaming::HeightPageOverlay>(
        levels, raised, std::vector<float32>(raised.Width() * raised.Height(), 5.0f));
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
                               PageStreaming::PageAddress{0, static_cast<uint8>(level), x, z}, raised)
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

// The published table entry of level-0 page (x, z) (the binding-22 layout the packing test pins).
uint32 Level0Entry(const TerrainHeightPages& pages, uint32 terrain, uint32 x, uint32 z)
{
    uint64 version = 0;
    const std::vector<uint32>* words = pages.TableWords(terrain, version);
    if (words == nullptr)
        return PageStreaming::kNoPage;
    const uint32 entriesAt = 8u + 4u * 32u + (*words)[4];
    const uint32 pagesX = (*words)[8u + 1u];
    return (*words)[entriesAt + z * pagesX + x];
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

TEST(TerrainHeightPages, AMovedTerrainStreamsThePagesUnderTheCameraAtItsNewPlacement)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords});
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
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords});
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
    request.Overlay = std::make_shared<const PageStreaming::HeightPageOverlay>(
        levels, edit, std::vector<float32>(edit.Width() * edit.Height(), 5.0f));
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

TEST(TerrainHeightPages, AnEditOnALevelZeroPageDirtiesTheEditAndOneSampleAroundItNotThePage)
{
    // One level-0 page holds the whole 128-sample terrain; the edit is samples 40 to 50. The
    // surface reads a sample through the bilinear tap of the cells on either side of it.
    const HeightPageDirtyRect dirty = EditDirtyUnion(FlatTileRequest(128), PageStreaming::PageSampleRect{40, 40, 50, 50});
    EXPECT_FLOAT_EQ(dirty.MinU, 39.0f / 127.0f);
    EXPECT_FLOAT_EQ(dirty.MinV, 39.0f / 127.0f);
    EXPECT_FLOAT_EQ(dirty.MaxU, 51.0f / 127.0f);
    EXPECT_FLOAT_EQ(dirty.MaxV, 51.0f / 127.0f);
}

TEST(TerrainHeightPages, AnEditDirtiesWhatItReachesAtEachCoarseLevelNotTheCoarsePagesFootprints)
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

TEST(TerrainHeightPages, AStreamedInPageDirtiesItsWholeFootprint)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords});
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

TEST(TerrainHeightPages, ATerrainPagedAgainAtTheSameIndexNeverRepeatsATableVersion)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords});
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    const Mathematics::Vector3 camera{100.0f, 10.0f, 100.0f};
    std::vector<uint64> firstVersions;
    uint64 frame = 2;
    for (; frame < 6; ++frame)
    {
        pages.Request(StreamedFieldRequest(0.0f, 77), {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        uint64 version = 0;
        ASSERT_NE(pages.TableWords(0, version), nullptr);
        firstVersions.push_back(version);
    }
    pages.Update(frame++, 1.0f / 60.0f, false, 2); // not requested: forgotten
    pages.Request(StreamedFieldRequest(0.0f, 78), {&camera, 1}, view);
    pages.Update(frame, 1.0f / 60.0f, false, 2);
    uint64 version = 0;
    ASSERT_NE(pages.TableWords(0, version), nullptr);
    for (uint64 seen : firstVersions)
        EXPECT_NE(version, seen) << "the CBT's per-ring-slot upload check would keep the old terrain's table";
}

TEST(TerrainHeightPages, The60By15KmTerrainAtAQuarterMeterKeepsOneTierOnDesktopAndIsTieredOnTheDeck)
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

TEST(TerrainHeightPages, AStorePastTheCpuDirectorysLimitIsRefusedWithTheLimitAndTheFix)
{
    // A store may hold 2^30 samples a side; its pages stream up to 2^22, where the CPU directory's
    // 32-bit counts end.
    constexpr uint32 kLimit = PageStreaming::kCpuDirectoryMaxSamplesPerAxis;
    EXPECT_EQ(DenseDirectoryRefusal("Earth", kLimit, kLimit), "");
    EXPECT_EQ(DenseDirectoryRefusal("Near", 8193, 2049), "");
    const std::string refusal = DenseDirectoryRefusal("Earth", kLimit + 1u, 2049);
    EXPECT_NE(refusal.find("Earth (4194305 x 2049 samples) does not stream its height"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("at most 4194304 samples"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("Split it into terrains"), std::string::npos) << refusal;
}

TEST(TerrainHeightPages, A1KmSquareModifierOnAQuarterMeterTerrainFitsEveryOverlayCapAndATerrainWideOneIsRefused)
{
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(240001, 60001);
    // 1 km x 1 km at 0.25 m: 4001 x 4001 samples and the bake's one-sample margin on each side.
    const uint64 local = PageStreaming::HeightPageOverlay::Bytes(levels, {100000, 20000, 104002, 24002});
    EXPECT_GT(local, 4003ull * 4003ull * sizeof(float32)) << "the coarser levels hold samples too";
    EXPECT_EQ(OverlayRefusal("Outer", local, kDesktopHeightPages), "");
    EXPECT_EQ(OverlayRefusal("Outer", local, kSteamDeckHeightPages), "");
    // A modifier over the whole 60 x 15 km terrain reaches all of its 14.4 G samples.
    const uint64 wide = PageStreaming::HeightPageOverlay::Bytes(levels, {0, 0, 240000, 60000});
    const std::string refusal = OverlayRefusal("Outer", wide, kDesktopHeightPages);
    EXPECT_NE(refusal.find("Outer does not stream its height"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find(std::format("needs {} bytes", wide)), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("cap of 536870912 bytes"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("Shrink the modifiers' bounds"), std::string::npos) << refusal;
}

// A power-of-two raw heightmap (the 0.25 m canyon, 16384 x 4096) is off its 16 x 4 tile lattice of
// 1025-sample tiles (16385 x 4097): it pages, but takes no modifier overlay, and the refusal names
// the lattice to import at.
TEST(TerrainHeightPages, AStoreOffTheTileLatticeTakesNoModifierOverlayAndTheRefusalNamesTheLattice)
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

TEST(TerrainHeightPageFeature, ATableBecomesReadableOnlyWithTheUploadPassThatRecordsItsPages)
{
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";
    {
        TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords});
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
        uint64 published = 0;
        ASSERT_NE(pages.TableWords(request.Terrain, published), nullptr);

        TerrainHeightPageFeature feature;
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

TEST(TerrainHeightPageFeature, TheCacheTextureIsReadableOnlyFromTheLatchAfterItsClear)
{
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";
    {
        TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords});
        const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
        const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
        pages.Update(1, 1.0f / 60.0f, false, 2);
        for (uint64 frame = 2; frame < 20 && pages.Uploads().empty(); ++frame)
        {
            pages.Request(FlatTileRequest(257), {&camera, 1}, view);
            pages.Update(frame, 1.0f / 60.0f, false, 2);
        }
        ASSERT_FALSE(pages.Uploads().empty());

        TerrainHeightPageFeature feature;
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

// Planet 7: a flat 2 km planet at 2 m (a dense six-face table), drawn by render terrain 3.
PlanetPagedRequest FlatPlanetRequest(bool gpuPages)
{
    PageStreaming::GeneratedPlanetLattice lattice;
    lattice.ReferenceRadius = 2000.0;
    lattice.FaceSamples = PlanetFaceSamples(lattice.ReferenceRadius, 2.0f);
    lattice.Height = [](const Mathematics::Vector3&, float32) { return 0.0f; };
    lattice.HeightRange = [](const Mathematics::Vector3&, float32, float32) { return PageStreaming::PageHeightRange{}; };
    PlanetPagedRequest request;
    request.Planet = 7;
    request.RenderTerrain = 3;
    request.GpuPages = gpuPages;
    request.SourceIdentity = 1;
    request.Name = "planet";
    request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
    request.Shape.ReferenceRadius = lattice.ReferenceRadius;
    request.Shape.FaceSamples = lattice.FaceSamples;
    return request;
}

} // namespace

TEST(TerrainHeightPages, APlanetTheGpuDoesNotDrawStreamsItsWantedPagesUploadsNoneAndIsForgottenWhenNotRequested)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{512, kDesktopHeightPages.DenseTableWords});
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
        EXPECT_EQ(pages.PlanetDirtyRegions().Count, 0u);
    }
    std::vector<uint32> paged;
    pages.PagedTerrains(paged);
    EXPECT_TRUE(paged.empty());
    uint64 version = 0;
    EXPECT_EQ(pages.TableWords(request.RenderTerrain, version), nullptr);
    std::vector<HeightPageStreamCounts> streams;
    pages.StreamCounts(streams);
    ASSERT_EQ(streams.size(), 1u);
    EXPECT_TRUE(streams[0].Planet);
    EXPECT_EQ(streams[0].Label, "planet");
    ASSERT_EQ(streams[0].Levels.size(), PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples).size());
    uint32 wanted = 0;
    for (const HeightPageLevelCounts& level : streams[0].Levels)
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

TEST(TerrainHeightPages, APlanetsResidentPagesReachItsTableWordsUploadsAndDirtyFacesUnderItsRenderTerrain)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{512, kDesktopHeightPages.DenseTableWords});
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
        const CBTTerrain::SphereEditRegions& dirty = pages.PlanetDirtyRegions();
        for (uint32 i = 0; i < dirty.Count; ++i)
        {
            const CBTTerrain::SphereFaceUVRect& rect = dirty.Rects[i];
            ASSERT_LT(rect.Face, PageStreaming::kPageStoreMaxFaces);
            EXPECT_FALSE(rect.IsEmpty());
            EXPECT_GE(rect.MinU, 0.0f);
            EXPECT_LE(rect.MaxU, 1.0f);
            dirtyFaces[rect.Face] = true;
        }
        pages.PlanetDirtyRegions() = {};
        EXPECT_TRUE(pages.DirtyRects().empty()) << "a planet's changes are face rects, not terrain-UV rects";
    }
    for (uint32 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
        EXPECT_TRUE(dirtyFaces[face]) << "face " << face << ": the switch to pages re-evaluates every face";

    std::vector<uint32> paged;
    pages.PagedTerrains(paged);
    ASSERT_EQ(paged, std::vector<uint32>{request.RenderTerrain});
    uint64 version = 0;
    const std::vector<uint32>* words = pages.TableWords(request.RenderTerrain, version);
    ASSERT_NE(words, nullptr);
    EXPECT_GT(version, 0u);
    ASSERT_EQ((*words)[6], 0u) << "a 2 km planet at 2 m keeps a dense table";

    // Every resident page of the stream is its dense entry in the words, and its slot was uploaded;
    // no other entry is set.
    const PageStreaming::PageCache& cache = pages.Residency().FieldCache(kHeightPageField);
    const uint32 slots = (*words)[4];
    const uint32 entriesOffset = CBTTerrain::kCBTPageFadeOffsetWords + slots;
    std::sort(uploadedSlots.begin(), uploadedSlots.end());
    uint32 resident = 0;
    for (const PageStreaming::CachedPage& page : cache.Resident())
    {
        const PageStreaming::PageAddress& address = page.Address;
        const uint32* shape = words->data() + CBTTerrain::kCBTPageHeaderWords + 4u * address.Level;
        const uint32 index = shape[0] + (address.Face * shape[2] + address.Z) * shape[1] + address.X;
        const uint32 slot = cache.SlotOf(page);
        EXPECT_EQ((*words)[entriesOffset + index], PageStreaming::PackPageEntry(slot, address.Level))
            << "face " << int(address.Face) << " L" << int(address.Level) << " (" << address.X << ","
            << address.Z << ")";
        EXPECT_TRUE(std::binary_search(uploadedSlots.begin(), uploadedSlots.end(), slot));
        ++resident;
    }
    uint32 set = 0;
    for (std::size_t i = entriesOffset; i < words->size(); ++i)
        set += (*words)[i] != PageStreaming::kNoPage ? 1u : 0u;
    EXPECT_GT(resident, 6u);
    EXPECT_EQ(set, resident);
}

namespace
{

// The wanted pages per level of planet 7 (a flat 2 km planet at 0.5 m, the camera 1.7 m up) after
// 40 frames on a cache of `slots`, with the narrow arm's fit or without it.
std::vector<HeightPageLevelCounts> FlatPlanetCounts(uint32 slots, bool fitToSlots)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{slots, kDesktopHeightPages.DenseTableWords});
    PageStreaming::GeneratedPlanetLattice lattice;
    lattice.ReferenceRadius = 2000.0;
    lattice.FaceSamples = PlanetFaceSamples(lattice.ReferenceRadius, 0.5f);
    lattice.Height = [](const Mathematics::Vector3&, float32) { return 0.0f; };
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
    std::vector<HeightPageStreamCounts> streams;
    pages.StreamCounts(streams);
    EXPECT_EQ(streams.size(), 1u);
    return streams.empty() ? std::vector<HeightPageLevelCounts>{} : streams[0].Levels;
}

uint32 Wanted(const std::vector<HeightPageLevelCounts>& levels)
{
    uint32 wanted = 0;
    for (const HeightPageLevelCounts& level : levels)
        wanted += level.Wanted;
    return wanted;
}

} // namespace

TEST(TerrainHeightPages, TheNarrowArmFitsAPlanetToItsSlotsALevelCoarserAndChangesNothingThatFits)
{
    constexpr uint32 kSlots = 32u;
    const std::vector<HeightPageLevelCounts> overflowing = FlatPlanetCounts(kSlots, false);
    ASSERT_GT(Wanted(overflowing), kSlots) << "the pose must pass the slots for the fit to act";
    uint32 overflowingResident = 0;
    for (const HeightPageLevelCounts& level : overflowing)
        overflowingResident += level.Resident;
    EXPECT_LT(overflowingResident, Wanted(overflowing)) << "past its slots a wanted page is never resident";

    const std::vector<HeightPageLevelCounts> fitted = FlatPlanetCounts(kSlots, true);
    EXPECT_LE(Wanted(fitted), kSlots);
    for (const HeightPageLevelCounts& level : fitted)
        EXPECT_EQ(level.Resident, level.Wanted) << "every page the fitted request wants is resident";
    EXPECT_GT(fitted.front().Wanted, 0u) << "the camera 1.7 m up still needs level 0";
    EXPECT_LT(fitted.front().Wanted, overflowing.front().Wanted);
    RecordProperty("overflowing_wanted", static_cast<int>(Wanted(overflowing)));
    RecordProperty("overflowing_resident", static_cast<int>(overflowingResident));
    RecordProperty("fitted_wanted", static_cast<int>(Wanted(fitted)));

    const std::vector<HeightPageLevelCounts> desktop = FlatPlanetCounts(kDesktopHeightPages.Slots, false);
    const std::vector<HeightPageLevelCounts> narrow = FlatPlanetCounts(kDesktopHeightPages.Slots, true);
    ASSERT_EQ(desktop.size(), narrow.size());
    for (std::size_t level = 0; level < desktop.size(); ++level)
        EXPECT_EQ(narrow[level].Wanted, desktop[level].Wanted) << "level " << level;
}
