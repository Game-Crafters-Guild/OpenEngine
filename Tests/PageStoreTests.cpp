// PageStoreTests: the terrain page store (Engine/Modules/PageStreaming) as pure oracles. The store
// round trip, the apron's bit-equality under the per-level step (a 32-bit page beside a 16-bit one
// included), the present-page index against the dense lookup, the sparse resolve, the incremental
// re-cook, the lattice-preserving tent, the generated provider's level 0 against the terrain's own
// tile fill, the cook key, the step rule, the terrain container, the PNG limit, the
// asynchronous loader, and the surface kinds' codecs and filters. HeightPageCodec.h,
// HeightStepPlanner.h, PagePyramidBuilder.h and PageStoreSerialization.h are the module's private
// headers: the height encoding, the step rule, the builder the surface stores are made with, and
// the byte layout a refusal test patches.

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/HeightPageCooker.h"
#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/HeightStoreCache.h"
#include "PageStreaming/HeightStoreKey.h"
#include "PageStreaming/PageLoader.h"
#include "PageStreaming/PageResidency.h"
#include "PageStreaming/PageStoreFormat.h"
#include "PageStreaming/PageStorePatcher.h"
#include "PageStreaming/PageStoreReader.h"
#include "PageStreaming/PageStoreWriter.h"
#include "PageStreaming/PageTableEntry.h"
#include "PageStreaming/TerrainPageContainer.h"

#include "HeightPageCodec.h"
#include "HeightStepPlanner.h"
#include "PagePyramidBuilder.h"
#include "PageStoreSerialization.h"

#include "Assets/AssetIOService.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTPlanetShading.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/CubeFaces.h"
#include "Mathematics/VectorOps.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/TerrainService.h"
#include "TestTempDir.h"
#include "Types/Color.h"
#include "Types/Fnv1a.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <numbers>
#include <thread>
#include <tuple>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::PageStreaming;

namespace
{

constexpr auto kLoadTimeout = std::chrono::seconds(10);

using HeightFunction = std::function<float32(uint32 x, uint32 z)>;

void WriteR32(const std::filesystem::path& file, uint32 width, uint32 height, const HeightFunction& heightAt)
{
    std::vector<float32> samples(static_cast<std::size_t>(width) * height);
    for (uint32 z = 0; z < height; ++z)
        for (uint32 x = 0; x < width; ++x)
            samples[static_cast<std::size_t>(z) * width + x] = heightAt(x, z);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size() * 4u));
}

std::vector<float32> ReadR32(const std::filesystem::path& file, uint32 width, uint32 height)
{
    std::vector<float32> samples(static_cast<std::size_t>(width) * height);
    std::ifstream in(file, std::ios::binary);
    in.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(samples.size() * 4u));
    return samples;
}

// A smooth relief in [100, 400] m: a 300 m range, so the store is 16-bit.
float32 Relief(uint32 x, uint32 z)
{
    return 250.0f + 120.0f * std::sin(0.031f * static_cast<float32>(x)) * std::cos(0.017f * static_cast<float32>(z)) +
           30.0f * std::sin(0.11f * static_cast<float32>(x + 3u * z));
}

std::vector<uint8> FileBytes(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::vector<uint8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint32 ClampSample(int64 index, uint32 count)
{
    return static_cast<uint32>(std::clamp<int64>(index, 0, static_cast<int64>(count) - 1));
}

class PageStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Dir = TestUtils::MakeUniqueTempDirectory("page_store_test");
        std::filesystem::create_directories(m_Dir);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    // Scans and cooks `source` into `output`, patching `previous` when it can.
    HeightCookResult Cook(const std::filesystem::path& source, uint32 width, uint32 height,
                          const std::filesystem::path& output, const std::filesystem::path& previous = {},
                          JobSystem::WorkStealingThreadPool* pool = nullptr, AssetIOService* io = nullptr)
    {
        HeightCookRequest request;
        EXPECT_EQ(ResolveHeightCookSource(source, width, height, request.Source), "");
        EXPECT_EQ(ScanHeightSource(request.Source, io, request.Identity), "");
        request.Key = ComputeHeightStoreKey(
            {request.Identity.ContentHash, request.Source.Format, request.Source.SamplesX, request.Source.SamplesZ});
        request.Output = output;
        request.Previous = previous;
        request.Pool = pool;
        request.Io = io;
        return CookHeightPageStore(request);
    }

    std::filesystem::path m_Dir;
};

std::vector<float32> PageSamples(const PageStoreReader& reader, const PageAddress& address)
{
    std::vector<float32> samples(kPageSampleCount);
    EXPECT_TRUE(reader.ReadPageSamples(address, {reinterpret_cast<uint8*>(samples.data()), samples.size() * sizeof(float32)}));
    return samples;
}

// A loaded height page's heights (HeightSamples), copied for comparison.
std::vector<float32> Heights(std::span<const uint8> bytes)
{
    const std::span<const float32> heights = HeightSamples(bytes);
    return {heights.begin(), heights.end()};
}

// Every stored level-N sample of a store, as one row-major grid of the level (owned samples only).
std::vector<float32> LevelGrid(const PageStoreReader& reader, uint8 level)
{
    const PageStoreLevel& shape = reader.Layout().Levels[level];
    std::vector<float32> grid(static_cast<std::size_t>(shape.SamplesX) * shape.SamplesZ);
    for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
    {
        for (uint32 px = 0; px < shape.PagesX; ++px)
        {
            const std::vector<float32> page = PageSamples(reader, PageAddress{0, level, px, pz});
            for (uint32 r = 0; r < kPageOwnedSamples; ++r)
            {
                for (uint32 c = 0; c < kPageOwnedSamples; ++c)
                {
                    const uint32 x = px * kPageOwnedSamples + c;
                    const uint32 z = pz * kPageOwnedSamples + r;
                    if (x < shape.SamplesX && z < shape.SamplesZ)
                        grid[static_cast<std::size_t>(z) * shape.SamplesX + x] =
                            page[static_cast<std::size_t>(r + 1u) * kPageStrideSamples + c + 1u];
                }
            }
        }
    }
    return grid;
}

// A store of `format` and `filter` built from its level-0 samples (row-major, PageTexelBytes(format)
// bytes each), pushed in bands of 50 rows as a cook pushes them.
void BuildSurfaceStore(const std::filesystem::path& file, PageFieldFormat format, PageFieldFilter filter, uint32 samplesX,
                       uint32 samplesZ, const std::vector<uint8>& level0)
{
    PageStoreHeader header;
    header.Format = format;
    header.Filter = filter;
    header.SamplesX = samplesX;
    header.SamplesZ = samplesZ;
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(file, MakePageStoreLayout(header)), "");
    PagePyramidBuilder builder(writer, false, nullptr, nullptr);
    const std::size_t rowBytes = static_cast<std::size_t>(samplesX) * PageTexelBytes(format);
    constexpr uint32 kBandRows = 50u;
    for (uint32 row = 0; row < samplesZ; row += kBandRows)
        builder.PushLevel0Rows(level0.data() + row * rowBytes, std::min(kBandRows, samplesZ - row));
    ASSERT_TRUE(builder.Finish());
    ASSERT_EQ(writer.Finish(), "");
}

std::vector<uint8> SurfacePage(const PageStoreReader& reader, const PageAddress& address)
{
    const PageFieldFormat format = reader.Layout().Header.Format;
    const uint32 stride = PageStrideTexels(format);
    std::vector<uint8> samples(static_cast<std::size_t>(stride) * stride * PageTexelBytes(format));
    EXPECT_TRUE(reader.ReadPageSamples(address, samples));
    return samples;
}

// The owned samples of every page of a surface store's level, as one row-major grid of the level.
std::vector<uint8> SurfaceLevelGrid(const PageStoreReader& reader, uint8 level)
{
    const PageStoreLevel& shape = reader.Layout().Levels[level];
    const PageFieldFormat format = reader.Layout().Header.Format;
    const uint32 stride = PageStrideTexels(format);
    const uint32 apron = (stride - kPageOwnedSamples) / 2u;
    const uint32 texel = PageTexelBytes(format);
    std::vector<uint8> grid(static_cast<std::size_t>(shape.SamplesX) * shape.SamplesZ * texel);
    for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
        for (uint32 px = 0; px < shape.PagesX; ++px)
        {
            const std::vector<uint8> page = SurfacePage(reader, PageAddress{0, level, px, pz});
            for (uint32 r = 0; r < kPageOwnedSamples; ++r)
                for (uint32 c = 0; c < kPageOwnedSamples; ++c)
                {
                    const uint32 x = px * kPageOwnedSamples + c;
                    const uint32 z = pz * kPageOwnedSamples + r;
                    if (x < shape.SamplesX && z < shape.SamplesZ)
                        std::memcpy(&grid[(static_cast<std::size_t>(z) * shape.SamplesX + x) * texel],
                                    &page[(static_cast<std::size_t>(r + apron) * stride + c + apron) * texel], texel);
                }
        }
    return grid;
}

// The 16 bytes of block (bx, by) of a Bc5 page's stored bytes.
std::vector<uint8> Bc5Block(const std::vector<uint8>& bytes, uint32 bx, uint32 by)
{
    const uint32 blocks = PageStrideTexels(PageFieldFormat::Bc5) / kPageBlockTexels;
    const auto first = bytes.begin() + static_cast<std::ptrdiff_t>((by * blocks + bx) * kBc5BlockBytes);
    return std::vector<uint8>(first, first + kBc5BlockBytes);
}

// The slope of a 30-degree flank, rise over run.
const float32 kThirtyDegreeSlope = std::tan(30.0f * Mathematics::Pi / 180.0f);
constexpr float32 kDegreesPerRadian = 180.0f / Mathematics::Pi;

// A ridge along Z at x = 100 with 30-degree flanks, one unit between samples, edge-clamped to `width`.
float32 RidgeHeight(int64 x, uint32 width)
{
    return -kThirtyDegreeSlope * static_cast<float32>(std::abs(static_cast<int64>(ClampSample(x, width)) - 100));
}

// A copy of `source` beside it, with the `bytes` low bytes of `value` at `offset`.
std::filesystem::path PatchedCopy(const std::filesystem::path& source, std::size_t offset, uint32 value, std::size_t bytes)
{
    std::vector<uint8> data = FileBytes(source);
    std::memcpy(data.data() + offset, &value, bytes);
    std::filesystem::path copy = source;
    copy.replace_filename("patched" + source.extension().string());
    std::ofstream(copy, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return copy;
}

// Four splat weights summing to 255 at every sample, from a hash of its position.
std::vector<uint8> SplatWeights(uint32 width, uint32 height)
{
    std::vector<uint8> weights(static_cast<std::size_t>(width) * height * 4u);
    for (uint32 z = 0; z < height; ++z)
        for (uint32 x = 0; x < width; ++x)
        {
            const uint32 hash = (x * 73856093u) ^ (z * 19349663u) ^ ((x + z) * 83492791u);
            uint8* texel = &weights[(static_cast<std::size_t>(z) * width + x) * 4u];
            texel[0] = static_cast<uint8>(hash % 256u);
            texel[1] = static_cast<uint8>((hash >> 8) % (256u - texel[0]));
            texel[2] = static_cast<uint8>((hash >> 16) % (256u - texel[0] - texel[1]));
            texel[3] = static_cast<uint8>(255u - texel[0] - texel[1] - texel[2]);
        }
    return weights;
}

// The weights tent at coarser sample (i, j) of a level of `width` x `height` four-weight samples:
// [1 2 1] x [1 2 1] around (2i, 2j), edges clamped; each weight the floor of its share of 255, the
// remainder on the largest (the lowest channel on a tie).
std::array<uint8, 4> ReferenceWeightsTent(const std::vector<uint8>& grid, uint32 width, uint32 height, uint32 i, uint32 j)
{
    std::array<uint32, 4> sums{};
    for (int32 dz = -1; dz <= 1; ++dz)
        for (int32 dx = -1; dx <= 1; ++dx)
        {
            const uint32 weight = static_cast<uint32>((2 - std::abs(dz)) * (2 - std::abs(dx)));
            const uint32 x = ClampSample(2 * static_cast<int64>(i) + dx, width);
            const uint32 z = ClampSample(2 * static_cast<int64>(j) + dz, height);
            for (uint32 c = 0; c < 4u; ++c)
                sums[c] += weight * grid[(static_cast<std::size_t>(z) * width + x) * 4u + c];
        }
    const uint32 total = sums[0] + sums[1] + sums[2] + sums[3];
    std::array<uint8, 4> out{};
    uint32 assigned = 0;
    uint32 largest = 0;
    for (uint32 c = 0; c < 4u; ++c)
    {
        out[c] = static_cast<uint8>(sums[c] * 255u / total);
        assigned += out[c];
        largest = sums[c] > sums[largest] ? c : largest;
    }
    out[largest] = static_cast<uint8>(out[largest] + 255u - assigned);
    return out;
}

} // namespace

// ---- Format ----------------------------------------------------------------------------------------

TEST(PageStoreFormatTest, EntryBitPackAndSentinelMatchTheSculptPageTable)
{
    static_assert(kNoPage == CBTTerrain::kSculptNoPage);
    static_assert(kPageSlotMask == CBTTerrain::kSculptPageIdMask);
    static_assert(kPageLevelShift == CBTTerrain::kSculptPageLevelShift);
    const uint32 entry = PackPageEntry(0x00ABCDEFu, 7u);
    EXPECT_EQ(PageEntrySlot(entry), 0x00ABCDEFu);
    EXPECT_EQ(PageEntryLevel(entry), 7u);
    EXPECT_NE(PackPageEntry(kPageSlotMask, 254u), kNoPage);
}

TEST(PageStoreFormatTest, PyramidOfTheSixtyByFifteenKilometerDemHasTheNotesPageCounts)
{
    // 60 x 15 km at 0.25 m: the design note's 12 levels, 1,875 x 469 level-0 pages, 1,173,760 in all.
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(240000u, 60000u);
    ASSERT_EQ(levels.size(), 12u);
    EXPECT_EQ(levels[0].PagesX, 1875u);
    EXPECT_EQ(levels[0].PagesZ, 469u);
    EXPECT_EQ(levels[6].SamplesX, 3751u); // the first pinned level (16 m texels)
    EXPECT_EQ(levels.back().PagesX, 1u);
    EXPECT_EQ(levels.back().PagesZ, 1u);
    EXPECT_EQ(levels.back().FirstEntry + 1u, 1173760u);
}

TEST(PageStoreFormatTest, SixteenBitStepIsTheFinestPowerOfTwoWhoseWordsHoldTheRange)
{
    // The DEM's outer terrain spans 132.7 m in one page: 2 mm steps (2^-9) need 67,942 words, 4 mm
    // (2^-8) fit.
    EXPECT_EQ(SixteenBitStepExponent(304.3f - 171.6f), -8);
    // The boundary: 65,533 steps fit (two are left for the base's floor and the top's rounding).
    EXPECT_EQ(SixteenBitStepExponent(std::ldexp(65533.0f, -10)), -10);
    EXPECT_EQ(SixteenBitStepExponent(std::ldexp(65534.0f, -10)), -9);
    // Never finer than 2^-20, flat or not.
    EXPECT_EQ(SixteenBitStepExponent(0.0f), kHeightStepExponentMin);
    EXPECT_EQ(SixteenBitStepExponent(1.0e-6f), kHeightStepExponentMin);
    // Earth's relief in one page needs a 50 cm step (2^-1).
    EXPECT_EQ(SixteenBitStepExponent(19900.0f), -1);
}

TEST(PageStoreFormatTest, NormalizedStepKeepsTodaysSixteenBitValues)
{
    constexpr float32 kInv65535 = 1.0f / 65535.0f; // Terrain/Heightfield.cpp's decode
    for (uint32 word = 0; word <= 65535u; word += 97u)
    {
        const float32 today = static_cast<float32>(word) * kInv65535;
        EXPECT_EQ(std::bit_cast<uint32>(QuantizeHeight(today, kNormalizedHeightStep)), std::bit_cast<uint32>(today));
    }
}

TEST(PageStoreFormatTest, AThirtyTwoBitPageBesideASixteenBitOneSharesItsApronIntegers)
{
    // Two level-0 pages side by side at a 2^-6 m step: the left one rolls gently, the right one
    // holds a 2 km tower in its owned samples, so its range passes 65,535 steps and it stores
    // 32-bit integers. The columns each stores of the other decode bit for bit alike.
    PageStoreHeader header;
    header.SamplesX = 2u * kPageOwnedSamples + 1u;
    header.SamplesZ = kPageOwnedSamples;
    header.StepPerLevel.fill(std::ldexp(1.0f, -6));
    const float32 step = header.StepPerLevel[0];
    const auto height = [&](int64 x, int64 z) {
        const uint32 cx = ClampSample(x, header.SamplesX);
        const uint32 cz = ClampSample(z, header.SamplesZ);
        const bool tower = cx >= 180u && cx < 200u && cz >= 40u && cz < 60u;
        return QuantizeHeight(Relief(cx, cz) + (tower ? 2000.0f : 0.0f), step);
    };
    std::array<EncodedPage, 2> encoded;
    std::array<PageIndexEntry, 2> entries;
    std::array<std::vector<float32>, 2> decoded;
    for (uint32 page = 0; page < 2u; ++page)
    {
        std::vector<float32> samples(kPageSampleCount);
        float32 lowest = std::numeric_limits<float32>::max();
        float32 highest = std::numeric_limits<float32>::lowest();
        for (uint32 r = 0; r < kPageStrideSamples; ++r)
        {
            for (uint32 c = 0; c < kPageStrideSamples; ++c)
            {
                const float32 value = height(int64(page) * kPageOwnedSamples + c - 1, int64(r) - 1);
                samples[static_cast<std::size_t>(r) * kPageStrideSamples + c] = value;
                if (r >= 1u && c >= 1u)
                {
                    lowest = std::min(lowest, value);
                    highest = std::max(highest, value);
                }
            }
        }
        EncodeHeightPage(header, 0, samples, lowest, highest, encoded[page]);
        entries[page].MinWord = encoded[page].MinWord;
        entries[page].MaxWord = encoded[page].MaxWord;
        entries[page].Flags = encoded[page].Flags;
        decoded[page].resize(kPageSampleCount);
        DecodeHeightPage(header, 0, entries[page], encoded[page].Bytes, decoded[page]);
        for (std::size_t i = 0; i < kPageSampleCount; ++i)
            ASSERT_EQ(decoded[page][i], samples[i]) << page << " " << i;
    }
    ASSERT_FALSE(entries[0].HasWideWords());
    ASSERT_TRUE(entries[1].HasWideWords());
    EXPECT_EQ(encoded[0].Bytes.size(), PageStoredBytes(PageFieldFormat::HeightSteps, false));
    EXPECT_EQ(encoded[1].Bytes.size(), PageStoredBytes(PageFieldFormat::HeightSteps, true));
    for (uint32 r = 0; r < kPageStrideSamples; ++r)
    {
        const std::size_t row = static_cast<std::size_t>(r) * kPageStrideSamples;
        EXPECT_EQ(std::bit_cast<uint32>(decoded[0][row + kPageStrideSamples - 1u]),
                  std::bit_cast<uint32>(decoded[1][row + 1u]));
        EXPECT_EQ(std::bit_cast<uint32>(decoded[0][row + kPageStrideSamples - 2u]), std::bit_cast<uint32>(decoded[1][row]));
    }
}

TEST(PageStoreFormatTest, APageWhoseIntegersSpan65535StepsStaysSixteenBitAndOneMoreStoresThirtyTwo)
{
    // At a step of 1, a page of zeros with one sample at 65,535 fits its words exactly (the top word
    // is 65,535 above MinWord 0); one at 65,536 does not fit, so the page stores 32-bit integers.
    PageStoreHeader header;
    header.SamplesX = kPageOwnedSamples;
    header.SamplesZ = kPageOwnedSamples;
    header.StepPerLevel.fill(1.0f);
    for (const float32 top : {65535.0f, 65536.0f})
    {
        std::vector<float32> samples(kPageSampleCount, 0.0f);
        samples[kPageStrideSamples + 1u] = top;
        EncodedPage encoded;
        EncodeHeightPage(header, 0, samples, 0.0f, top, encoded);
        PageIndexEntry entry;
        entry.MinWord = encoded.MinWord;
        entry.MaxWord = encoded.MaxWord;
        entry.Flags = encoded.Flags;
        EXPECT_EQ(entry.MinWord, 0);
        EXPECT_EQ(entry.HasWideWords(), top == 65536.0f) << top;
        EXPECT_EQ(encoded.Bytes.size(), PageStoredBytes(PageFieldFormat::HeightSteps, top == 65536.0f)) << top;
        std::vector<float32> decoded(kPageSampleCount);
        DecodeHeightPage(header, 0, entry, encoded.Bytes, decoded);
        EXPECT_EQ(decoded, samples) << top;
    }
}

TEST(PageStoreFormatTest, PresentPageLookupEqualsTheDenseLookup)
{
    // The 60 x 15 km synthetic keeps every page present: 1,173,760 entries of 33 bytes (38.7 MB).
    PageStoreHeader synthetic;
    synthetic.SamplesX = 240000u;
    synthetic.SamplesZ = 60000u;
    const PageStoreLayout full = MakePageStoreLayout(synthetic);
    ASSERT_EQ(full.Entries.size(), 1173760u);
    EXPECT_EQ(full.Entries.size() * kPageIndexEntryBytes, 38734080u);
    for (uint8 level = 0; level < full.Levels.size(); ++level)
    {
        const PageStoreLevel& shape = full.Levels[level];
        for (uint32 z = 0; z < shape.PagesZ; z += 7u)
        {
            for (uint32 x = 0; x < shape.PagesX; x += 13u)
            {
                const PageIndexEntry* entry = full.FindPresent(PageAddress{0, level, x, z});
                ASSERT_NE(entry, nullptr) << int(level) << " " << x << "," << z;
                ASSERT_EQ(entry->Morton, PageMortonCode(x, z));
            }
        }
        EXPECT_FALSE(full.Contains(PageAddress{0, level, shape.PagesX, 0}));
    }

    // A sparse store: the lookup finds exactly the pages the dense presence marks, and an absent
    // page resolves to the ancestor the dense walk finds.
    PageStoreHeader header;
    header.SamplesX = 3000u;
    header.SamplesZ = 1700u;
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
    std::vector<uint8> present(static_cast<std::size_t>(DensePageCount(levels)));
    for (std::size_t i = 0; i < present.size(); ++i)
        present[i] = static_cast<uint8>(((i * 2654435761u) >> 7u) % 3u != 0u);
    present.back() = 1u;
    const PageStoreLayout sparse = MakePageStoreLayout(header, present);
    const auto densePresent = [&](const PageAddress& a) {
        const PageStoreLevel& shape = levels[a.Level];
        return present[static_cast<std::size_t>(shape.FirstEntry + static_cast<uint64>(a.Z) * shape.PagesX + a.X)] != 0;
    };
    std::size_t found = 0;
    for (uint8 level = 0; level < levels.size(); ++level)
    {
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
        {
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
            {
                const PageAddress address{0, level, x, z};
                ASSERT_EQ(sparse.FindPresent(address) != nullptr, densePresent(address))
                    << int(level) << " " << x << "," << z;
                found += densePresent(address) ? 1u : 0u;
                PageAddress walk = address;
                while (!densePresent(walk))
                    walk = walk.Parent();
                ASSERT_EQ(sparse.ResolvePresent(address), walk);
            }
        }
    }
    EXPECT_EQ(found, sparse.Entries.size());
}

TEST_F(PageStoreTest, StoreRoundTripsHeaderIndexAndPagesWithTheIndexInMortonOrder)
{
    PageStoreHeader header;
    header.SamplesX = 300;
    header.SamplesZ = 200;
    header.StepPerLevel.fill(0.5f);
    header.StepPerLevel[1] = 0.25f;
    header.ReferenceRadius = 6371000.0; // Earth's: samples stay h = r - R, so the pages decode as written
    header.Key = 0x1234567890ABCDEFull;
    const std::filesystem::path file = m_Dir / "roundtrip.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(file, MakePageStoreLayout(header)), "");

    const PageStoreLayout& layout = writer.Layout();
    std::vector<std::vector<float32>> written;
    for (uint8 level = 0; level < layout.Levels.size(); ++level)
    {
        for (uint32 pz = 0; pz < layout.Levels[level].PagesZ; ++pz)
        {
            for (uint32 px = 0; px < layout.Levels[level].PagesX; ++px)
            {
                std::vector<float32> samples(kPageSampleCount);
                for (uint32 i = 0; i < kPageSampleCount; ++i)
                    samples[i] = static_cast<float32>(level * 1000000u + pz * 10000u + px * 100u) + 0.5f * i;
                EncodedPage page;
                EncodeHeightPage(header, level, samples, samples.front(), samples.back(), page);
                ASSERT_TRUE(writer.WritePage(PageAddress{0, level, px, pz}, page));
                written.push_back(std::move(samples));
            }
        }
    }
    ASSERT_EQ(writer.Finish(), "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Height), "");
    EXPECT_EQ(reader.Layout().Header.Key, header.Key);
    EXPECT_EQ(reader.Layout().Header.SamplesX, 300u);
    EXPECT_EQ(reader.Layout().Header.StepPerLevel, writer.Layout().Header.StepPerLevel);
    EXPECT_EQ(reader.Layout().Header.StepPerLevel[1], 0.25f);
    EXPECT_EQ(reader.Layout().Header.StepPerLevel[3], 0.0f); // past the store's three levels
    EXPECT_EQ(reader.Layout().Header.ReferenceRadius, 6371000.0);
    ASSERT_EQ(reader.Layout().Levels.size(), 3u); // 300 x 200 -> 151 x 101 -> 76 x 51
    std::size_t next = 0;
    for (uint8 level = 0; level < reader.Layout().Levels.size(); ++level)
        for (uint32 pz = 0; pz < reader.Layout().Levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < reader.Layout().Levels[level].PagesX; ++px)
                EXPECT_EQ(PageSamples(reader, PageAddress{0, level, px, pz}), written[next++]);
    EXPECT_EQ(std::filesystem::file_size(file), reader.Layout().TotalBytes());

    // Level 0's index (3 x 2 pages) lists it in Morton order: (0,0) (1,0) (0,1) (1,1) (2,0) (2,1);
    // its pages lie in the order they were written, row by row.
    const PageStoreRange& level0 = reader.Layout().Ranges[0];
    ASSERT_EQ(level0.Count, 6u);
    const std::array<uint64, 6> morton{PageMortonCode(0, 0), PageMortonCode(1, 0), PageMortonCode(0, 1),
                                       PageMortonCode(1, 1), PageMortonCode(2, 0), PageMortonCode(2, 1)};
    for (std::size_t i = 0; i < morton.size(); ++i)
        EXPECT_EQ(reader.Layout().Entries[level0.First + i].Morton, morton[i]);
    const auto offset = [&](uint32 x, uint32 z) { return reader.Layout().FindPresent(PageAddress{0, 0, x, z})->Offset; };
    EXPECT_EQ(offset(0, 0), reader.Layout().DataOffset());
    EXPECT_LT(offset(0, 0), offset(1, 0));
    EXPECT_LT(offset(1, 0), offset(2, 0));
    EXPECT_LT(offset(2, 0), offset(0, 1));
}

TEST_F(PageStoreTest, EachFaceOfATwoFaceStoreReadsItsOwnPagesAndResolvesWithinItself)
{
    // Two faces of 300 x 200 samples (levels of 3 x 2, 2 x 1 and 1 x 1 pages). Face 0 holds every
    // page; face 1 lacks level 0's (1, 1), which resolves to its own face's level-1 page. Every page
    // stores its face, level and position, so a page read from the wrong face or level shows.
    PageStoreHeader header;
    header.SamplesX = 300;
    header.SamplesZ = 200;
    header.FaceCount = 2;
    header.StepPerLevel.fill(1.0f);
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
    const uint64 perFace = DensePageCount(levels);
    std::vector<uint8> present(static_cast<std::size_t>(2u * perFace), 1u);
    present[static_cast<std::size_t>(perFace + 1u * levels[0].PagesX + 1u)] = 0u;
    const auto valueOf = [](const PageAddress& a) {
        return static_cast<float32>(a.Face * 1000u + a.Level * 100u + a.Z * 10u + a.X);
    };
    const std::filesystem::path file = m_Dir / "faces.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(file, MakePageStoreLayout(header, present)), "");
    std::vector<PageAddress> written;
    for (uint8 face = 0; face < 2u; ++face)
        for (uint8 level = 0; level < levels.size(); ++level)
            for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
                for (uint32 px = 0; px < levels[level].PagesX; ++px)
                {
                    const PageAddress address{face, level, px, pz};
                    if (!writer.Layout().FindPresent(address))
                        continue;
                    const std::vector<float32> samples(kPageSampleCount, valueOf(address));
                    EncodedPage page;
                    EncodeHeightPage(header, level, samples, samples.front(), samples.front(), page);
                    ASSERT_TRUE(writer.WritePage(address, page));
                    written.push_back(address);
                }
    ASSERT_EQ(writer.Finish(), "");
    ASSERT_EQ(written.size(), 2u * perFace - 1u);

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Height), "");
    for (const PageAddress& address : written)
        EXPECT_EQ(PageSamples(reader, address).front(), valueOf(address))
            << int(address.Face) << " " << int(address.Level) << " " << address.X << "," << address.Z;
    EXPECT_EQ(reader.Layout().FindPresent(PageAddress{1, 0, 1, 1}), nullptr);
    EXPECT_EQ(reader.Layout().ResolvePresent(PageAddress{1, 0, 1, 1}), (PageAddress{1, 1, 0, 0}));
    EXPECT_EQ(reader.Layout().ResolvePresent(PageAddress{0, 0, 1, 1}), (PageAddress{0, 0, 1, 1}));
}

TEST_F(PageStoreTest, TruncatedOrForeignFilesAreRefusedWithAReason)
{
    PageStoreHeader header;
    header.SamplesX = 200;
    header.SamplesZ = 200;
    header.StepPerLevel.fill(1.0f);
    const std::filesystem::path file = m_Dir / "short.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(file, MakePageStoreLayout(header)), "");
    std::vector<float32> samples(kPageSampleCount, 1.0f);
    EncodedPage page;
    EncodeHeightPage(header, 0, samples, 1.0f, 1.0f, page);
    for (uint8 level = 0; level < writer.Layout().Levels.size(); ++level)
        for (uint32 pz = 0; pz < writer.Layout().Levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < writer.Layout().Levels[level].PagesX; ++px)
                ASSERT_TRUE(writer.WritePage(PageAddress{0, level, px, pz}, page));
    ASSERT_EQ(writer.Finish(), "");

    std::filesystem::resize_file(file, std::filesystem::file_size(file) - 1u);
    PageStoreReader reader;
    EXPECT_NE(reader.Open(file, PageFieldKind::Height).find("outside the store"), std::string::npos);
    EXPECT_FALSE(reader.IsOpen());

    const std::filesystem::path foreign = m_Dir / "foreign.gepage";
    std::ofstream(foreign, std::ios::binary) << std::string(64, 'x');
    EXPECT_NE(reader.Open(foreign, PageFieldKind::Height).find("not a page store"), std::string::npos);

    // A store of format 1 is refused with the way to cook it again.
    const std::filesystem::path older = m_Dir / "older.gepage";
    {
        std::vector<uint8> bytes = FileBytes(file);
        const uint32 version = 1u;
        std::memcpy(bytes.data() + 4u, &version, sizeof(version));
        std::ofstream(older, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const std::string refusal = reader.Open(older, PageFieldKind::Height);
    EXPECT_NE(refusal.find("the page store is format 1 and this engine reads format 2"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("export the game again"), std::string::npos) << refusal;
    EXPECT_FALSE(reader.IsOpen());

    // A level step past 2^100 can decode heights past float32: refused.
    const std::filesystem::path coarse = m_Dir / "coarse.gepage";
    {
        std::vector<uint8> bytes = FileBytes(file);
        const float32 step = std::ldexp(1.0f, kHeightStepExponentMax + 1);
        std::memcpy(bytes.data() + kPageStoreHeaderBytes + kPageStoreLevelBytes - sizeof(float32), &step, sizeof(step));
        std::ofstream(coarse, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const std::string coarseRefusal = reader.Open(coarse, PageFieldKind::Height);
    EXPECT_NE(coarseRefusal.find("height step is past 2^100"), std::string::npos) << coarseRefusal;
}

TEST_F(PageStoreTest, AbsentPagesResolveToTheirNearestPresentAncestor)
{
    PageStoreHeader header;
    header.SamplesX = 300;
    header.SamplesZ = 300;
    header.StepPerLevel.fill(1.0f);
    std::vector<uint8> present(MakePageStoreLayout(header).Entries.size(), 1u);
    // Level 0 is 3 x 3 pages; the near field covers all but the center page, and level 1 (2 x 2)
    // lacks the page above that center, so the center resolves two levels up.
    present[1u * 3u + 1u] = 0;
    present[9u + 0u] = 0;
    const PageStoreLayout layout = MakePageStoreLayout(header, present);

    EXPECT_EQ(layout.FindPresent(PageAddress{0, 0, 1, 1}), nullptr);
    EXPECT_EQ(layout.ResolvePresent(PageAddress{0, 0, 1, 1}), (PageAddress{0, 2, 0, 0}));
    EXPECT_EQ(layout.ResolvePresent(PageAddress{0, 0, 2, 2}), (PageAddress{0, 0, 2, 2}));
    EXPECT_EQ(layout.ResolvePresent(PageAddress{0, 0, 0, 0}), (PageAddress{0, 0, 0, 0}));
    EXPECT_EQ(layout.ResolvePresent(PageAddress{0, 1, 0, 0}), (PageAddress{0, 2, 0, 0}));
    EXPECT_EQ(layout.ResolvePresent(PageAddress{0, 0, 3, 0}), std::nullopt);

    const std::filesystem::path file = m_Dir / "sparse.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(file, layout), "");
    std::vector<float32> samples(kPageSampleCount, 7.0f);
    EncodedPage page;
    EncodeHeightPage(header, 0, samples, 7.0f, 7.0f, page);
    EXPECT_FALSE(writer.WritePage(PageAddress{0, 0, 1, 1}, page));
    for (uint8 level = 0; level < layout.Levels.size(); ++level)
        for (uint32 pz = 0; pz < layout.Levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < layout.Levels[level].PagesX; ++px)
                if (layout.FindPresent(PageAddress{0, level, px, pz}))
                {
                    ASSERT_TRUE(writer.WritePage(PageAddress{0, level, px, pz}, page));
                }
    ASSERT_EQ(writer.Finish(), "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Height), "");
    std::vector<uint8> out;
    EXPECT_FALSE(reader.ReadPageBytes(PageAddress{0, 0, 1, 1}, out));
    EXPECT_EQ(reader.Layout().ResolvePresent(PageAddress{0, 0, 1, 1}), (PageAddress{0, 2, 0, 0}));
    // Absent pages take no bytes: 9 + 4 + 1 pages less the two absent ones.
    EXPECT_EQ(reader.Layout().TotalBytes(),
              reader.Layout().DataOffset() + 12u * PageStoredBytes(PageFieldFormat::HeightSteps, false));
}

// ---- Cooking ---------------------------------------------------------------------------------------

TEST_F(PageStoreTest, CookedLevelZeroIsTheSourceInItsStepWithinHalfAStepWithEdgeClampedAprons)
{
    constexpr uint32 kWidth = 300;
    constexpr uint32 kHeight = 260;
    const std::filesystem::path source = m_Dir / "relief.r32";
    WriteR32(source, kWidth, kHeight, Relief);
    const std::filesystem::path store = m_Dir / "relief.gepage";
    const HeightCookResult result = Cook(source, kWidth, kHeight, store);
    ASSERT_EQ(result.Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store, PageFieldKind::Height), "");
    const PageStoreHeader& header = reader.Layout().Header;
    EXPECT_EQ(header.Units, PageHeightUnits::Absolute);
    EXPECT_GT(header.StepPerLevel[0], 0.0f);
    for (const PageIndexEntry& entry : reader.Layout().Entries)
        EXPECT_FALSE(entry.HasWideWords());
    EXPECT_EQ(result.PagesWritten, reader.Layout().Entries.size());

    const std::vector<float32> input = ReadR32(source, kWidth, kHeight);
    for (uint32 pz = 0; pz < 3u; ++pz)
    {
        for (uint32 px = 0; px < 3u; ++px)
        {
            const std::vector<float32> page = PageSamples(reader, PageAddress{0, 0, px, pz});
            for (uint32 r = 0; r < kPageStrideSamples; ++r)
            {
                for (uint32 c = 0; c < kPageStrideSamples; ++c)
                {
                    const uint32 x = ClampSample(static_cast<int64>(px) * kPageOwnedSamples + c - 1, kWidth);
                    const uint32 z = ClampSample(static_cast<int64>(pz) * kPageOwnedSamples + r - 1, kHeight);
                    const float32 sourceHeight = input[static_cast<std::size_t>(z) * kWidth + x];
                    const float32 stored = page[static_cast<std::size_t>(r) * kPageStrideSamples + c];
                    ASSERT_EQ(stored, QuantizeHeight(sourceHeight, header.StepPerLevel[0]))
                        << "page " << px << "," << pz << " stride " << c << "," << r;
                    ASSERT_LE(std::abs(stored - sourceHeight), 0.5f * header.StepPerLevel[0]);
                }
            }
        }
    }
}

TEST_F(PageStoreTest, ApronSamplesAreBitEqualToTheNeighborsOwnedSamplesAtEveryLevel)
{
    // A relief whose pages each span a different height range, so each page has its own base: a
    // sample two pages store is one integer above two bases and decodes alike in both.
    constexpr uint32 kWidth = 520;
    constexpr uint32 kHeight = 390;
    const std::filesystem::path source = m_Dir / "apron.r32";
    WriteR32(source, kWidth, kHeight, [](uint32 x, uint32 z) { return Relief(x, z) + 0.37f * static_cast<float32>(x); });
    const std::filesystem::path store = m_Dir / "apron.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, store).Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store, PageFieldKind::Height), "");
    std::size_t pairs = 0;
    for (uint8 level = 0; level < reader.Layout().Levels.size(); ++level)
    {
        const PageStoreLevel& shape = reader.Layout().Levels[level];
        for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
        {
            for (uint32 px = 0; px < shape.PagesX; ++px)
            {
                // Decoded values, compared bit for bit: what two pages' samplers read.
                const std::vector<float32> page = PageSamples(reader, PageAddress{0, level, px, pz});
                const auto word = [&](const std::vector<float32>& p, uint32 c, uint32 r) {
                    return std::bit_cast<uint32>(p[static_cast<std::size_t>(r) * kPageStrideSamples + c]);
                };
                if (px + 1u < shape.PagesX)
                {
                    const std::vector<float32> right = PageSamples(reader, PageAddress{0, level, px + 1u, pz});
                    for (uint32 r = 0; r < kPageStrideSamples; ++r)
                    {
                        ASSERT_EQ(word(page, kPageStrideSamples - 1u, r), word(right, 1u, r)) << int(level);
                        ASSERT_EQ(word(page, kPageStrideSamples - 2u, r), word(right, 0u, r)) << int(level);
                    }
                    ++pairs;
                }
                if (pz + 1u < shape.PagesZ)
                {
                    const std::vector<float32> above = PageSamples(reader, PageAddress{0, level, px, pz + 1u});
                    for (uint32 c = 0; c < kPageStrideSamples; ++c)
                    {
                        ASSERT_EQ(word(page, c, kPageStrideSamples - 1u), word(above, c, 1u)) << int(level);
                        ASSERT_EQ(word(page, c, kPageStrideSamples - 2u), word(above, c, 0u)) << int(level);
                    }
                    ++pairs;
                }
            }
        }
    }
    EXPECT_GE(pairs, 20u);
}

TEST_F(PageStoreTest, CoarserLevelsAreTheTentOfTheStoredLevelBelow)
{
    constexpr uint32 kWidth = 300;
    constexpr uint32 kHeight = 260;
    const std::filesystem::path source = m_Dir / "tent.r32";
    WriteR32(source, kWidth, kHeight, Relief);
    const std::filesystem::path store = m_Dir / "tent.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, store).Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store, PageFieldKind::Height), "");
    const PageStoreHeader& header = reader.Layout().Header;
    for (uint8 level = 1; level < reader.Layout().Levels.size(); ++level)
    {
        const PageStoreLevel& finer = reader.Layout().Levels[level - 1u];
        const PageStoreLevel& coarser = reader.Layout().Levels[level];
        const std::vector<float32> below = LevelGrid(reader, level - 1u);
        const std::vector<float32> grid = LevelGrid(reader, level);
        const auto at = [&](int64 x, int64 z) {
            return below[static_cast<std::size_t>(ClampSample(z, finer.SamplesZ)) * finer.SamplesX +
                         ClampSample(x, finer.SamplesX)];
        };
        for (uint32 j = 0; j < coarser.SamplesZ; ++j)
        {
            for (uint32 i = 0; i < coarser.SamplesX; ++i)
            {
                const auto across = [&](int64 z) {
                    return 0.25f * at(2 * int64(i) - 1, z) + 0.5f * at(2 * int64(i), z) + 0.25f * at(2 * int64(i) + 1, z);
                };
                const float32 tent = 0.25f * across(2 * int64(j) - 1) + 0.5f * across(2 * int64(j)) +
                                     0.25f * across(2 * int64(j) + 1);
                // Each level is the tent of the level below in its own step, within half a step of it.
                const float32 stored = grid[static_cast<std::size_t>(j) * coarser.SamplesX + i];
                ASSERT_EQ(stored, QuantizeHeight(tent, header.StepPerLevel[level]))
                    << "level " << int(level) << " sample " << i << "," << j;
                ASSERT_LE(std::abs(stored - tent), 0.5f * header.StepPerLevel[level]);
            }
        }
    }
}

TEST_F(PageStoreTest, TentKeepsEveryCoarserSampleAtItsLatticePosition)
{
    // A plane through the origin with integer heights, on every level's lattice: the tent of a
    // plane centered on a sample is the sample, so level N sample j is the plane at level-0
    // sample j * 2^N wherever the tent stays inside the field. A cell-centered average would sit
    // half a finer cell off.
    constexpr uint32 kWidth = 401;
    constexpr uint32 kHeight = 257;
    const std::filesystem::path source = m_Dir / "plane.r32";
    WriteR32(source, kWidth, kHeight,
             [](uint32 x, uint32 z) { return 2.0f * static_cast<float32>(x) + 3.0f * static_cast<float32>(z); });
    const std::filesystem::path store = m_Dir / "plane.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, store).Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store, PageFieldKind::Height), "");
    for (uint8 level = 1; level < reader.Layout().Levels.size(); ++level)
    {
        const PageStoreLevel& shape = reader.Layout().Levels[level];
        const std::vector<float32> grid = LevelGrid(reader, level);
        const uint32 step = 1u << level;
        for (uint32 j = 1; (j + 1u) * step < kHeight; ++j)
            for (uint32 i = 1; (i + 1u) * step < kWidth; ++i)
                ASSERT_EQ(grid[static_cast<std::size_t>(j) * shape.SamplesX + i],
                          2.0f * static_cast<float32>(i * step) + 3.0f * static_cast<float32>(j * step))
                    << "level " << int(level) << " sample " << i << "," << j;
    }
}

TEST_F(PageStoreTest, PageBoundsHoldTheLevelZeroSurfaceUnderTheirFootprintAndEveryStoredSample)
{
    constexpr uint32 kWidth = 520;
    constexpr uint32 kHeight = 390;
    const std::filesystem::path source = m_Dir / "bounds.r32";
    WriteR32(source, kWidth, kHeight, Relief);
    const std::filesystem::path store = m_Dir / "bounds.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, store).Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store, PageFieldKind::Height), "");
    const PageStoreHeader& header = reader.Layout().Header;
    const std::vector<float32> level0 = LevelGrid(reader, 0);
    for (uint8 level = 0; level < reader.Layout().Levels.size(); ++level)
    {
        const PageStoreLevel& shape = reader.Layout().Levels[level];
        const uint32 span = kPageOwnedSamples << level;
        for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
        {
            for (uint32 px = 0; px < shape.PagesX; ++px)
            {
                float32 lowest = std::numeric_limits<float32>::max();
                float32 highest = std::numeric_limits<float32>::lowest();
                for (uint32 z = pz * span; z <= std::min(pz * span + span, kHeight - 1u); ++z)
                    for (uint32 x = px * span; x <= std::min(px * span + span, kWidth - 1u); ++x)
                    {
                        lowest = std::min(lowest, level0[static_cast<std::size_t>(z) * kWidth + x]);
                        highest = std::max(highest, level0[static_cast<std::size_t>(z) * kWidth + x]);
                    }
                const PageIndexEntry* entry = reader.Layout().FindPresent(PageAddress{0, level, px, pz});
                ASSERT_NE(entry, nullptr);
                if (px * span > kWidth - 1u || pz * span > kHeight - 1u)
                    continue; // a page wholly past the field's edge holds edge samples only
                // The bounds also hold every sample the page stores, apron included: the base of its
                // words. At level 0 they are exactly the lowest and highest of both.
                const std::vector<float32> page = PageSamples(reader, PageAddress{0, level, px, pz});
                const float32 storedLow = std::min(lowest, *std::min_element(page.begin(), page.end()));
                const float32 storedHigh = std::max(highest, *std::max_element(page.begin(), page.end()));
                if (level == 0)
                {
                    EXPECT_EQ(DecodeHeightWord(header, level, entry->MinWord), storedLow);
                    EXPECT_EQ(DecodeHeightWord(header, level, entry->MaxWord), storedHigh);
                }
                EXPECT_LE(DecodeHeightWord(header, level, entry->MinWord), storedLow) << int(level) << " " << px << "," << pz;
                EXPECT_GE(DecodeHeightWord(header, level, entry->MaxWord), storedHigh) << int(level) << " " << px << "," << pz;
            }
        }
    }
}

TEST_F(PageStoreTest, RecookOfAnEditedSourceRewritesOnlyThePagesWhoseBytesChanged)
{
    constexpr uint32 kWidth = 640;
    constexpr uint32 kHeight = 520;
    const std::filesystem::path before = m_Dir / "before.r32";
    WriteR32(before, kWidth, kHeight, Relief);
    const std::filesystem::path storeA = m_Dir / "terrain-a.gepage";
    ASSERT_EQ(Cook(before, kWidth, kHeight, storeA).Error, "");
    PageStoreLayout layoutA;
    {
        PageStoreReader reader;
        ASSERT_EQ(reader.Open(storeA, PageFieldKind::Height), "");
        layoutA = reader.Layout();
    }

    // A small edit inside one level-0 page, within the source's range (the quantum is unchanged).
    const std::filesystem::path after = m_Dir / "after.r32";
    WriteR32(after, kWidth, kHeight, [](uint32 x, uint32 z) {
        const bool edited = x >= 300 && x < 306 && z >= 200 && z < 205;
        return edited ? 260.0f : Relief(x, z);
    });
    const std::filesystem::path storeB = m_Dir / "terrain-b.gepage";
    const HeightCookResult patched = Cook(after, kWidth, kHeight, storeB, storeA);
    ASSERT_EQ(patched.Error, "");
    EXPECT_TRUE(patched.Patched);
    EXPECT_FALSE(std::filesystem::exists(storeA)); // moved and patched, not copied

    const std::filesystem::path fresh = m_Dir / "terrain-fresh.gepage";
    const HeightCookResult full = Cook(after, kWidth, kHeight, fresh);
    ASSERT_EQ(full.Error, "");
    EXPECT_FALSE(full.Patched);

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(fresh, PageFieldKind::Height), "");
    uint64 changed = 0;
    for (std::size_t i = 0; i < layoutA.Entries.size(); ++i)
        changed += layoutA.Entries[i].Hash != reader.Layout().Entries[i].Hash ? 1u : 0u;
    // The page with the edit and the one ancestor per coarser level above it.
    EXPECT_GE(changed, reader.Layout().Levels.size());
    EXPECT_LT(changed, layoutA.Entries.size() / 4u);
    EXPECT_EQ(patched.PagesWritten, changed);
    EXPECT_EQ(patched.PagesWritten + patched.PagesUnchanged, layoutA.Entries.size());
    EXPECT_EQ(FileBytes(storeB), FileBytes(fresh));
}

TEST_F(PageStoreTest, ARangeChangeCooksAFreshStoreAndLeavesThePreviousOne)
{
    constexpr uint32 kWidth = 200;
    constexpr uint32 kHeight = 200;
    const std::filesystem::path before = m_Dir / "before.r32";
    WriteR32(before, kWidth, kHeight, Relief);
    const std::filesystem::path storeA = m_Dir / "a.gepage";
    ASSERT_EQ(Cook(before, kWidth, kHeight, storeA).Error, "");
    const std::filesystem::path after = m_Dir / "after.r32";
    WriteR32(after, kWidth, kHeight, [](uint32 x, uint32 z) { return Relief(x, z) + (x == 7 && z == 9 ? 500.0f : 0.0f); });
    const std::filesystem::path storeB = m_Dir / "b.gepage";
    const HeightCookResult result = Cook(after, kWidth, kHeight, storeB, storeA);
    ASSERT_EQ(result.Error, "");
    EXPECT_FALSE(result.Patched);
    EXPECT_TRUE(std::filesystem::exists(storeA));
}

TEST_F(PageStoreTest, LevelStepsComeFromTheDataAndOnlyTheSteepestPagesStoreThirtyTwoBits)
{
    // 11 x 11 level-0 pages of the relief, one with a 3 km tower. Every level of two or more pages
    // (121, 36, 9 and 4) may let one page escape, so it keeps the step the relief alone gets and the
    // tower's page alone stores 32-bit integers. The one-page top level has no page to spare: its
    // step coarsens until the tower fits 16 bits.
    constexpr uint32 kSide = 11u * kPageOwnedSamples;
    const auto tower = [](uint32 x, uint32 z) {
        const bool inside = x >= 650u && x < 670u && z >= 650u && z < 670u;
        return Relief(x, z) + (inside ? 3000.0f : 0.0f);
    };
    const std::filesystem::path plain = m_Dir / "plain.r32";
    const std::filesystem::path towered = m_Dir / "tower.r32";
    WriteR32(plain, kSide, kSide, Relief);
    WriteR32(towered, kSide, kSide, tower);
    const std::filesystem::path plainStore = m_Dir / "plain.gepage";
    const std::filesystem::path towerStore = m_Dir / "tower.gepage";
    ASSERT_EQ(Cook(plain, kSide, kSide, plainStore).Error, "");
    ASSERT_EQ(Cook(towered, kSide, kSide, towerStore).Error, "");

    PageStoreReader plainReader;
    PageStoreReader reader;
    ASSERT_EQ(plainReader.Open(plainStore, PageFieldKind::Height), "");
    ASSERT_EQ(reader.Open(towerStore, PageFieldKind::Height), "");
    const PageStoreLayout& layout = reader.Layout();
    ASSERT_EQ(layout.Levels[0].PagesX, 11u);
    ASSERT_EQ(layout.Levels.size(), 5u);
    for (uint8 level = 0; level < layout.Levels.size(); ++level)
    {
        const bool top = level + 1u == layout.Levels.size();
        const float32 plainStep = plainReader.Layout().Header.StepPerLevel[level];
        if (top)
            EXPECT_GT(layout.Header.StepPerLevel[level], plainStep);
        else
            EXPECT_EQ(layout.Header.StepPerLevel[level], plainStep) << int(level);
        const PageStoreRange& range = layout.Ranges[level];
        uint64 wide = 0;
        for (uint64 i = 0; i < range.Count; ++i)
            wide += layout.Entries[range.First + i].HasWideWords() ? 1u : 0u;
        EXPECT_EQ(wide, top ? 0u : 1u) << int(level);
    }
    EXPECT_TRUE(layout.FindPresent(PageAddress{0, 0, 5, 5})->HasWideWords());

    // Every level-0 sample, the tower's page included, is within half a step of its source.
    const std::vector<float32> grid = LevelGrid(reader, 0);
    const float32 half = 0.5f * layout.Header.StepPerLevel[0];
    for (uint32 z = 0; z < kSide; ++z)
        for (uint32 x = 0; x < kSide; ++x)
            ASSERT_LE(std::abs(grid[static_cast<std::size_t>(z) * kSide + x] - tower(x, z)), half) << x << "," << z;
}

TEST_F(PageStoreTest, AHeightStoreKeepsItsBytesWhenTheStoreCarriesSurfaceKinds)
{
    // A 3 x 3 page relief with a tower, so its levels hold 16-bit and 32-bit pages. The whole
    // file's hash is the one the height-only store had: a height store is never cooked again.
    constexpr uint32 kSide = 3u * kPageOwnedSamples;
    const auto tower = [](uint32 x, uint32 z) {
        const bool inside = x >= 150u && x < 170u && z >= 150u && z < 170u;
        return Relief(x, z) + (inside ? 3000.0f : 0.0f);
    };
    const std::filesystem::path source = m_Dir / "identity.r32";
    WriteR32(source, kSide, kSide - 7u, tower);
    const std::filesystem::path store = m_Dir / "identity.gepage";
    ASSERT_EQ(Cook(source, kSide, kSide - 7u, store).Error, "");
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store, PageFieldKind::Height), "");
    EXPECT_TRUE(reader.Layout().FindPresent(PageAddress{0, 0, 1, 1})->HasWideWords());
    EXPECT_FALSE(reader.Layout().FindPresent(PageAddress{0, 0, 0, 0})->HasWideWords());
    const std::vector<uint8> bytes = FileBytes(store);
    EXPECT_EQ(Hashing::Fnv1a64(bytes.data(), bytes.size()), 0xac1c78a7532c8722ull);
}

// ---- Surface kinds ---------------------------------------------------------------------------------

TEST_F(PageStoreTest, ASplatStoreRoundTripsByteExactAtEveryLevel)
{
    // 300 x 270 weights: 3 x 3 pages, then 2 x 2, then one. Level 0 reads back as written; each
    // coarser level is the weights tent of the level below as stored; every apron is the
    // neighbor's sample, edge-clamped past the field.
    constexpr uint32 kWidth = 300u;
    constexpr uint32 kHeight = 270u;
    const std::vector<uint8> weights = SplatWeights(kWidth, kHeight);
    const std::filesystem::path file = m_Dir / "splat.gepage";
    BuildSurfaceStore(file, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kWidth, kHeight, weights);
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Splat), "");
    const PageStoreLayout& layout = reader.Layout();
    ASSERT_EQ(layout.Levels.size(), 3u);
    EXPECT_EQ(layout.StoredBytes(layout.Entries.front()), 67600u);

    std::vector<uint8> expected = weights;
    for (uint8 level = 0; level < layout.Levels.size(); ++level)
    {
        const PageStoreLevel& shape = layout.Levels[level];
        if (level > 0)
        {
            const PageStoreLevel& finer = layout.Levels[level - 1u];
            const std::vector<uint8> below = SurfaceLevelGrid(reader, static_cast<uint8>(level - 1u));
            expected.assign(static_cast<std::size_t>(shape.SamplesX) * shape.SamplesZ * 4u, 0u);
            for (uint32 j = 0; j < shape.SamplesZ; ++j)
                for (uint32 i = 0; i < shape.SamplesX; ++i)
                {
                    const std::array<uint8, 4> texel = ReferenceWeightsTent(below, finer.SamplesX, finer.SamplesZ, i, j);
                    std::memcpy(&expected[(static_cast<std::size_t>(j) * shape.SamplesX + i) * 4u], texel.data(), 4u);
                }
        }
        for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
            for (uint32 px = 0; px < shape.PagesX; ++px)
            {
                const std::vector<uint8> page = SurfacePage(reader, PageAddress{0, level, px, pz});
                for (uint32 r = 0; r < kPageStrideSamples; ++r)
                    for (uint32 c = 0; c < kPageStrideSamples; ++c)
                    {
                        const uint32 x = ClampSample(static_cast<int64>(px) * kPageOwnedSamples + c - 1, shape.SamplesX);
                        const uint32 z = ClampSample(static_cast<int64>(pz) * kPageOwnedSamples + r - 1, shape.SamplesZ);
                        ASSERT_EQ(std::memcmp(&page[(static_cast<std::size_t>(r) * kPageStrideSamples + c) * 4u],
                                              &expected[(static_cast<std::size_t>(z) * shape.SamplesX + x) * 4u], 4u),
                                  0)
                            << "level " << int(level) << " page " << px << "," << pz << " sample " << c << "," << r;
                    }
            }
    }
}

TEST_F(PageStoreTest, TheWeightsTentKeepsEveryTexelsWeightsSummingTo255)
{
    constexpr uint32 kSide = 260u;
    const std::filesystem::path file = m_Dir / "sum.gepage";
    BuildSurfaceStore(file, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kSide, kSide, SplatWeights(kSide, kSide));
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Splat), "");
    for (uint8 level = 1; level < reader.Layout().Levels.size(); ++level)
    {
        const PageStoreLevel& shape = reader.Layout().Levels[level];
        for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
            for (uint32 px = 0; px < shape.PagesX; ++px)
            {
                const std::vector<uint8> page = SurfacePage(reader, PageAddress{0, level, px, pz});
                for (std::size_t t = 0; t < page.size(); t += 4u)
                    ASSERT_EQ(page[t] + page[t + 1u] + page[t + 2u] + page[t + 3u], 255)
                        << "level " << int(level) << " texel " << t / 4u;
            }
    }
}

namespace
{

// The level-0 pages of `layout` whose stored samples hold a sample of `rect`.
std::vector<PageAddress> Level0PagesHolding(const PageStoreLayout& layout, const PageSampleRect& rect)
{
    std::vector<PageAddress> pages;
    const PageStoreLevel& shape = layout.Levels.front();
    for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
        for (uint32 px = 0; px < shape.PagesX; ++px)
        {
            const int64 firstX = static_cast<int64>(px) * kPageOwnedSamples - 1;
            const int64 firstZ = static_cast<int64>(pz) * kPageOwnedSamples - 1;
            if (firstX <= rect.MaxX && firstX + kPageStrideSamples > rect.MinX && firstZ <= rect.MaxZ &&
                firstZ + kPageStrideSamples > rect.MinZ)
                pages.push_back(PageAddress{0, 0, px, pz});
        }
    return pages;
}

// Every page of two stores of one layout holds the same samples.
void ExpectSameSurfacePages(const PageStoreReader& actual, const PageStoreReader& expected)
{
    for (uint8 level = 0; level < expected.Layout().Levels.size(); ++level)
    {
        const PageStoreLevel& shape = expected.Layout().Levels[level];
        for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
            for (uint32 px = 0; px < shape.PagesX; ++px)
                ASSERT_EQ(SurfacePage(actual, PageAddress{0, level, px, pz}),
                          SurfacePage(expected, PageAddress{0, level, px, pz}))
                    << "level " << int(level) << " page " << px << "," << pz;
    }
}

// `weights` with the texels of `rect` turned: each texel's four weights in reverse order.
std::vector<uint8> TurnedWeights(std::vector<uint8> weights, uint32 width, const PageSampleRect& rect)
{
    for (uint32 z = rect.MinZ; z <= rect.MaxZ; ++z)
        for (uint32 x = rect.MinX; x <= rect.MaxX; ++x)
        {
            uint8* texel = &weights[(static_cast<std::size_t>(z) * width + x) * 4u];
            std::reverse(texel, texel + 4);
        }
    return weights;
}

// The stride samples of level-0 page `address` of a row-major grid of `format` texels, the apron
// edge-clamped.
std::vector<uint8> GridPageSamples(const std::vector<uint8>& grid, uint32 width, uint32 height, PageFieldFormat format,
                                   const PageAddress& address)
{
    const uint32 stride = PageStrideTexels(format);
    const int64 apron = (stride - kPageOwnedSamples) / 2u;
    const uint32 texel = PageTexelBytes(format);
    std::vector<uint8> samples(static_cast<std::size_t>(stride) * stride * texel);
    for (uint32 r = 0; r < stride; ++r)
        for (uint32 c = 0; c < stride; ++c)
        {
            const int64 x = std::clamp<int64>(int64{address.X} * kPageOwnedSamples - apron + c, 0, width - 1);
            const int64 z = std::clamp<int64>(int64{address.Z} * kPageOwnedSamples - apron + r, 0, height - 1);
            std::memcpy(&samples[(static_cast<std::size_t>(r) * stride + c) * texel],
                        &grid[(static_cast<std::size_t>(z) * width + static_cast<std::size_t>(x)) * texel], texel);
        }
    return samples;
}

} // namespace

TEST_F(PageStoreTest, APatchedPageIsWrittenAtItsAddressAndTheBytesTheStoreHoldsAreNotWrittenAgain)
{
    constexpr uint32 kWidth = 300u;
    constexpr uint32 kHeight = 270u;
    const std::filesystem::path file = m_Dir / "splat.gepage";
    BuildSurfaceStore(file, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kWidth, kHeight,
                      SplatWeights(kWidth, kHeight));
    const PageAddress address{0, 0, 1, 1};
    std::vector<uint8> page;
    PageStoreLayout layout;
    {
        PageStoreReader reader;
        ASSERT_EQ(reader.Open(file, PageFieldKind::Splat), "");
        page = SurfacePage(reader, address);
        layout = reader.Layout();
    }
    std::reverse(page.begin(), page.end());

    PageStoreWriter writer;
    ASSERT_EQ(writer.OpenForPatch(file, file, layout), "");
    PageStorePatcher patcher(writer, nullptr);
    ASSERT_TRUE(patcher.WritePage(address, page));
    ASSERT_TRUE(patcher.WritePage(address, page));
    EXPECT_FALSE(patcher.WritePage(address, std::span<const uint8>(page).first(page.size() - 4u)));
    EXPECT_EQ(patcher.PagesWritten(), 1u);
    EXPECT_EQ(patcher.PagesUnchanged(), 1u);
    ASSERT_EQ(writer.Finish(), "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Splat), "");
    EXPECT_EQ(SurfacePage(reader, address), page);

    // A Bc5 store refilters from its unencoded samples: without its unencoded store it is refused.
    PageStoreHeader header;
    header.Format = PageFieldFormat::Bc5;
    header.Filter = PageFieldFilter::NormalTent;
    header.SamplesX = kWidth;
    header.SamplesZ = kHeight;
    PageStoreWriter normals;
    ASSERT_EQ(normals.Create(m_Dir / "normal.gepage", MakePageStoreLayout(header)), "");
    PageStorePatcher normalPatcher(normals, nullptr);
    const std::vector<uint8> samples(static_cast<std::size_t>(136u) * 136u * 2u, 128u);
    EXPECT_FALSE(normalPatcher.WritePage(PageAddress{0, 0, 0, 0}, samples));
    EXPECT_FALSE(normalPatcher.RefilterAncestors(PageSampleRect{0, 0, 10, 10}));
}

TEST_F(PageStoreTest, RefilteringTheAncestorsOfAPatchedRectGivesTheFullBuildAndRewritesOnlyThePagesThatChanged)
{
    // 600 x 600 weights: 5 x 5 pages, then 3 x 3, 2 x 2 and one. The rect crosses page edges at
    // level 0 and level 1.
    constexpr uint32 kSide = 600u;
    const PageSampleRect rect{200u, 140u, 330u, 260u};
    const std::vector<uint8> before = SplatWeights(kSide, kSide);
    const std::vector<uint8> after = TurnedWeights(before, kSide, rect);
    const std::filesystem::path patched = m_Dir / "patched.gepage";
    const std::filesystem::path built = m_Dir / "built.gepage";
    BuildSurfaceStore(patched, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kSide, kSide, before);
    BuildSurfaceStore(built, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kSide, kSide, after);

    PageStoreReader expected;
    ASSERT_EQ(expected.Open(built, PageFieldKind::Splat), "");
    PageStoreLayout layout;
    uint64 changedPages = 0;
    {
        PageStoreReader original;
        ASSERT_EQ(original.Open(patched, PageFieldKind::Splat), "");
        layout = original.Layout();
        for (std::size_t i = 0; i < layout.Entries.size(); ++i)
            changedPages += layout.Entries[i].Hash != expected.Layout().Entries[i].Hash ? 1u : 0u;
    }
    ASSERT_GT(changedPages, 9u);
    ASSERT_LT(changedPages, layout.Entries.size());

    JobSystem::WorkStealingThreadPool pool(4);
    PageStoreWriter writer;
    ASSERT_EQ(writer.OpenForPatch(patched, patched, layout), "");
    PageStorePatcher patcher(writer, &pool);
    for (const PageAddress& address : Level0PagesHolding(layout, rect))
        ASSERT_TRUE(patcher.WritePage(address, SurfacePage(expected, address)));
    ASSERT_TRUE(patcher.RefilterAncestors(rect));
    EXPECT_EQ(patcher.PagesWritten(), changedPages);
    ASSERT_EQ(writer.Finish(), "");

    PageStoreReader actual;
    ASSERT_EQ(actual.Open(patched, PageFieldKind::Splat), "");
    ExpectSameSurfacePages(actual, expected);
}

TEST_F(PageStoreTest, ABc5StorePatchedAPageAtATimeIsTheFullBuildFilteredFromItsUnencodedSamples)
{
    // The coarser Bc5 pages are filtered from the Rg8 store of the unencoded samples, never from
    // blocks, so the patched store holds the blocks of the full build, which filters unencoded rows.
    constexpr uint32 kSide = 600u;
    std::vector<uint8> normals(static_cast<std::size_t>(kSide) * kSide * 2u);
    for (std::size_t i = 0; i < normals.size(); ++i)
        normals[i] = static_cast<uint8>(64u + ((static_cast<uint32>(i) * 2654435761u) >> 25));
    const std::filesystem::path built = m_Dir / "built.gepage";
    BuildSurfaceStore(built, PageFieldFormat::Bc5, PageFieldFilter::NormalTent, kSide, kSide, normals);
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(built, PageFieldKind::Normal), "");

    const PageStoreHeader header = expected.Layout().Header;
    PageStoreHeader samplesHeader = header;
    samplesHeader.Format = PageFieldFormat::Rg8;
    const std::filesystem::path patched = m_Dir / "patched.gepage";
    PageStoreWriter writer;
    PageStoreWriter unencoded;
    ASSERT_EQ(writer.Create(patched, MakePageStoreLayout(header)), "");
    ASSERT_EQ(unencoded.Create(m_Dir / "samples.gepage", MakePageStoreLayout(samplesHeader)), "");
    JobSystem::WorkStealingThreadPool pool(4);
    PageStorePatcher patcher(writer, unencoded, &pool);
    const PageStoreLevel& level0 = expected.Layout().Levels.front();
    for (uint32 pz = level0.PagesZ; pz-- > 0;)
        for (uint32 px = level0.PagesX; px-- > 0;)
        {
            const PageAddress address{0, 0, px, pz};
            ASSERT_TRUE(patcher.WritePage(address, GridPageSamples(normals, kSide, kSide, header.Format, address)));
            ASSERT_TRUE(patcher.RefilterAncestors(PageSampleRect{px * kPageOwnedSamples, pz * kPageOwnedSamples,
                                                                 px * kPageOwnedSamples + kPageOwnedSamples - 1u,
                                                                 pz * kPageOwnedSamples + kPageOwnedSamples - 1u}));
        }
    ASSERT_EQ(writer.Finish(), "");
    ASSERT_EQ(unencoded.Finish(), "");
    PageStoreReader actual;
    ASSERT_EQ(actual.Open(patched, PageFieldKind::Normal), "");
    ExpectSameSurfacePages(actual, expected);
}

TEST_F(PageStoreTest, ACoarseLevelWrittenFirstIsReplacedByTheFilterOfLevelZeroAsItsPagesLand)
{
    // The cold bake's order: every coarser page is written first from its own evaluation (here a
    // flat weight), then the level-0 pages land in any order. A coarser page waits for every page
    // it reads; once the last one lands, the store equals the full build.
    constexpr uint32 kSide = 600u;
    const std::vector<uint8> weights = SplatWeights(kSide, kSide);
    const std::filesystem::path built = m_Dir / "built.gepage";
    BuildSurfaceStore(built, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kSide, kSide, weights);
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(built, PageFieldKind::Splat), "");

    PageStoreHeader header = expected.Layout().Header;
    const std::filesystem::path baked = m_Dir / "baked.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(baked, MakePageStoreLayout(header)), "");
    PageStorePatcher patcher(writer, nullptr);
    std::vector<uint8> flat(static_cast<std::size_t>(kPageSampleCount) * 4u, 0u);
    for (std::size_t t = 0; t < flat.size(); t += 4u)
        flat[t] = 255u;
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    for (uint8 level = static_cast<uint8>(levels.size() - 1u); level > 0; --level)
        for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < levels[level].PagesX; ++px)
                ASSERT_TRUE(patcher.WritePage(PageAddress{0, level, px, pz}, flat));

    // The first level-0 page alone changes no coarser page: each one it reaches also reads a
    // page not yet written.
    const uint64 coarsePages = patcher.PagesWritten();
    ASSERT_TRUE(patcher.WritePage(PageAddress{0, 0, 0, 0}, SurfacePage(expected, PageAddress{0, 0, 0, 0})));
    ASSERT_TRUE(patcher.RefilterAncestors(PageSampleRect{0, 0, 127, 127}));
    EXPECT_EQ(patcher.PagesWritten(), coarsePages + 1u);

    // The rest land from the far corner back, each followed by its refilter.
    for (uint32 pz = levels[0].PagesZ; pz-- > 0;)
        for (uint32 px = levels[0].PagesX; px-- > 0;)
        {
            const PageAddress address{0, 0, px, pz};
            ASSERT_TRUE(patcher.WritePage(address, SurfacePage(expected, address)));
            ASSERT_TRUE(patcher.RefilterAncestors(PageSampleRect{px * kPageOwnedSamples, pz * kPageOwnedSamples,
                                                                 px * kPageOwnedSamples + kPageOwnedSamples - 1u,
                                                                 pz * kPageOwnedSamples + kPageOwnedSamples - 1u}));
        }
    ASSERT_EQ(writer.Finish(), "");
    PageStoreReader actual;
    ASSERT_EQ(actual.Open(baked, PageFieldKind::Splat), "");
    ExpectSameSurfacePages(actual, expected);
}

TEST_F(PageStoreTest, ACoarsePageWrittenByWritePageIsANewInputToTheLevelAbove)
{
    // Level 1 is written with the bytes its refilter makes, so the refilter changes no level-1
    // page. Level 2 and up were written from another evaluation and must still become the filter.
    constexpr uint32 kSide = 600u;
    const std::filesystem::path built = m_Dir / "built.gepage";
    BuildSurfaceStore(built, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kSide, kSide,
                      SplatWeights(kSide, kSide));
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(built, PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    ASSERT_GE(levels.size(), 3u);

    const std::filesystem::path baked = m_Dir / "baked.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(baked, MakePageStoreLayout(expected.Layout().Header)), "");
    PageStorePatcher patcher(writer, nullptr);
    std::vector<uint8> flat(static_cast<std::size_t>(kPageSampleCount) * 4u, 0u);
    for (std::size_t t = 0; t < flat.size(); t += 4u)
        flat[t] = 255u;
    for (uint8 level = static_cast<uint8>(levels.size()); level-- > 0;)
        for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < levels[level].PagesX; ++px)
            {
                const PageAddress address{0, level, px, pz};
                ASSERT_TRUE(patcher.WritePage(address, level >= 2u ? flat : SurfacePage(expected, address)));
            }
    ASSERT_TRUE(patcher.RefilterAncestors(PageSampleRect{0, 0, kSide - 1u, kSide - 1u}));
    ASSERT_EQ(writer.Finish(), "");
    PageStoreReader actual;
    ASSERT_EQ(actual.Open(baked, PageFieldKind::Splat), "");
    ExpectSameSurfacePages(actual, expected);
}

TEST_F(PageStoreTest, AStoreGrowsItsPresentPagesInAPatchAndKeepsTheBytesOfThePagesItHeld)
{
    // The coarser levels are baked first into a store that holds only them; level 0 is added by a
    // patch, whose longer index moves the pages it covers. Every page then reads as the full build.
    constexpr uint32 kSide = 600u;
    const std::filesystem::path built = m_Dir / "built.gepage";
    BuildSurfaceStore(built, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kSide, kSide, SplatWeights(kSide, kSide));
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(built, PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    const std::size_t level0Pages = static_cast<std::size_t>(levels[0].PagesX) * levels[0].PagesZ;

    const std::filesystem::path grown = m_Dir / "grown.gepage";
    std::vector<uint8> present(static_cast<std::size_t>(DensePageCount(levels)), 1u);
    std::fill_n(present.begin(), level0Pages, uint8{0});
    {
        PageStoreWriter writer;
        ASSERT_EQ(writer.Create(grown, MakePageStoreLayout(expected.Layout().Header, present)), "");
        PageStorePatcher patcher(writer, nullptr);
        for (uint8 level = 1; level < levels.size(); ++level)
            for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
                for (uint32 px = 0; px < levels[level].PagesX; ++px)
                    ASSERT_TRUE(patcher.WritePage(PageAddress{0, level, px, pz},
                                                  SurfacePage(expected, PageAddress{0, level, px, pz})));
        ASSERT_EQ(writer.Finish(), "");
    }
    PageStoreLayout layout;
    {
        PageStoreReader coarse;
        ASSERT_EQ(coarse.Open(grown, PageFieldKind::Splat), "");
        EXPECT_EQ(coarse.Layout().ResolvePresent(PageAddress{0, 0, 3, 2}), (PageAddress{0, 1, 1, 1}));
        layout = coarse.Layout();
    }

    std::vector<uint8> level0(present.size(), 0u);
    std::fill_n(level0.begin(), level0Pages, uint8{1});
    PageStoreWriter writer;
    ASSERT_EQ(writer.OpenForPatch(grown, grown, AddPresentPages(layout, level0)), "");
    ASSERT_EQ(writer.Layout().Entries.size(), expected.Layout().Entries.size());
    PageStorePatcher patcher(writer, nullptr);
    for (uint32 pz = 0; pz < levels[0].PagesZ; ++pz)
        for (uint32 px = 0; px < levels[0].PagesX; ++px)
            ASSERT_TRUE(patcher.WritePage(PageAddress{0, 0, px, pz}, SurfacePage(expected, PageAddress{0, 0, px, pz})));
    ASSERT_EQ(writer.Finish(), "");
    PageStoreReader actual;
    ASSERT_EQ(actual.Open(grown, PageFieldKind::Splat), "");
    ExpectSameSurfacePages(actual, expected);
}

TEST_F(PageStoreTest, ABc5PagesApronBlockIsItsNeighborsEdgeBlockBitForBit)
{
    // 300 x 300 normals: 3 x 3 pages at level 0, 2 x 2 at level 1. A page stores its 128 samples and
    // a 4-sample apron each side, so its block grid sits on the page origin and its last block holds
    // the samples of the next page's first owned block.
    constexpr uint32 kSide = 300u;
    std::vector<uint8> normals(static_cast<std::size_t>(kSide) * kSide * 2u);
    for (std::size_t i = 0; i < normals.size(); ++i)
        normals[i] = static_cast<uint8>(64u + ((static_cast<uint32>(i) * 2654435761u) >> 25));
    const std::filesystem::path file = m_Dir / "normal.gepage";
    BuildSurfaceStore(file, PageFieldFormat::Bc5, PageFieldFilter::NormalTent, kSide, kSide, normals);
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Normal), "");
    const PageStoreLayout& layout = reader.Layout();
    EXPECT_EQ(layout.StoredBytes(layout.Entries.front()), 18496u);

    // Page p's last block starts at sample 128p - apron + stride - 4; page p + 1 stores that sample
    // at stride - 4 - 128, which must start a block of its own.
    const uint32 stride = PageStrideTexels(PageFieldFormat::Bc5);
    const uint32 blocks = stride / kPageBlockTexels;
    const uint32 shared = stride - kPageBlockTexels - kPageOwnedSamples;
    ASSERT_EQ(shared % kPageBlockTexels, 0u) << "the block grid is not on the page origin";
    const uint32 sharedBlock = shared / kPageBlockTexels;
    for (uint8 level = 0; level < 2u; ++level)
    {
        std::vector<uint8> page;
        std::vector<uint8> right;
        std::vector<uint8> below;
        ASSERT_TRUE(reader.ReadPageBytes(PageAddress{0, level, 0, 0}, page));
        ASSERT_TRUE(reader.ReadPageBytes(PageAddress{0, level, 1, 0}, right));
        ASSERT_TRUE(reader.ReadPageBytes(PageAddress{0, level, 0, 1}, below));
        for (uint32 b = 0; b < blocks; ++b)
        {
            EXPECT_EQ(Bc5Block(page, blocks - 1u, b), Bc5Block(right, sharedBlock, b)) << "level " << int(level) << " row " << b;
            EXPECT_EQ(Bc5Block(page, b, blocks - 1u), Bc5Block(below, b, sharedBlock))
                << "level " << int(level) << " column " << b;
        }
    }
}

TEST_F(PageStoreTest, ABc5PageHoldsTheSamplesAtItsOwnPositionsApronIncluded)
{
    // 300 x 300 normals, one value per 4 x 4 block of the field and another in each neighbor: BC5
    // stores a constant block exactly, so stored sample c of page p is field sample 128p - 4 + c
    // (edge-clamped) at every level-0 page. A window one sample off mixes two field blocks in its
    // blocks and reads every sample from its neighbor's position.
    constexpr uint32 kSide = 300u;
    std::vector<uint8> normals(static_cast<std::size_t>(kSide) * kSide * 2u);
    for (uint32 z = 0; z < kSide; ++z)
        for (uint32 x = 0; x < kSide; ++x)
        {
            const uint32 blockX = x / kPageBlockTexels;
            const uint32 blockZ = z / kPageBlockTexels;
            uint8* texel = &normals[(static_cast<std::size_t>(z) * kSide + x) * 2u];
            texel[0] = static_cast<uint8>(64u + (blockX * 7u + blockZ * 3u) % 128u);
            texel[1] = static_cast<uint8>(64u + (blockX * 5u + blockZ * 11u) % 128u);
        }
    const std::filesystem::path file = m_Dir / "blocks.gepage";
    BuildSurfaceStore(file, PageFieldFormat::Bc5, PageFieldFilter::NormalTent, kSide, kSide, normals);
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Normal), "");
    const uint32 stride = PageStrideTexels(PageFieldFormat::Bc5);
    const uint32 apron = (stride - kPageOwnedSamples) / 2u;
    const PageStoreLevel& shape = reader.Layout().Levels[0];
    for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
        for (uint32 px = 0; px < shape.PagesX; ++px)
        {
            const std::vector<uint8> page = SurfacePage(reader, PageAddress{0, 0, px, pz});
            for (uint32 r = 0; r < stride; ++r)
                for (uint32 c = 0; c < stride; ++c)
                {
                    const uint32 x = ClampSample(static_cast<int64>(px) * kPageOwnedSamples + c - apron, kSide);
                    const uint32 z = ClampSample(static_cast<int64>(pz) * kPageOwnedSamples + r - apron, kSide);
                    ASSERT_EQ(std::memcmp(&page[(static_cast<std::size_t>(r) * stride + c) * 2u],
                                          &normals[(static_cast<std::size_t>(z) * kSide + x) * 2u], 2u),
                              0)
                        << "page " << px << "," << pz << " sample " << c << "," << r;
                }
        }
}

TEST_F(PageStoreTest, TheNormalTentKeepsAThirtyDegreeSlopeThatTheNormalOfAveragedHeightsFlattens)
{
    // A ridge along Z at x = 100 with 30-degree flanks, one unit between samples. Level 0 holds the
    // central-difference normals of its heights. Coarser sample 51 covers finer samples 101 to 103,
    // all on the flank, so the tent of their normals is the 30-degree normal. The normal of the
    // averaged heights there is 26.8 degrees: coarser sample 50's height has the ridge in it.
    constexpr uint32 kWidth = 200u;
    constexpr uint32 kHeight = 130u;
    std::vector<uint8> normals(static_cast<std::size_t>(kWidth) * kHeight * 2u);
    for (uint32 z = 0; z < kHeight; ++z)
        for (uint32 x = 0; x < kWidth; ++x)
        {
            const float32 dhdx = 0.5f * (RidgeHeight(static_cast<int64>(x) + 1, kWidth) - RidgeHeight(static_cast<int64>(x) - 1, kWidth));
            uint8* texel = &normals[(static_cast<std::size_t>(z) * kWidth + x) * 2u];
            texel[0] = static_cast<uint8>(std::lround((-dhdx / std::sqrt(1.0f + dhdx * dhdx) * 0.5f + 0.5f) * 255.0f));
            texel[1] = 128u;
        }
    const std::filesystem::path file = m_Dir / "ridge.gepage";
    BuildSurfaceStore(file, PageFieldFormat::Bc5, PageFieldFilter::NormalTent, kWidth, kHeight, normals);
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Normal), "");
    ASSERT_EQ(reader.Layout().Levels.size(), 2u);

    const std::vector<uint8> level1 = SurfaceLevelGrid(reader, 1);
    const uint32 width1 = reader.Layout().Levels[1].SamplesX;
    const float32 x = static_cast<float32>(level1[(static_cast<std::size_t>(30) * width1 + 51u) * 2u]) / 255.0f * 2.0f - 1.0f;
    const float32 degrees = std::asin(std::abs(x)) * kDegreesPerRadian;
    EXPECT_NEAR(degrees, 30.0f, 0.5f);

    // The method the tent replaces: tent the heights, then difference the coarser heights 50 and 52
    // (finer samples 99 to 101 and 103 to 105), four units apart.
    float32 coarse[2] = {};
    for (uint32 n = 0; n < 2u; ++n)
    {
        const int64 center = 100 + 4 * static_cast<int64>(n);
        coarse[n] = 0.25f * RidgeHeight(center - 1, kWidth) + 0.5f * RidgeHeight(center, kWidth) +
                    0.25f * RidgeHeight(center + 1, kWidth);
    }
    const float32 averagedDegrees = std::atan(std::abs(coarse[1] - coarse[0]) / 4.0f) * kDegreesPerRadian;
    EXPECT_GT(std::abs(degrees - averagedDegrees), 2.0f) << averagedDegrees;
}

TEST_F(PageStoreTest, TheNormalTentRenormalizesTheUnitNormalsOfAFootprintThatMixesSlopes)
{
    // A 60-degree flank for x <= 100, flat past it. Coarser sample 50 covers finer samples 99 and
    // 100 on the flank (weights 1 and 2) and 101 on the flat (weight 1): the tent of the unit
    // normals, renormalized, tilts 46.1 degrees (47.8 after BC5); a tent of the stored bytes tilts
    // 40.3 (38.0 after BC5).
    constexpr uint32 kWidth = 200u;
    constexpr uint32 kHeight = 130u;
    const uint8 flankX = static_cast<uint8>(std::lround((-std::sin(60.0f / kDegreesPerRadian) * 0.5f + 0.5f) * 255.0f));
    std::vector<uint8> normals(static_cast<std::size_t>(kWidth) * kHeight * 2u, 128u);
    for (uint32 z = 0; z < kHeight; ++z)
        for (uint32 x = 0; x <= 100u; ++x)
            normals[(static_cast<std::size_t>(z) * kWidth + x) * 2u] = flankX;
    const std::filesystem::path file = m_Dir / "mixed.gepage";
    BuildSurfaceStore(file, PageFieldFormat::Bc5, PageFieldFilter::NormalTent, kWidth, kHeight, normals);
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Normal), "");
    ASSERT_EQ(reader.Layout().Levels.size(), 2u);
    const std::vector<uint8> level1 = SurfaceLevelGrid(reader, 1);
    const uint32 width1 = reader.Layout().Levels[1].SamplesX;
    const float32 x = static_cast<float32>(level1[(static_cast<std::size_t>(30) * width1 + 50u) * 2u]) / 255.0f * 2.0f - 1.0f;
    const float32 degrees = std::asin(std::abs(x)) * kDegreesPerRadian;
    EXPECT_GT(degrees, 43.0f);
    EXPECT_LT(degrees, 51.0f);
}

TEST_F(PageStoreTest, ThePremultipliedTentOfAWrittenTexelBesideAnUnwrittenOneIsTheHalfMix)
{
    // Written columns (a linear color at weight 1, stored sRGB-encoded) alternate with unwritten
    // ones (zero). Every coarser sample's footprint is half written: its color is half the linear
    // color at half the weight, which the ground reads as the half mix.
    constexpr uint32 kWidth = 200u;
    constexpr uint32 kHeight = 130u;
    const ColorLinear color(0.6f, 0.3f, 0.1f, 1.0f);
    const ColorSRGB encoded = color.ToSRGB();
    std::vector<uint8> texels(static_cast<std::size_t>(kWidth) * kHeight * 4u, 0u);
    for (uint32 z = 0; z < kHeight; ++z)
        for (uint32 x = 1; x < kWidth; x += 2u)
        {
            uint8* texel = &texels[(static_cast<std::size_t>(z) * kWidth + x) * 4u];
            texel[0] = static_cast<uint8>(std::lround(encoded.r * 255.0f));
            texel[1] = static_cast<uint8>(std::lround(encoded.g * 255.0f));
            texel[2] = static_cast<uint8>(std::lround(encoded.b * 255.0f));
            texel[3] = 255u;
        }
    const std::filesystem::path file = m_Dir / "color.gepage";
    BuildSurfaceStore(file, PageFieldFormat::Rgba8Srgb, PageFieldFilter::PremultipliedTent, kWidth, kHeight, texels);
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Color), "");
    const std::vector<uint8> level1 = SurfaceLevelGrid(reader, 1);
    const uint32 width1 = reader.Layout().Levels[1].SamplesX;
    const uint8* texel = &level1[(static_cast<std::size_t>(30) * width1 + 40u) * 4u];
    const ColorLinear mixed = ColorSRGB::FromSRGB255(texel[0], texel[1], texel[2], texel[3]).ToLinear();
    EXPECT_NEAR(mixed.a, 0.5f, 1.0f / 255.0f);
    EXPECT_NEAR(mixed.r, 0.5f * color.r, 0.005f);
    EXPECT_NEAR(mixed.g, 0.5f * color.g, 0.005f);
    EXPECT_NEAR(mixed.b, 0.5f * color.b, 0.005f);
}

TEST_F(PageStoreTest, AStoreOrContainerOfAKindFormatOrFilterThisEngineDoesNotKnowIsRefusedWithTheFix)
{
    constexpr uint32 kSide = 130u;
    const std::filesystem::path store = m_Dir / "grass.gepage";
    BuildSurfaceStore(store, PageFieldFormat::Rg8, PageFieldFilter::Tent, kSide, kSide,
                      std::vector<uint8>(static_cast<std::size_t>(kSide) * kSide * 2u, 255u));
    const std::filesystem::path container = m_Dir / "grass.geterrain";
    const TerrainContainerField field{PageFieldKind::GrassControl, store};
    ASSERT_EQ(WriteTerrainContainer(container, {&field, 1}), "");
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store, PageFieldKind::GrassControl), "");
    ASSERT_EQ(OpenTerrainContainerField(container, PageFieldKind::GrassControl, reader), "");
    EXPECT_NE(OpenTerrainContainerField(container, PageFieldKind::Splat, reader).find("has no splat field"),
              std::string::npos);

    const std::string fix = "the page store was written by a newer engine or is damaged: update the engine, or "
                            "export the game again (in the editor, opening the scene cooks the terrain again)";
    // The store's format byte follows its magic and version, and its filter byte follows the format;
    // the container's first kind follows its magic, version and field count.
    const std::string format = reader.Open(PatchedCopy(store, 8u, 9u, 1u), PageFieldKind::GrassControl);
    const std::string filter = reader.Open(PatchedCopy(store, 9u, 9u, 1u), PageFieldKind::GrassControl);
    const std::string kind =
        OpenTerrainContainerField(PatchedCopy(container, 12u, 99u, 4u), PageFieldKind::GrassControl, reader);
    EXPECT_NE(format.find(fix), std::string::npos) << format;
    EXPECT_NE(filter.find(fix), std::string::npos) << filter;
    EXPECT_NE(kind.find(fix), std::string::npos) << kind;
}

TEST_F(PageStoreTest, AContainerFieldWhoseStoreIsOfAnotherKindIsRefused)
{
    // A whole height store under the splat kind in the field directory.
    const std::filesystem::path source = m_Dir / "c.r32";
    WriteR32(source, 260, 200, Relief);
    const std::filesystem::path store = m_Dir / "c.gepage";
    ASSERT_EQ(Cook(source, 260, 200, store).Error, "");
    const std::filesystem::path container = m_Dir / "relabeled.geterrain";
    const TerrainContainerField field{PageFieldKind::Splat, store};
    ASSERT_EQ(WriteTerrainContainer(container, {&field, 1}), "");
    PageStoreReader reader;
    const std::string refusal = OpenTerrainContainerField(container, PageFieldKind::Splat, reader);
    EXPECT_NE(refusal.find("the page store is not a splat store"), std::string::npos) << refusal;
    EXPECT_FALSE(reader.IsOpen());
}

namespace
{

// A store of `Kind`, written in its own format and filter, whose format and filter bytes are then
// changed to `Format` and `Filter`.
struct PageStoreRelabel
{
    PageFieldKind Kind;
    PageFieldFormat WrittenFormat;
    PageFieldFilter WrittenFilter;
    PageFieldFormat Format;
    PageFieldFilter Filter;
};

class PageStoreRelabelTest : public PageStoreTest, public ::testing::WithParamInterface<PageStoreRelabel>
{
};

} // namespace

TEST_P(PageStoreRelabelTest, AStoreWhoseFormatOrFilterByteChangedIsRefused)
{
    // Every relabel leaves the store unfit for its kind: refused as a bare store of that kind and in
    // a container.
    const PageStoreRelabel relabel = GetParam();
    constexpr uint32 kSide = 260u;
    const std::vector<uint8> weights = SplatWeights(kSide, kSide);
    std::vector<uint8> level0(static_cast<std::size_t>(kSide) * kSide * PageTexelBytes(relabel.WrittenFormat));
    for (std::size_t i = 0; i < level0.size(); ++i)
        level0[i] = weights[i % weights.size()];
    const std::filesystem::path store = m_Dir / "written.gepage";
    BuildSurfaceStore(store, relabel.WrittenFormat, relabel.WrittenFilter, kSide, kSide, level0);
    // The format byte follows the store's magic and version; the filter byte follows the format.
    const uint32 bytes = static_cast<uint32>(relabel.Format) | static_cast<uint32>(relabel.Filter) << 8u;
    const std::filesystem::path relabeled = PatchedCopy(store, 8u, bytes, 2u);

    PageStoreReader reader;
    EXPECT_NE(reader.Open(relabeled, relabel.Kind), "");
    EXPECT_FALSE(reader.IsOpen());
    const std::filesystem::path container = m_Dir / "relabeled.geterrain";
    const TerrainContainerField field{relabel.Kind, relabeled};
    ASSERT_EQ(WriteTerrainContainer(container, {&field, 1}), "");
    EXPECT_NE(OpenTerrainContainerField(container, relabel.Kind, reader), "");
    EXPECT_FALSE(reader.IsOpen());
}

INSTANTIATE_TEST_SUITE_P(
    RelabeledSurfaceStores, PageStoreRelabelTest,
    ::testing::Values(
        PageStoreRelabel{PageFieldKind::Splat, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, PageFieldFormat::Rgba8,
                         PageFieldFilter::Tent},
        PageStoreRelabel{PageFieldKind::Splat, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent,
                         PageFieldFormat::Rgba8Srgb, PageFieldFilter::PremultipliedTent},
        PageStoreRelabel{PageFieldKind::Splat, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, PageFieldFormat::Rg8,
                         PageFieldFilter::Tent},
        PageStoreRelabel{PageFieldKind::Splat, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, PageFieldFormat::Bc5,
                         PageFieldFilter::NormalTent},
        PageStoreRelabel{PageFieldKind::Color, PageFieldFormat::Rgba8Srgb, PageFieldFilter::PremultipliedTent,
                         PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent},
        PageStoreRelabel{PageFieldKind::Color, PageFieldFormat::Rgba8Srgb, PageFieldFilter::PremultipliedTent,
                         PageFieldFormat::Rgba8, PageFieldFilter::Tent},
        PageStoreRelabel{PageFieldKind::Color, PageFieldFormat::Rgba8Srgb, PageFieldFilter::PremultipliedTent,
                         PageFieldFormat::Rg8, PageFieldFilter::Tent},
        PageStoreRelabel{PageFieldKind::Color, PageFieldFormat::Rgba8Srgb, PageFieldFilter::PremultipliedTent,
                         PageFieldFormat::Bc5, PageFieldFilter::NormalTent},
        PageStoreRelabel{PageFieldKind::GrassControl, PageFieldFormat::Rg8, PageFieldFilter::Tent, PageFieldFormat::Bc5,
                         PageFieldFilter::NormalTent}));

TEST_F(PageStoreTest, NineteenPointNineKilometersOfReliefStoreSixteenBitPagesWithinHalfAStep)
{
    // Earth's relief over one field: a ramp of 19.9 km. One store-wide 16-bit step would be 30 cm,
    // past the 1 cm the encoding allows; a step per level with a base per page keeps the pages 16-bit
    // (each level-0 page spans about 3.1 km, so level 0's step is at most 2^-4 m). A level under 100
    // pages may let its steepest page store 32-bit integers, one page at most.
    constexpr uint32 kWidth = 1024;
    constexpr uint32 kHeight = 640;
    constexpr float32 kRelief = 19900.0f;
    const auto ramp = [](uint32 x, uint32 z) {
        return kRelief * static_cast<float32>(x + z) / static_cast<float32>(kWidth + kHeight - 2u);
    };
    const std::filesystem::path source = m_Dir / "earth.r32";
    WriteR32(source, kWidth, kHeight, ramp);
    const std::filesystem::path store = m_Dir / "earth.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, store).Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store, PageFieldKind::Height), "");
    const PageStoreLayout& layout = reader.Layout();
    uint64 wideTotal = 0;
    for (uint8 level = 0; level < layout.Levels.size(); ++level)
    {
        const PageStoreRange& range = layout.Ranges[level];
        uint64 wide = 0;
        for (uint64 i = 0; i < range.Count; ++i)
            wide += layout.Entries[range.First + i].HasWideWords() ? 1u : 0u;
        EXPECT_LE(wide, 1u) << int(level);
        wideTotal += wide;
    }
    const uint64 pageBytes = std::filesystem::file_size(store) - layout.DataOffset();
    EXPECT_EQ(pageBytes, (layout.Entries.size() + wideTotal) * kPageSampleCount * sizeof(uint16));
    const std::vector<float32> grid = LevelGrid(reader, 0);
    for (uint32 z = 0; z < kHeight; ++z)
        for (uint32 x = 0; x < kWidth; ++x)
            ASSERT_NEAR(grid[static_cast<std::size_t>(z) * kWidth + x], ramp(x, z), 0.5f / 16.0f) << x << "," << z;
}

TEST_F(PageStoreTest, SixteenBitSourcesCookLosslesslyAsNormalizedHeights)
{
    constexpr uint32 kWidth = 200;
    constexpr uint32 kHeight = 150;
    std::vector<uint16> words(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::size_t i = 0; i < words.size(); ++i)
        words[i] = static_cast<uint16>((i * 2654435761u) >> 16u);
    const std::filesystem::path source = m_Dir / "words.r16";
    std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char*>(words.data()),
                                                  static_cast<std::streamsize>(words.size() * 2u));
    const std::filesystem::path store = m_Dir / "words.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, store).Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store, PageFieldKind::Height), "");
    EXPECT_EQ(reader.Layout().Header.Units, PageHeightUnits::Normalized);
    const std::vector<float32> grid = LevelGrid(reader, 0);
    Terrain::HeightfieldData today;
    ASSERT_TRUE(today.LoadFromRawUInt16(source, kWidth, kHeight));
    for (std::size_t i = 0; i < grid.size(); ++i)
        ASSERT_EQ(std::bit_cast<uint32>(grid[i]), std::bit_cast<uint32>(today.GetRawSamples()[i])) << i;
}

TEST_F(PageStoreTest, CookRunsTheSameOnThePoolAsOnOneThread)
{
    constexpr uint32 kWidth = 700;
    constexpr uint32 kHeight = 300;
    const std::filesystem::path source = m_Dir / "pool.r32";
    WriteR32(source, kWidth, kHeight, Relief);
    JobSystem::WorkStealingThreadPool pool(4);
    AssetIOService io;
    io.Start(pool, 1);
    // The pooled cook reads its scan chunks and bands through the reader threads.
    const std::filesystem::path pooled = m_Dir / "pooled.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, pooled, {}, &pool, &io).Error, "");
    io.Stop();
    pool.Shutdown();
    const std::filesystem::path serial = m_Dir / "serial.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, serial).Error, "");
    EXPECT_EQ(FileBytes(pooled), FileBytes(serial));
}

// ---- Sources and keys ------------------------------------------------------------------------------

TEST_F(PageStoreTest, SourcesTheCookerCannotReadAreRefusedWithTheFix)
{
    HeightCookSource source;
    EXPECT_NE(ResolveHeightCookSource(m_Dir / "dem.tif", 0, 0, source).find(".r32"), std::string::npos);
    EXPECT_NE(ResolveHeightCookSource(m_Dir / "dem.tiff", 0, 0, source).find("GeoTIFF"), std::string::npos);
    EXPECT_NE(ResolveHeightCookSource(m_Dir / "dem.exr", 0, 0, source).find("not a heightmap format"),
              std::string::npos);

    const std::filesystem::path raw = m_Dir / "grid.r32";
    WriteR32(raw, 100, 50, Relief);
    EXPECT_NE(ResolveHeightCookSource(raw, 100, 49, source).find("do not account"), std::string::npos);
    EXPECT_EQ(ResolveHeightCookSource(raw, 100, 50, source), "");
    EXPECT_EQ(source.Format, HeightSourceFormat::R32);

    // Past the store's axis bound (every page count of a face must fit 32 bits).
    const std::filesystem::path wide = m_Dir / "wide.r16";
    std::ofstream(wide, std::ios::binary).close();
    std::filesystem::resize_file(wide, (static_cast<uintmax_t>(kPageStoreMaxSamplesPerAxis) + 1u) * 2u * 2u);
    EXPECT_NE(ResolveHeightCookSource(wide, kPageStoreMaxSamplesPerAxis + 1u, 2, source).find("split it"),
              std::string::npos);

    const std::filesystem::path nodata = m_Dir / "nodata.r32";
    WriteR32(nodata, 100, 50, [](uint32 x, uint32 z) {
        return x == 40 && z == 20 ? std::numeric_limits<float32>::quiet_NaN() : 1.0f;
    });
    ASSERT_EQ(ResolveHeightCookSource(nodata, 100, 50, source), "");
    HeightSourceIdentity identity;
    EXPECT_NE(ScanHeightSource(source, nullptr, identity).find("nodata"), std::string::npos);
}

namespace
{

// A PNG's signature and header chunk only: what the decoder's size check reads.
void WritePngHeader(const std::filesystem::path& file, uint32 width, uint32 height, uint8 colorType, uint8 bitDepth)
{
    const auto bigEndian = [](uint32 value) {
        return std::array<char, 4>{char(value >> 24), char(value >> 16), char(value >> 8), char(value)};
    };
    std::ofstream out(file, std::ios::binary);
    out.write("\x89PNG\r\n\x1a\n", 8);
    out.write(bigEndian(13).data(), 4);
    out.write("IHDR", 4);
    out.write(bigEndian(width).data(), 4);
    out.write(bigEndian(height).data(), 4);
    const char rest[5] = {static_cast<char>(bitDepth), static_cast<char>(colorType), 0, 0, 0};
    out.write(rest, 5);
    out.write(bigEndian(0).data(), 4); // CRC, unchecked by the size check
    out.write(bigEndian(0).data(), 4); // an empty IDAT: where the header scan stops
    out.write("IDAT", 4);
    out.write(bigEndian(0).data(), 4);
}

} // namespace

TEST_F(PageStoreTest, PngHeightmapsPastTheDecodersLimitAreRefusedWithTheFix)
{
    const std::filesystem::path fits = m_Dir / "fits.png";
    WritePngHeader(fits, 32768, 16384, 0, 16);
    Terrain::PngHeightmapSize size;
    EXPECT_EQ(Terrain::ResolvePngHeightmapSize(fits, size), "");
    EXPECT_EQ(size.Width, 32768u);

    const std::filesystem::path tooLarge = m_Dir / "large.png";
    WritePngHeader(tooLarge, 40000, 40000, 0, 16);
    const std::string reason = Terrain::ResolvePngHeightmapSize(tooLarge, size);
    EXPECT_NE(reason.find("more than a PNG heightmap can hold"), std::string::npos);
    EXPECT_NE(reason.find(".r32"), std::string::npos);

    // Three 16-bit channels take three times the bytes: a grid a grayscale PNG holds, RGB does not.
    const std::filesystem::path rgb = m_Dir / "rgb.png";
    WritePngHeader(rgb, 32768, 16384, 2, 16);
    const std::string rgbReason = Terrain::ResolvePngHeightmapSize(rgb, size);
    EXPECT_NE(rgbReason.find(".r32"), std::string::npos) << rgbReason;

    // A palette image is checked at four channels: 20000 x 15000 passes as three, fails as four.
    const std::filesystem::path palette = m_Dir / "palette.png";
    WritePngHeader(palette, 20000, 15000, 3, 8);
    const std::string paletteReason = Terrain::ResolvePngHeightmapSize(palette, size);
    EXPECT_NE(paletteReason.find(".r32"), std::string::npos) << paletteReason;

    // The decoder reads an 8-bit grayscale PNG of 2^30 samples, but the heightmap decodes it
    // to 16 bits, 2^31 bytes: refused with the same fix.
    const std::filesystem::path gray8 = m_Dir / "gray8.png";
    WritePngHeader(gray8, 32768, 32768, 0, 8);
    const std::string gray8Reason = Terrain::ResolvePngHeightmapSize(gray8, size);
    EXPECT_NE(gray8Reason.find("more than a PNG heightmap can hold"), std::string::npos) << gray8Reason;
    EXPECT_NE(gray8Reason.find(".r32"), std::string::npos) << gray8Reason;

    HeightCookSource source;
    EXPECT_EQ(ResolveHeightCookSource(tooLarge, 0, 0, source), reason);
}

TEST_F(PageStoreTest, CookKeyFollowsTheContentAndImportGridOnly)
{
    const HeightStoreCookInputs base{0x1111222233334444ull, HeightSourceFormat::R32, 8193, 2049};
    const uint64 key = ComputeHeightStoreKey(base);
    EXPECT_EQ(ComputeHeightStoreKey(base), key);
    HeightStoreCookInputs changed = base;
    changed.SourceContentHash ^= 1u;
    EXPECT_NE(ComputeHeightStoreKey(changed), key);
    changed = base;
    changed.Format = HeightSourceFormat::R16;
    EXPECT_NE(ComputeHeightStoreKey(changed), key);
    changed = base;
    changed.SamplesX = 2049;
    changed.SamplesZ = 8193;
    EXPECT_NE(ComputeHeightStoreKey(changed), key);
    // The inputs are the asset's content and import grid and nothing else: an entity, a scene, a
    // streaming budget or a material cannot reach the key. A field added here must change the bytes.
    static_assert(sizeof(HeightStoreCookInputs) == 24, "every cook input must decide the store's bytes");

    const GUID asset = GUID::Generate();
    EXPECT_EQ(HeightStoreFile(m_Dir, asset, 0xABCull).filename().string(), asset.ToString() + "-0000000000000abc.gepage");
}

TEST_F(PageStoreTest, TwoTerrainsOnOneHeightmapResolveOneStoreAndATouchKeepsTheKey)
{
    constexpr uint32 kWidth = 300;
    constexpr uint32 kHeight = 300;
    const std::filesystem::path source = m_Dir / "dem.r32";
    WriteR32(source, kWidth, kHeight, Relief);
    const GUID asset = GUID::Generate();
    const std::filesystem::path cache = m_Dir / "TerrainPages";

    // What a terrain's lookup does: the remembered identity when the file is as it was, else a
    // full read; then the key from the identity and the import grid.
    const auto storeFor = [&](uint32 samplesX, uint32 samplesZ) {
        HeightCookSource resolved;
        EXPECT_EQ(ResolveHeightCookSource(source, samplesX, samplesZ, resolved), "");
        std::optional<HeightSourceIdentity> identity = ReadHeightSourceMemo(cache, asset, source);
        if (!identity)
        {
            identity.emplace();
            EXPECT_EQ(ScanHeightSource(resolved, nullptr, *identity), "");
            EXPECT_TRUE(WriteHeightSourceMemo(cache, asset, source, *identity));
        }
        return HeightStoreFile(cache, asset,
                               ComputeHeightStoreKey({identity->ContentHash, resolved.Format, samplesX, samplesZ}));
    };

    const std::filesystem::path first = storeFor(kWidth, kHeight); // terrain 1, scene 1
    EXPECT_TRUE(ReadHeightSourceMemo(cache, asset, source).has_value());
    const std::filesystem::path second = storeFor(kWidth, kHeight); // terrain 2, scene 2
    EXPECT_EQ(first, second);

    // A touch changes the modification time, not the bytes: the memo misses, the full read finds
    // the same content, and the store keeps its key.
    std::filesystem::last_write_time(source, std::filesystem::last_write_time(source) + std::chrono::seconds(5));
    EXPECT_FALSE(ReadHeightSourceMemo(cache, asset, source).has_value());
    EXPECT_EQ(storeFor(kWidth, kHeight), first);

    // An edit in the middle of the file, past what the asset registry's sparse fingerprint reads,
    // re-keys the store.
    std::vector<float32> samples = ReadR32(source, kWidth, kHeight);
    samples[samples.size() / 2u + 777u] += 0.25f;
    std::ofstream(source, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size() * 4u));
    EXPECT_NE(storeFor(kWidth, kHeight), first);
}

// ---- Generated provider ----------------------------------------------------------------------------

namespace
{

GeneratedHeightLattice NoiseLattice(float32 originX, float32 originZ, float32 tileWorld, uint32 tilesX, uint32 tilesZ)
{
    GeneratedHeightLattice lattice;
    lattice.Source = GeneratedHeightSource::Noise;
    lattice.TerrainOriginX = originX;
    lattice.TerrainOriginZ = originZ;
    lattice.TileWorldSize = tileWorld;
    lattice.TileSamples = Terrain::kMaxTileResolution;
    lattice.TilesX = tilesX;
    lattice.TilesZ = tilesZ;
    lattice.Noise = {TerrainECS::kTileNoiseFrequency, TerrainECS::kTileNoiseAmplitude, TerrainECS::kTileNoiseOctaves,
                     TerrainECS::kTileNoiseSeed};
    return lattice;
}

// Every level-0 sample of the provider against the tile fill of the tile that owns it.
void ExpectLevelZeroIsTheTileFill(const GeneratedHeightLattice& lattice)
{
    const GeneratedHeightPageProvider provider(lattice);
    ASSERT_FALSE(provider.Levels().empty());
    const PageStoreLevel& level0 = provider.Levels().front();
    const uint32 interval = lattice.TileSamples - 1u;
    ASSERT_EQ(level0.SamplesX, lattice.TilesX * interval + 1u);

    std::vector<std::vector<float32>> pages(static_cast<std::size_t>(level0.PagesX) * level0.PagesZ);
    for (uint32 pz = 0; pz < level0.PagesZ; ++pz)
        for (uint32 px = 0; px < level0.PagesX; ++px)
        {
            auto& page = pages[static_cast<std::size_t>(pz) * level0.PagesX + px];
            page.resize(kPageSampleCount);
            ASSERT_TRUE(provider.FillPage(PageAddress{0, 0, px, pz}, page));
        }

    TerrainECS::TiledTerrainConfig config;
    config.Base.Source = Components::TerrainBaseSource::ProceduralNoise;
    config.TileWorldSize = lattice.TileWorldSize;
    uint64 compared = 0;
    for (uint32 tz = 0; tz < lattice.TilesZ; ++tz)
    {
        for (uint32 tx = 0; tx < lattice.TilesX; ++tx)
        {
            // The tile job's own origin expression (TileStreamingManager::DispatchFullJob).
            const TerrainECS::TileCoord coord{static_cast<int32>(tx), static_cast<int32>(tz)};
            const float32 tileOriginX = lattice.TerrainOriginX + coord.X * lattice.TileWorldSize;
            const float32 tileOriginZ = lattice.TerrainOriginZ + coord.Z * lattice.TileWorldSize;
            Terrain::HeightfieldData tile(lattice.TileSamples, lattice.TileSamples, 0.0f);
            TerrainECS::FillTiledBaseRegion(tile, config, lattice.TerrainOriginX, lattice.TerrainOriginZ, tileOriginX,
                                            tileOriginZ, lattice.TileWorldSize, lattice.TileWorldSize, 0, 0,
                                            static_cast<int32>(interval), static_cast<int32>(interval));
            // A tile owns its samples but the last row and column, unless it is the grid's last.
            const uint32 lastX = tx + 1u == lattice.TilesX ? interval : interval - 1u;
            const uint32 lastZ = tz + 1u == lattice.TilesZ ? interval : interval - 1u;
            for (uint32 z = 0; z <= lastZ; ++z)
            {
                for (uint32 x = 0; x <= lastX; ++x)
                {
                    const uint32 gx = tx * interval + x;
                    const uint32 gz = tz * interval + z;
                    const auto& page = pages[static_cast<std::size_t>(gz / kPageOwnedSamples) * level0.PagesX +
                                             gx / kPageOwnedSamples];
                    const float32 paged = page[static_cast<std::size_t>(gz % kPageOwnedSamples + 1u) * kPageStrideSamples +
                                               gx % kPageOwnedSamples + 1u];
                    ASSERT_EQ(std::bit_cast<uint32>(paged), std::bit_cast<uint32>(tile.GetSample(x, z)))
                        << "tile " << tx << "," << tz << " sample " << x << "," << z;
                    ++compared;
                }
            }
        }
    }
    EXPECT_EQ(compared, static_cast<uint64>(level0.SamplesX) * level0.SamplesZ);
}

} // namespace

TEST(GeneratedHeightPageProviderTest, LevelZeroIsByteIdenticalToTheTileNoiseFill)
{
    // One sample per meter (1 km tiles) at the origin, and four per meter (256 m tiles) off it.
    ExpectLevelZeroIsTheTileFill(NoiseLattice(0.0f, 0.0f, 1024.0f, 3, 2));
    ExpectLevelZeroIsTheTileFill(NoiseLattice(-1536.5f, 733.25f, 256.0f, 2, 3));
    // Three samples per meter: a tile size and spacing with no short binary form, so a sample's
    // position from one global spacing rounds differently from the tile's corner plus its offset.
    ExpectLevelZeroIsTheTileFill(NoiseLattice(1000.1f, -77.7f, 1024.0f / 3.0f, 3, 1));
}

TEST(GeneratedHeightPageProviderTest, CoarserLevelsSitOnTheLevelZeroLatticeAndKeepItsMean)
{
    // One broad octave: nothing to band-limit, so level N sample j is level-0 sample j * 2^N.
    GeneratedHeightLattice broad = NoiseLattice(-100.0f, 50.0f, 1024.0f, 2, 1);
    broad.Noise.Octaves = 1;
    const GeneratedHeightPageProvider provider(broad);
    std::vector<float32> fine(kPageSampleCount);
    std::vector<float32> coarse(kPageSampleCount);
    ASSERT_TRUE(provider.FillPage(PageAddress{0, 0, 0, 0}, fine));
    ASSERT_TRUE(provider.FillPage(PageAddress{0, 1, 0, 0}, coarse));
    for (uint32 r = 1; r < 64u; ++r)
        for (uint32 c = 1; c < 64u; ++c)
            ASSERT_EQ(coarse[static_cast<std::size_t>(r) * kPageStrideSamples + c],
                      fine[static_cast<std::size_t>(2u * r - 1u) * kPageStrideSamples + 2u * c - 1u]);

    // The default noise: a coarse level drops the octaves it cannot carry for their mean, so its
    // mean height stays the field's.
    const GeneratedHeightPageProvider full(NoiseLattice(0.0f, 0.0f, 1024.0f, 3, 3));
    const auto levelMean = [&](uint8 level) {
        double sum = 0.0;
        uint64 count = 0;
        std::vector<float32> page(kPageSampleCount);
        const PageStoreLevel& shape = full.Levels()[level];
        for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
            for (uint32 px = 0; px < shape.PagesX; ++px)
            {
                EXPECT_TRUE(full.FillPage(PageAddress{0, level, px, pz}, page));
                for (uint32 r = 0; r < kPageOwnedSamples && pz * kPageOwnedSamples + r < shape.SamplesZ; ++r)
                    for (uint32 c = 0; c < kPageOwnedSamples && px * kPageOwnedSamples + c < shape.SamplesX; ++c)
                    {
                        sum += page[static_cast<std::size_t>(r + 1u) * kPageStrideSamples + c + 1u];
                        ++count;
                    }
            }
        return sum / static_cast<double>(count);
    };
    EXPECT_NEAR(levelMean(5), levelMean(0), 0.01);
}

TEST(GeneratedHeightPageProviderTest, FlatBaseIsZeroAndOutOfRangeAddressesFail)
{
    GeneratedHeightLattice flat = NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1);
    flat.Source = GeneratedHeightSource::Flat;
    const GeneratedHeightPageProvider provider(flat);
    std::vector<float32> page(kPageSampleCount, 1.0f);
    ASSERT_TRUE(provider.FillPage(PageAddress{0, 2, 1, 1}, page));
    EXPECT_TRUE(std::all_of(page.begin(), page.end(), [](float32 v) { return v == 0.0f; }));
    EXPECT_FALSE(provider.FillPage(PageAddress{0, 0, 9, 0}, page));
    EXPECT_FALSE(provider.FillPage(PageAddress{1, 0, 0, 0}, page));
    EXPECT_FALSE(provider.FillPage(PageAddress{0, static_cast<uint8>(provider.Levels().size()), 0, 0}, page));
}

// ---- Container and loader --------------------------------------------------------------------------

TEST_F(PageStoreTest, ATerrainContainerRefusesANormalSamplesCache)
{
    // The unencoded samples beside a Normal store are a bake cache: an export packs the Bc5 store.
    constexpr uint32 kSide = 300u;
    const std::vector<uint8> normals(static_cast<std::size_t>(kSide) * kSide * 2u, 128u);
    const std::filesystem::path samples = m_Dir / "normal.samples.gepage";
    BuildSurfaceStore(samples, PageFieldFormat::Rg8, PageFieldFilter::NormalTent, kSide, kSide, normals);
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(samples, PageFieldKind::NormalSamples), "");
    const TerrainContainerField field{PageFieldKind::NormalSamples, samples};
    const std::filesystem::path container = m_Dir / "cache.geterrain";
    EXPECT_NE(WriteTerrainContainer(container, {&field, 1}).find("never ships"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(container));
}

TEST_F(PageStoreTest, TerrainContainerReadsItsHeightFieldAtItsOffsetUnchanged)
{
    const std::filesystem::path source = m_Dir / "c.r32";
    WriteR32(source, 260, 200, Relief);
    const std::filesystem::path store = m_Dir / "c.gepage";
    ASSERT_EQ(Cook(source, 260, 200, store).Error, "");
    const std::filesystem::path container = m_Dir / "Cooked" / "Terrain" / "c.geterrain";
    const TerrainContainerField field{PageFieldKind::Height, store};
    ASSERT_EQ(WriteTerrainContainer(container, {&field, 1}), "");

    PageStoreReader loose;
    ASSERT_EQ(loose.Open(store, PageFieldKind::Height), "");
    PageStoreReader packed;
    ASSERT_EQ(OpenTerrainContainerField(container, PageFieldKind::Height, packed), "");
    EXPECT_GT(packed.BaseOffset(), 0u);
    EXPECT_EQ(packed.Layout().Header.Key, loose.Layout().Header.Key);
    for (uint8 level = 0; level < loose.Layout().Levels.size(); ++level)
        for (uint32 pz = 0; pz < loose.Layout().Levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < loose.Layout().Levels[level].PagesX; ++px)
                EXPECT_EQ(PageSamples(packed, PageAddress{0, level, px, pz}), PageSamples(loose, PageAddress{0, level, px, pz}));

    const TerrainContainerField twice[2] = {field, field};
    EXPECT_NE(WriteTerrainContainer(m_Dir / "twice.geterrain", twice), "");
    EXPECT_NE(OpenTerrainContainerField(store, PageFieldKind::Height, packed).find("not a terrain container"),
              std::string::npos);

    // A container of format 1 is refused with the fix a game's reader has: export the game again.
    const std::filesystem::path older = m_Dir / "older.geterrain";
    {
        std::vector<uint8> bytes = FileBytes(container);
        const uint32 version = 1u;
        std::memcpy(bytes.data() + 4u, &version, sizeof(version));
        std::ofstream(older, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const std::string refusal = OpenTerrainContainerField(older, PageFieldKind::Height, packed);
    EXPECT_NE(refusal.find("is format 1 and this engine reads format 2: export the game again"), std::string::npos)
        << refusal;
}

TEST_F(PageStoreTest, PagesLoadThroughTheAssetReaderThreadsAndDecodeOffThem)
{
    const std::filesystem::path source = m_Dir / "load.r32";
    WriteR32(source, 400, 300, Relief);
    const std::filesystem::path store = m_Dir / "load.gepage";
    ASSERT_EQ(Cook(source, 400, 300, store).Error, "");
    auto reader = std::make_shared<PageStoreReader>();
    ASSERT_EQ(reader->Open(store, PageFieldKind::Height), "");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetIOService io;
    io.Start(pool, 1);

    const auto load = [&](const PageAddress& address, std::shared_ptr<std::atomic<bool>> cancel) {
        auto loaded = std::make_shared<std::promise<std::vector<uint8>>>();
        auto failed = std::make_shared<std::promise<std::string>>();
        PageLoadRequest request;
        request.Store = reader;
        request.StoreAsset = GUID::Generate();
        request.Address = address;
        request.Priority = AssetLoadPriority::High;
        request.Cancel = std::move(cancel);
        request.OnLoaded = [loaded](const PageAddress&, std::vector<uint8> bytes) { loaded->set_value(std::move(bytes)); };
        request.OnFailed = [failed](const PageAddress&, const std::string& error) { failed->set_value(error); };
        SubmitPageLoad(io, std::move(request));
        return std::make_pair(loaded->get_future(), failed->get_future());
    };

    for (const PageAddress address : {PageAddress{0, 0, 0, 0}, PageAddress{0, 0, 3, 2}, PageAddress{0, 1, 1, 1}})
    {
        auto [samples, failure] = load(address, std::make_shared<std::atomic<bool>>(false));
        ASSERT_EQ(samples.wait_for(kLoadTimeout), std::future_status::ready);
        EXPECT_EQ(Heights(samples.get()), PageSamples(*reader, address));
    }

    auto [absentSamples, absent] = load(PageAddress{0, 0, 9, 9}, nullptr);
    ASSERT_EQ(absent.wait_for(std::chrono::seconds(0)), std::future_status::ready); // inline
    EXPECT_NE(absent.get().find("not in the store"), std::string::npos);

    auto [cancelledSamples, cancelled] = load(PageAddress{0, 0, 1, 1}, std::make_shared<std::atomic<bool>>(true));
    ASSERT_EQ(cancelled.wait_for(kLoadTimeout), std::future_status::ready);
    // OnLoaded never ran: its promise was dropped with the request, unfulfilled.
    EXPECT_THROW(cancelledSamples.get(), std::future_error);

    io.Stop();
    pool.Shutdown();
}

TEST_F(PageStoreTest, APageLoadWhoseDecodeNeverRunsStillResolvesOnce)
{
    const std::filesystem::path source = m_Dir / "shutdown.r32";
    WriteR32(source, 200, 200, Relief);
    const std::filesystem::path store = m_Dir / "shutdown.gepage";
    ASSERT_EQ(Cook(source, 200, 200, store).Error, "");
    auto reader = std::make_shared<PageStoreReader>();
    ASSERT_EQ(reader->Open(store, PageFieldKind::Height), "");

    // One worker, held by a job until the pool shuts down: the page's read completes and its
    // decode is queued behind that job, so the shutdown cancels the decode without running it.
    JobSystem::WorkStealingThreadPool pool(1);
    std::atomic<bool> release{false};
    pool.Submit([&release]() {
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    AssetIOService io;
    io.Start(pool, 1);

    std::atomic<int> loaded{0};
    std::atomic<int> failed{0};
    PageLoadRequest request;
    request.Store = reader;
    request.StoreAsset = GUID::Generate();
    request.Address = PageAddress{0, 0, 0, 0};
    request.OnLoaded = [&loaded](const PageAddress&, std::vector<uint8>) { ++loaded; };
    request.OnFailed = [&failed](const PageAddress&, const std::string&) { ++failed; };
    SubmitPageLoad(io, std::move(request));

    const auto deadline = std::chrono::steady_clock::now() + kLoadTimeout;
    while (io.GetQueuedReadCount() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    io.Stop(); // joins the reader, which has read the page and queued its decode
    std::thread releaser([&release]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        release.store(true);
    });
    pool.Shutdown();
    releaser.join();

    EXPECT_EQ(loaded.load(), 0);
    EXPECT_EQ(failed.load(), 1) << "exactly one of OnLoaded and OnFailed runs";
}

// A page of any field loads through the one loader at the bytes its format holds: a splat page its
// decoded RGBA8 samples (130^2 x 4 = 67,600 B), a BC5 normal page its blocks as stored (34^2 blocks
// x 16 = 18,496 B), which the GPU samples as they are. Through the reader threads and inline alike.
TEST_F(PageStoreTest, APageOfEveryFormatLoadsAtTheBytesItsFormatHolds)
{
    constexpr uint32 kSide = 300u;
    const std::filesystem::path splatFile = m_Dir / "splat.gepage";
    BuildSurfaceStore(splatFile, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kSide, kSide,
                      SplatWeights(kSide, kSide));
    std::vector<uint8> normals(static_cast<std::size_t>(kSide) * kSide * 2u);
    for (std::size_t i = 0; i < normals.size(); ++i)
        normals[i] = static_cast<uint8>(64u + ((static_cast<uint32>(i) * 2654435761u) >> 25));
    const std::filesystem::path normalFile = m_Dir / "normal.gepage";
    BuildSurfaceStore(normalFile, PageFieldFormat::Bc5, PageFieldFilter::NormalTent, kSide, kSide, normals);
    auto splat = std::make_shared<PageStoreReader>();
    ASSERT_EQ(splat->Open(splatFile, PageFieldKind::Splat), "");
    auto normal = std::make_shared<PageStoreReader>();
    ASSERT_EQ(normal->Open(normalFile, PageFieldKind::Normal), "");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetIOService io;
    io.Start(pool, 1);
    const PageAddress address{0, 0, 1, 1};
    std::vector<uint8> stored;
    ASSERT_TRUE(normal->ReadPageBytes(address, stored));
    struct Case
    {
        std::shared_ptr<PageStoreReader> Reader;
        uint32 Bytes = 0;
        std::vector<uint8> Payload; // what the page holds once loaded
    };
    const Case cases[] = {{splat, 67600u, SurfacePage(*splat, address)}, {normal, 18496u, stored}};
    for (const Case& page : cases)
    {
        const PageSource source{page.Reader, nullptr};
        EXPECT_EQ(source.PayloadBytes(), page.Bytes);
        auto done = std::make_shared<std::promise<PageFetchOutcome>>();
        SubmitPageFetch(&pool, &io, source, address, nullptr,
                        [done](PageFetchOutcome outcome) { done->set_value(std::move(outcome)); });
        std::future<PageFetchOutcome> loaded = done->get_future();
        ASSERT_EQ(loaded.wait_for(kLoadTimeout), std::future_status::ready);
        const PageFetchOutcome outcome = loaded.get();
        ASSERT_NE(outcome.Bytes, nullptr) << outcome.Error;
        EXPECT_EQ(outcome.Bytes->size(), page.Bytes);
        EXPECT_EQ(*outcome.Bytes, page.Payload);
        const PageFetchOutcome fetched = FetchPage(source, address);
        ASSERT_NE(fetched.Bytes, nullptr) << fetched.Error;
        EXPECT_EQ(*fetched.Bytes, page.Payload);
    }
    io.Stop();
    pool.Shutdown();
}

TEST_F(PageStoreTest, TheCacheFolderKeepsOneCurrentStorePerHeightmapAndSweepsDeadCooks)
{
    const std::filesystem::path source = m_Dir / "dem.r32";
    WriteR32(source, 300, 260, Relief);
    const std::filesystem::path folder = m_Dir / "TerrainPages";
    const HeightStoreCache cache(folder);
    const GUID asset = GUID::Generate();

    const HeightStoreCache::Result first = cache.Ensure(asset, source, 300, 260, nullptr, nullptr);
    ASSERT_EQ(first.Error, "");
    EXPECT_TRUE(first.Cooked);
    const HeightStoreCache::Result again = cache.Ensure(asset, source, 300, 260, nullptr, nullptr);
    EXPECT_EQ(again.Store, first.Store);
    EXPECT_FALSE(again.Cooked) << "a second terrain on the same heightmap re-cooked it";

    // A cook that died left its temporary store; an edit re-keys the heightmap.
    const std::filesystem::path orphan = first.Store.string() + ".2147483600-1-0.tmp";
    std::ofstream(orphan, std::ios::binary) << "partial";
    WriteR32(source, 300, 260, [](uint32 x, uint32 z) { return Relief(x, z) + (x == 150 && z == 100 ? 1.0f : 0.0f); });
    const HeightStoreCache::Result edited = cache.Ensure(asset, source, 300, 260, nullptr, nullptr);
    ASSERT_EQ(edited.Error, "");
    EXPECT_TRUE(edited.Cooked);
    EXPECT_NE(edited.Store, first.Store);
    EXPECT_FALSE(std::filesystem::exists(first.Store)) << "the previous key's store is gone";
    EXPECT_FALSE(std::filesystem::exists(orphan));
    std::size_t stores = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        stores += entry.path().extension() == ".gepage" ? 1u : 0u;
    EXPECT_EQ(stores, 1u);
}

// The progress the editor's cook banner shows: every source row read twice (identified, then built).
TEST_F(PageStoreTest, ACookReportsEverySourceRowReadTwice)
{
    const std::filesystem::path source = m_Dir / "dem.r32";
    WriteR32(source, 300, 260, Relief);
    const HeightStoreCache cache(m_Dir / "TerrainPages");
    const GUID asset = GUID::Generate();

    HeightCookProgress cooked;
    ASSERT_EQ(cache.Ensure(asset, source, 300, 260, nullptr, nullptr, nullptr, &cooked).Error, "");
    EXPECT_EQ(cooked.RowsTotal.load(), 2u * 260u);
    EXPECT_EQ(cooked.RowsDone.load(), cooked.RowsTotal.load());

    HeightCookProgress found;
    const HeightStoreCache::Result again = cache.Ensure(asset, source, 300, 260, nullptr, nullptr, nullptr, &found);
    ASSERT_FALSE(again.Cooked);
    EXPECT_EQ(found.RowsDone.load(), 260u) << "a found store is identified and not built again";
}

// ---- Residency ---------------------------------------------------------------------------------------

namespace
{

// Every page of a generated lattice, as a level rule closed under parents would want them.
std::vector<PageWant> EveryPage(std::span<const PageStoreLevel> levels)
{
    std::vector<PageWant> wanted;
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
                wanted.push_back({PageAddress{0, uint8(level), x, z}, float32(x + z), false});
    return wanted;
}

std::vector<PageWant> EveryPage(const GeneratedHeightPageProvider& provider)
{
    return EveryPage(provider.Levels());
}

// Frames until `done` holds (loads land between frames on the pool), at most a few seconds.
template <typename Done>
uint64 RunFramesUntil(PageResidencyManager& residency, uint64 frame, Done done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < deadline)
    {
        residency.Update(++frame, 1.0f / 60.0f, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return frame;
}

} // namespace

TEST(PageResidencyTest, WantedPagesLoadAssignAndThenAParkedCameraDoesNoWork)
{
    JobSystem::WorkStealingThreadPool pool(2);
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(7, 128, 2, 0.4f);
    residency.Configure(7, 7, PageSource{nullptr, provider});
    const std::vector<PageWant> wanted = EveryPage(*provider);

    uint64 frame = 0;
    residency.Request(7, wanted);
    // Each frame's uploads carry their page's samples until the next Update.
    std::vector<float32> expected(kPageSampleCount);
    uint32 uploaded = 0;
    frame = RunFramesUntil(residency, frame, [&] {
        for (const PageUploadRequest& upload : residency.FieldCache(7).Uploads())
        {
            EXPECT_TRUE(provider->FillPage(upload.Page.Address, expected));
            const std::vector<uint8>* bytes = residency.Bytes(7, upload.Page.Address);
            EXPECT_NE(bytes, nullptr);
            if (bytes != nullptr)
            {
                EXPECT_EQ(Heights(*bytes), expected);
            }
            ++uploaded;
        }
        return residency.FieldCache(7).ResidentCount() == wanted.size();
    });
    ASSERT_EQ(residency.FieldCache(7).ResidentCount(), wanted.size());
    EXPECT_EQ(uploaded, wanted.size());

    // Past the arrival fades, a parked camera starts and assigns nothing.
    for (int i = 0; i < 40; ++i)
        residency.Update(++frame, 1.0f / 60.0f, false);
    for (int i = 0; i < 5; ++i)
    {
        residency.Request(7, wanted);
        residency.Update(++frame, 1.0f / 60.0f, false);
        EXPECT_EQ(residency.LoadsStartedThisFrame(), 0u);
        EXPECT_EQ(residency.FieldCache(7).AssignsThisFrame(), 0u);
        EXPECT_TRUE(residency.FieldCache(7).Transitions().empty());
    }
    // Uploaded, the pages keep their samples on the CPU, for the CPU readers and a later upload:
    // under the budget nothing is dropped, so nothing is fetched twice.
    EXPECT_EQ(residency.CpuBytes(), uint64(wanted.size()) * kPageSampleCount * sizeof(float32));
    EXPECT_NE(residency.Bytes(7, wanted.front().Address), nullptr);
    pool.Shutdown();
}

TEST(PageResidencyTest, EveryStreamSharesOneUploadCapAndOneCpuBudget)
{
    JobSystem::WorkStealingThreadPool pool(2);
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    PageResidencyBudget budget;
    budget.UploadsPerFrame = 3;
    budget.CpuBytes = 7u * kPageSampleCount * sizeof(float32); // 7 pages, fewer than both streams want
    PageResidencyManager residency(&pool, nullptr, budget);
    residency.ConfigureField(1, 128, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    residency.ConfigureField(2, 128, 2, 0.0f);
    residency.Configure(2, 2, PageSource{nullptr, provider});
    const std::vector<PageWant> wanted = EveryPage(*provider);
    std::vector<PageWant> few(wanted.end() - 5, wanted.end()); // the top two levels, closed under parents
    residency.Request(1, few);
    residency.Request(2, few);

    uint64 frame = 0;
    frame = RunFramesUntil(residency, frame, [&] {
        EXPECT_LE(residency.FieldCache(1).AssignsThisFrame() + residency.FieldCache(2).AssignsThisFrame(), 3u);
        return residency.FieldCache(1).ResidentCount() == 5u && residency.FieldCache(2).ResidentCount() == 5u;
    });
    EXPECT_EQ(residency.FieldCache(1).ResidentCount(), 5u);
    EXPECT_EQ(residency.FieldCache(2).ResidentCount(), 5u);

    // The placeable pages stay past the budget (10 pages against 7); once stream 2 wants nothing,
    // its pages are dropped down to the budget.
    residency.Update(++frame, 1.0f / 60.0f, false);
    EXPECT_EQ(residency.CpuBytes(), 10u * kPageSampleCount * sizeof(float32));
    residency.Request(2, {});
    residency.Update(++frame, 1.0f / 60.0f, false);
    EXPECT_EQ(residency.CpuBytes(), budget.CpuBytes);
    pool.Shutdown();
}

// The CPU budget counts each page by the bytes it holds: a height page 67,600 B, a BC5 normal page
// 18,496 B, so a field of small pages keeps more of them under the same budget.
TEST(PageResidencyTest, TheCpuBudgetCountsEachPageByTheBytesItHolds)
{
    const std::filesystem::path dir = TestUtils::MakeUniqueTempDirectory("cpu_bytes");
    std::filesystem::create_directories(dir);
    constexpr uint32 kSide = 300u;
    const std::vector<uint8> normals(static_cast<std::size_t>(kSide) * kSide * 2u, 128u);
    BuildSurfaceStore(dir / "normal.gepage", PageFieldFormat::Bc5, PageFieldFilter::NormalTent, kSide, kSide, normals);
    auto normal = std::make_shared<PageStoreReader>();
    ASSERT_EQ(normal->Open(dir / "normal.gepage", PageFieldKind::Normal), "");
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    constexpr uint64 kHeightPage = 67600u;
    constexpr uint64 kNormalPage = 18496u;

    JobSystem::WorkStealingThreadPool pool(2);
    PageResidencyBudget budget;
    budget.CpuBytes = 5u * kHeightPage + 4u * kNormalPage; // the height pages and 4 of the 14 normal pages
    PageResidencyManager residency(&pool, nullptr, budget);
    residency.ConfigureField(1, 128, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    residency.ConfigureField(2, 128, 2, 0.0f);
    residency.Configure(2, 2, PageSource{normal, nullptr});
    const std::vector<PageWant> heights = EveryPage(*provider);
    const std::vector<PageWant> few(heights.end() - 5, heights.end()); // the top two levels, closed under parents
    const std::vector<PageWant> normalPages = EveryPage(normal->Layout().Levels);
    ASSERT_EQ(normalPages.size(), 14u);

    uint64 frame = 0;
    frame = RunFramesUntil(residency, frame, [&] {
        residency.Request(1, few);
        residency.Request(2, normalPages);
        return residency.FieldCache(1).ResidentCount() == 5u && residency.FieldCache(2).ResidentCount() == 14u;
    });
    ASSERT_EQ(residency.FieldCache(2).ResidentCount(), 14u);
    EXPECT_EQ(residency.CpuBytes(), 5u * kHeightPage + 14u * kNormalPage) << "the placeable pages stay past the budget";

    // Once the normal stream wants nothing, its pages are dropped down to the budget by their bytes.
    residency.Request(1, few);
    residency.Request(2, {});
    residency.Update(++frame, 1.0f / 60.0f, false);
    EXPECT_EQ(residency.CpuBytes(), budget.CpuBytes);
    uint32 normalsKept = 0;
    for (const PageWant& want : normalPages)
        normalsKept += residency.Bytes(2, want.Address) != nullptr ? 1u : 0u;
    EXPECT_EQ(normalsKept, 4u);
    residency.Forget(2);
    EXPECT_EQ(residency.CpuBytes(), 5u * kHeightPage) << "Forget releases the stream's pages by their bytes";
    pool.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

namespace
{

// The number of `pages` of stream `stream` whose bytes are on the CPU.
uint32 ResidentPages(const PageResidencyManager& residency, uint32 stream, const std::vector<PageWant>& pages)
{
    uint32 count = 0;
    for (const PageWant& want : pages)
        count += residency.Bytes(stream, want.Address) != nullptr ? 1u : 0u;
    return count;
}

} // namespace

// The trim drops the least recently wanted page first: the normal stream's level-0 pages stop being
// wanted a frame before its top pages, and the height pages then push the bytes over the budget by
// one normal page.
TEST(PageResidencyTest, TheTrimDropsTheLeastRecentlyWantedPageFirst)
{
    const std::filesystem::path dir = TestUtils::MakeUniqueTempDirectory("trim_order");
    std::filesystem::create_directories(dir);
    constexpr uint32 kSide = 300u;
    const std::vector<uint8> normals(static_cast<std::size_t>(kSide) * kSide * 2u, 128u);
    BuildSurfaceStore(dir / "normal.gepage", PageFieldFormat::Bc5, PageFieldFilter::NormalTent, kSide, kSide, normals);
    auto normal = std::make_shared<PageStoreReader>();
    ASSERT_EQ(normal->Open(dir / "normal.gepage", PageFieldKind::Normal), "");
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    constexpr uint64 kHeightPage = 67600u;
    constexpr uint64 kNormalPage = 18496u;

    JobSystem::WorkStealingThreadPool pool(2);
    PageResidencyBudget budget;
    budget.CpuBytes = 5u * kHeightPage + 13u * kNormalPage;
    PageResidencyManager residency(&pool, nullptr, budget);
    residency.ConfigureField(1, 128, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    residency.ConfigureField(2, 128, 2, 0.0f);
    residency.Configure(2, 2, PageSource{normal, nullptr});
    const std::vector<PageWant> heights = EveryPage(*provider);
    const std::vector<PageWant> few(heights.end() - 5, heights.end()); // the top two levels, closed under parents
    const std::vector<PageWant> normalPages = EveryPage(normal->Layout().Levels);
    ASSERT_EQ(normalPages.size(), 14u);
    const std::vector<PageWant> normalLevel0(normalPages.begin(), normalPages.begin() + 9);
    const std::vector<PageWant> normalTop(normalPages.begin() + 9, normalPages.end());

    uint64 frame = 0;
    frame = RunFramesUntil(residency, frame, [&] {
        residency.Request(2, normalPages);
        return residency.FieldCache(2).ResidentCount() == 14u;
    });
    ASSERT_EQ(residency.CpuBytes(), 14u * kNormalPage);
    residency.Request(2, normalTop);
    residency.Update(++frame, 1.0f / 60.0f, false);
    frame = RunFramesUntil(residency, frame, [&] {
        residency.Request(1, few);
        residency.Request(2, {});
        return residency.FieldCache(1).ResidentCount() == 5u;
    });
    EXPECT_EQ(residency.CpuBytes(), budget.CpuBytes);
    EXPECT_EQ(ResidentPages(residency, 2, normalTop), 5u) << "a top page, wanted later, was dropped before a level-0 page";
    EXPECT_EQ(ResidentPages(residency, 2, normalLevel0), 8u);
    pool.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Two terrains of one field divide its cache by the one order (design D5: the budget is the
// field's, whatever the number of terrains): the field holds its slot count and no more, both
// terrains' nearest pages are among them, and no page past the budget is ever loaded.
namespace
{

// Ten frames in which stream `owner` (1 or 2) wants `few` and the other stream nothing.
void SettleOwner(PageResidencyManager& residency, const std::vector<PageWant>& few, uint32 owner, uint64& frame)
{
    for (int i = 0; i < 10; ++i)
    {
        residency.Request(owner, few);
        residency.Request(3u - owner, {});
        residency.Update(++frame, 1.0f / 60.0f, false);
    }
}

} // namespace

// One fetch for both sides: an uploaded page's samples stay on the CPU, so the CPU readers read the
// GPU's own fetch, and a page released from its slot and wanted again is placed with no new load.
TEST(PageResidencyTest, AnUploadedPageStaysOnTheCpuAndIsPlacedAgainWithNoNewLoad)
{
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const std::vector<PageWant> wanted = EveryPage(*provider);
    const std::vector<PageWant> few(wanted.end() - 5, wanted.end()); // the top two levels, closed under parents
    PageResidencyManager residency(nullptr, nullptr, PageResidencyBudget{}); // inline loads, drained a frame later
    residency.ConfigureField(1, 6, 2, 0.0f); // one stream's pages and the other's top page, which stays
    residency.Configure(1, 1, PageSource{nullptr, provider});
    residency.Configure(2, 1, PageSource{nullptr, provider});
    uint64 frame = 0;
    SettleOwner(residency, few, 1, frame);
    ASSERT_NE(residency.FieldCache(1).SlotOf(CachedPage{1, few.back().Address}), kNoResidentSlot);
    std::span<const uint8> cpu;
    ASSERT_EQ(residency.CpuPages(1).Page(few.back().Address, cpu), CpuPageState::Resident);
    EXPECT_EQ(cpu.data(), residency.Bytes(1, few.back().Address)->data()) << "the CPU reads the GPU's fetch";

    SettleOwner(residency, few, 2, frame); // stream 2 takes the slots: stream 1's level-1 pages leave the cache
    ASSERT_EQ(residency.FieldCache(1).SlotOf(CachedPage{1, few.front().Address}), kNoResidentSlot);
    uint32 loads = 0;
    for (int i = 0; i < 10; ++i)
    {
        residency.Request(1, few);
        residency.Request(2, {});
        residency.Update(++frame, 1.0f / 60.0f, false);
        loads += residency.LoadsStartedThisFrame();
    }
    EXPECT_NE(residency.FieldCache(1).SlotOf(CachedPage{1, few.front().Address}), kNoResidentSlot);
    EXPECT_EQ(loads, 0u) << "stream 1's pages were fetched again";
}

TEST(PageResidencyTest, TwoTerrainsOfAFieldShareItsSlotsNearestFirst)
{
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 2, 2));
    const std::vector<PageWant> nearTerrain = EveryPage(*provider);
    std::vector<PageWant> farTerrain = nearTerrain;
    for (PageWant& want : farTerrain)
        want.Distance += 0.5f; // the two terrains' pages interleave in the one order
    constexpr uint32 kFieldSlots = 12;
    ASSERT_GT(nearTerrain.size(), kFieldSlots);

    PageResidencyManager residency(nullptr, nullptr, PageResidencyBudget{}); // inline loads, drained a frame later
    residency.ConfigureField(1, kFieldSlots, 2, 0.0f);
    residency.Configure(10, 1, PageSource{nullptr, provider});
    residency.Configure(11, 1, PageSource{nullptr, provider});
    uint32 loads = 0;
    for (uint64 frame = 1; frame <= 40; ++frame)
    {
        residency.Request(10, nearTerrain);
        residency.Request(11, farTerrain);
        residency.Update(frame, 1.0f / 60.0f, false);
        loads += residency.LoadsStartedThisFrame();
    }

    const PageCache& cache = residency.FieldCache(1);
    EXPECT_EQ(cache.ResidentCount(), kFieldSlots);
    EXPECT_EQ(loads, kFieldSlots) << "pages past the field's budget were loaded";
    const PageAddress top = nearTerrain.back().Address;
    EXPECT_NE(cache.SlotOf(CachedPage{10, top}), kNoResidentSlot);
    EXPECT_NE(cache.SlotOf(CachedPage{11, top}), kNoResidentSlot);
    EXPECT_NE(cache.SlotOf(CachedPage{10, PageAddress{0, 0, 0, 0}}), kNoResidentSlot);
    EXPECT_NE(cache.SlotOf(CachedPage{11, PageAddress{0, 0, 0, 0}}), kNoResidentSlot);

    // A terrain that leaves takes its pages out of the field's cache at once.
    residency.Forget(11);
    residency.Request(10, nearTerrain);
    residency.Update(41, 1.0f / 60.0f, false);
    for (const CachedPage& page : cache.Resident())
        EXPECT_EQ(page.Stream, 10u);
}

// ---- Review fold (PR #3072): one priority across streams, bounded loads, retries, restarts --------------

TEST(PageResidencyTest, APinnedPageOfOneStreamLoadsBeforeAnotherStreamsFinePages)
{
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const std::vector<PageWant> every = EveryPage(*provider);
    ASSERT_GT(every.size(), 4u);
    for (uint32 pinnedStream : {1u, 2u})
    {
        PageResidencyBudget budget;
        budget.LoadsInFlight = 2;
        PageResidencyManager residency(nullptr, nullptr, budget); // inline loads, drained a frame later
        residency.ConfigureField(1, 128, 2, 0.0f);
        residency.Configure(1, 1, PageSource{nullptr, provider});
        residency.ConfigureField(2, 128, 2, 0.0f);
        residency.Configure(2, 2, PageSource{nullptr, provider});
        const uint32 fineStream = pinnedStream == 1u ? 2u : 1u;
        PageWant pinned = every.back();
        pinned.Pinned = true;
        residency.Request(pinnedStream, std::vector<PageWant>{pinned});
        residency.Request(fineStream, every);
        residency.Update(1, 1.0f / 60.0f, false);
        residency.Update(2, 1.0f / 60.0f, false);
        EXPECT_NE(residency.Bytes(pinnedStream, pinned.Address), nullptr)
            << "stream " << pinnedStream << "'s pinned page waited behind stream " << fineStream << "'s pages";
    }
}

TEST(PageResidencyTest, AStreamDoesNotLoadAndKeepMorePagesThanItsCacheCanHold)
{
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const std::vector<PageWant> every = EveryPage(*provider);
    PageResidencyBudget budget;
    budget.CpuBytes = 4u * kPageSampleCount * sizeof(float32);
    PageResidencyManager residency(nullptr, nullptr, budget);
    residency.ConfigureField(1, 4, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    residency.Request(1, every);
    for (uint64 frame = 1; frame <= 20; ++frame)
        residency.Update(frame, 1.0f / 60.0f, false);
    EXPECT_EQ(residency.FieldCache(1).ResidentCount(), 4u);
    EXPECT_LE(residency.CpuBytes(), budget.CpuBytes)
        << "pages on the CPU: " << residency.CpuBytes() / (kPageSampleCount * sizeof(float32)) << " of "
        << every.size() << " wanted, for a cache of 4 slots and a budget of 4 pages";
}

TEST_F(PageStoreTest, ATransientReadFailureIsRetried)
{
    const std::filesystem::path source = m_Dir / "transient.r32";
    WriteR32(source, 300, 260, Relief);
    const HeightStoreCache cache(m_Dir / "TerrainPages");
    const HeightStoreCache::Result store = cache.Ensure(GUID::Generate(), source, 300, 260, nullptr, nullptr);
    ASSERT_EQ(store.Error, "");
    auto reader = std::make_shared<PageStoreReader>();
    ASSERT_EQ(reader->Open(store.Store, PageFieldKind::Height), "");
    std::vector<char> bytes;
    {
        std::ifstream in(store.Store, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const PageAddress top{0, uint8(reader->Layout().Levels.size() - 1u), 0, 0};
    PageResidencyManager residency(nullptr, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, 16, 2, 0.0f);
    residency.Configure(1, 1, PageSource{reader, nullptr});

    std::error_code ec;
    std::filesystem::resize_file(store.Store, 64, ec); // the read fails once (a store being rewritten)
    ASSERT_FALSE(ec) << ec.message();
    residency.Request(1, std::vector<PageWant>{PageWant{top, 0.0f, true}});
    residency.Update(1, 1.0f / 60.0f, false);
    residency.Update(2, 1.0f / 60.0f, false);
    ASSERT_EQ(residency.Bytes(1, top), nullptr) << "the truncated read should have failed";

    {
        std::ofstream out(store.Store, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), std::streamsize(bytes.size()));
    }
    for (uint64 frame = 3; frame < 200; ++frame)
    {
        residency.Request(1, std::vector<PageWant>{PageWant{top, 0.0f, true}});
        residency.Update(frame, 1.0f / 60.0f, false);
    }
    EXPECT_NE(residency.FieldCache(1).SlotOf(CachedPage{1, top}), kNoResidentSlot)
        << "the store is whole again, but the stream marked its top page unloadable for good";
}

TEST(PageResidencyTest, ALoadOfARestartedStreamsOldSourceNeverLands)
{
    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    pool.Submit([gate] { gate.wait(); }, JobSystem::JobPriority::Background); // holds the one worker

    GeneratedHeightLattice flatLattice = NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1);
    flatLattice.Source = GeneratedHeightSource::Flat;
    const auto oldSource = std::make_shared<GeneratedHeightPageProvider>(flatLattice);
    const auto newSource = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const PageWant top = EveryPage(*newSource).back();

    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, 16, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, oldSource});
    residency.Request(1, std::vector<PageWant>{top});
    residency.Update(1, 1.0f / 60.0f, false); // the old source's load queues behind the gate
    residency.Configure(1, 1, PageSource{nullptr, newSource}); // a re-cook restarts the stream
    residency.Request(1, std::vector<PageWant>{top});
    residency.Update(2, 1.0f / 60.0f, false); // the new source's load queues too
    release.set_value();

    uint64 frame = 2;
    frame = RunFramesUntil(residency, frame,
                           [&] { return residency.FieldCache(1).SlotOf(CachedPage{1, top.Address}) != kNoResidentSlot; });
    std::vector<float32> expected(kPageSampleCount);
    ASSERT_TRUE(newSource->FillPage(top.Address, expected));
    ASSERT_NE(residency.Bytes(1, top.Address), nullptr);
    EXPECT_EQ(Heights(*residency.Bytes(1, top.Address)), expected) << "the stream holds the old source's page";
    EXPECT_EQ(residency.CpuBytes(), uint64(kPageSampleCount) * sizeof(float32))
        << "the old source's load was counted into the restarted stream";
    for (int i = 0; i < 10; ++i)
    {
        residency.Request(1, std::vector<PageWant>{top});
        residency.Update(++frame, 1.0f / 60.0f, false);
    }
    EXPECT_EQ(residency.CpuBytes(), uint64(kPageSampleCount) * sizeof(float32))
        << "the old source's load landed after the restart";
    EXPECT_EQ(Heights(*residency.Bytes(1, top.Address)), expected) << "the old source's load landed after the restart";
    pool.Shutdown();
}

TEST(PageResidencyTest, ASmallPyramidUnderItsCapStaysWhollyResidentWithNoFurtherLoads)
{
    JobSystem::WorkStealingThreadPool pool(2);
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    TerrainECS::PageRequestTerrain terrain;
    terrain.SamplesX = terrain.SamplesZ = 1025;
    terrain.Level0TexelX = terrain.Level0TexelZ = 1.0f;
    terrain.MaxHeight = 1.0f;
    terrain.CacheSlots = 512; // the desktop height cache: a pyramid of up to 128 pages is pinned whole
    TerrainECS::HeightPageRequester requester;
    requester.Configure(terrain);
    ASSERT_EQ(requester.FirstPinnedLevel(), 0u);

    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, terrain.CacheSlots, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    const Mathematics::Vector3 far{1.0e6f, 1.0e6f, 1.0e6f}; // nowhere near: only pinning keeps pages
    const TerrainECS::PageLevelView view = TerrainECS::MakePageLevelView(1080, 1.0f, 11.0f);
    std::vector<PageWant> wants;
    const std::size_t pages = EveryPage(*provider).size();
    uint64 frame = 0;
    frame = RunFramesUntil(residency, frame, [&] {
        requester.Build(std::span(&far, 1), view, wants);
        residency.Request(1, wants);
        return residency.FieldCache(1).ResidentCount() == pages;
    });
    ASSERT_EQ(residency.FieldCache(1).ResidentCount(), pages);
    for (int i = 0; i < 10; ++i)
    {
        requester.Build(std::span(&far, 1), view, wants);
        residency.Request(1, wants);
        residency.Update(++frame, 1.0f / 60.0f, false);
        EXPECT_EQ(residency.LoadsStartedThisFrame(), 0u);
        EXPECT_EQ(residency.FieldCache(1).ResidentCount(), pages);
    }
    pool.Shutdown();
}

// ---- Review fold 2 (PR #3072) -------------------------------------------------------------------------

TEST(PageResidencyTest, ForgettingAStreamWhileAnotherIsParkedIsSafe)
{
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const std::vector<PageWant> every = EveryPage(*provider);
    PageResidencyManager residency(nullptr, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, 128, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    residency.ConfigureField(2, 128, 2, 0.0f);
    residency.Configure(2, 2, PageSource{nullptr, provider});
    uint64 frame = 0;
    for (int i = 0; i < 10; ++i)
    {
        residency.Request(1, every);
        residency.Request(2, every);
        residency.Update(++frame, 1.0f / 60.0f, false);
    }
    residency.Forget(2); // a terrain deleted while the other's camera is parked
    for (int i = 0; i < 3; ++i)
    {
        residency.Request(1, every);
        EXPECT_NO_THROW(residency.Update(++frame, 1.0f / 60.0f, false)) << "frame " << i << " after Forget";
    }
    EXPECT_EQ(residency.FieldCache(1).ResidentCount(), every.size());
}

TEST(PageResidencyTest, AnOldSourcesFinishedLoadNeverLandsInARestartedStream)
{
    // Inline loads: the old source's page has finished (past any cancel check) before the restart,
    // so the epoch is the only guard. (The cancel token guards loads still queued at the restart;
    // ALoadOfARestartedStreamsOldSourceNeverLands covers both together.)
    GeneratedHeightLattice flatLattice = NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1);
    flatLattice.Source = GeneratedHeightSource::Flat;
    const auto oldSource = std::make_shared<GeneratedHeightPageProvider>(flatLattice);
    const auto newSource = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const PageWant top = EveryPage(*newSource).back();
    PageResidencyManager residency(nullptr, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, 16, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, oldSource});
    residency.Request(1, std::vector<PageWant>{top});
    residency.Update(1, 1.0f / 60.0f, false); // the old page is filled and queued
    residency.Configure(1, 1, PageSource{nullptr, newSource});
    for (uint64 frame = 2; frame < 8 && residency.FieldCache(1).SlotOf(CachedPage{1, top.Address}) == kNoResidentSlot;
         ++frame)
    {
        residency.Request(1, std::vector<PageWant>{top});
        residency.Update(frame, 1.0f / 60.0f, false);
    }
    std::vector<float32> expected(kPageSampleCount);
    ASSERT_TRUE(newSource->FillPage(top.Address, expected));
    ASSERT_NE(residency.Bytes(1, top.Address), nullptr);
    EXPECT_EQ(Heights(*residency.Bytes(1, top.Address)), expected) << "the stream holds the old source's page";
}

TEST(PageResidencyTest, ACancelledCookLeavesNoStoreAndNoTemporaryFile)
{
    const std::filesystem::path dir = TestUtils::MakeUniqueTempDirectory("cancelled_cook");
    std::filesystem::create_directories(dir);
    const std::filesystem::path source = dir / "dem.r32";
    WriteR32(source, 300, 260, Relief);
    HeightCookRequest request;
    ASSERT_EQ(ResolveHeightCookSource(source, 300, 260, request.Source), "");
    ASSERT_EQ(ScanHeightSource(request.Source, nullptr, request.Identity), "");
    request.Output = dir / "dem.gepage";
    const std::atomic<bool> cancelled{true}; // raised while the cook runs its first band
    request.Cancel = &cancelled;
    const HeightCookResult result = CookHeightPageStore(request);
    EXPECT_NE(result.Error.find("cancelled"), std::string::npos);
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        files += entry.path() != source ? 1u : 0u;
    EXPECT_EQ(files, 0u) << "a cancelled cook left a store or a temporary file";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- Height page overlay (the modifiers' baked-page cache) ----------------------------------------

namespace
{

// The store's pyramid filter, written out: level L + 1 sample (x, z) is the [1 2 1] / 4 tent of
// level L over clamped indices (PagePyramidBuilder).
std::vector<std::vector<float32>> TentPyramid(std::vector<float32> level0, const std::vector<PageStoreLevel>& levels)
{
    std::vector<std::vector<float32>> out{std::move(level0)};
    for (std::size_t level = 1; level < levels.size(); ++level)
    {
        const PageStoreLevel& finer = levels[level - 1];
        const PageStoreLevel& coarser = levels[level];
        std::vector<float32> next(static_cast<std::size_t>(coarser.SamplesX) * coarser.SamplesZ);
        const auto at = [&](int64 x, int64 z) {
            x = std::clamp<int64>(x, 0, finer.SamplesX - 1);
            z = std::clamp<int64>(z, 0, finer.SamplesZ - 1);
            return out.back()[static_cast<std::size_t>(z) * finer.SamplesX + static_cast<std::size_t>(x)];
        };
        for (uint32 z = 0; z < coarser.SamplesZ; ++z)
            for (uint32 x = 0; x < coarser.SamplesX; ++x)
            {
                float32 sum = 0.0f;
                for (int64 dz = -1; dz <= 1; ++dz)
                {
                    const float32 row = 0.25f * at(2 * int64{x} - 1, 2 * int64{z} + dz) +
                                        0.5f * at(2 * int64{x}, 2 * int64{z} + dz) +
                                        0.25f * at(2 * int64{x} + 1, 2 * int64{z} + dz);
                    sum += (dz == 0 ? 0.5f : 0.25f) * row;
                }
                next[static_cast<std::size_t>(z) * coarser.SamplesX + x] = sum;
            }
        out.push_back(std::move(next));
    }
    return out;
}

// A deterministic delta over `rect`, as a modifier footprint would leave.
std::vector<float32> RectDelta(const PageSampleRect& rect, float32 seed)
{
    std::vector<float32> delta(static_cast<std::size_t>(rect.Width()) * rect.Height());
    for (uint32 z = 0; z < rect.Height(); ++z)
        for (uint32 x = 0; x < rect.Width(); ++x)
            delta[static_cast<std::size_t>(z) * rect.Width() + x] =
                std::sin(seed + 0.37f * static_cast<float32>(x)) * std::cos(0.21f * static_cast<float32>(z));
    return delta;
}

std::vector<float32> Dense(const PageSampleRect& rect, const std::vector<float32>& delta, const PageStoreLevel& level0)
{
    std::vector<float32> dense(static_cast<std::size_t>(level0.SamplesX) * level0.SamplesZ, 0.0f);
    for (uint32 z = rect.MinZ; z <= rect.MaxZ; ++z)
        for (uint32 x = rect.MinX; x <= rect.MaxX; ++x)
            dense[static_cast<std::size_t>(z) * level0.SamplesX + x] =
                delta[static_cast<std::size_t>(z - rect.MinZ) * rect.Width() + (x - rect.MinX)];
    return dense;
}

void ExpectOverlayIsThePyramidOf(const HeightPageOverlay& overlay, const std::vector<std::vector<float32>>& pyramid,
                                 const std::vector<PageStoreLevel>& levels)
{
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].SamplesZ; ++z)
            for (uint32 x = 0; x < levels[level].SamplesX; ++x)
                ASSERT_NEAR(overlay.Delta(level, x, z),
                            pyramid[level][static_cast<std::size_t>(z) * levels[level].SamplesX + x], 1.0e-6f)
                    << "level " << level << " sample (" << x << ", " << z << ")";
}

} // namespace

TEST(HeightPageOverlayTest, EachLevelIsTheStoresTentOfTheBakedMinusTheBase)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(601, 300);
    const PageSampleRect rect{100, 40, 250, 170};
    const std::vector<float32> delta = RectDelta(rect, 0.5f);
    const HeightPageOverlay overlay(levels, rect, delta);
    ExpectOverlayIsThePyramidOf(overlay, TentPyramid(Dense(rect, delta, levels.front()), levels), levels);
}

TEST(HeightPageOverlayTest, AnEditRebakesOnlyItsRectAndEqualsAFreshOverlay)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(601, 300);
    const PageSampleRect rect{100, 40, 250, 170};
    std::vector<float32> delta = RectDelta(rect, 0.5f);
    HeightPageOverlay after(levels, rect, delta);
    const PageSampleRect edit{180, 90, 210, 140};
    const std::vector<float32> edited = RectDelta(edit, 2.0f);
    after.Rebake(edit, edited);
    for (uint32 z = edit.MinZ; z <= edit.MaxZ; ++z)
        for (uint32 x = edit.MinX; x <= edit.MaxX; ++x)
            delta[static_cast<std::size_t>(z - rect.MinZ) * rect.Width() + (x - rect.MinX)] =
                edited[static_cast<std::size_t>(z - edit.MinZ) * edit.Width() + (x - edit.MinX)];
    ExpectOverlayIsThePyramidOf(after, TentPyramid(Dense(rect, delta, levels.front()), levels), levels);
}

TEST(HeightPageOverlayTest, APageGainsItsLevelsDeltaUnderEveryStoredSampleApronIncluded)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(601, 300);
    // Level-0 samples 120 to 140 on both axes: across the edge between pages (1, 0) and (0, 0) and
    // the edge between (1, 0) and (1, 1), so page (1, 0)'s first apron column (x 127) and last apron
    // row (z 128) lie inside the overlay.
    const PageSampleRect rect{120, 120, 140, 140};
    const HeightPageOverlay overlay(levels, rect, RectDelta(rect, 0.5f));
    const uint32 pageX = 1u;
    const PageAddress page{0, 0, pageX, 0};
    std::vector<float32> samples(kPageSampleCount, 1.0f);
    ASSERT_TRUE(overlay.ApplyToPage(page, samples));
    const auto sampleAt = [&](uint32 stored, uint32 origin, uint32 count) {
        return std::min<uint32>(origin + stored > 0u ? origin + stored - 1u : 0u, count - 1u);
    };
    for (uint32 row = 0; row < kPageStrideSamples; ++row)
        for (uint32 i = 0; i < kPageStrideSamples; ++i)
        {
            const uint32 x = sampleAt(i, pageX * kPageOwnedSamples, levels[0].SamplesX);
            const uint32 z = sampleAt(row, 0u, levels[0].SamplesZ);
            ASSERT_FLOAT_EQ(samples[row * kPageStrideSamples + i], 1.0f + overlay.Delta(0, x, z));
        }
    const uint32 lastRow = kPageStrideSamples - 1u;
    // Stored row r holds level-0 row r - 1 on this page (z 124 is row 125); stored column i holds
    // x = 127 + i.
    EXPECT_NE(overlay.Delta(0, pageX * kPageOwnedSamples - 1u, 124u), 0.0f) << "the first apron column is reached";
    EXPECT_NE(samples[125u * kPageStrideSamples + 0u], 1.0f) << "the first apron column gains its delta";
    EXPECT_NE(overlay.Delta(0, 130u, kPageOwnedSamples), 0.0f) << "the last apron row is reached";
    EXPECT_NE(samples[lastRow * kPageStrideSamples + (130u - pageX * kPageOwnedSamples + 1u)], 1.0f)
        << "the last apron row gains its delta";
    std::vector<float32> far(kPageSampleCount, 1.0f);
    EXPECT_FALSE(overlay.ApplyToPage(PageAddress{0, 0, 4, 2}, far)) << "a page no modifier reaches is untouched";
    EXPECT_EQ(far, std::vector<float32>(kPageSampleCount, 1.0f));
}

TEST(PageResidencyTest, ARefreshedResidentPageIsRewrittenInItsSlotWithTheRectsRefreshedUnderIt)
{
    JobSystem::WorkStealingThreadPool pool(2);
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(7, 128, 2, 0.4f);
    residency.Configure(7, 7, PageSource{nullptr, provider});
    const std::vector<PageWant> wanted = EveryPage(*provider);
    residency.Request(7, wanted);
    uint64 frame = RunFramesUntil(residency, 0, [&] { return residency.FieldCache(7).ResidentCount() == wanted.size(); });
    ASSERT_EQ(residency.FieldCache(7).ResidentCount(), wanted.size());
    for (int i = 0; i < 40; ++i)
    {
        residency.Request(7, wanted);
        residency.Update(++frame, 1.0f / 60.0f, false);
    }

    // An edit over level-0 samples 10 to 20, then one over 30 to 40 by 4 to 12 before any rewrite:
    // the level-0 page (0, 0) and its ancestors read both, and each rewrite records their union.
    const PageSampleRect edit{10, 10, 20, 20};
    const PageSampleRect second{30, 4, 40, 12};
    const PageSampleRect both{10, 4, 40, 20};
    const CachedPage edited{7, PageAddress{0, 0, 0, 0}};
    const uint32 slot = residency.FieldCache(7).SlotOf(edited);
    residency.Refresh(7, edit);
    residency.Refresh(7, second);
    std::vector<CachedPage> rewritten;
    frame = RunFramesUntil(residency, frame, [&] {
        residency.Request(7, wanted);
        EXPECT_EQ(residency.FieldCache(7).ResidentCount(), wanted.size()) << "a refreshed page left its slot";
        for (const PageUploadRequest& upload : residency.FieldCache(7).Uploads())
        {
            EXPECT_NE(residency.Bytes(7, upload.Page.Address), nullptr) << "a rewrite without its samples";
            rewritten.push_back(upload.Page);
        }
        for (const PageRewrite& rewrite : residency.FieldCache(7).Rewrites())
        {
            const PageSampleRect& r = rewrite.Level0Rect;
            EXPECT_EQ(std::tie(r.MinX, r.MinZ, r.MaxX, r.MaxZ), std::tie(both.MinX, both.MinZ, both.MaxX, both.MaxZ))
                << "level " << int(rewrite.Page.Address.Level) << " recorded another rect than the two edits under it";
        }
        return rewritten.size() >= provider->Levels().size();
    });
    EXPECT_EQ(residency.FieldCache(7).SlotOf(edited), slot);
    EXPECT_NE(std::find(rewritten.begin(), rewritten.end(), edited), rewritten.end());
    for (const CachedPage& page : rewritten)
        EXPECT_TRUE(PageReadsLevel0Rect(page.Address, edit))
            << "level " << int(page.Address.Level) << " page (" << page.Address.X << ", " << page.Address.Z
            << ") was rewritten though the edit is not under it";
    EXPECT_EQ(rewritten.size(), provider->Levels().size()) << "one page per level reads the edit";
    pool.Shutdown();
}

// ---- Planet stores ----------------------------------------------------------------------------------

namespace
{

// The 2 km planet of the Terrain component's defaults: its radius and its relief.
constexpr float64 kPlanetRadius = 2000.0;

float32 DefaultPlanetHeight(uint8, uint32, uint32, const Mathematics::Vector3& direction)
{
    const Components::TerrainPlanetRelief relief{};
    return CBTTerrain::PlanetRelief(direction.x, direction.y, direction.z, relief.Amplitude, relief.Frequency,
                                    relief.Octaves);
}

// The unit direction of sample (x, z) of `face` on the equal-angle lattice of `samples` a side.
Mathematics::Vector3 LatticeDirection(uint8 face, uint32 x, uint32 z, uint32 samples)
{
    const Mathematics::CubeFaceAxes& axes = Mathematics::kCubeFaceAxes[face];
    std::array<float64, 3> cube{};
    cube[axes.NormalAxis] = axes.NormalSign;
    cube[axes.XAxis] = GnomonicFaceCoordinate(EqualAngleSampleCoordinate(x, samples));
    cube[axes.ZAxis] = GnomonicFaceCoordinate(EqualAngleSampleCoordinate(z, samples));
    const float64 length = std::sqrt(cube[0] * cube[0] + cube[1] * cube[1] + cube[2] * cube[2]);
    return Mathematics::Vector3(static_cast<float32>(cube[0] / length), static_cast<float32>(cube[1] / length),
                                static_cast<float32>(cube[2] / length));
}

// The 2 km planet at 257 samples a face side (about 12 m): three levels of 3 x 3, 2 x 2 and one page a
// face.
PlanetCookRequest SmallPlanetRequest(const std::filesystem::path& output)
{
    PlanetCookRequest request;
    request.Height = DefaultPlanetHeight;
    request.ReferenceRadius = kPlanetRadius;
    request.FaceSamples = 2u * kPageOwnedSamples + 1u;
    request.Key = 0x5eed;
    request.Output = output;
    return request;
}

// A face's stored samples at a level, (x, z) in [-1, samples]: an owned sample, or the apron one past
// a side, read from the page that owns the nearest owned sample.
class FaceSamples
{
public:
    FaceSamples(const PageStoreReader& reader, uint8 face, uint8 level)
        : m_Reader(reader), m_Face(face), m_Level(level), m_Last(reader.Layout().Levels[level].SamplesX - 1u)
    {
    }

    float32 At(int64 x, int64 z)
    {
        const uint32 pageX = static_cast<uint32>(std::clamp<int64>(x, 0, m_Last)) / kPageOwnedSamples;
        const uint32 pageZ = static_cast<uint32>(std::clamp<int64>(z, 0, m_Last)) / kPageOwnedSamples;
        const PageAddress address{m_Face, m_Level, pageX, pageZ};
        auto [page, inserted] = m_Pages.try_emplace(address);
        if (inserted)
            page->second = PageSamples(m_Reader, address);
        const int64 column = x - static_cast<int64>(pageX) * kPageOwnedSamples + kPageApronSamples;
        const int64 row = z - static_cast<int64>(pageZ) * kPageOwnedSamples + kPageApronSamples;
        return page->second[static_cast<std::size_t>(row * kPageStrideSamples + column)];
    }

    // The sample `depth` in from `side` (-1 is the apron past it) at `along`.
    float32 FromSide(PageFaceSide side, int64 along, int64 depth)
    {
        const int64 last = m_Last;
        switch (side)
        {
        case PageFaceSide::MinZ: return At(along, depth);
        case PageFaceSide::MaxZ: return At(along, last - depth);
        case PageFaceSide::MinX: return At(depth, along);
        case PageFaceSide::MaxX: return At(last - depth, along);
        }
        return 0.0f;
    }

private:
    const PageStoreReader& m_Reader;
    uint8 m_Face;
    uint8 m_Level;
    uint32 m_Last;
    std::map<PageAddress, std::vector<float32>> m_Pages;
};

// The point (x, z) of the cube face `axes`, each of x and z -1 or +1: a corner of the face.
Mathematics::Vector3Int FaceCubePoint(const Mathematics::CubeFaceAxes& axes, int32 x, int32 z)
{
    const auto component = [&axes, x, z](uint8 axis) {
        return axis == axes.NormalAxis ? int32{axes.NormalSign} : (axis == axes.XAxis ? axes.XSign * x : axes.ZSign * z);
    };
    return Mathematics::Vector3Int(component(0), component(1), component(2));
}

} // namespace

TEST(PageStoreFormatTest, TheEqualAngleCoordinateIsOddExactAtTheEdgesAndNewtonInvertsIt)
{
    EXPECT_EQ(EqualAngleFaceCoordinate(1.0), 1.0);
    EXPECT_EQ(EqualAngleFaceCoordinate(-1.0), -1.0);
    EXPECT_EQ(EqualAngleFaceCoordinate(0.0), 0.0);
    EXPECT_EQ(GnomonicFaceCoordinate(1.0), 1.0);
    EXPECT_EQ(GnomonicFaceCoordinate(-1.0), -1.0);
    float64 previous = -2.0;
    for (int32 i = -1000; i <= 1000; ++i)
    {
        const float64 a = i / 1000.0;
        const float64 s = EqualAngleFaceCoordinate(a);
        EXPECT_EQ(EqualAngleFaceCoordinate(-a), -s);
        EXPECT_GT(s, previous);
        EXPECT_NEAR(s, 4.0 / std::numbers::pi * std::atan(a), 3.7e-5);
        EXPECT_NEAR(GnomonicFaceCoordinate(s), a, 1e-14);
        EXPECT_EQ(GnomonicFaceCoordinate(-s), -GnomonicFaceCoordinate(s));
        previous = s;
    }
    for (uint32 index = 0; index < 257u; ++index)
        EXPECT_EQ(EqualAngleSampleCoordinate(256u - index, 257u), -EqualAngleSampleCoordinate(index, 257u));
}

TEST(PageStoreFormatTest, TheEqualAngleCoordinateAndItsInverseKeepTheirRecordedBits)
{
    // Recorded without contraction. At these lattice points a fused multiply-add changes the bits.
    struct Pin
    {
        uint32 Index;
        uint64 EqualAngleBits;
        uint64 GnomonicBits;
    };
    constexpr Pin kPins[] = {
        {4u, 0xbfef5a77e71c1f8eull, 0xbfee775bc10fbb38ull},
        {16u, 0xbfed49c7b5b42538ull, 0xbfea4301ee0e1fb9ull},
        {55u, 0xbfe51e0441b53dceull, 0xbfdec01676186171ull},
        {73u, 0xbfe0894798b708fbull, 0xbfd674a2e394d8d6ull},
        {95u, 0xbfd48fcd339e54eaull, 0xbfca4724a57772feull},
        {99u, 0xbfd227ef2b17f50aull, 0xbfc705295dfd3983ull},
        {109u, 0xbfc803f6476b13dcull, 0xbfbdfb8e69612e86ull},
        {114u, 0xbfc1c0f76ef5005eull, 0xbfb60c04fd5e2667ull},
        {122u, 0xbfae8862eeba6d73ull, 0xbfa2dc1930c14c73ull},
        {142u, 0x3fc1c0f76ef5005eull, 0x3fb60c04fd5e2667ull},
        {147u, 0x3fc803f6476b13dcull, 0x3fbdfb8e69612e86ull},
        {157u, 0x3fd227ef2b17f50aull, 0x3fc705295dfd3983ull},
        {161u, 0x3fd48fcd339e54eaull, 0x3fca4724a57772feull},
        {183u, 0x3fe0894798b708fbull, 0x3fd674a2e394d8d6ull},
        {240u, 0x3fed49c7b5b42538ull, 0x3fea4301ee0e1fb9ull},
        {251u, 0x3fef3048316a5696ull, 0x3fee180cef6ec44bull},
    };
    for (const Pin& pin : kPins)
    {
        const float64 coordinate = EqualAngleSampleCoordinate(pin.Index, 257u);
        EXPECT_EQ(std::bit_cast<uint64>(EqualAngleFaceCoordinate(coordinate)), pin.EqualAngleBits) << pin.Index;
        EXPECT_EQ(std::bit_cast<uint64>(GnomonicFaceCoordinate(coordinate)), pin.GnomonicBits) << pin.Index;
    }
}

TEST(PageStoreFormatTest, CubeFaceFramesRunFromTheirCornersAndEveryEdgePairsTwoFaces)
{
    for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
    {
        // The face's corners 0, 1 and 3: sample (0, 0), the end of X, the end of Z.
        const Mathematics::CubeFaceAxes& axes = Mathematics::kCubeFaceAxes[face];
        EXPECT_EQ(FaceCubePoint(axes, -1, -1), Mathematics::CubeCorner(Mathematics::kCubeFaces[face * 4u + 0u]));
        EXPECT_EQ(FaceCubePoint(axes, 1, -1), Mathematics::CubeCorner(Mathematics::kCubeFaces[face * 4u + 1u]));
        EXPECT_EQ(FaceCubePoint(axes, -1, 1), Mathematics::CubeCorner(Mathematics::kCubeFaces[face * 4u + 3u]));
        for (uint32 side = 0; side < kPageFaceSides; ++side)
        {
            const PageFaceEdge edge = PageFaceNeighbor(face, static_cast<PageFaceSide>(side));
            EXPECT_NE(edge.Face, face);
            const PageFaceEdge back = PageFaceNeighbor(edge.Face, edge.Side);
            EXPECT_EQ(back.Face, face);
            EXPECT_EQ(back.Side, static_cast<PageFaceSide>(side));
            EXPECT_EQ(back.Reversed, edge.Reversed);
        }
    }
}

TEST_F(PageStoreTest, APlanetSourceGetsEachLatticePointOnceWithItsDirection)
{
    PlanetCookRequest request = SmallPlanetRequest(m_Dir / "planet.gepage");
    std::atomic<uint64> calls{0};
    std::atomic<uint64> mismatches{0};
    request.Height = [&calls, &mismatches, &request](uint8 face, uint32 x, uint32 z, const Mathematics::Vector3& direction) {
        const Mathematics::Vector3 expected = LatticeDirection(face, x, z, request.FaceSamples);
        if (Mathematics::Vector3::Dot(direction - expected, direction - expected) > 1e-12f)
            mismatches.fetch_add(1);
        calls.fetch_add(1);
        return DefaultPlanetHeight(face, x, z, direction);
    };
    ASSERT_EQ(CookPlanetPageStore(request).Error, "");
    EXPECT_EQ(mismatches.load(), 0u);
    EXPECT_EQ(calls.load(), uint64{kPageStoreMaxFaces} * request.FaceSamples * request.FaceSamples);
}

TEST_F(PageStoreTest, APlanetCookWithoutAHeightFunctionIsRefusedAndWritesNothing)
{
    PlanetCookRequest request = SmallPlanetRequest(m_Dir / "planet.gepage");
    request.Height = nullptr;
    EXPECT_EQ(CookPlanetPageStore(request).Error, "the planet cook has no height function: set PlanetCookRequest::Height");
    EXPECT_TRUE(std::filesystem::is_empty(m_Dir));
}

TEST_F(PageStoreTest, APlanetStoreHasSixFacesWhoseEdgesAndApronsAreTheNeighborsBitForBitAtEveryLevel)
{
    const PlanetCookRequest request = SmallPlanetRequest(m_Dir / "planet.gepage");
    const HeightCookResult result = CookPlanetPageStore(request);
    ASSERT_EQ(result.Error, "");
    // The level-0 file the cook reads back is gone with the cook: the store is all it leaves.
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(m_Dir), std::filesystem::directory_iterator()), 1);
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(request.Output, PageFieldKind::Height), "");
    const PageStoreLayout& layout = reader.Layout();
    EXPECT_EQ(layout.Header.FaceCount, kPageStoreMaxFaces);
    EXPECT_EQ(layout.Header.FaceMap, PageFaceMap::EqualAngleCube);
    EXPECT_EQ(layout.Header.Units, PageHeightUnits::Absolute);
    EXPECT_EQ(layout.Header.ReferenceRadius, kPlanetRadius);
    ASSERT_EQ(layout.Levels.size(), 3u);
    EXPECT_EQ(layout.Entries.size(), 6u * (9u + 4u + 1u));
    EXPECT_EQ(result.PagesWritten, layout.Entries.size());

    for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
        for (uint8 level = 0; level < layout.Levels.size(); ++level)
            for (uint32 z = 0; z < layout.Levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < layout.Levels[level].PagesX; ++x)
                {
                    const PageAddress address{face, level, x, z};
                    EXPECT_NE(layout.FindPresent(address), nullptr);
                    EXPECT_EQ(layout.ResolvePresent(address), std::optional<PageAddress>(address));
                }

    // Level 0 is the relief at each lattice point's direction, within half a step.
    const float32 expected = DefaultPlanetHeight(4, 200, 37, LatticeDirection(4, 200, 37, request.FaceSamples));
    FaceSamples plusZ(reader, 4, 0);
    EXPECT_NEAR(plusZ.At(200, 37), expected, 0.5f * layout.Header.StepPerLevel[0] + 1e-4f);

    // Every edge from both of its faces, at every level: the edge sample is one value, and the apron
    // past it is the neighbor's sample one in; past a corner the apron repeats the corner sample.
    for (uint8 level = 0; level < layout.Levels.size(); ++level)
    {
        const int64 last = layout.Levels[level].SamplesX - 1;
        for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
        {
            FaceSamples own(reader, face, level);
            for (uint32 side = 0; side < kPageFaceSides; ++side)
            {
                const PageFaceEdge edge = PageFaceNeighbor(face, static_cast<PageFaceSide>(side));
                FaceSamples neighbor(reader, edge.Face, level);
                for (int64 t = 0; t <= last; ++t)
                {
                    const int64 mapped = edge.Reversed ? last - t : t;
                    const PageFaceSide ownSide = static_cast<PageFaceSide>(side);
                    ASSERT_EQ(std::bit_cast<uint32>(own.FromSide(ownSide, t, 0)),
                              std::bit_cast<uint32>(neighbor.FromSide(edge.Side, mapped, 0)))
                        << "edge, face " << int(face) << " side " << side << " level " << int(level) << " at " << t;
                    ASSERT_EQ(std::bit_cast<uint32>(own.FromSide(ownSide, t, -1)),
                              std::bit_cast<uint32>(neighbor.FromSide(edge.Side, mapped, 1)))
                        << "apron, face " << int(face) << " side " << side << " level " << int(level) << " at " << t;
                }
            }
            for (const auto [x, z] : {std::pair<int64, int64>{0, 0}, {0, last}, {last, 0}, {last, last}})
                EXPECT_EQ(own.At(x == 0 ? -1 : last + 1, z == 0 ? -1 : last + 1), own.At(x, z));
        }
    }
}

TEST_F(PageStoreTest, APlanetStoresPagesLandOnTheirOwnFaceInTheResidencyAndTheCpuDirectory)
{
    const PlanetCookRequest request = SmallPlanetRequest(m_Dir / "planet.gepage");
    ASSERT_EQ(CookPlanetPageStore(request).Error, "");
    auto reader = std::make_shared<PageStoreReader>();
    ASSERT_EQ(reader->Open(request.Output, PageFieldKind::Height), "");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetIOService io;
    io.Start(pool, 1);
    PageResidencyManager residency(&pool, &io, PageResidencyBudget{});
    residency.ConfigureField(1, 128, 2, 0.4f);
    residency.Configure(1, 1, PageSource{reader, nullptr});

    // Every page of faces 3 and 5; face 0, whose pages share their levels and positions, is not wanted.
    std::vector<PageWant> wanted;
    const std::vector<PageStoreLevel>& levels = reader->Layout().Levels;
    for (const uint8 face : {uint8{3}, uint8{5}})
        for (uint32 level = 0; level < levels.size(); ++level)
            for (uint32 z = 0; z < levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < levels[level].PagesX; ++x)
                    wanted.push_back({PageAddress{face, uint8(level), x, z}, float32(x + z), false});
    residency.Request(1, wanted);
    RunFramesUntil(residency, 0, [&] { return residency.FieldCache(1).ResidentCount() == wanted.size(); });
    ASSERT_EQ(residency.FieldCache(1).ResidentCount(), wanted.size());

    const CpuPageDirectory directory = residency.CpuPages(1);
    for (const PageWant& want : wanted)
    {
        const PageAddress& address = want.Address;
        const std::vector<float32> stored = PageSamples(*reader, address);
        const std::vector<uint8>* bytes = residency.Bytes(1, address);
        ASSERT_NE(bytes, nullptr);
        EXPECT_EQ(Heights(*bytes), stored) << int(address.Face) << " " << int(address.Level) << " " << address.X << "," << address.Z;
        std::span<const uint8> cpu;
        ASSERT_EQ(directory.Page(address, cpu), CpuPageState::Resident);
        EXPECT_EQ(Heights(cpu), stored);
        const PageAddress faceZero{0, address.Level, address.X, address.Z};
        EXPECT_NE(PageSamples(*reader, faceZero), stored);
        EXPECT_EQ(directory.Page(faceZero, cpu), CpuPageState::Missing);
    }
    io.Stop();
    pool.Shutdown();
}
