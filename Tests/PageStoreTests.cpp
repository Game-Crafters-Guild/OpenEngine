// PageStoreTests: the terrain page store (Engine/Modules/PageStreaming) as pure oracles. The store
// round trip, the apron's bit-equality under the per-level step (a 32-bit page beside a 16-bit one
// included), the present-page index against the dense lookup, the sparse resolve, the incremental
// re-cook, the lattice-preserving tent, the generated provider's level 0 against the terrain's own
// tile fill, the cook key, the step rule, the terrain container, the PNG limit, the
// asynchronous loader, and the surface kinds' codecs and filters. HeightPageCodec.h,
// HeightStepPlanner.h, PagePyramidBuilder.h and PageStoreSerialization.h are the module's private
// headers: the height encoding, the step rule, the builder the surface stores are made with, and
// the byte layout a refusal test patches.

#include "PageStreaming/CpuPage/CpuPageDirectory.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "TerrainECS/TerrainHeightFunction/ITerrainHeightFunction.h"
#include "TerrainECS/TerrainHeightFunction/RevocableTerrainHeightFunction.h"
#include "PageStreaming/Height/HeightLayerStore.h"
#include "PageStreaming/Height/HeightPageCooker.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/Height/HeightStoreCache.h"
#include "PageStreaming/Height/HeightStoreKey.h"
#include "PageStreaming/PageLoader.h"
#include "PageStreaming/PageResidency.h"
#include "PageStreaming/PageStore/PageStoreAccess.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "PageStreaming/PageStore/PageStorePatcher.h"
#include "PageStreaming/PageStore/PageStoreReader.h"
#include "PageStreaming/PageStore/PageStoreWriter.h"
#include "PageStreaming/PageTableEntry.h"
#include "PageStreaming/TerrainPageContainer.h"

#include "Height/HeightLayerAuthoredPages.h"
#include "Height/HeightPageCodec.h"
#include "Height/HeightStepPlanner.h"
#include "PageCodec.h"
#include "PagePyramidBuilder.h"
#include "PageStore/PageStoreSerialization.h"

#include "Assets/AssetIOService.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTPlanetShading.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/CubeFaces.h"
#include "Mathematics/VectorOps.h"
#include "Memory/AllocationCountScope.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/Planet/PlanetSculptDab.h"
#include "TerrainECS/TerrainService.h"
#include "PageResidency/UploadedPages.h"
#include "TestTempDir.h"
#include "Types/ByteBuffer.h"
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
#include <memory>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::PageStreaming;
using GameEngine::PageStreaming::Test::UploadedPages;

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

// A triangle wave of `period` samples (even) in [-period / 2, period / 2].
int64 TriangleWave(uint32 sample, uint32 period)
{
    const int64 half = period / 2u;
    return 2 * std::abs(static_cast<int64>(sample % period) - half) - half;
}

// A continuous relief in [100, 400] m: a 300 m range, so the store is 16-bit. The height is integer
// millimeters with one float divide, so the bytes are the same with every compiler and C library.
float32 Relief(uint32 x, uint32 z)
{
    const int64 millimeters = 250000 + 120000 * TriangleWave(x, 200u) * TriangleWave(z, 368u) / (100 * 184) +
                              30000 * TriangleWave(x + 3u * z, 56u) / 28;
    return static_cast<float32>(millimeters) / 1000.0f;
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
std::vector<uint8> Bc5Block(const ByteBuffer& bytes, uint32 bx, uint32 by)
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

TEST(PageStoreFormatTest, EntryBitPackKeepsSlotAndLevelApartFromTheSentinel)
{
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

TEST(PageStoreFormatTest, AddedPresentPagesGiveTheLayoutOfTheWholePresentSetAndKeepTheHeldEntries)
{
    // A store that holds a third of its pages grows by a list out of index order that names some
    // pages twice and some it holds. The result is the layout of the whole present set.
    PageStoreHeader header;
    header.SamplesX = 3000u;
    header.SamplesZ = 1700u;
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
    std::vector<uint8> held(static_cast<std::size_t>(DensePageCount(levels)));
    std::vector<uint8> whole(held.size());
    std::vector<PageAddress> added;
    for (uint8 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
            {
                const std::size_t i = static_cast<std::size_t>(levels[level].FirstEntry + uint64{z} * levels[level].PagesX + x);
                held[i] = static_cast<uint8>(((i * 2654435761u) >> 7u) % 3u == 0u);
                const bool add = ((i * 40503u) >> 5u) % 4u == 0u;
                whole[i] = static_cast<uint8>(held[i] != 0 || add);
                if (add)
                    added.push_back(PageAddress{0, level, x, z});
            }
    std::ranges::reverse(added);
    added.push_back(added.front());
    PageStoreLayout layout = MakePageStoreLayout(header, held);
    for (std::size_t i = 0; i < layout.Entries.size(); ++i)
    {
        layout.Entries[i].Offset = 4096u + i;
        layout.Entries[i].Hash = i * 7u + 1u;
    }

    const std::optional<PageStoreLayout> grown = AddPresentPages(layout, added);
    ASSERT_TRUE(grown);
    const PageStoreLayout expected = MakePageStoreLayout(header, whole);
    ASSERT_EQ(grown->Entries.size(), expected.Entries.size());
    ASSERT_EQ(grown->Ranges.size(), expected.Ranges.size());
    for (std::size_t r = 0; r < expected.Ranges.size(); ++r)
    {
        EXPECT_EQ(grown->Ranges[r].First, expected.Ranges[r].First) << r;
        EXPECT_EQ(grown->Ranges[r].Count, expected.Ranges[r].Count) << r;
    }
    for (uint8 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
            {
                const PageAddress address{0, level, x, z};
                const PageIndexEntry* entry = grown->FindPresent(address);
                ASSERT_EQ(entry != nullptr, expected.FindPresent(address) != nullptr) << int(level) << " " << x << "," << z;
                if (const PageIndexEntry* before = layout.FindPresent(address))
                {
                    EXPECT_EQ(entry->Offset, before->Offset);
                    EXPECT_EQ(entry->Hash, before->Hash);
                }
                else if (entry)
                    EXPECT_EQ(entry->Offset, 0u);
            }

    const PageAddress outside{0, 0, levels[0].PagesX, 0};
    EXPECT_FALSE(AddPresentPages(layout, std::span<const PageAddress>(&outside, 1u)));
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
    ByteBuffer out;
    EXPECT_FALSE(reader.ReadPageBytes(PageAddress{0, 0, 1, 1}, out));
    EXPECT_EQ(reader.Layout().ResolvePresent(PageAddress{0, 0, 1, 1}), (PageAddress{0, 2, 0, 0}));
    // Absent pages take no bytes: 9 + 4 + 1 pages less the two absent ones.
    EXPECT_EQ(reader.Layout().TotalBytes(),
              reader.Layout().DataOffset() + 12u * PageStoredBytes(PageFieldFormat::HeightSteps, false));
}

TEST_F(PageStoreTest, APageReadHoldsThePageBytesOfTheFileAndAFailedReadHoldsNone)
{
    PageStoreHeader header;
    header.SamplesX = 300;
    header.SamplesZ = 300;
    header.StepPerLevel.fill(1.0f);
    const PageStoreLayout layout = MakePageStoreLayout(header);
    const std::filesystem::path file = m_Dir / "pages.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(file, layout), "");
    std::vector<float32> samples(kPageSampleCount);
    EncodedPage page;
    uint32 pageIndex = 0;
    for (uint8 level = 0; level < layout.Levels.size(); ++level)
        for (uint32 pz = 0; pz < layout.Levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < layout.Levels[level].PagesX; ++px, ++pageIndex)
            {
                for (std::size_t i = 0; i < samples.size(); ++i)
                    samples[i] = static_cast<float32>((i * 7u + pageIndex) % 50u);
                EncodeHeightPage(header, level, samples, 0.0f, 49.0f, page);
                if (pageIndex == 0)
                    continue;
                ASSERT_TRUE(writer.WritePage(PageAddress{0, level, px, pz}, page));
            }
    ByteBuffer written;
    PageIndexEntry writtenEntry;
    ASSERT_TRUE(writer.ReadPage(PageAddress{0, 0, 1, 0}, written, writtenEntry));
    EXPECT_FALSE(writer.ReadPage(PageAddress{0, 0, 0, 0}, written, writtenEntry));
    EXPECT_EQ(written.Size(), 0u) << "a page not written yet hands over no bytes";
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float32>((i * 7u) % 50u);
    EncodeHeightPage(header, 0, samples, 0.0f, 49.0f, page);
    ASSERT_TRUE(writer.WritePage(PageAddress{0, 0, 0, 0}, page));
    ASSERT_EQ(writer.Finish(), "");
    EXPECT_FALSE(writer.ReadPage(PageAddress{0, 0, 1, 0}, written, writtenEntry));
    EXPECT_EQ(written.Size(), 0u) << "a read from the closed file hands over no bytes";

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file, PageFieldKind::Height), "");
    const std::vector<uint8> fileBytes = FileBytes(file);
    PageAddress last{};
    uint64 lastOffset = 0;
    ByteBuffer bytes;
    for (uint8 level = 0; level < layout.Levels.size(); ++level)
        for (uint32 pz = 0; pz < layout.Levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < layout.Levels[level].PagesX; ++px)
            {
                const PageAddress address{0, level, px, pz};
                const PageIndexEntry& entry = *reader.Layout().FindPresent(address);
                const uint64 offset = reader.BaseOffset() + entry.Offset;
                ASSERT_TRUE(reader.ReadPageBytes(address, bytes));
                ASSERT_EQ(bytes.Size(), reader.Layout().StoredBytes(entry));
                EXPECT_TRUE(std::ranges::equal(bytes, std::span(fileBytes).subspan(offset, bytes.Size())))
                    << "page (" << int(level) << ", " << px << ", " << pz << ")";
                if (offset >= lastOffset)
                {
                    last = address;
                    lastOffset = offset;
                }
            }

    // The file now ends half way into its last page.
    ASSERT_TRUE(reader.ReadPageBytes(last, bytes));
    std::filesystem::resize_file(file, lastOffset + bytes.Size() / 2u);
    EXPECT_FALSE(reader.ReadPageBytes(last, bytes));
    EXPECT_EQ(bytes.Size(), 0u) << "a short read hands over no bytes";
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
    // The relief is integer-exact, so the hash is the same with every compiler and C library.
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
    EXPECT_EQ(Hashing::Fnv1a64(bytes.data(), bytes.size()), 0x5f1ed4357f5551ffull);
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

// Writes `dir`/`name`.gepage (Bc5, `header`) and `name`-samples.gepage (Rg8) over `present`. Each
// coarser page is written from samples of its own, as a direct bake does. Then each landing writes its
// level-0 pages from `normals` and refilters the ancestors of their owned samples.
void WriteBc5Stores(const std::filesystem::path& dir, const std::string& name, const PageStoreHeader& header,
                    const std::vector<uint8>& present, const std::vector<uint8>& normals,
                    std::span<const std::vector<PageAddress>> landings)
{
    PageStoreHeader samplesHeader = header;
    samplesHeader.Format = PageFieldFormat::Rg8;
    const PageStoreLayout layout = MakePageStoreLayout(header, present);
    const uint32 stride = PageStrideTexels(header.Format);
    PageStoreWriter writer;
    PageStoreWriter unencoded;
    ASSERT_EQ(writer.Create(dir / (name + ".gepage"), layout), "");
    ASSERT_EQ(unencoded.Create(dir / (name + "-samples.gepage"), MakePageStoreLayout(samplesHeader, present)), "");
    PageStorePatcher patcher(writer, unencoded, nullptr);
    for (uint8 level = 1; level < layout.Levels.size(); ++level)
        for (uint32 pz = 0; pz < layout.Levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < layout.Levels[level].PagesX; ++px)
            {
                std::vector<uint8> direct(static_cast<std::size_t>(stride) * stride * 2u);
                for (std::size_t i = 0; i < direct.size(); ++i)
                    direct[i] = static_cast<uint8>(
                        32u + ((static_cast<uint32>(i + 7919u * (level * 16u + pz * 4u + px)) * 2246822519u) >> 25));
                ASSERT_TRUE(patcher.WritePage(PageAddress{0, level, px, pz}, direct));
            }
    for (const std::vector<PageAddress>& pages : landings)
    {
        PageSampleRect owned{header.SamplesX, header.SamplesZ, 0u, 0u};
        for (const PageAddress& address : pages)
        {
            ASSERT_TRUE(patcher.WritePage(
                address, GridPageSamples(normals, header.SamplesX, header.SamplesZ, header.Format, address)));
            owned.MinX = std::min(owned.MinX, address.X * kPageOwnedSamples);
            owned.MinZ = std::min(owned.MinZ, address.Z * kPageOwnedSamples);
            owned.MaxX = std::max(owned.MaxX, std::min((address.X + 1u) * kPageOwnedSamples, header.SamplesX) - 1u);
            owned.MaxZ = std::max(owned.MaxZ, std::min((address.Z + 1u) * kPageOwnedSamples, header.SamplesZ) - 1u);
        }
        ASSERT_TRUE(patcher.RefilterAncestors(owned));
    }
    ASSERT_EQ(writer.Finish(), "");
    ASSERT_EQ(unencoded.Finish(), "");
}

// The first and last finer pages that the taps of coarser sample `sample` read, on one axis.
std::pair<uint32, uint32> TapPageSpan(int64 sample, uint32 finerSamples)
{
    return {ClampSample(2 * sample - 1, finerSamples) / kPageOwnedSamples,
            ClampSample(2 * sample + 1, finerSamples) / kPageOwnedSamples};
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
    // flat weight), then the level-0 pages land in any order. A coarser sample becomes the filter
    // when every page its taps read has landed; once the last one lands, the store equals the full build.
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

    // The first level-0 page alone changes one coarser page at each level: the one over it, whose
    // samples with every tap in that page become the filter.
    const uint64 coarsePages = patcher.PagesWritten();
    ASSERT_TRUE(patcher.WritePage(PageAddress{0, 0, 0, 0}, SurfacePage(expected, PageAddress{0, 0, 0, 0})));
    ASSERT_TRUE(patcher.RefilterAncestors(PageSampleRect{0, 0, 127, 127}));
    EXPECT_EQ(patcher.PagesWritten(), coarsePages + levels.size());

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

TEST_F(PageStoreTest, ACoarsePageOverHalfItsFinerPagesFiltersTheSamplesWhoseTapsTheyHoldAndKeepsTheRest)
{
    // 600 x 600 weights: 5 x 5 pages at level 0, 3 x 3 at level 1. Every coarser page is written
    // flat, then the level-0 columns 0 and 1 land. A level-1 sample whose taps lie in those columns
    // is the full build's sample; any other sample keeps the flat value.
    constexpr uint32 kSide = 600u;
    constexpr uint32 kLandedColumns = 2u;
    const std::filesystem::path built = m_Dir / "built.gepage";
    BuildSurfaceStore(built, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kSide, kSide,
                      SplatWeights(kSide, kSide));
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(built, PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    std::vector<uint8> present(static_cast<std::size_t>(DensePageCount(levels)), 1u);
    for (uint32 pz = 0; pz < levels[0].PagesZ; ++pz)
        for (uint32 px = kLandedColumns; px < levels[0].PagesX; ++px)
            present[static_cast<std::size_t>(pz) * levels[0].PagesX + px] = 0u;

    const std::filesystem::path baked = m_Dir / "baked.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(baked, MakePageStoreLayout(expected.Layout().Header, present)), "");
    PageStorePatcher patcher(writer, nullptr);
    std::vector<uint8> flat(static_cast<std::size_t>(kPageSampleCount) * 4u, 0u);
    for (std::size_t t = 0; t < flat.size(); t += 4u)
        flat[t] = 255u;
    for (uint8 level = 1; level < levels.size(); ++level)
        for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < levels[level].PagesX; ++px)
                ASSERT_TRUE(patcher.WritePage(PageAddress{0, level, px, pz}, flat));
    for (uint32 pz = 0; pz < levels[0].PagesZ; ++pz)
        for (uint32 px = 0; px < kLandedColumns; ++px)
            ASSERT_TRUE(patcher.WritePage(PageAddress{0, 0, px, pz}, SurfacePage(expected, PageAddress{0, 0, px, pz})));
    ASSERT_TRUE(patcher.RefilterAncestors(PageSampleRect{0, 0, kLandedColumns * kPageOwnedSamples - 1u, kSide - 1u}));
    ASSERT_EQ(writer.Finish(), "");

    PageStoreReader actual;
    ASSERT_EQ(actual.Open(baked, PageFieldKind::Splat), "");
    uint32 filtered = 0;
    uint32 kept = 0;
    for (uint32 pz = 0; pz < levels[1].PagesZ; ++pz)
        for (uint32 px = 0; px < levels[1].PagesX; ++px)
        {
            const std::vector<uint8> page = SurfacePage(actual, PageAddress{0, 1, px, pz});
            const std::vector<uint8> full = SurfacePage(expected, PageAddress{0, 1, px, pz});
            for (uint32 c = 0; c < kPageStrideSamples; ++c)
            {
                const int64 sampleX = std::clamp<int64>(int64{px} * kPageOwnedSamples - 1 + c, 0, levels[1].SamplesX - 1);
                const int64 lastTap = std::min<int64>(2 * sampleX + 1, levels[0].SamplesX - 1);
                const bool tapsLanded = lastTap / kPageOwnedSamples < kLandedColumns;
                for (uint32 r = 0; r < kPageStrideSamples; ++r)
                {
                    const std::size_t at = (static_cast<std::size_t>(r) * kPageStrideSamples + c) * 4u;
                    const auto texel = std::span(page).subspan(at, 4u);
                    ASSERT_TRUE(std::ranges::equal(texel, std::span(tapsLanded ? full : flat).subspan(at, 4u)))
                        << "level-1 page " << px << "," << pz << " sample " << c << "," << r
                        << (tapsLanded ? " is not the filter" : " is not the stored sample");
                }
                (tapsLanded ? filtered : kept) += 1u;
            }
        }
    EXPECT_GT(filtered, 0u);
    EXPECT_GT(kept, 0u);
}

TEST_F(PageStoreTest, ABc5StoreHoldsTheSameBytesWhicheverOrderTwoFinerPagesUnderOneCoarsePageLand)
{
    // 600 x 600 normals: each coarser page is written direct, then level-0 pages (1,2) and (1,1) land
    // in each order. Both orders give the same bytes. A level-1 block with no sample whose taps all
    // lie on the two pages keeps the direct bytes.
    constexpr uint32 kSide = 600u;
    PageStoreHeader header;
    header.Format = PageFieldFormat::Bc5;
    header.Filter = PageFieldFilter::NormalTent;
    header.SamplesX = kSide;
    header.SamplesZ = kSide;
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(kSide, kSide);
    std::vector<uint8> coarserOnly(static_cast<std::size_t>(DensePageCount(levels)), 1u);
    std::fill_n(coarserOnly.begin(), static_cast<std::size_t>(levels[0].PagesX) * levels[0].PagesZ, uint8{0});
    const PageAddress lower{0, 0, 1, 1};
    const PageAddress upper{0, 0, 1, 2};
    std::vector<uint8> present = coarserOnly;
    present[static_cast<std::size_t>(lower.Z) * levels[0].PagesX + lower.X] = 1u;
    present[static_cast<std::size_t>(upper.Z) * levels[0].PagesX + upper.X] = 1u;
    std::vector<uint8> normals(static_cast<std::size_t>(kSide) * kSide * 2u);
    for (std::size_t i = 0; i < normals.size(); ++i)
        normals[i] = static_cast<uint8>(64u + ((static_cast<uint32>(i) * 2654435761u) >> 25));
    const std::vector<std::vector<PageAddress>> upperFirst{{upper}, {lower}};
    const std::vector<std::vector<PageAddress>> lowerFirst{{lower}, {upper}};
    WriteBc5Stores(m_Dir, "direct", header, coarserOnly, normals, {});
    WriteBc5Stores(m_Dir, "upper-first", header, present, normals, upperFirst);
    WriteBc5Stores(m_Dir, "lower-first", header, present, normals, lowerFirst);
    for (const bool samples : {false, true})
    {
        const std::string suffix = samples ? "-samples.gepage" : ".gepage";
        const PageFieldKind kind = samples ? PageFieldKind::NormalSamples : PageFieldKind::Normal;
        PageStoreReader first;
        PageStoreReader second;
        ASSERT_EQ(first.Open(m_Dir / ("upper-first" + suffix), kind), "");
        ASSERT_EQ(second.Open(m_Dir / ("lower-first" + suffix), kind), "");
        ASSERT_EQ(first.Layout().Entries.size(), second.Layout().Entries.size());
        for (uint8 level = 0; level < levels.size(); ++level)
            for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
                for (uint32 px = 0; px < levels[level].PagesX; ++px)
                {
                    const PageAddress address{0, level, px, pz};
                    if (!first.Layout().FindPresent(address))
                        continue;
                    ByteBuffer firstBytes;
                    ByteBuffer secondBytes;
                    ASSERT_TRUE(first.ReadPageBytes(address, firstBytes));
                    ASSERT_TRUE(second.ReadPageBytes(address, secondBytes));
                    EXPECT_TRUE(std::ranges::equal(firstBytes, secondBytes)) << suffix << " level " << int(level) << " page " << px << "," << pz;
                }
    }

    PageStoreReader direct;
    PageStoreReader landed;
    ASSERT_EQ(direct.Open(m_Dir / "direct.gepage", PageFieldKind::Normal), "");
    ASSERT_EQ(landed.Open(m_Dir / "upper-first.gepage", PageFieldKind::Normal), "");
    const uint32 stride = PageStrideTexels(header.Format);
    const uint32 apron = (stride - kPageOwnedSamples) / 2u;
    const uint32 blocks = stride / kPageBlockTexels;
    uint32 keptBlocksOverLanded = 0;
    uint32 changedBlocks = 0;
    for (uint32 pz = 0; pz < levels[1].PagesZ; ++pz)
        for (uint32 px = 0; px < levels[1].PagesX; ++px)
        {
            const PageAddress address{0, 1, px, pz};
            ByteBuffer directBytes;
            ByteBuffer landedBytes;
            ASSERT_TRUE(direct.ReadPageBytes(address, directBytes));
            ASSERT_TRUE(landed.ReadPageBytes(address, landedBytes));
            for (uint32 by = 0; by < blocks; ++by)
                for (uint32 bx = 0; bx < blocks; ++bx)
                {
                    // A sample is the filter when its column and its row taps lie on the landed pages.
                    bool columnOn = false;
                    bool columnReads = false;
                    bool rowOn = false;
                    bool rowReads = false;
                    for (uint32 texel = 0; texel < kPageBlockTexels; ++texel)
                    {
                        const auto [firstX, lastX] = TapPageSpan(
                            ClampSample(int64{px} * kPageOwnedSamples - apron + bx * kPageBlockTexels + texel, levels[1].SamplesX),
                            levels[0].SamplesX);
                        const auto [firstZ, lastZ] = TapPageSpan(
                            ClampSample(int64{pz} * kPageOwnedSamples - apron + by * kPageBlockTexels + texel, levels[1].SamplesZ),
                            levels[0].SamplesZ);
                        columnOn = columnOn || (firstX >= lower.X && lastX <= lower.X);
                        columnReads = columnReads || (firstX <= lower.X && lastX >= lower.X);
                        rowOn = rowOn || (firstZ >= lower.Z && lastZ <= upper.Z);
                        rowReads = rowReads || (firstZ <= upper.Z && lastZ >= lower.Z);
                    }
                    const std::size_t at = (static_cast<std::size_t>(by) * blocks + bx) * kBc5BlockBytes;
                    const bool same = std::ranges::equal(std::span(directBytes).subspan(at, kBc5BlockBytes),
                                                         std::span(landedBytes).subspan(at, kBc5BlockBytes));
                    const bool kept = !(columnOn && rowOn);
                    EXPECT_TRUE(!kept || same) << "level-1 page " << px << "," << pz << " block " << bx << "," << by;
                    keptBlocksOverLanded += kept && columnReads && rowReads ? 1u : 0u;
                    changedBlocks += same ? 0u : 1u;
                }
        }
    EXPECT_GT(keptBlocksOverLanded, 0u);
    EXPECT_GT(changedBlocks, 0u);
}

TEST_F(PageStoreTest, ABc5CoarsePageKeepsTheBytesOfEveryBlockWhoseSamplesNoLandedPageFilters)
{
    // 600 x 600 normals: each coarser page is written from its own samples, then the level-0
    // columns 0 and 1 land. A level-1 block with no sample over those columns keeps the direct
    // page's bytes. In the outer block ring such samples come from the Bc5 decode.
    constexpr uint32 kSide = 600u;
    constexpr uint32 kLandedColumns = 2u;
    PageStoreHeader header;
    header.Format = PageFieldFormat::Bc5;
    header.Filter = PageFieldFilter::NormalTent;
    header.SamplesX = kSide;
    header.SamplesZ = kSide;
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(kSide, kSide);
    std::vector<uint8> present(static_cast<std::size_t>(DensePageCount(levels)), 1u);
    for (uint32 pz = 0; pz < levels[0].PagesZ; ++pz)
        for (uint32 px = kLandedColumns; px < levels[0].PagesX; ++px)
            present[static_cast<std::size_t>(pz) * levels[0].PagesX + px] = 0u;
    std::vector<uint8> coarserOnly = present;
    std::fill_n(coarserOnly.begin(), static_cast<std::size_t>(levels[0].PagesX) * levels[0].PagesZ, uint8{0});
    const uint32 stride = PageStrideTexels(header.Format);
    const uint32 apron = (stride - kPageOwnedSamples) / 2u;
    std::vector<uint8> normals(static_cast<std::size_t>(kSide) * kSide * 2u);
    for (std::size_t i = 0; i < normals.size(); ++i)
        normals[i] = static_cast<uint8>(64u + ((static_cast<uint32>(i) * 2654435761u) >> 25));

    std::vector<PageAddress> columns;
    for (uint32 pz = 0; pz < levels[0].PagesZ; ++pz)
        for (uint32 px = 0; px < kLandedColumns; ++px)
            columns.push_back(PageAddress{0, 0, px, pz});
    const std::vector<std::vector<PageAddress>> landing{columns};
    WriteBc5Stores(m_Dir, "direct", header, coarserOnly, normals, {});
    WriteBc5Stores(m_Dir, "landed", header, present, normals, landing);

    PageStoreReader direct;
    PageStoreReader landed;
    ASSERT_EQ(direct.Open(m_Dir / "direct.gepage", PageFieldKind::Normal), "");
    ASSERT_EQ(landed.Open(m_Dir / "landed.gepage", PageFieldKind::Normal), "");
    const uint32 blocks = stride / kPageBlockTexels;
    uint32 keptRingBlocks = 0;
    uint32 changedBlocks = 0;
    for (uint32 pz = 0; pz < levels[1].PagesZ; ++pz)
        for (uint32 px = 0; px < levels[1].PagesX; ++px)
        {
            const PageAddress address{0, 1, px, pz};
            ByteBuffer directBytes;
            ByteBuffer landedBytes;
            ASSERT_TRUE(direct.ReadPageBytes(address, directBytes));
            ASSERT_TRUE(landed.ReadPageBytes(address, landedBytes));
            for (uint32 bx = 0; bx < blocks; ++bx)
            {
                bool kept = true;
                for (uint32 c = bx * kPageBlockTexels; c < (bx + 1u) * kPageBlockTexels; ++c)
                {
                    const int64 sampleX = std::clamp<int64>(int64{px} * kPageOwnedSamples - apron + c, 0, levels[1].SamplesX - 1);
                    kept = kept && std::min<int64>(2 * sampleX + 1, levels[0].SamplesX - 1) / kPageOwnedSamples >= kLandedColumns;
                }
                for (uint32 by = 0; by < blocks; ++by)
                {
                    const std::size_t at = (static_cast<std::size_t>(by) * blocks + bx) * kBc5BlockBytes;
                    const bool same = std::ranges::equal(std::span(directBytes).subspan(at, kBc5BlockBytes),
                                                         std::span(landedBytes).subspan(at, kBc5BlockBytes));
                    EXPECT_TRUE(!kept || same) << "level-1 page " << px << "," << pz << " block " << bx << "," << by;
                    const bool ring = bx == 0u || by == 0u || bx + 1u == blocks || by + 1u == blocks;
                    keptRingBlocks += kept && ring ? 1u : 0u;
                    changedBlocks += same ? 0u : 1u;
                }
            }
        }
    EXPECT_GT(keptRingBlocks, 0u);
    EXPECT_GT(changedBlocks, 0u);
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

    std::vector<PageAddress> level0;
    for (uint32 pz = 0; pz < levels[0].PagesZ; ++pz)
        for (uint32 px = 0; px < levels[0].PagesX; ++px)
            level0.push_back(PageAddress{0, 0, px, pz});
    std::optional<PageStoreLayout> withLevel0 = AddPresentPages(layout, level0);
    ASSERT_TRUE(withLevel0);
    PageStoreWriter writer;
    ASSERT_EQ(writer.OpenForPatch(grown, grown, *withLevel0), "");
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

TEST_F(PageStoreTest, AStoreHeldOpenGrowsItsPresentPagesBetweenWritesAndItsFinishPublishesEveryPage)
{
    // The coarser levels are written into a store that holds only them, and level 0 is added in two
    // steps to the same open store. No file is at the target until Finish; the reopened store then
    // reads as the full build, the pages its longer index moved included.
    constexpr uint32 kSide = 600u;
    const std::filesystem::path built = m_Dir / "built.gepage";
    BuildSurfaceStore(built, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kSide, kSide, SplatWeights(kSide, kSide));
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(built, PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    const std::size_t level0Pages = static_cast<std::size_t>(levels[0].PagesX) * levels[0].PagesZ;

    const std::filesystem::path held = m_Dir / "held.gepage";
    std::vector<uint8> present(static_cast<std::size_t>(DensePageCount(levels)), 1u);
    std::fill_n(present.begin(), level0Pages, uint8{0});
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(held, MakePageStoreLayout(expected.Layout().Header, present)), "");
    PageStorePatcher patcher(writer, nullptr);
    for (uint8 level = 1; level < levels.size(); ++level)
        for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < levels[level].PagesX; ++px)
                ASSERT_TRUE(patcher.WritePage(PageAddress{0, level, px, pz},
                                              SurfacePage(expected, PageAddress{0, level, px, pz})));

    for (uint32 half = 0; half < 2u; ++half)
    {
        const uint32 firstRow = half * levels[0].PagesZ / 2u;
        const uint32 endRow = (half + 1u) * levels[0].PagesZ / 2u;
        std::vector<PageAddress> rows;
        for (uint32 pz = firstRow; pz < endRow; ++pz)
            for (uint32 px = 0; px < levels[0].PagesX; ++px)
                rows.push_back(PageAddress{0, 0, px, pz});
        ASSERT_EQ(writer.AddPresentPages(rows), "") << "half " << half;
        for (uint32 pz = firstRow; pz < endRow; ++pz)
            for (uint32 px = 0; px < levels[0].PagesX; ++px)
                ASSERT_TRUE(patcher.WritePage(PageAddress{0, 0, px, pz}, SurfacePage(expected, PageAddress{0, 0, px, pz})))
                    << "half " << half << " page (" << px << ", " << pz << ")";
        ByteBuffer stored;
        ByteBuffer wanted;
        PageIndexEntry entry;
        ASSERT_TRUE(writer.ReadPage(PageAddress{0, 1, 0, 0}, stored, entry));
        ASSERT_TRUE(expected.ReadPageBytes(PageAddress{0, 1, 0, 0}, wanted));
        EXPECT_TRUE(std::ranges::equal(stored, wanted)) << "a page the longer index moved lost its bytes, half " << half;
    }
    EXPECT_EQ(writer.Layout().Entries.size(), expected.Layout().Entries.size());
    EXPECT_FALSE(std::filesystem::exists(held)) << "the store is at its target before Finish";
    ASSERT_EQ(writer.Finish(), "");
    PageStoreReader actual;
    ASSERT_EQ(actual.Open(held, PageFieldKind::Splat), "");
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
        ByteBuffer page;
        ByteBuffer right;
        ByteBuffer below;
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
    return lattice;
}

// The standard deviation of the owned samples of level `level`'s pages on the diagonal of `provider`.
double LevelHeightDeviation(const GeneratedHeightPageProvider& provider, uint8 level)
{
    const PageStoreLevel& extent = provider.Levels()[level];
    std::vector<float32> page(kPageSampleCount);
    double sum = 0.0;
    double sumSquares = 0.0;
    uint64 count = 0;
    for (uint32 i = 0; i < std::min(extent.PagesX, extent.PagesZ); ++i)
    {
        EXPECT_TRUE(provider.FillPage(PageAddress{0, level, i, i}, page));
        for (uint32 row = kPageApronSamples; row < kPageApronSamples + kPageOwnedSamples; ++row)
            for (uint32 column = kPageApronSamples; column < kPageApronSamples + kPageOwnedSamples; ++column)
            {
                const double height = page[static_cast<std::size_t>(row) * kPageStrideSamples + column];
                sum += height;
                sumSquares += height * height;
                ++count;
            }
    }
    const double mean = sum / static_cast<double>(count);
    return std::sqrt(std::max(0.0, sumSquares / static_cast<double>(count) - mean * mean));
}

// Every level-0 sample of the provider against the tile fill of the tile that owns it. A Function
// lattice's tile fill reads `function` at `heightBand`.
void ExpectLevelZeroIsTheTileFill(const GeneratedHeightLattice& lattice,
                                  std::shared_ptr<const TerrainECS::RevocableTerrainHeightFunction> function = nullptr,
                                  float32 heightBand = 0.0f)
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
    config.Base.Source = Components::TerrainBaseSource::Flat;
    if (lattice.Source == GeneratedHeightSource::Noise)
        config.Base.Noise = lattice.Noise;
    if (lattice.Source == GeneratedHeightSource::Heightmap)
    {
        config.Base.Source = Components::TerrainBaseSource::HeightmapAsset;
        config.Base.Heightmap = lattice.Heightmap;
        config.WorldSizeX = lattice.AuthoredSizeX;
        config.WorldSizeZ = lattice.AuthoredSizeZ;
    }
    if (lattice.Source == GeneratedHeightSource::Function)
    {
        config.Base.Source = Components::TerrainBaseSource::Function;
        config.Base.HeightFunction = function;
        config.HeightScale = heightBand;
    }
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
                                            static_cast<int32>(interval), static_cast<int32>(interval), 0.0f);
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

namespace
{

// A tilted plane plus a paraboloid in meters: no transcendental, so its value is exact on every
// platform. Counts its row calls and keeps the last band limit it was asked for.
class PlaneParaboloid final : public TerrainECS::ITerrainHeightFunction
{
public:
    static float32 Height(float32 x, float32 z)
    {
        const float32 dx = x - 300.0f;
        const float32 dz = z - 200.0f;
        return 20.0f + 0.05f * x + 0.03f * z + 1.0e-5f * (dx * dx + dz * dz);
    }
    void HeightRow(float32 z, std::span<const float32> x, float32 minResolvedWavelength,
                   std::span<float32> outMeters) const override
    {
        Rows.fetch_add(1u);
        LastWavelength.store(minResolvedWavelength);
        for (std::size_t i = 0; i < x.size(); ++i)
            outMeters[i] = Height(x[i], z);
    }
    TerrainECS::TerrainHeightRange HeightRange(float32, float32, float32, float32, float32) const override
    {
        return {0.0f, 256.0f};
    }

    mutable std::atomic<uint32> Rows{0};
    mutable std::atomic<float32> LastWavelength{-1.0f};
};

constexpr float32 kFunctionBand = 256.0f;

GeneratedHeightLattice FunctionLattice(const std::shared_ptr<const TerrainECS::RevocableTerrainHeightFunction>& function,
                                       float32 originX, float32 originZ, float32 tileWorld, uint32 tilesX, uint32 tilesZ)
{
    GeneratedHeightLattice lattice = NoiseLattice(originX, originZ, tileWorld, tilesX, tilesZ);
    lattice.Source = GeneratedHeightSource::Function;
    lattice.FunctionRow = [function](float32 localZ, std::span<const float32> localX, float32 minResolvedWavelength,
                                     std::span<float32> out) {
        return TerrainECS::FillFunctionBaseRow(*function, {}, localZ, localX, minResolvedWavelength, kFunctionBand,
                                               out) == TerrainECS::FunctionBaseRowFill::Filled;
    };
    return lattice;
}

struct BuiltPlaneParaboloid
{
    const PlaneParaboloid* Function = nullptr;
    std::shared_ptr<TerrainECS::RevocableTerrainHeightFunction> Instance;
};

BuiltPlaneParaboloid BuildPlaneParaboloid()
{
    auto function = std::make_unique<PlaneParaboloid>();
    BuiltPlaneParaboloid built{function.get(), nullptr};
    built.Instance = std::make_shared<TerrainECS::RevocableTerrainHeightFunction>(std::move(function));
    return built;
}

} // namespace

TEST(GeneratedHeightPageProviderTest, FunctionLevelZeroIsTheTileFillAndTheClosedFormOverTheBand)
{
    const BuiltPlaneParaboloid built = BuildPlaneParaboloid();
    ExpectLevelZeroIsTheTileFill(FunctionLattice(built.Instance, 0.0f, 0.0f, 1024.0f, 2, 2), built.Instance,
                                 kFunctionBand);
    ExpectLevelZeroIsTheTileFill(FunctionLattice(built.Instance, 1000.1f, -77.7f, 1024.0f / 3.0f, 3, 1),
                                 built.Instance, kFunctionBand);

    // Terrain-local positions: page (1, 0)'s first owned sample is level-0 sample (128, 0), 128 m
    // from the corner at one sample a meter, wherever the terrain stands.
    const GeneratedHeightPageProvider provider(FunctionLattice(built.Instance, -5000.0f, 7000.0f, 1024.0f, 2, 2));
    std::vector<float32> page(kPageSampleCount);
    ASSERT_TRUE(provider.FillPage(PageAddress{0, 0, 1, 0}, page));
    EXPECT_EQ(page[kPageStrideSamples + 1u], PlaneParaboloid::Height(128.0f, 0.0f) / kFunctionBand);
}

TEST(GeneratedHeightPageProviderTest, FunctionPageCallsOneRowARowWithTheNoiseBandLimit)
{
    const BuiltPlaneParaboloid built = BuildPlaneParaboloid();
    const GeneratedHeightPageProvider provider(FunctionLattice(built.Instance, 0.0f, 0.0f, 1024.0f, 2, 2));
    std::vector<float32> page(kPageSampleCount);
    ASSERT_TRUE(provider.FillPage(PageAddress{0, 0, 0, 0}, page));
    EXPECT_EQ(built.Function->Rows.load(), kPageStrideSamples);
    EXPECT_EQ(built.Function->LastWavelength.load(), 0.0f);

    ASSERT_GE(provider.Levels().size(), 3u);
    ASSERT_TRUE(provider.FillPage(PageAddress{0, 2, 0, 0}, page));
    EXPECT_EQ(built.Function->Rows.load(), 2u * kPageStrideSamples);
    // Two level-2 sample intervals at one sample a meter, as the Noise source drops its octaves.
    EXPECT_EQ(built.Function->LastWavelength.load(), 2.0f * 1.0f * 4.0f);
}

TEST(GeneratedHeightPageProviderTest, ARevokedFunctionFillsNoPage)
{
    const BuiltPlaneParaboloid built = BuildPlaneParaboloid();
    const GeneratedHeightPageProvider provider(FunctionLattice(built.Instance, 0.0f, 0.0f, 1024.0f, 1, 1));
    built.Instance->Revoke();
    std::vector<float32> page(kPageSampleCount);
    EXPECT_FALSE(provider.FillPage(PageAddress{0, 0, 0, 0}, page));
}

// A 576 x 300 m heightmap terrain is 2 x 1 tiles of 300 m: the last 24 m along X lie past the
// heightmap, and both the provider and the tile fill read its last column there. The same terrain
// turned to 300 x 576 m reads its last row along Z.
TEST(GeneratedHeightPageProviderTest, HeightmapLevelZeroIsByteIdenticalToTheTileFillWhereTheGridOverhangs)
{
    auto heightmap = std::make_shared<Terrain::HeightfieldData>(64u, 32u, 0.0f);
    for (uint32 z = 0; z < heightmap->GetHeight(); ++z)
        for (uint32 x = 0; x < heightmap->GetWidth(); ++x)
            heightmap->SetSample(x, z, 0.37f * static_cast<float32>(x) + 1.3f * static_cast<float32>(z) +
                                           std::sin(0.5f * static_cast<float32>(x * z)));

    GeneratedHeightLattice lattice = NoiseLattice(-120.5f, 40.25f, 300.0f, 2, 1);
    lattice.Source = GeneratedHeightSource::Heightmap;
    lattice.Heightmap = heightmap;
    lattice.AuthoredSizeX = 576.0f;
    lattice.AuthoredSizeZ = 300.0f;
    ExpectLevelZeroIsTheTileFill(lattice);

    std::swap(lattice.TilesX, lattice.TilesZ);
    std::swap(lattice.AuthoredSizeX, lattice.AuthoredSizeZ);
    ExpectLevelZeroIsTheTileFill(lattice);
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

// #6523: a 250 km noise terrain's coarse levels, which the far view and the view from altitude draw,
// hold relief at their own scale instead of the constant mean.
TEST(GeneratedHeightPageProviderTest, LargeNoiseTerrainHasReliefOnItsCoarseLevels)
{
    Components::Terrain terrain;
    terrain.SizeX = 250000.0f;
    terrain.SizeZ = 250000.0f;
    constexpr float32 kTileWorld = 2048.0f; // 1024 spacings of 2 m: 0.5 samples per meter
    constexpr uint32 kTiles = 122;
    GeneratedHeightLattice lattice = NoiseLattice(-125000.0f, -125000.0f, kTileWorld, kTiles, kTiles);
    lattice.Noise = TerrainECS::GeneratedNoiseStackOf(terrain, {Components::TerrainNoisePreset::Rolling});
    const GeneratedHeightPageProvider provider(lattice);
    ASSERT_GT(provider.Levels().size(), 9u);

    // Level 7 (256 m spacing) and level 9 (1 km spacing) drop every octave under 512 m and 2 km.
    constexpr double kMinDeviation = 0.04;
    EXPECT_GT(LevelHeightDeviation(provider, 7), kMinDeviation);
    EXPECT_GT(LevelHeightDeviation(provider, 9), kMinDeviation);
}

// A noise terrain under 2 km has the 5-octave stack; a larger one adds larger octaves and keeps the
// 250 m to 15.625 m octaves.
TEST(GeneratedHeightPageProviderTest, NoiseStackGrowsWithTheTerrainAndKeepsSmallTerrains)
{
    const Terrain::TerrainNoiseStack smooth{0.004f, 5u, Terrain::TerrainNoiseStack::kSmoothGain, 42u};
    Components::Terrain terrain;
    for (const float32 size : {256.0f, 1000.0f, 1024.0f, 1999.0f})
    {
        terrain.SizeX = size;
        terrain.SizeZ = size * 0.5f;
        EXPECT_EQ(TerrainECS::GeneratedNoiseStackOf(terrain, {Components::TerrainNoisePreset::Smooth}), smooth) << size;
    }
    EXPECT_EQ(smooth.BandTop(), 1.0f);

    // The 5-octave sum with gain 0.5 over the 5-octave total: float32 values of the sum, bit for bit.
    struct Golden
    {
        float32 X, Z;
        uint32 Bits;
    };
    constexpr Golden kGolden[] = {{0.0f, 0.0f, 0x3f2d60c6u},
                                  {137.25f, -411.5f, 0x3f2157a6u},
                                  {-999.0f, 512.75f, 0x3f5f6d4eu},
                                  {333.3f, 777.7f, 0x3f023942u}};
    for (const Golden& golden : kGolden)
        EXPECT_EQ(std::bit_cast<uint32>(Terrain::SampleWorldSpaceNoise(golden.X, golden.Z, smooth)), golden.Bits)
            << golden.X << ", " << golden.Z;

    terrain.SizeX = 250000.0f;
    const Terrain::TerrainNoiseStack large = TerrainECS::GeneratedNoiseStackOf(terrain, {Components::TerrainNoisePreset::Rolling});
    ASSERT_EQ(large.Octaves, 12u);
    const uint32 added = large.Octaves - smooth.Octaves;
    EXPECT_EQ(large.TopFrequency * static_cast<float32>(1u << added), smooth.TopFrequency);
    EXPECT_EQ(large.TopSeed + added, smooth.TopSeed);

    EXPECT_GT(TerrainECS::GeneratedNoiseStackOf(terrain, {Components::TerrainNoisePreset::Rugged}).Gain, large.Gain);
    EXPECT_LT(TerrainECS::GeneratedNoiseStackOf(terrain, {Components::TerrainNoisePreset::Smooth}).Gain, large.Gain);
}

// The octaves above 250 m grow with the square root of their cell, in units of the detail band (the
// 250 m to 15.625 m octaves): a 250 km Rolling terrain's 32 km octave spans 2^3.5 / 2.202 = 5.137 units,
// and the sum reaches 16.99. The units are Height Scale meters: 1.3 km and 4.35 km at the default 256.
TEST(GeneratedHeightPageProviderTest, LargeOctavesGrowWithTheirWavelength)
{
    Components::Terrain terrain;
    terrain.SizeX = 250000.0f;
    terrain.SizeZ = 250000.0f;
    const Terrain::TerrainNoiseStack stack = TerrainECS::GeneratedNoiseStackOf(terrain, {Components::TerrainNoisePreset::Rolling});
    ASSERT_EQ(stack.Octaves, 12u);
    EXPECT_NEAR(stack.BandTop(), 16.99, 0.01);

    // Only the 32 km octave is evaluated (every finer cell is under 20 km); the 8 x 8 lattice points
    // inside the terrain sample its full range.
    constexpr float32 kTopCell = 32000.0f;
    constexpr float32 kOnlyTopOctave = 20000.0f;
    constexpr double kTopOctaveSpan = 5.137;
    float32 low = std::numeric_limits<float32>::max();
    float32 high = std::numeric_limits<float32>::lowest();
    for (int32 z = -4; z < 4; ++z)
        for (int32 x = -4; x < 4; ++x)
        {
            const float32 value = Terrain::SampleWorldSpaceNoise(static_cast<float32>(x) * kTopCell,
                                                                 static_cast<float32>(z) * kTopCell, stack,
                                                                 kOnlyTopOctave);
            low = std::min(low, value);
            high = std::max(high, value);
        }
    EXPECT_LE(high - low, kTopOctaveSpan + 0.01);
    EXPECT_GT(high - low, 0.8 * kTopOctaveSpan);
}

// RealizedTop() is within 5 percent of the highest of 251 x 251 points across the terrain, for every preset,
// on the smallest terrain of each large-octave count (2 km to 65,536 km) and on 250 km.
TEST(GeneratedHeightPageProviderTest, RealizedTopIsTheHighestPointAtEveryTerrainSize)
{
    constexpr int32 kHalfSamples = 125;
    constexpr float32 kTolerance = 0.05f;
    constexpr uint32 kMaxLargeOctaves = 16;
    std::vector<float32> sizes;
    for (uint32 largeOctaves = 1; largeOctaves <= kMaxLargeOctaves; ++largeOctaves)
        sizes.push_back(1000.0f * static_cast<float32>(1u << largeOctaves));
    sizes.push_back(250000.0f);
    Components::Terrain terrain;
    for (const float32 size : sizes)
    {
        terrain.SizeX = size;
        terrain.SizeZ = size;
        const float32 spacing = size / static_cast<float32>(2 * kHalfSamples);
        for (const Components::TerrainNoisePreset preset :
             {Components::TerrainNoisePreset::Smooth, Components::TerrainNoisePreset::Rolling,
              Components::TerrainNoisePreset::Rugged})
        {
            const Terrain::TerrainNoiseStack stack = TerrainECS::GeneratedNoiseStackOf(terrain, {preset});
            float32 high = std::numeric_limits<float32>::lowest();
            for (int32 z = -kHalfSamples; z <= kHalfSamples; ++z)
                for (int32 x = -kHalfSamples; x <= kHalfSamples; ++x)
                    high = std::max(high, Terrain::SampleWorldSpaceNoise(static_cast<float32>(x) * spacing,
                                                                         static_cast<float32>(z) * spacing, stack));
            ::testing::Test::RecordProperty("share_" + std::to_string(static_cast<int64>(size)) + "_" +
                                                std::to_string(static_cast<int32>(preset)),
                                            std::to_string(high / stack.BandTop()));
            EXPECT_NEAR(stack.RealizedTop(), high, kTolerance * high)
                << size << " m, preset " << static_cast<int32>(preset) << ", share " << high / stack.BandTop();
        }
    }
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

namespace
{

// Waits for the one fetch in flight of `fetcher` and returns its outcome; an error when none ends in time.
PageFetchOutcome WaitForFetch(PageFetcher& fetcher)
{
    std::vector<FinishedPageFetch> finished;
    const auto deadline = std::chrono::steady_clock::now() + kLoadTimeout;
    do
    {
        fetcher.Finished(finished);
        if (!finished.empty())
        {
            EXPECT_EQ(finished.size(), 1u) << "a fetch ends once";
            return std::move(finished.front().Outcome);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return PageFetchOutcome{{}, nullptr, "no fetch ended in time", false};
}

} // namespace

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

    PageFetcher fetcher(&pool, &io, 1);
    const auto load = [&](const PageAddress& address, std::shared_ptr<std::atomic<bool>> cancel) {
        fetcher.Fetch(PageSource{reader, nullptr}, address, std::move(cancel), PageFetchTag{});
        return WaitForFetch(fetcher);
    };

    for (const PageAddress address : {PageAddress{0, 0, 0, 0}, PageAddress{0, 0, 3, 2}, PageAddress{0, 1, 1, 1}})
    {
        const PageFetchOutcome loaded = load(address, std::make_shared<std::atomic<bool>>(false));
        ASSERT_NE(loaded.Bytes, nullptr) << loaded.Error;
        EXPECT_EQ(loaded.Address, address);
        EXPECT_EQ(Heights(*loaded.Bytes), PageSamples(*reader, address));
    }

    const PageFetchOutcome absent = load(PageAddress{0, 0, 9, 9}, nullptr);
    EXPECT_TRUE(absent.Absent);
    EXPECT_NE(absent.Error.find("not in the store"), std::string::npos);

    const PageFetchOutcome cancelled = load(PageAddress{0, 0, 1, 1}, std::make_shared<std::atomic<bool>>(true));
    EXPECT_EQ(cancelled.Bytes, nullptr);
    EXPECT_FALSE(cancelled.Absent);
    EXPECT_FALSE(cancelled.Error.empty());

    // Reuse keeps out a buffer that another owner holds: the next page does not decode into it.
    const PageFetchOutcome held = load(PageAddress{0, 0, 0, 0}, nullptr);
    ASSERT_NE(held.Bytes, nullptr) << held.Error;
    fetcher.Reuse(held.Bytes);
    const PageFetchOutcome next = load(PageAddress{0, 0, 3, 2}, nullptr);
    ASSERT_NE(next.Bytes, nullptr) << next.Error;
    EXPECT_NE(next.Bytes.get(), held.Bytes.get()) << "the decode wrote into bytes another owner holds";
    EXPECT_EQ(Heights(*held.Bytes), PageSamples(*reader, PageAddress{0, 0, 0, 0}));

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
    // Declared before the pool: the held job can still read it after the release.
    std::atomic<bool> release{false};
    JobSystem::WorkStealingThreadPool pool(1);
    pool.Submit([&release]() {
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    AssetIOService io;
    io.Start(pool, 1);

    PageFetcher fetcher(&pool, &io, 1);
    fetcher.Fetch(PageSource{reader, nullptr}, PageAddress{0, 0, 0, 0}, nullptr, PageFetchTag{});

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

    std::vector<FinishedPageFetch> finished;
    fetcher.Finished(finished);
    ASSERT_EQ(finished.size(), 1u) << "the fetch ends exactly once";
    EXPECT_EQ(finished.front().Outcome.Bytes, nullptr);
    EXPECT_FALSE(finished.front().Outcome.Error.empty());
    fetcher.Finished(finished);
    EXPECT_TRUE(finished.empty());
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
    ByteBuffer stored;
    ASSERT_TRUE(normal->ReadPageBytes(address, stored));
    struct Case
    {
        std::shared_ptr<PageStoreReader> Reader;
        uint32 Bytes = 0;
        std::vector<uint8> Payload; // what the page holds once loaded
    };
    const Case cases[] = {{splat, 67600u, SurfacePage(*splat, address)}, {normal, 18496u, std::vector<uint8>(stored.begin(), stored.end())}};
    for (const Case& page : cases)
    {
        const PageSource source{page.Reader, nullptr};
        EXPECT_EQ(source.PayloadBytes(), page.Bytes);
        PageFetcher fetcher(&pool, &io, 1);
        fetcher.Fetch(source, address, nullptr, PageFetchTag{});
        const PageFetchOutcome outcome = WaitForFetch(fetcher);
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

// A camera at `position` that looks every way.
std::array<TerrainECS::PageRequestCamera, 1> DiscCamera(const Mathematics::Vector3& position)
{
    return {TerrainECS::PageRequestCamera{position, {}, 0.0f, false}};
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

// A camera that pans between two halves of a store streams the level-0 pages of each half in and out
// through the asset reader threads. Once each half streamed twice, a page that streams in again
// allocates only in its asset read: its fetch, its decode and its landing reuse what the first pages left.
TEST_F(PageStoreTest, AStorePageThatStreamsInAgainAllocatesOnlyInItsAssetRead)
{
    const std::filesystem::path source = m_Dir / "steady.r32";
    WriteR32(source, 1024, 512, Relief);
    const std::filesystem::path store = m_Dir / "steady.gepage";
    ASSERT_EQ(Cook(source, 1024, 512, store).Error, "");
    auto reader = std::make_shared<PageStoreReader>();
    ASSERT_EQ(reader->Open(store, PageFieldKind::Height), "");
    const std::vector<PageStoreLevel>& levels = reader->Layout().Levels;
    std::vector<PageWant> halves[2]; // each half's level-0 pages and every coarser page
    for (const PageWant& want : EveryPage(levels))
        if (want.Address.Level != 0)
        {
            halves[0].push_back(want);
            halves[1].push_back(want);
        }
        else
        {
            halves[want.Address.X < levels[0].PagesX / 2u ? 0 : 1].push_back(want);
        }
    ASSERT_EQ(halves[0].size(), halves[1].size());

    JobSystem::WorkStealingThreadPool pool(2);
    AssetIOService io;
    io.Start(pool, 1);
    PageResidencyBudget budget;
    budget.CpuBytes = uint64(halves[0].size()) * PagePayloadBytes(PageFieldFormat::HeightSteps); // one half
    budget.UploadsPerFrame = 1024;
    PageResidencyManager residency(&pool, &io, budget);
    residency.ConfigureField(1, static_cast<uint32>(halves[0].size()), 2, 0.0f);
    residency.Configure(1, 1, PageSource{reader, nullptr});

    uint64 frame = 0;
    uint32 loads = 0;
    const auto pan = [&](const std::vector<PageWant>& half) {
        residency.Request(1, half);
        const auto resident = [&] {
            const PageCache& cache = residency.FieldCache(1);
            return std::all_of(half.begin(), half.end(), [&](const PageWant& want) {
                return cache.SlotOf(CachedPage{1, want.Address}) != kNoResidentSlot;
            });
        };
        const auto deadline = std::chrono::steady_clock::now() + kLoadTimeout;
        do
        {
            residency.Update(++frame, 1.0f / 60.0f, false);
            loads += residency.LoadsStartedThisFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (!resident() && std::chrono::steady_clock::now() < deadline);
        return resident();
    };
    for (int round = 0; round < 2; ++round)
        for (const std::vector<PageWant>& half : halves)
            ASSERT_TRUE(pan(half));

    // The least of three pans: a list that reaches a new largest size (more pages land in one frame
    // than before) grows in one of them only.
    uint64 allocations = ~0ull;
    for (int round = 0; round < 3; ++round)
    {
        loads = 0;
        Memory::AllocationCountScope scope(Memory::CountWindow::Process);
        for (const std::vector<PageWant>& half : halves)
            if (!pan(half))
                break;
        allocations = std::min(allocations, scope.Count());
        ASSERT_EQ(loads, uint32(levels[0].PagesX * levels[0].PagesZ)) << "each level-0 page streams in once";
    }
    // The asset read's four a page: the file path that ReadStorePage copies, the ReadFileRange buffer,
    // the read job and the job's task data. Below Debug level, the read's Debug log line formats nothing.
    // The count is process-wide, so a slack of 8 lets another thread allocate: far below one a page (32).
    EXPECT_LE(allocations, 4u * loads + 8) << "over " << loads << " pages";
    io.Stop();
    pool.Shutdown();
}

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

// A parked frame repeats no per-page pass: each pass runs only when its input changed. Pages no
// longer wanted are still released after the hysteresis while the frames are otherwise quiet.
TEST(PageResidencyTest, AParkedFrameAfterASettledSetVisitsNoPage)
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
    ASSERT_GT(wanted.size(), 5u);
    for (int i = 0; i < 40; ++i) // past the arrival fades
        residency.Update(++frame, 1.0f / 60.0f, false);
    for (int i = 0; i < 5; ++i)
    {
        residency.Request(7, wanted);
        residency.Update(++frame, 1.0f / 60.0f, false);
        EXPECT_EQ(residency.PageVisitsThisFrame(), 0u) << "parked frame " << i;
    }

    // The top two levels only: the others go after the hysteresis, then the frames are quiet again.
    const std::vector<PageWant> few(wanted.end() - 5, wanted.end());
    for (uint32 i = 0; i <= kPageReleaseHysteresisFrames + 4u; ++i)
    {
        residency.Request(7, few);
        residency.Update(++frame, 1.0f / 60.0f, false);
    }
    EXPECT_EQ(residency.FieldCache(7).ResidentCount(), few.size());
    residency.Request(7, few);
    residency.Update(++frame, 1.0f / 60.0f, false);
    EXPECT_EQ(residency.PageVisitsThisFrame(), 0u);
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
    residency.RequestCpu(1, static_cast<uint32>(provider->Levels().size()) - 1u, {}); // a CPU reader of stream 1
    uint64 frame = 0;
    SettleOwner(residency, few, 1, frame);
    ASSERT_NE(residency.FieldCache(1).SlotOf(CachedPage{1, few.back().Address}), kNoResidentSlot);
    const CpuPageEntry cpu = residency.CpuPages(1).Page(few.back().Address);
    ASSERT_EQ(State(cpu), CpuPageState::Resident);
    EXPECT_EQ(Bytes(cpu), residency.Bytes(1, few.back().Address)->data()) << "the CPU reads the GPU's fetch";

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

// ---- A store read while it is baked ----------------------------------------------------------------

namespace
{

constexpr uint32 kLiveReadSide = 600u;

// The splat store, 600 samples a side, whose pages the store access tests bake again.
std::filesystem::path BuildLiveReadSource(const std::filesystem::path& directory)
{
    const std::filesystem::path built = directory / "built.gepage";
    BuildSurfaceStore(built, PageFieldFormat::Rgba8, PageFieldFilter::WeightsTent, kLiveReadSide, kLiveReadSide,
                      SplatWeights(kLiveReadSide, kLiveReadSide));
    return built;
}

// The loaded bytes of page `address` of `reader`.
std::vector<uint8> StoredPayload(const PageStoreReader& reader, const PageAddress& address)
{
    ByteBuffer stored;
    std::vector<uint8> payload;
    EXPECT_TRUE(reader.ReadPageBytes(address, stored) && reader.DecodePayload(address, stored, payload));
    return payload;
}

// Every page of a store and its loaded bytes.
struct StorePages
{
    std::vector<PageAddress> Addresses;
    std::vector<std::vector<uint8>> Payloads;
};

StorePages EveryStorePage(const PageStoreReader& reader)
{
    StorePages pages;
    for (const PageWant& want : EveryPage(reader.Layout().Levels))
    {
        pages.Addresses.push_back(want.Address);
        pages.Payloads.push_back(StoredPayload(reader, want.Address));
    }
    return pages;
}

// What the reads of ReadUntilStopped found.
struct LiveReadTally
{
    std::atomic<uint64> Reads{0};
    std::atomic<uint64> Wrong{0};   // bytes that are not the page's
    std::atomic<uint64> Missing{0}; // a coarse page, written before the reads began, read as absent
    std::atomic<uint64> Failed{0};  // a read error
};

// Reads every page of `access` again and again until `stop` is set, on a worker.
void ReadUntilStopped(const PageStoreAccess& access, const StorePages& expected, const std::atomic<bool>& stop,
                      LiveReadTally& tally)
{
    while (!stop.load())
        for (std::size_t page = 0; page < expected.Addresses.size(); ++page)
        {
            const PageFetchOutcome outcome = access.Fetch(expected.Addresses[page]);
            ++tally.Reads;
            if (outcome.Bytes && *outcome.Bytes != expected.Payloads[page])
                ++tally.Wrong;
            if (outcome.Absent && expected.Addresses[page].Level > 0)
                ++tally.Missing;
            if (!outcome.Bytes && !outcome.Absent)
                ++tally.Failed;
        }
}

// The level-0 pages of the rows [firstRow, endRow) of `levels`.
std::vector<PageAddress> Level0Rows(std::span<const PageStoreLevel> levels, uint32 firstRow, uint32 endRow)
{
    std::vector<PageAddress> rows;
    for (uint32 z = firstRow; z < endRow; ++z)
        for (uint32 x = 0; x < levels[0].PagesX; ++x)
            rows.push_back(PageAddress{0, 0, x, z});
    return rows;
}

// The level-0 samples that the level-0 page rows [firstRow, endRow) own.
PageSampleRect Level0RowsRect(std::span<const PageStoreLevel> levels, uint32 firstRow, uint32 endRow)
{
    return PageSampleRect{0u, firstRow * kPageOwnedSamples, levels[0].SamplesX - 1u,
                          std::min(endRow * kPageOwnedSamples, levels[0].SamplesZ) - 1u};
}

// Writes the level-0 rows [firstRow, endRow) of `expected` into the store `access` writes.
std::string WriteLevel0Rows(PageStoreAccess& access, const PageStoreReader& expected, uint32 firstRow, uint32 endRow)
{
    PageStorePatcher patcher(access.Writer(), nullptr);
    for (uint32 pz = firstRow; pz < endRow; ++pz)
        for (uint32 px = 0; px < expected.Layout().Levels[0].PagesX; ++px)
            if (!patcher.WritePage(PageAddress{0, 0, px, pz}, SurfacePage(expected, PageAddress{0, 0, px, pz})))
                return "level-0 page (" + std::to_string(px) + ", " + std::to_string(pz) + ") was not written";
    return {};
}

// Makes the store `access` reads at `file` with every coarse page of `expected` and no level-0 page.
std::string CreateCoarseStore(PageStoreAccess& access, const std::filesystem::path& file,
                              const PageStoreReader& expected)
{
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    std::vector<uint8> present(static_cast<std::size_t>(DensePageCount(levels)), 1u);
    std::fill_n(present.begin(), static_cast<std::size_t>(levels[0].PagesX) * levels[0].PagesZ, uint8{0});
    if (std::string error = access.Create(file, MakePageStoreLayout(expected.Layout().Header, present)); !error.empty())
        return error;
    PageStorePatcher patcher(access.Writer(), nullptr);
    for (uint8 level = 1; level < levels.size(); ++level)
        for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < levels[level].PagesX; ++px)
            {
                const PageAddress address{0, level, px, pz};
                if (!patcher.WritePage(address, SurfacePage(expected, address)))
                    return "a coarse page was not written";
            }
    return access.Finish();
}

// One patch run of the store at `file`: grows the index by the level-0 rows [firstRow, endRow),
// which moves the pages under it, writes them, and publishes the store.
std::string PatchRun(PageStoreAccess& access, const std::filesystem::path& file, const PageStoreReader& expected,
                     uint32 firstRow, uint32 endRow)
{
    PageStoreLayout layout;
    {
        PageStoreReader published;
        if (std::string error = published.Open(file, PageFieldKind::Splat); !error.empty())
            return error;
        layout = published.Layout();
    }
    if (std::string error = access.OpenForPatch(file, std::move(layout)); !error.empty())
        return error;
    if (std::string error = access.AddPresentPages(Level0Rows(expected.Layout().Levels, firstRow, endRow));
        !error.empty())
        return error;
    if (std::string error = WriteLevel0Rows(access, expected, firstRow, endRow); !error.empty())
        return error;
    access.Commit(Level0RowsRect(expected.Layout().Levels, firstRow, endRow), 0u);
    return access.Finish();
}

// Bakes level 0 of `expected` into the store at `file` in two patch runs, one for each half of its rows.
std::string BakeLevel0InTwoRuns(PageStoreAccess& access, const std::filesystem::path& file,
                                const PageStoreReader& expected)
{
    const uint32 rows = expected.Layout().Levels[0].PagesZ;
    if (std::string error = PatchRun(access, file, expected, 0u, rows / 2u); !error.empty())
        return error;
    return PatchRun(access, file, expected, rows / 2u, rows);
}

// True when stream `stream` of `residency` holds every page of `wanted` of `level` in rows [firstRow, endRow).
// RunFramesUntil, taking each frame's uploads into `uploaded`.
template <typename Done>
uint64 RunFramesUploading(PageResidencyManager& residency, uint64 frame, UploadedPages& uploaded, Done done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < deadline)
    {
        residency.Update(++frame, 1.0f / 60.0f, false);
        uploaded.Take(residency);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return frame;
}

bool AllUploaded(const UploadedPages& uploaded, uint32 stream, std::span<const PageWant> wanted, uint32 level,
                 uint32 firstRow, uint32 endRow)
{
    for (const PageWant& want : wanted)
        if (want.Address.Level == level && want.Address.Z >= firstRow && want.Address.Z < endRow &&
            uploaded.Find(stream, want.Address) == nullptr)
            return false;
    return true;
}

} // namespace

// A worker reads a store while the bake writes its pages, grows its index (which moves the pages
// under it), renames it at each run's start and end, and reopens it. Every read is the page's bytes:
// a coarse page is never absent, a level-0 page is absent until it is written.
TEST_F(PageStoreTest, AStoreAccessReadsEveryPageByteExactWhileItsBakeWritesGrowsAndPublishesIt)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const StorePages pages = EveryStorePage(expected);

    const std::filesystem::path live = m_Dir / "live.gepage";
    PageStoreAccess access(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(access, live, expected), "");
    const uint64 published = access.Version();

    // Declared before the pool: a reader can still read it after the stop.
    std::atomic<bool> stop{false};
    JobSystem::WorkStealingThreadPool readers(2);
    LiveReadTally tally;
    std::vector<JobSystem::TaskHandle> reads;
    for (std::size_t worker = 0; worker < readers.GetWorkerCount(); ++worker)
        reads.push_back(readers.Submit([&] { ReadUntilStopped(access, pages, stop, tally); }));
    while (tally.Reads.load() < pages.Addresses.size())
        std::this_thread::yield();
    const std::string baked = BakeLevel0InTwoRuns(access, live, expected);
    const uint64 readsWhileBaking = tally.Reads.load();
    stop.store(true);
    for (JobSystem::TaskHandle& read : reads)
        read.Wait();

    ASSERT_EQ(baked, "");
    EXPECT_GT(readsWhileBaking, 2u * pages.Addresses.size()) << "the reads did not overlap the bake";
    EXPECT_EQ(tally.Wrong.load(), 0u) << "a read got bytes that are not its page's";
    EXPECT_EQ(tally.Missing.load(), 0u) << "a coarse page read as absent while the store was baked";
    EXPECT_EQ(tally.Failed.load(), 0u) << "a read of the store failed while it was baked";
    EXPECT_GT(access.Version(), published) << "the runs made no new page readable";
    for (std::size_t page = 0; page < pages.Addresses.size(); ++page)
    {
        const PageFetchOutcome outcome = access.Fetch(pages.Addresses[page]);
        ASSERT_NE(outcome.Bytes, nullptr) << "page " << page << " is not readable after the runs";
        EXPECT_EQ(*outcome.Bytes, pages.Payloads[page]) << "page " << page;
    }
    readers.Shutdown();
}

// Two stores, each baked on its own worker, read by two other workers at once.
TEST_F(PageStoreTest, TwoStoreAccessesBakeAndReadAtOnce)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const StorePages pages = EveryStorePage(expected);

    const std::array<std::filesystem::path, 2> files = {m_Dir / "first.gepage", m_Dir / "second.gepage"};
    PageStoreAccess first(PageFieldKind::Splat, expected.Layout().Header);
    PageStoreAccess second(PageFieldKind::Splat, expected.Layout().Header);
    const std::array<PageStoreAccess*, 2> accesses = {&first, &second};
    for (std::size_t store = 0; store < 2u; ++store)
        ASSERT_EQ(CreateCoarseStore(*accesses[store], files[store], expected), "") << "store " << store;

    // Declared before the pool: a reader can still read it after the stop.
    std::atomic<bool> stop{false};
    JobSystem::WorkStealingThreadPool workers(4);
    std::array<LiveReadTally, 2> tallies;
    std::array<std::string, 2> baked;
    std::vector<JobSystem::TaskHandle> reads;
    for (std::size_t store = 0; store < 2u; ++store)
        reads.push_back(
            workers.Submit([&, store] { ReadUntilStopped(*accesses[store], pages, stop, tallies[store]); }));
    std::vector<JobSystem::TaskHandle> bakes;
    for (std::size_t store = 0; store < 2u; ++store)
        bakes.push_back(workers.Submit(
            [&, store] { baked[store] = BakeLevel0InTwoRuns(*accesses[store], files[store], expected); }));
    for (JobSystem::TaskHandle& bake : bakes)
        bake.Wait();
    stop.store(true);
    for (JobSystem::TaskHandle& read : reads)
        read.Wait();

    for (std::size_t store = 0; store < 2u; ++store)
    {
        ASSERT_EQ(baked[store], "") << "store " << store;
        EXPECT_GT(tallies[store].Reads.load(), 0u) << "store " << store;
        EXPECT_EQ(tallies[store].Wrong.load(), 0u) << "store " << store;
        EXPECT_EQ(tallies[store].Missing.load(), 0u) << "store " << store;
        EXPECT_EQ(tallies[store].Failed.load(), 0u) << "store " << store;
        PageStoreReader published;
        ASSERT_EQ(published.Open(files[store], PageFieldKind::Splat), "");
        ExpectSameSurfacePages(published, expected);
    }
    workers.Shutdown();
}

// A stream over a store access loads a level-0 page as soon as the run that bakes it commits it.
// A stream configured before the run and read after it reads the pages that the run moved at their
// new place. Each stream is configured once.
TEST_F(PageStoreTest, AStreamOverAStoreAccessLoadsAPageBakedMidRunAndAStreamOpenedBeforeTheRunReadsTheMovedPages)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    const std::filesystem::path live = m_Dir / "live.gepage";
    auto access = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(*access, live, expected), "");

    JobSystem::WorkStealingThreadPool pool(2);
    PageResidencyBudget budget;
    budget.UploadsPerFrame = 1024;
    PageResidencyManager residency(&pool, nullptr, budget);
    const std::vector<PageWant> wanted = EveryPage(levels);
    residency.ConfigureField(3, 2u * static_cast<uint32>(wanted.size()), 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {access}});
    residency.Configure(4, 3, PageSource{nullptr, nullptr, {access}});
    residency.Request(3, wanted);
    residency.Request(4, {});
    UploadedPages uploaded(3);
    uint64 frame = RunFramesUploading(residency, 0, uploaded,
                                      [&] { return AllUploaded(uploaded, 3, wanted, 1, 0u, levels[1].PagesZ); });
    ASSERT_TRUE(AllUploaded(uploaded, 3, wanted, 1, 0u, levels[1].PagesZ)) << "the coarse pages did not load";
    ASSERT_EQ(uploaded.Find(3, PageAddress{0, 0, 0, 0}), nullptr) << "a level-0 page loaded before it was baked";

    // A run: the first rows are baked and committed; the store is not published yet.
    const uint32 half = levels[0].PagesZ / 2u;
    PageStoreLayout layout;
    {
        PageStoreReader before;
        ASSERT_EQ(before.Open(live, PageFieldKind::Splat), "");
        layout = before.Layout();
    }
    ASSERT_EQ(access->OpenForPatch(live, std::move(layout)), "");
    ASSERT_EQ(access->AddPresentPages(Level0Rows(levels, 0u, levels[0].PagesZ)), "");
    ASSERT_EQ(WriteLevel0Rows(*access, expected, 0u, half), "");
    access->Commit(Level0RowsRect(levels, 0u, half), 0u);
    frame = RunFramesUploading(residency, frame, uploaded,
                               [&] { return AllUploaded(uploaded, 3, wanted, 0, 0u, half); });
    EXPECT_TRUE(AllUploaded(uploaded, 3, wanted, 0, 0u, half))
        << "a page baked mid-run did not load before the run ended";
    EXPECT_FALSE(std::filesystem::exists(live)) << "the run published the store";
    for (uint32 px = 0; px < levels[0].PagesX; ++px)
        if (const std::vector<uint8>* bytes = uploaded.Find(3, PageAddress{0, 0, px, 0}))
        {
            EXPECT_EQ(*bytes, StoredPayload(expected, PageAddress{0, 0, px, 0})) << "level-0 page " << px;
        }

    // The run ends: the rest is baked and the store published at its file.
    ASSERT_EQ(WriteLevel0Rows(*access, expected, half, levels[0].PagesZ), "");
    ASSERT_EQ(access->Finish(), "");
    frame = RunFramesUploading(residency, frame, uploaded,
                               [&] { return AllUploaded(uploaded, 3, wanted, 0, 0u, levels[0].PagesZ); });
    ASSERT_TRUE(AllUploaded(uploaded, 3, wanted, 0, 0u, levels[0].PagesZ))
        << "the pages baked at the run's end did not load";

    // A stream configured before the run reads after it: the pages the run moved read at their new place.
    residency.Request(4, wanted);
    frame = RunFramesUploading(residency, frame, uploaded,
                               [&] { return AllUploaded(uploaded, 4, wanted, 0, 0u, levels[0].PagesZ); });
    for (const PageWant& want : wanted)
    {
        const std::vector<uint8>* bytes = uploaded.Find(4, want.Address);
        ASSERT_NE(bytes, nullptr);
        EXPECT_EQ(*bytes, StoredPayload(expected, want.Address))
            << "level " << int(want.Address.Level) << " page " << want.Address.X << "," << want.Address.Z;
    }
    pool.Shutdown();
}

// A read finds a page absent before the commit that writes it, and lands after the stream saw the
// commit: the page is read again, not marked absent until the next commit.
TEST_F(PageStoreTest, APageReadAbsentBeforeItsCommitAndLandedAfterItIsReadAgain)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    const std::filesystem::path live = m_Dir / "live.gepage";
    auto access = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(*access, live, expected), "");
    PageStoreLayout layout;
    {
        PageStoreReader before;
        ASSERT_EQ(before.Open(live, PageFieldKind::Splat), "");
        layout = before.Layout();
    }
    ASSERT_EQ(access->OpenForPatch(live, std::move(layout)), "");
    ASSERT_EQ(access->AddPresentPages(Level0Rows(levels, 0u, 1u)), "");

    JobSystem::WorkStealingThreadPool pool(1);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(3, 4, 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {access}});
    const PageAddress page{0, 0, 0, 0};
    residency.Request(3, std::vector<PageWant>{PageWant{page, 0.0f, true}});
    residency.Update(1, 1.0f / 60.0f, false); // the read starts and finds the page absent
    pool.Submit([] {}, JobSystem::JobPriority::Background).Wait();
    ASSERT_EQ(WriteLevel0Rows(*access, expected, 0u, 1u), "");
    access->Commit(Level0RowsRect(levels, 0u, 1u), 0u);
    RunFramesUntil(residency, 1, [&] { return residency.Bytes(3, page) != nullptr; });
    const std::vector<uint8>* bytes = residency.Bytes(3, page);
    ASSERT_NE(bytes, nullptr) << "the read before the commit marked the page absent";
    EXPECT_EQ(*bytes, StoredPayload(expected, page));
    residency.Forget(3);
    pool.Shutdown();
    access->Close();
}

namespace
{

// The loads that `frames` Updates of `residency` start, a short sleep apart so the reads land.
uint64 LoadsOverFrames(PageResidencyManager& residency, uint64& frame, uint32 frames)
{
    uint64 loads = 0;
    for (uint32 i = 0; i < frames; ++i)
    {
        residency.Update(++frame, 1.0f / 60.0f, false);
        loads += residency.LoadsStartedThisFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return loads;
}

} // namespace

// A commit makes a stream read again only its absent pages that read the commit's rect. The other
// absent pages are not read again.
TEST_F(PageStoreTest, ACommitReadsAgainOnlyTheAbsentPagesThatReadItsRect)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    ASSERT_GE(levels[0].PagesZ, 4u);
    const std::filesystem::path live = m_Dir / "live.gepage";
    auto access = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(*access, live, expected), "");
    PageStoreLayout layout;
    {
        PageStoreReader before;
        ASSERT_EQ(before.Open(live, PageFieldKind::Splat), "");
        layout = before.Layout();
    }
    ASSERT_EQ(access->OpenForPatch(live, std::move(layout)), "");
    ASSERT_EQ(access->AddPresentPages(Level0Rows(levels, 0u, levels[0].PagesZ)), "");

    JobSystem::WorkStealingThreadPool pool(1);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    std::vector<PageWant> wanted;
    for (uint32 pz = 0; pz < levels[0].PagesZ; ++pz)
        for (uint32 px = 0; px < levels[0].PagesX; ++px)
            wanted.push_back(PageWant{PageAddress{0, 0, px, pz}, 0.0f, true});
    residency.ConfigureField(3, static_cast<uint32>(wanted.size()), 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {access}});
    residency.Request(3, wanted);
    uint64 frame = 0;
    ASSERT_EQ(LoadsOverFrames(residency, frame, 100), wanted.size()) << "each page is read once and found absent";

    ASSERT_EQ(WriteLevel0Rows(*access, expected, 0u, 1u), "");
    const PageSampleRect rect = Level0RowsRect(levels, 0u, 1u);
    access->Commit(rect, 0u);
    const std::size_t reading = static_cast<std::size_t>(std::ranges::count_if(
        wanted, [&rect](const PageWant& want) { return PageReadsLevel0Rect(want.Address, rect, kPageApronSamples); }));
    ASSERT_LT(reading, wanted.size());
    EXPECT_EQ(LoadsOverFrames(residency, frame, 100), reading) << "the commit read again a page outside its rect";
    residency.Forget(3);
    pool.Shutdown();
    access->Close();
}

// A stream over two planes: a page whose second plane is not written yet stays absent, and lands
// with both planes' bytes, the first plane's first, once the second plane commits it.
TEST_F(PageStoreTest, AStreamOverTwoPlanesLoadsAPageOnlyWithEveryPlaneAndHoldsThePlanesInOrder)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    auto first = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    auto second = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    const std::array<std::shared_ptr<PageStoreAccess>, 2> planes = {first, second};
    for (std::size_t plane = 0; plane < planes.size(); ++plane)
    {
        const std::filesystem::path file = m_Dir / ("plane" + std::to_string(plane) + ".gepage");
        ASSERT_EQ(CreateCoarseStore(*planes[plane], file, expected), "");
        PageStoreLayout layout;
        {
            PageStoreReader published;
            ASSERT_EQ(published.Open(file, PageFieldKind::Splat), "");
            layout = published.Layout();
        }
        ASSERT_EQ(planes[plane]->OpenForPatch(file, std::move(layout)), "");
        ASSERT_EQ(planes[plane]->AddPresentPages(Level0Rows(levels, 0u, 1u)), "");
    }
    ASSERT_EQ(WriteLevel0Rows(*first, expected, 0u, 1u), "");
    first->Commit(Level0RowsRect(levels, 0u, 1u), 0u);

    JobSystem::WorkStealingThreadPool pool(1);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(3, 4, 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {first, second}});
    const PageAddress page{0, 0, 0, 0};
    residency.Request(3, std::vector<PageWant>{PageWant{page, 0.0f, true}});
    uint64 frame = 0;
    for (uint32 attempt = 0; attempt < 8u; ++attempt)
    {
        residency.Update(++frame, 1.0f / 60.0f, false);
        pool.Submit([] {}, JobSystem::JobPriority::Background).Wait();
    }
    EXPECT_EQ(residency.Bytes(3, page), nullptr) << "a page loaded before its second plane held it";

    ASSERT_EQ(WriteLevel0Rows(*second, expected, 0u, 1u), "");
    second->Commit(Level0RowsRect(levels, 0u, 1u), 0u);
    RunFramesUntil(residency, frame, [&] { return residency.Bytes(3, page) != nullptr; });
    const std::vector<uint8>* bytes = residency.Bytes(3, page);
    ASSERT_NE(bytes, nullptr) << "the page did not load once every plane held it";
    std::vector<uint8> both = StoredPayload(expected, page);
    both.insert(both.end(), both.begin(), both.end());
    EXPECT_EQ(*bytes, both);
    residency.Forget(3);
    pool.Shutdown();
    first->Close();
    second->Close();
}

// The owner of a store access lets it go while a run writes it and a stream reads it: the stream's
// reads keep the access alive and get the pages' bytes; the last holder drops the store.
TEST_F(PageStoreTest, AStoreAccessItsOwnerLetsGoMidRunServesTheStreamUntilTheStreamEnds)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    const std::filesystem::path live = m_Dir / "live.gepage";
    auto owner = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(*owner, live, expected), "");
    PageStoreLayout layout;
    {
        PageStoreReader before;
        ASSERT_EQ(before.Open(live, PageFieldKind::Splat), "");
        layout = before.Layout();
    }
    ASSERT_EQ(owner->OpenForPatch(live, std::move(layout)), "");
    ASSERT_EQ(owner->AddPresentPages(Level0Rows(levels, 0u, levels[0].PagesZ)), "");
    ASSERT_EQ(WriteLevel0Rows(*owner, expected, 0u, levels[0].PagesZ), "");
    owner->Commit(Level0RowsRect(levels, 0u, levels[0].PagesZ), 0u);

    JobSystem::WorkStealingThreadPool pool(2);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    const std::vector<PageWant> wanted = EveryPage(levels);
    residency.ConfigureField(3, static_cast<uint32>(wanted.size()), 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {owner}});
    const std::weak_ptr<PageStoreAccess> watched = owner;
    residency.Request(3, wanted);
    residency.Update(1, 1.0f / 60.0f, false); // the loads start with the owner alive
    owner.reset();
    EXPECT_FALSE(watched.expired()) << "the stream does not hold its source";
    UploadedPages uploaded(3);
    uploaded.Take(residency);
    RunFramesUploading(residency, 1, uploaded,
                       [&] { return AllUploaded(uploaded, 3, wanted, 0u, 0u, levels[0].PagesZ); });
    for (const PageWant& want : wanted)
    {
        const std::vector<uint8>* bytes = uploaded.Find(3, want.Address);
        ASSERT_NE(bytes, nullptr) << "a page did not load after its owner let the store go";
        EXPECT_EQ(*bytes, StoredPayload(expected, want.Address));
    }
    residency.Forget(3);
    pool.Shutdown();
    EXPECT_TRUE(watched.expired()) << "the store access outlived its last holder";
    std::size_t temporaries = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(m_Dir))
        if (entry.path().extension() == ".tmp")
            ++temporaries;
    EXPECT_EQ(temporaries, 0u) << "the dropped store left its temporary file";
}

// A closed store access reads every page as absent, after a publish and while a run writes it, and
// drops the store being written. A publish reads the store again.
TEST_F(PageStoreTest, AClosedStoreAccessReadsNoPageUntilItPublishesAgain)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    const std::filesystem::path live = m_Dir / "live.gepage";
    PageStoreAccess access(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(access, live, expected), "");
    const PageAddress top{0, static_cast<uint8>(levels.size() - 1u), 0, 0};
    ASSERT_NE(access.Fetch(top).Bytes, nullptr);

    access.Close();
    const PageFetchOutcome published = access.Fetch(top);
    EXPECT_TRUE(published.Absent) << "a closed access read the published store";
    EXPECT_EQ(published.Bytes, nullptr);
    EXPECT_TRUE(published.Error.empty()) << published.Error;

    ASSERT_EQ(access.Publish(live), "");
    ASSERT_NE(access.Fetch(top).Bytes, nullptr) << "a publish after the close did not read the store";
    PageStoreLayout layout;
    {
        PageStoreReader before;
        ASSERT_EQ(before.Open(live, PageFieldKind::Splat), "");
        layout = before.Layout();
    }
    ASSERT_EQ(access.OpenForPatch(live, std::move(layout)), "");
    access.Close();
    const PageFetchOutcome written = access.Fetch(top);
    EXPECT_TRUE(written.Absent) << "a closed access read the store being written";
    EXPECT_TRUE(written.Error.empty()) << written.Error;
    std::size_t temporaries = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(m_Dir))
        if (entry.path().extension() == ".tmp")
            ++temporaries;
    EXPECT_EQ(temporaries, 0u) << "the close kept the store being written";
}

// A stream over a store access has no CPU readers: a CPU read runs on the frame thread, and a read of
// a store being baked waits on the bake. RequestCpu asserts and leaves the stream as it was.
TEST_F(PageStoreTest, RequestCpuOnAStreamOverAStoreAccessIsRefused)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    auto access = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(*access, m_Dir / "live.gepage", expected), "");
    PageResidencyManager residency(nullptr, nullptr, PageResidencyBudget{});
    residency.ConfigureField(3, 4, 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {access}});
    EXPECT_DEBUG_DEATH(residency.RequestCpu(3, static_cast<uint32>(levels.size()) - 1u, {}), "PageStoreAccess");
}

// A stream over generated planes loads a page as a stream over stored planes does: the texel plane as
// filled, the Bc5 plane as the blocks the store's codec encodes from the filled channels.
TEST(GeneratedPagePlanesTest, AGeneratedSurfacePageReachesTheStreamAsAStoredPageDoes)
{
    const auto texel = [](const PageAddress& address, std::size_t index, uint32 salt) {
        return static_cast<uint8>((index * 7u + address.X * 31u + address.Z * 17u + address.Face * 5u + salt) & 0xFFu);
    };
    auto planes = std::make_shared<GeneratedPagePlanes>();
    planes->Levels = BuildPageStoreLevels(2u * kPageOwnedSamples + 1u, 2u * kPageOwnedSamples + 1u);
    planes->FaceCount = kPageStoreMaxFaces;
    planes->Formats = {PageFieldFormat::Rgba8, PageFieldFormat::Bc5};
    planes->Fill = [texel](const PageAddress& address, std::span<const std::span<uint8>> filled) {
        for (std::size_t plane = 0; plane < filled.size(); ++plane)
            for (std::size_t i = 0; i < filled[plane].size(); ++i)
                filled[plane][i] = texel(address, i, static_cast<uint32>(plane) * 101u);
        return true;
    };
    const PageSource source{nullptr, nullptr, {}, planes};
    ASSERT_EQ(source.PayloadBytes(), PagePayloadBytes(PageFieldFormat::Rgba8) + PagePayloadBytes(PageFieldFormat::Bc5));

    JobSystem::WorkStealingThreadPool pool(1);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(3, 4, 2, 0.0f);
    residency.Configure(3, 3, source);
    const PageAddress page{4, 0, 1, 0};
    residency.Request(3, std::vector<PageWant>{PageWant{page, 0.0f, true}});
    RunFramesUntil(residency, 0, [&] { return residency.Bytes(3, page) != nullptr; });
    const std::vector<uint8>* bytes = residency.Bytes(3, page);
    ASSERT_NE(bytes, nullptr) << "the generated page did not load";

    std::vector<uint8> expected(PagePayloadBytes(PageFieldFormat::Rgba8));
    for (std::size_t i = 0; i < expected.size(); ++i)
        expected[i] = texel(page, i, 0u);
    const uint32 normalStride = PageStrideTexels(PageFieldFormat::Bc5);
    std::vector<uint8> normal(static_cast<std::size_t>(normalStride) * normalStride * PageTexelBytes(PageFieldFormat::Bc5));
    for (std::size_t i = 0; i < normal.size(); ++i)
        normal[i] = texel(page, i, 101u);
    PageStoreHeader header;
    header.Format = PageFieldFormat::Bc5;
    EncodedPage encoded;
    MakePageCodec(header)->Encode(page.Level, normal, PageFootprint{}, encoded);
    expected.insert(expected.end(), encoded.Bytes.begin(), encoded.Bytes.end());
    EXPECT_EQ(*bytes, expected);
    residency.Forget(3);
    pool.Shutdown();
}

namespace
{

// The frames an ended fill's completion has to drain before the test reads the page.
constexpr uint32 kOldFillDrainFrames = 20u;

// A fill's progress: Started, then it waits until Released is set, then Finished.
struct FillGate
{
    std::atomic<bool> Started = false;
    std::atomic<bool> Released = false;
    std::atomic<bool> Finished = false;
};

// One Rgba8 plane of 129 samples a side whose every byte is `value`, each fill passing through `gate`.
std::shared_ptr<GeneratedPagePlanes> MakeGatedPlanes(uint8 value, FillGate& gate)
{
    auto planes = std::make_shared<GeneratedPagePlanes>();
    planes->Levels = BuildPageStoreLevels(kPageOwnedSamples + 1u, kPageOwnedSamples + 1u);
    planes->FaceCount = 1;
    planes->Formats = {PageFieldFormat::Rgba8};
    planes->Fill = [value, &gate](const PageAddress&, std::span<const std::span<uint8>> filled) {
        gate.Started = true;
        while (!gate.Released)
            std::this_thread::yield();
        std::ranges::fill(filled[0], value);
        gate.Finished = true;
        return true;
    };
    return planes;
}

} // namespace

// A stream over generated planes changes its source while a page of the old source fills: the old
// fill ends while the new one is in flight and never lands, and the page arrives with the new bytes.
TEST(GeneratedPagePlanesTest, AFillOfTheOldSourceNeverLandsAfterTheGeneratedPlanesChange)
{
    FillGate oldFill;
    FillGate newFill;
    JobSystem::WorkStealingThreadPool pool(2);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(3, 4, 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {}, MakeGatedPlanes(1u, oldFill)});
    const PageAddress page{0, 0, 0, 0};
    residency.Request(3, std::vector<PageWant>{PageWant{page, 0.0f, true}});
    uint64 frame = RunFramesUntil(residency, 0, [&] { return oldFill.Started.load(); });
    ASSERT_TRUE(oldFill.Started) << "the old source's fill never started";

    residency.ChangeGeneratedPlanes(3, PageSource{nullptr, nullptr, {}, MakeGatedPlanes(2u, newFill)});
    frame = RunFramesUntil(residency, frame, [&] { return newFill.Started.load(); });
    ASSERT_TRUE(newFill.Started) << "the new source's fill never started";
    oldFill.Released = true;
    frame = RunFramesUntil(residency, frame, [&] { return oldFill.Finished.load(); });
    for (uint32 drain = 0; drain < kOldFillDrainFrames; ++drain)
    {
        residency.Update(++frame, 1.0f / 60.0f, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(residency.Bytes(3, page), nullptr) << "the old source's fill landed";

    newFill.Released = true;
    RunFramesUntil(residency, frame, [&] { return residency.Bytes(3, page) != nullptr; });
    const std::vector<uint8>* bytes = residency.Bytes(3, page);
    ASSERT_NE(bytes, nullptr) << "the page never arrived";
    EXPECT_TRUE(std::ranges::all_of(*bytes, [](uint8 byte) { return byte == 2u; }))
        << "the page holds the old source's bytes";
    residency.Forget(3);
    pool.Shutdown();
}

// PagesProduced counts every fill that returned bytes: the old source's fill that never lands and the
// new source's fill that lands. A stream that is not configured reads 0.
TEST(GeneratedPagePlanesTest, PagesProducedCountsEveryFinishedFillLandedOrDropped)
{
    FillGate oldFill;
    FillGate newFill;
    JobSystem::WorkStealingThreadPool pool(2);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(3, 4, 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {}, MakeGatedPlanes(1u, oldFill)});
    EXPECT_EQ(residency.PagesProduced(3), 0u) << "a stream with no finished fill";
    EXPECT_EQ(residency.PagesProduced(4), 0u) << "a stream that is not configured";
    const PageAddress page{0, 0, 0, 0};
    residency.Request(3, std::vector<PageWant>{PageWant{page, 0.0f, true}});
    uint64 frame = RunFramesUntil(residency, 0, [&] { return oldFill.Started.load(); });
    ASSERT_TRUE(oldFill.Started) << "the old source's fill never started";

    residency.ChangeGeneratedPlanes(3, PageSource{nullptr, nullptr, {}, MakeGatedPlanes(2u, newFill)});
    frame = RunFramesUntil(residency, frame, [&] { return newFill.Started.load(); });
    ASSERT_TRUE(newFill.Started) << "the new source's fill never started";
    oldFill.Released = true;
    frame = RunFramesUntil(residency, frame, [&] { return residency.PagesProduced(3) == 1u; });
    EXPECT_EQ(residency.PagesProduced(3), 1u) << "the old source's fill was not counted";
    EXPECT_EQ(residency.Bytes(3, page), nullptr) << "the old source's fill landed";

    newFill.Released = true;
    RunFramesUntil(residency, frame, [&] { return residency.Bytes(3, page) != nullptr; });
    ASSERT_NE(residency.Bytes(3, page), nullptr) << "the page never arrived";
    EXPECT_EQ(residency.PagesProduced(3), 2u) << "the fill that landed was not counted";
    residency.Forget(3);
    EXPECT_EQ(residency.PagesProduced(3), 0u) << "a forgotten stream";
    pool.Shutdown();
}

// A stream over planes has no CPU readers: a page's bytes are readable in the frame it is uploaded,
// then dropped at the next Update, and a page that stays resident is not fetched again.
TEST_F(PageStoreTest, AStreamOverPlanesDropsAPagesBytesAfterItsUploadAndDoesNotFetchItAgain)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    auto access = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(*access, m_Dir / "live.gepage", expected), "");
    JobSystem::WorkStealingThreadPool pool(1);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(3, 4, 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {access}});
    const PageAddress top{0, static_cast<uint8>(levels.size() - 1u), 0, 0};
    const std::vector<PageWant> wanted{PageWant{top, 0.0f, true}};
    residency.Request(3, wanted);
    uint64 frame = RunFramesUntil(
        residency, 0, [&] { return residency.FieldCache(3).SlotOf(CachedPage{3, top}) != kNoResidentSlot; });
    ASSERT_NE(residency.FieldCache(3).SlotOf(CachedPage{3, top}), kNoResidentSlot) << "the page never arrived";
    ASSERT_EQ(residency.FieldCache(3).Uploads().size(), 1u);
    ASSERT_NE(residency.Bytes(3, top), nullptr) << "the owner reads the bytes of an upload after its Update";

    uint32 loads = 0;
    for (uint32 i = 0; i < 10u; ++i)
    {
        residency.Request(3, wanted);
        residency.Update(++frame, 1.0f / 60.0f, false);
        pool.Submit([] {}, JobSystem::JobPriority::Background).Wait();
        loads += residency.LoadsStartedThisFrame();
    }
    EXPECT_EQ(residency.Bytes(3, top), nullptr) << "the uploaded page's bytes stayed on the CPU";
    EXPECT_EQ(residency.CpuBytes(), 0u);
    EXPECT_NE(residency.FieldCache(3).SlotOf(CachedPage{3, top}), kNoResidentSlot);
    EXPECT_EQ(loads, 0u) << "a resident page was fetched again";
    EXPECT_EQ(residency.PageVisitsThisFrame(), 0u) << "a parked frame walked the resident page";
    residency.Forget(3);
    pool.Shutdown();
    access->Close();
}

// A bake writes a resident page of a stream over planes again and commits a level-0 rect the
// page reads: the page reloads and is rewritten in its slot with the new bytes.
TEST_F(PageStoreTest, AResidentPageOfAStreamOverPlanesThatABakeWritesAgainIsRewrittenInItsSlot)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    const std::filesystem::path live = m_Dir / "live.gepage";
    auto access = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(*access, live, expected), "");
    PageStoreLayout layout;
    {
        PageStoreReader before;
        ASSERT_EQ(before.Open(live, PageFieldKind::Splat), "");
        layout = before.Layout();
    }
    ASSERT_EQ(access->OpenForPatch(live, std::move(layout)), "");

    JobSystem::WorkStealingThreadPool pool(1);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(3, 4, 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {access}});
    const PageAddress page{0, static_cast<uint8>(levels.size() - 1u), 0, 0};
    const CachedPage cached{3, page};
    residency.Request(3, std::vector<PageWant>{PageWant{page, 0.0f, true}});
    uint64 frame = RunFramesUntil(residency, 0, [&] { return residency.FieldCache(3).SlotOf(cached) != kNoResidentSlot; });
    const uint32 slot = residency.FieldCache(3).SlotOf(cached);
    ASSERT_NE(slot, kNoResidentSlot) << "the page never arrived";
    ASSERT_NE(residency.Bytes(3, page), nullptr);
    const std::vector<uint8> first = *residency.Bytes(3, page);
    residency.Update(++frame, 1.0f / 60.0f, false);
    ASSERT_TRUE(residency.FieldCache(3).Uploads().empty());

    std::vector<uint8> texels = SurfacePage(expected, page);
    for (uint8& texel : texels)
        texel = static_cast<uint8>(255u - texel);
    {
        PageStorePatcher patcher(access->Writer(), nullptr);
        ASSERT_TRUE(patcher.WritePage(page, texels));
    }
    access->Commit(Level0RowsRect(levels, 0u, 1u), 0u);
    const auto rewritten = [&] {
        for (const PageUploadRequest& upload : residency.FieldCache(3).Uploads())
            if (upload.Page == cached)
                return true;
        return false;
    };
    RunFramesUntil(residency, frame, [&] {
        pool.Submit([] {}, JobSystem::JobPriority::Background).Wait();
        return rewritten();
    });
    ASSERT_TRUE(rewritten()) << "the page the bake wrote again was not rewritten";
    EXPECT_EQ(residency.FieldCache(3).SlotOf(cached), slot) << "the rewrite left the page's slot";
    ASSERT_NE(residency.Bytes(3, page), nullptr);
    EXPECT_NE(*residency.Bytes(3, page), first) << "the page was rewritten with its old bytes";
    residency.Forget(3);
    pool.Shutdown();
    access->Close();
}

// A bake writes the resident pages of levels 1 to 3 over a rect again and commits level 2: the
// pages of level 2 and coarser are rewritten in their slots, and the level-1 page waits for a
// commit of its own level.
TEST_F(PageStoreTest, ACommitOfALevelRewritesTheResidentPagesOfThatLevelAndCoarserAndNoFinerPage)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    ASSERT_EQ(levels.size(), 4u);
    const std::filesystem::path live = m_Dir / "live.gepage";
    auto access = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(*access, live, expected), "");
    PageStoreLayout layout;
    {
        PageStoreReader before;
        ASSERT_EQ(before.Open(live, PageFieldKind::Splat), "");
        layout = before.Layout();
    }
    ASSERT_EQ(access->OpenForPatch(live, std::move(layout)), "");

    JobSystem::WorkStealingThreadPool pool(1);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(3, 8, 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {access}});
    std::vector<PageWant> wanted;
    for (uint8 level = 1; level < levels.size(); ++level)
        wanted.push_back(PageWant{PageAddress{0, level, 0, 0}, 0.0f, true});
    residency.Request(3, wanted);
    const auto allResident = [&] {
        return std::ranges::all_of(wanted, [&](const PageWant& want) {
            return residency.FieldCache(3).SlotOf(CachedPage{3, want.Address}) != kNoResidentSlot;
        });
    };
    uint64 frame = RunFramesUntil(residency, 0, allResident);
    ASSERT_TRUE(allResident()) << "the pages never arrived";
    residency.Update(++frame, 1.0f / 60.0f, false);
    ASSERT_TRUE(residency.FieldCache(3).Uploads().empty());

    {
        PageStorePatcher patcher(access->Writer(), nullptr);
        for (const PageWant& want : wanted)
        {
            std::vector<uint8> texels = SurfacePage(expected, want.Address);
            for (uint8& texel : texels)
                texel = static_cast<uint8>(255u - texel);
            ASSERT_TRUE(patcher.WritePage(want.Address, texels));
        }
    }
    std::vector<PageAddress> rewritten;
    const auto rewrittenLevels = [&](uint32 firstLevel) {
        pool.Submit([] {}, JobSystem::JobPriority::Background).Wait();
        for (const PageUploadRequest& upload : residency.FieldCache(3).Uploads())
            if (std::ranges::find(rewritten, upload.Page.Address) == rewritten.end())
                rewritten.push_back(upload.Page.Address);
        return std::ranges::count_if(rewritten, [&](const PageAddress& a) { return a.Level >= firstLevel; }) ==
               static_cast<std::ptrdiff_t>(levels.size() - firstLevel);
    };
    access->Commit(Level0RowsRect(levels, 0u, 1u), 2u);
    frame = RunFramesUntil(residency, frame, [&] { return rewrittenLevels(2u); });
    ASSERT_TRUE(rewrittenLevels(2u)) << "a page of the committed level or coarser was not rewritten";
    for (uint32 i = 0; i < 8u; ++i)
    {
        residency.Update(++frame, 1.0f / 60.0f, false);
        rewrittenLevels(2u);
    }
    EXPECT_EQ(std::ranges::find(rewritten, PageAddress{0, 1, 0, 0}), rewritten.end())
        << "a commit of level 2 rewrote the level-1 page under it";

    access->Commit(Level0RowsRect(levels, 0u, 1u), 1u);
    RunFramesUntil(residency, frame, [&] { return rewrittenLevels(1u); });
    EXPECT_TRUE(rewrittenLevels(1u)) << "a commit of level 1 did not rewrite the level-1 page";
    residency.Forget(3);
    pool.Shutdown();
    access->Close();
}

// A stream over one plane gets a second plane: its resident page keeps its slot and is rewritten
// with both planes' bytes, and the stream refreshes until then.
TEST_F(PageStoreTest, AStreamThatChangesItsPlanesRewritesItsResidentPagesInTheirSlots)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    auto first = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    auto second = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(*first, m_Dir / "first.gepage", expected), "");
    ASSERT_EQ(CreateCoarseStore(*second, m_Dir / "second.gepage", expected), "");

    JobSystem::WorkStealingThreadPool pool(1);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(3, 4, 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {first}});
    const PageAddress page{0, static_cast<uint8>(levels.size() - 1u), 0, 0};
    const CachedPage cached{3, page};
    residency.Request(3, std::vector<PageWant>{PageWant{page, 0.0f, true}});
    uint64 frame = RunFramesUntil(residency, 0, [&] { return residency.FieldCache(3).SlotOf(cached) != kNoResidentSlot; });
    const uint32 slot = residency.FieldCache(3).SlotOf(cached);
    ASSERT_NE(slot, kNoResidentSlot) << "the page never arrived";
    const std::vector<uint8> one = *residency.Bytes(3, page);
    residency.Update(++frame, 1.0f / 60.0f, false);
    EXPECT_FALSE(residency.Refreshing(3));

    residency.ChangePlanes(3, PageSource{nullptr, nullptr, {first, second}});
    EXPECT_TRUE(residency.Refreshing(3)) << "the resident page was not marked to reload";
    EXPECT_EQ(residency.FieldCache(3).SlotOf(cached), slot) << "the plane change released the resident page";
    const auto rewritten = [&] {
        for (const PageUploadRequest& upload : residency.FieldCache(3).Uploads())
            if (upload.Page == cached)
                return true;
        return false;
    };
    RunFramesUntil(residency, frame, [&] {
        pool.Submit([] {}, JobSystem::JobPriority::Background).Wait();
        return rewritten();
    });
    ASSERT_TRUE(rewritten()) << "the resident page was not rewritten with the new planes";
    EXPECT_EQ(residency.FieldCache(3).SlotOf(cached), slot);
    EXPECT_FALSE(residency.Refreshing(3));
    ASSERT_NE(residency.Bytes(3, page), nullptr);
    std::vector<uint8> both = one;
    both.insert(both.end(), one.begin(), one.end());
    EXPECT_EQ(*residency.Bytes(3, page), both);
    residency.Forget(3);
    pool.Shutdown();
    first->Close();
    second->Close();
}

// A load reads a page, then a bake writes the page again and commits a rect it reads, then the load
// ends: the GPU gets the bytes of after the commit.
TEST_F(PageStoreTest, ALoadThatReadBeforeACommitOverItsPageIsFetchedAgain)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    const std::filesystem::path live = m_Dir / "live.gepage";
    auto access = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(*access, live, expected), "");
    PageStoreLayout layout;
    {
        PageStoreReader before;
        ASSERT_EQ(before.Open(live, PageFieldKind::Splat), "");
        layout = before.Layout();
    }
    ASSERT_EQ(access->OpenForPatch(live, std::move(layout)), "");
    JobSystem::WorkStealingThreadPool pool(1);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(3, 4, 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {access}});
    const PageAddress page{0, static_cast<uint8>(levels.size() - 1u), 0, 0};
    const CachedPage cached{3, page};
    residency.Request(3, std::vector<PageWant>{PageWant{page, 0.0f, true}});
    uint64 frame = 0;
    residency.Update(++frame, 1.0f / 60.0f, false); // starts the load
    pool.Submit([] {}, JobSystem::JobPriority::Background).Wait(); // the load read the old bytes
    std::vector<uint8> texels = SurfacePage(expected, page);
    for (uint8& texel : texels)
        texel = static_cast<uint8>(255u - texel);
    {
        PageStorePatcher patcher(access->Writer(), nullptr);
        ASSERT_TRUE(patcher.WritePage(page, texels));
    }
    access->Commit(Level0RowsRect(levels, 0u, 1u), 0u);
    std::vector<uint8> lastUploaded;
    for (uint32 i = 0; i < 60; ++i)
    {
        residency.Update(++frame, 1.0f / 60.0f, false);
        for (const PageUploadRequest& upload : residency.FieldCache(3).Uploads())
            if (upload.Page == cached && residency.Bytes(3, page))
                lastUploaded = *residency.Bytes(3, page);
        pool.Submit([] {}, JobSystem::JobPriority::Background).Wait();
    }
    ASSERT_FALSE(lastUploaded.empty()) << "the page never uploaded";
    PageResidencyManager fresh(&pool, nullptr, PageResidencyBudget{});
    fresh.ConfigureField(3, 4, 2, 0.0f);
    fresh.Configure(3, 3, PageSource{nullptr, nullptr, {access}});
    fresh.Request(3, std::vector<PageWant>{PageWant{page, 0.0f, true}});
    RunFramesUntil(fresh, 0, [&] { return fresh.FieldCache(3).SlotOf(cached) != kNoResidentSlot; });
    ASSERT_NE(fresh.Bytes(3, page), nullptr);
    EXPECT_EQ(lastUploaded, *fresh.Bytes(3, page)) << "the GPU kept the bytes of before the commit";
    residency.Forget(3);
    fresh.Forget(3);
    pool.Shutdown();
    access->Close();
}

// A page of a stream over planes is loaded and waits for a slot when a bake commits a rect it reads:
// its old bytes are dropped and the page is fetched again.
TEST_F(PageStoreTest, ALoadedPageThatWaitsForASlotIsFetchedAgainAfterACommitOverIt)
{
    PageStoreReader expected;
    ASSERT_EQ(expected.Open(BuildLiveReadSource(m_Dir), PageFieldKind::Splat), "");
    const std::vector<PageStoreLevel>& levels = expected.Layout().Levels;
    const std::filesystem::path live = m_Dir / "live.gepage";
    auto access = std::make_shared<PageStoreAccess>(PageFieldKind::Splat, expected.Layout().Header);
    ASSERT_EQ(CreateCoarseStore(*access, live, expected), "");
    PageStoreLayout layout;
    {
        PageStoreReader before;
        ASSERT_EQ(before.Open(live, PageFieldKind::Splat), "");
        layout = before.Layout();
    }
    ASSERT_EQ(access->OpenForPatch(live, std::move(layout)), "");
    JobSystem::WorkStealingThreadPool pool(1);
    PageResidencyBudget budget;
    budget.UploadsPerFrame = 0; // the page loads and is never placed
    PageResidencyManager residency(&pool, nullptr, budget);
    residency.ConfigureField(3, 4, 2, 0.0f);
    residency.Configure(3, 3, PageSource{nullptr, nullptr, {access}});
    const PageAddress page{0, static_cast<uint8>(levels.size() - 1u), 0, 0};
    residency.Request(3, std::vector<PageWant>{PageWant{page, 0.0f, true}});
    uint64 frame = RunFramesUntil(residency, 0, [&] {
        pool.Submit([] {}, JobSystem::JobPriority::Background).Wait();
        return residency.Bytes(3, page) != nullptr;
    });
    ASSERT_NE(residency.Bytes(3, page), nullptr) << "the page never loaded";
    ASSERT_EQ(residency.FieldCache(3).SlotOf(CachedPage{3, page}), kNoResidentSlot);
    const std::vector<uint8> before = *residency.Bytes(3, page);
    std::vector<uint8> texels = SurfacePage(expected, page);
    for (uint8& texel : texels)
        texel = static_cast<uint8>(255u - texel);
    {
        PageStorePatcher patcher(access->Writer(), nullptr);
        ASSERT_TRUE(patcher.WritePage(page, texels));
    }
    access->Commit(Level0RowsRect(levels, 0u, 1u), 0u);
    RunFramesUntil(residency, frame, [&] {
        pool.Submit([] {}, JobSystem::JobPriority::Background).Wait();
        const std::vector<uint8>* bytes = residency.Bytes(3, page);
        return bytes && *bytes != before;
    });
    ASSERT_NE(residency.Bytes(3, page), nullptr);
    EXPECT_NE(*residency.Bytes(3, page), before) << "the waiting page kept the bytes of before the commit";
    residency.Forget(3);
    pool.Shutdown();
    access->Close();
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
    TerrainECS::PageRequester requester;
    requester.Configure(terrain, TerrainECS::PageRequestArea::Disc);
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
        requester.Build(DiscCamera(far), view, wants);
        residency.Request(1, wants);
        return residency.FieldCache(1).ResidentCount() == pages;
    });
    ASSERT_EQ(residency.FieldCache(1).ResidentCount(), pages);
    for (int i = 0; i < 10; ++i)
    {
        requester.Build(DiscCamera(far), view, wants);
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

} // namespace

// ---- Height page layer (the sparse delta the modifiers and the sculpt write) ---------------------------

namespace
{

// The pyramid sample a page of a one-face layer stores at stride (i, j): the clamped sample of its level.
float32 StoredPyramidSample(const std::vector<std::vector<float32>>& pyramid, const std::vector<PageStoreLevel>& levels,
                            const PageAddress& page, uint32 i, uint32 j)
{
    const PageStoreLevel& level = levels[page.Level];
    const int64 x = std::clamp<int64>(int64{page.X} * kPageOwnedSamples - kPageApronSamples + i, 0, level.SamplesX - 1);
    const int64 z = std::clamp<int64>(int64{page.Z} * kPageOwnedSamples - kPageApronSamples + j, 0, level.SamplesZ - 1);
    return pyramid[page.Level][static_cast<std::size_t>(z) * level.SamplesX + static_cast<std::size_t>(x)];
}

// Every page of every level of `layer` holds the tent pyramid of `level0`, bit for bit, apron included; a
// page it does not hold is zero there. Returns the pages that hold a non-zero sample.
uint32 ExpectLayerIsThePyramidOf(const HeightPageLayer& layer, const std::vector<float32>& level0,
                                 const std::vector<PageStoreLevel>& levels)
{
    const std::vector<std::vector<float32>> pyramid = TentPyramid(level0, levels);
    uint32 nonZero = 0;
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
            {
                const PageAddress page{0, uint8(level), x, z};
                const std::span<const float32> deltas = layer.PageDeltas(page);
                bool any = false;
                for (uint32 j = 0; j < kPageStrideSamples; ++j)
                    for (uint32 i = 0; i < kPageStrideSamples; ++i)
                    {
                        const float32 expected = StoredPyramidSample(pyramid, levels, page, i, j);
                        any = any || expected != 0.0f;
                        const float32 held = deltas.empty() ? 0.0f : deltas[j * kPageStrideSamples + i];
                        if (held != expected)
                        {
                            ADD_FAILURE() << "level " << level << " page (" << x << ", " << z << ") stride (" << i
                                          << ", " << j << "): " << held << " held, " << expected << " expected";
                            return nonZero;
                        }
                    }
                nonZero += any ? 1u : 0u;
            }
    return nonZero;
}

void WriteDense(std::vector<float32>& level0, const PageStoreLevel& shape, const PageSampleRect& rect,
                std::span<const float32> deltas)
{
    for (uint32 z = rect.MinZ; z <= rect.MaxZ; ++z)
        for (uint32 x = rect.MinX; x <= rect.MaxX; ++x)
            level0[static_cast<std::size_t>(z) * shape.SamplesX + x] =
                deltas[static_cast<std::size_t>(z - rect.MinZ) * rect.Width() + (x - rect.MinX)];
}

} // namespace

// A random sequence of writes, some of zeros: after each, every level is the tent pyramid of the written
// level 0 (the closed form), and the layer holds only the pages with a non-zero sample.
TEST(HeightPageLayerTest, EveryLevelIsTheTentPyramidOfTheWritesAndOnlyNonZeroPagesAreHeld)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(601, 300);
    const PageSampleRect whole{0, 0, levels[0].SamplesX - 1u, levels[0].SamplesZ - 1u};
    std::vector<float32> level0(static_cast<std::size_t>(levels[0].SamplesX) * levels[0].SamplesZ, 0.0f);
    HeightPageLayer layer(1, levels);
    std::mt19937 random(5);
    for (int write = 0; write < 24; ++write)
    {
        const uint32 x0 = random() % levels[0].SamplesX;
        const uint32 z0 = random() % levels[0].SamplesZ;
        const PageSampleRect rect{x0, z0, std::min(x0 + static_cast<uint32>(random() % 200u), whole.MaxX),
                                  std::min(z0 + static_cast<uint32>(random() % 150u), whole.MaxZ)};
        std::vector<float32> deltas = RectDelta(rect, static_cast<float32>(write));
        if (write % 4 == 3)
            std::fill(deltas.begin(), deltas.end(), 0.0f);
        layer.WriteLevel0(0, rect, deltas);
        WriteDense(level0, levels[0], rect, deltas);
        const uint32 nonZero = ExpectLayerIsThePyramidOf(layer, level0, levels);
        EXPECT_EQ(layer.Bytes(), uint64{nonZero} * kPageSampleCount * sizeof(float32))
            << "write " << write << ": a page of zeros is held";
    }
}

TEST(HeightPageLayerTest, AWriteOfZerosOverEveryWrittenSampleLeavesNoPage)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(601, 300);
    HeightPageLayer layer(1, levels);
    const PageSampleRect rect{100, 40, 250, 170};
    layer.WriteLevel0(0, rect, RectDelta(rect, 0.5f));
    EXPECT_GT(layer.Bytes(), 0u);
    EXPECT_LE(layer.Bytes(), HeightPageLayerBytesOver(levels, std::span(&rect, 1))) << "the estimate bounds the pages";
    layer.WriteLevel0(0, rect, std::vector<float32>(rect.Width() * rect.Height(), 0.0f));
    EXPECT_EQ(layer.Bytes(), 0u);
}

// Two small writes at opposite corners of a large terrain hold the pages under them, not the rect between.
TEST(HeightPageLayerTest, TwoFarWritesHoldOnlyThePagesUnderThem)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(32769, 32769);
    const std::array<PageSampleRect, 2> rects{PageSampleRect{10, 10, 200, 200}, PageSampleRect{32500, 32500, 32700, 32700}};
    HeightPageLayer layer(1, levels);
    for (const PageSampleRect& rect : rects)
        layer.WriteLevel0(0, rect, std::vector<float32>(rect.Width() * rect.Height(), 1.0f));
    const uint64 estimate = HeightPageLayerBytesOver(levels, rects);
    EXPECT_LE(layer.Bytes(), estimate);
    EXPECT_LT(estimate, 64ull << 20) << "under an eighth of the 512 MiB desktop cap";
    EXPECT_GT(layer.Bytes(), 0u);
}

// A write one sample in from face 0's side changes the pages of the neighbor face past that side: their
// apron holds face 0's samples one in (PageCubeSampleAt), their own samples stay zero, and no other face
// holds a page. RefreshRects names the neighbor's edge band.
TEST(HeightPageLayerTest, AWriteNearAFaceSideChangesTheNeighborFacesApronOnly)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(257, 257);
    HeightPageLayer layer(kPageStoreMaxFaces, levels);
    const PageSampleRect rect{0, 100, 6, 140};
    layer.WriteLevel0(0, rect, std::vector<float32>(rect.Width() * rect.Height(), 2.0f));
    const PageFaceEdge edge = PageFaceNeighbor(0, PageFaceSide::MinX);
    uint32 neighborPages = 0;
    for (uint8 face = 1; face < kPageStoreMaxFaces; ++face)
        for (uint32 level = 0; level < levels.size(); ++level)
            for (uint32 z = 0; z < levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < levels[level].PagesX; ++x)
                {
                    const PageAddress page{face, uint8(level), x, z};
                    const std::span<const float32> deltas = layer.PageDeltas(page);
                    if (deltas.empty())
                        continue;
                    ASSERT_EQ(face, edge.Face) << "a face that shares no side with the write holds a page";
                    ++neighborPages;
                    const uint32 samples = levels[level].SamplesX;
                    for (uint32 j = 0; j < kPageStrideSamples; ++j)
                        for (uint32 i = 0; i < kPageStrideSamples; ++i)
                        {
                            const int64 sx = int64{x} * kPageOwnedSamples - 1 + i;
                            const int64 sz = int64{z} * kPageOwnedSamples - 1 + j;
                            const PageFaceSample source = PageCubeSampleAt(face, sx, sz, samples);
                            float32 expected = 0.0f;
                            if (source.Face == 0)
                            {
                                const std::span<const float32> own = layer.PageDeltas(PageAddress{
                                    0, uint8(level), source.X / kPageOwnedSamples, source.Z / kPageOwnedSamples});
                                if (!own.empty())
                                    expected = own[(source.Z % kPageOwnedSamples + 1u) * kPageStrideSamples +
                                                   source.X % kPageOwnedSamples + 1u];
                            }
                            ASSERT_EQ(deltas[j * kPageStrideSamples + i], expected)
                                << "face " << int(face) << " level " << level << " page (" << x << ", " << z
                                << ") stride (" << i << ", " << j << ")";
                        }
                }
    EXPECT_GT(neighborPages, 0u) << "the neighbor's apron past the shared side reads the write";
    std::vector<PageFaceRect> rects;
    layer.RefreshRects(0, rect, kPageApronSamples, rects);
    ASSERT_EQ(rects.size(), 2u);
    EXPECT_EQ(rects[1].Face, edge.Face);
    const uint32 last0 = levels.front().SamplesX - 1u;
    const uint32 across = edge.Side == PageFaceSide::MinX || edge.Side == PageFaceSide::MinZ ? 0u : last0;
    const bool column = edge.Side == PageFaceSide::MinX || edge.Side == PageFaceSide::MaxX;
    const PageSampleRect& band = rects[1].Level0Rect;
    const auto [acrossMin, acrossMax, alongMin, alongMax] =
        column ? std::tie(band.MinX, band.MaxX, band.MinZ, band.MaxZ) : std::tie(band.MinZ, band.MaxZ, band.MinX, band.MaxX);
    EXPECT_EQ(std::tie(acrossMin, acrossMax), std::tie(across, across)) << "the band is not the neighbor's edge sample";
    EXPECT_LE(alongMin, edge.Reversed ? last0 - rect.MaxZ : rect.MinZ) << "the band misses rows the write reaches";
    EXPECT_GE(alongMax, edge.Reversed ? last0 - rect.MinZ : rect.MaxZ) << "the band misses rows the write reaches";
}

// On a face of 1900545 samples the last page along an axis owns one column; the rest of its samples lie
// past the face side. A write at that side refreshes such a page at every level, on both faces, and each
// apron refresh finds each page it reads once: at most the eight pages around it and three a side past it.
TEST(HeightPageLayerTest, AnApronRefreshAtACubeFaceSideFindsEachPageItReadsOnce)
{
    constexpr uint32 kFaceSamples = 1900545;
    constexpr uint64 kMostLookupsAPage = 16;
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(kFaceSamples, kFaceSamples);
    HeightPageLayer layer(kPageStoreMaxFaces, levels);
    const PageSampleRect rect{kFaceSamples - 6u, 1000, kFaceSamples - 1u, 1010};
    layer.AddAtLevel(0, 0, rect, std::vector<float32>(rect.Width() * rect.Height(), 2.0f));
    const HeightPageLayerApronWork& work = layer.ApronWork();
    ASSERT_GT(work.Pages, 0u);
    EXPECT_LE(work.Lookups, kMostLookupsAPage * work.Pages)
        << work.Lookups << " page lookups for " << work.Pages << " apron refreshes";
}

namespace
{

// True when stride `stride` of page `page` along an axis of `samples` samples holds a sample the page owns.
bool IsOwnedStride(uint32 page, uint32 stride, uint32 samples)
{
    const int64 first = int64{page} * kPageOwnedSamples;
    const int64 sample = first - kPageApronSamples + stride;
    return sample >= first && sample <= std::min<int64>(first + kPageOwnedSamples - 1, int64{samples} - 1);
}

// The composed pages of `before` (TakeCapture) whose apron, the samples a page stores but does not own,
// differs now.
uint64 PagesWithChangedApron(const HeightPageLayer& layer, const std::vector<HeightPageLayerPage>& before)
{
    const std::vector<HeightPageLayerPage> now = layer.CopyPages(before);
    uint64 changed = 0;
    for (std::size_t page = 0; page < before.size(); ++page)
    {
        if (before[page].Written)
            continue;
        const PageAddress& address = before[page].Address;
        const PageStoreLevel& shape = layer.Levels()[address.Level];
        bool differs = false;
        for (uint32 strideZ = 0; strideZ < kPageStrideSamples && !differs; ++strideZ)
            for (uint32 strideX = 0; strideX < kPageStrideSamples && !differs; ++strideX)
            {
                if (IsOwnedStride(address.X, strideX, shape.SamplesX) &&
                    IsOwnedStride(address.Z, strideZ, shape.SamplesZ))
                    continue;
                const std::size_t index = std::size_t{strideZ} * kPageStrideSamples + strideX;
                const float32 was = before[page].Deltas.empty() ? 0.0f : before[page].Deltas[index];
                const float32 is = now[page].Deltas.empty() ? 0.0f : now[page].Deltas[index];
                differs = was != is;
            }
        changed += differs ? 1u : 0u;
    }
    return changed;
}

} // namespace

// A level-0 dab refreshes the apron of a page only where the dab changes a sample the apron stores: the
// pages it refreshes are the pages whose apron changes, inside a face and at a face side.
TEST(HeightPageLayerTest, ADabRefreshesOnlyTheApronsItChanges)
{
    constexpr uint32 kFaceSamples = 1900545;
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(kFaceSamples, kFaceSamples);
    for (const PageSampleRect& rect :
         {PageSampleRect{1000, 1000, 1010, 1010}, PageSampleRect{kFaceSamples - 7u, 1000, kFaceSamples - 2u, 1010}})
    {
        HeightPageLayer layer(kPageStoreMaxFaces, levels);
        layer.BeginCapture();
        layer.AddAtLevel(0, 0, rect, std::vector<float32>(rect.Width() * rect.Height(), 2.0f));
        const uint64 changed = PagesWithChangedApron(layer, layer.TakeCapture());
        EXPECT_GT(changed, 0u) << "rect min x " << rect.MinX;
        EXPECT_EQ(layer.ApronWork().Pages, changed) << "apron refreshes for rect min x " << rect.MinX;
    }
}

namespace
{

// The bilinear prolongation of a level of `coarser` samples to the next finer level: sample x reads
// coarser samples x / 2 and (x + 1) / 2, halfway between them when x is odd.
std::vector<float32> ProlongDense(const std::vector<float32>& coarse, const PageStoreLevel& coarser,
                                  const PageStoreLevel& finer)
{
    std::vector<float32> out(static_cast<std::size_t>(finer.SamplesX) * finer.SamplesZ);
    const auto at = [&](uint32 x, uint32 z) {
        return coarse[static_cast<std::size_t>(std::min(z, coarser.SamplesZ - 1u)) * coarser.SamplesX +
                      std::min(x, coarser.SamplesX - 1u)];
    };
    for (uint32 z = 0; z < finer.SamplesZ; ++z)
        for (uint32 x = 0; x < finer.SamplesX; ++x)
        {
            const float32 row0 = 0.5f * (at(x / 2u, z / 2u) + at((x + 1u) / 2u, z / 2u));
            const float32 row1 = 0.5f * (at(x / 2u, (z + 1u) / 2u) + at((x + 1u) / 2u, (z + 1u) / 2u));
            out[static_cast<std::size_t>(z) * finer.SamplesX + x] = 0.5f * (row0 + row1);
        }
    return out;
}

// Every page of `layer` (one face) composed on zeros: the address and its kPageSampleCount samples.
std::vector<std::pair<PageAddress, std::vector<float32>>> ComposedPages(const HeightPageLayer& layer,
                                                                        const std::vector<PageStoreLevel>& levels)
{
    std::vector<std::pair<PageAddress, std::vector<float32>>> out;
    for (uint8 face = 0; face < layer.FaceCount(); ++face)
        for (uint32 level = 0; level < levels.size(); ++level)
            for (uint32 z = 0; z < levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < levels[level].PagesX; ++x)
                {
                    const PageAddress page{face, uint8(level), x, z};
                    std::vector<float32> samples(kPageSampleCount, 0.0f);
                    if (layer.ApplyToPage(page, samples))
                        out.emplace_back(page, std::move(samples));
                }
    return out;
}

} // namespace

TEST(HeightPageLayerTest, WriteLevelIsTheFinestLevelWhoseGridHoldsTheFootprintInFourPagesAcross)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(32769, 32769);
    const HeightPageLayer layer(kPageStoreMaxFaces, levels);
    const uint32 most = HeightPageLayer::kWritePagesAcross * kPageOwnedSamples;
    EXPECT_EQ(layer.WriteLevel(1), 0u);
    EXPECT_EQ(layer.WriteLevel(most), 0u) << "a resolvable brush writes level 0";
    EXPECT_EQ(layer.WriteLevel(most + 1u), 1u);
    EXPECT_EQ(layer.WriteLevel(2u * most), 1u);
    EXPECT_EQ(layer.WriteLevel(2u * most + 1u), 2u);
    EXPECT_EQ(layer.WriteLevel(20000u), 6u) << "a 5 km brush at 0.5 m (20,000 samples) writes 313 samples across";
    EXPECT_EQ(layer.WriteLevel(~0u), uint32(levels.size() - 1u)) << "a larger footprint writes the coarsest level";
}

// A write at level 2 reads, at every page of every level, as the write itself at level 2, its bilinear
// prolongation at the finer levels and its tent at the coarser ones, aprons included.
TEST(HeightPageLayerTest, ACoarseWriteReadsAsItsProlongationBelowAndItsTentAbove)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(1025, 1025);
    constexpr uint32 kLevel = 2;
    const PageSampleRect rect{60, 70, 200, 150};
    const std::vector<float32> deltas = RectDelta(rect, 1.5f);
    HeightPageLayer layer(1, levels);
    const PageSampleRect level0 = layer.AddAtLevel(0, kLevel, rect, deltas);
    EXPECT_EQ(level0.MinX, (rect.MinX << kLevel) - (1u << kLevel));
    EXPECT_EQ(level0.MaxZ, (rect.MaxZ << kLevel) + (1u << kLevel));

    std::vector<float32> written(static_cast<std::size_t>(levels[kLevel].SamplesX) * levels[kLevel].SamplesZ, 0.0f);
    WriteDense(written, levels[kLevel], rect, deltas);
    std::vector<std::vector<float32>> expected(levels.size());
    const std::vector<PageStoreLevel> fromLevel(levels.begin() + kLevel, levels.end());
    std::vector<std::vector<float32>> above = TentPyramid(written, fromLevel);
    for (std::size_t level = kLevel; level < levels.size(); ++level)
        expected[level] = std::move(above[level - kLevel]);
    for (uint32 level = kLevel; level-- > 0;)
        expected[level] = ProlongDense(expected[level + 1u], levels[level + 1u], levels[level]);

    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
            {
                const PageAddress page{0, uint8(level), x, z};
                std::vector<float32> samples(kPageSampleCount, 0.0f);
                layer.ApplyToPage(page, samples);
                for (uint32 j = 0; j < kPageStrideSamples; ++j)
                    for (uint32 i = 0; i < kPageStrideSamples; ++i)
                        ASSERT_NEAR(samples[j * kPageStrideSamples + i],
                                    StoredPyramidSample(expected, levels, page, i, j), 1e-6f)
                            << "level " << level << " page (" << x << ", " << z << ") stride (" << i << ", " << j << ")";
            }
}

// Two level-0 adds over overlapping rects hold the tent pyramid of their sum, bit for bit.
TEST(HeightPageLayerTest, LevelZeroAddsSumAndHoldTheTentPyramidOfTheSum)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(601, 300);
    std::vector<float32> level0(static_cast<std::size_t>(levels[0].SamplesX) * levels[0].SamplesZ, 0.0f);
    HeightPageLayer layer(1, levels);
    for (const auto& [rect, seed] : {std::pair{PageSampleRect{100, 40, 250, 170}, 0.5f},
                                     std::pair{PageSampleRect{180, 90, 400, 260}, 2.0f}})
    {
        const std::vector<float32> deltas = RectDelta(rect, seed);
        layer.AddAtLevel(0, 0, rect, deltas);
        for (uint32 z = rect.MinZ; z <= rect.MaxZ; ++z)
            for (uint32 x = rect.MinX; x <= rect.MaxX; ++x)
                level0[static_cast<std::size_t>(z) * levels[0].SamplesX + x] +=
                    deltas[static_cast<std::size_t>(z - rect.MinZ) * rect.Width() + (x - rect.MinX)];
    }
    ExpectLayerIsThePyramidOf(layer, level0, levels);
}

// A capture of adds at level 0 and level 2 on a planet, near a face side, restores the layer before and
// after the adds byte for byte, and names every face whose samples the restore changes.
TEST(HeightPageLayerTest, ACapturedStrokeRestoresBeforeAndAfterByteForByte)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(1025, 1025);
    HeightPageLayer layer(kPageStoreMaxFaces, levels);
    const PageSampleRect first{300, 300, 420, 380};
    layer.AddAtLevel(1, 0, first, RectDelta(first, 0.25f));
    const auto before = ComposedPages(layer, levels);
    const uint64 bytesBefore = layer.Bytes();

    layer.BeginCapture();
    const PageSampleRect nearSide{0, 200, 40, 300};
    layer.AddAtLevel(1, 0, nearSide, RectDelta(nearSide, 1.0f));
    const PageSampleRect coarse{20, 60, 90, 120};
    layer.AddAtLevel(1, 2, coarse, RectDelta(coarse, 3.0f));
    layer.AddAtLevel(1, 0, first, RectDelta(first, -0.25f));
    const std::vector<HeightPageLayerPage> pre = layer.TakeCapture();
    const std::vector<HeightPageLayerPage> post = layer.CopyPages(pre);
    const auto after = ComposedPages(layer, levels);
    const uint64 bytesAfter = layer.Bytes();
    ASSERT_FALSE(pre.empty());
    EXPECT_TRUE(std::ranges::any_of(pre, [](const HeightPageLayerPage& page) { return page.Written; }))
        << "the level-2 add keeps its written pages";
    EXPECT_NE(before, after);

    std::vector<PageFaceRect> changed;
    layer.RestorePages(pre, changed);
    EXPECT_EQ(ComposedPages(layer, levels), before) << "undo is not byte exact";
    EXPECT_EQ(layer.Bytes(), bytesBefore);
    EXPECT_TRUE(std::ranges::any_of(changed, [](const PageFaceRect& rect) { return rect.Face == 1; }));
    EXPECT_TRUE(std::ranges::any_of(changed, [](const PageFaceRect& rect) { return rect.Face != 1; }))
        << "the neighbor face's apron pages change too";

    changed.clear();
    layer.RestorePages(post, changed);
    EXPECT_EQ(ComposedPages(layer, levels), after) << "redo is not byte exact";
    EXPECT_EQ(layer.Bytes(), bytesAfter);
}

// ---- Height layer store (the authored layer saved as a .gepage) -----------------------------------------

namespace
{

// RectDelta scaled to meters and put on the authored layer's lattice, as a sculpt writes it.
std::vector<float32> LayerDelta(const PageSampleRect& rect, float32 seed)
{
    std::vector<float32> deltas = RectDelta(rect, seed);
    for (float32& delta : deltas)
        delta = QuantizeHeight(37.0f * delta, kHeightLayerStep);
    return deltas;
}

void ExpectSameAuthoredPages(const HeightPageLayer& loaded, const HeightPageLayer& saved)
{
    const std::vector<HeightPageLayerPage> expected = HeightLayerAuthoredPages::Export(saved);
    const std::vector<HeightPageLayerPage> actual = HeightLayerAuthoredPages::Export(loaded);
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(actual[i].Address, expected[i].Address);
        EXPECT_EQ(actual[i].Written, expected[i].Written);
        EXPECT_EQ(actual[i].Deltas, expected[i].Deltas) << "authored page " << i;
    }
}

} // namespace

// Adds at level 0 and level 2 of a planet, one near a face side: the loaded layer holds the saved layer's
// authored pages and composes every page of every level bit for bit.
TEST(HeightLayerStoreTest, ALoadedLayerHoldsEveryPageOfEveryLevelOfTheSavedLayerBitForBit)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(513, 513);
    HeightPageLayer layer(kPageStoreMaxFaces, levels);
    const PageSampleRect nearSide{0, 100, 60, 200};
    layer.AddAtLevel(1, 0, nearSide, LayerDelta(nearSide, 0.5f));
    const PageSampleRect coarse{10, 20, 60, 70};
    layer.AddAtLevel(1, 2, coarse, LayerDelta(coarse, 2.0f));
    const PageSampleRect overlap{40, 150, 300, 260};
    layer.AddAtLevel(1, 0, overlap, LayerDelta(overlap, -1.0f));
    const PageSampleRect otherFace{100, 100, 200, 190};
    layer.AddAtLevel(4, 1, otherFace, LayerDelta(otherFace, 3.0f));
    ASSERT_GT(layer.Bytes(), 0u);

    const std::filesystem::path path = TestUtils::MakeUniqueTempDirectory("height_layer") / "layer.gepage";
    ASSERT_EQ(SaveHeightLayer(layer, path), "");
    std::shared_ptr<HeightPageLayer> loaded;
    ASSERT_EQ(LoadHeightLayer(path, loaded), "");
    ASSERT_NE(loaded, nullptr);

    ExpectSameAuthoredPages(*loaded, layer);
    EXPECT_EQ(ComposedPages(*loaded, levels), ComposedPages(layer, levels)) << "a composed page differs after the load";
    EXPECT_EQ(loaded->Bytes(), layer.Bytes());
}

TEST(HeightLayerStoreTest, ALayerWithNoPageSavesNoFile)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(513, 513);
    HeightPageLayer layer(kPageStoreMaxFaces, levels);
    const PageSampleRect rect{100, 100, 200, 200};
    layer.AddAtLevel(2, 0, rect, LayerDelta(rect, 1.0f));
    layer.WriteLevel0(2, rect, std::vector<float32>(static_cast<std::size_t>(rect.Width()) * rect.Height(), 0.0f));
    ASSERT_EQ(layer.Bytes(), 0u);

    const std::filesystem::path path = TestUtils::MakeUniqueTempDirectory("height_layer_empty") / "layer.gepage";
    EXPECT_EQ(SaveHeightLayer(layer, path), "");
    EXPECT_FALSE(std::filesystem::exists(path));
}

// A store in the layer's shape whose header names another layer format version is refused by its path
// and its version, and the output layer is unchanged.
TEST(HeightLayerStoreTest, ALayerOfAnotherFormatVersionIsRefusedByItsPathAndVersion)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(257, 257);
    PageStoreHeader header;
    header.FaceCount = 1;
    header.SamplesX = levels[0].SamplesX;
    header.SamplesZ = levels[0].SamplesZ;
    header.Units = PageHeightUnits::LayerMeters;
    std::fill_n(header.StepPerLevel.begin(), levels.size(), kHeightLayerStep);
    header.Key = kHeightLayerFormatVersion + 1u;
    const PageAddress page{0, 0, 0, 0};
    std::optional<PageStoreLayout> layout = MakeSparsePageStoreLayout(header, std::span(&page, 1));
    ASSERT_TRUE(layout.has_value());

    const std::filesystem::path path = TestUtils::MakeUniqueTempDirectory("height_layer_version") / "layer.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(path, std::move(*layout)), "");
    EncodedPage encoded;
    EncodeHeightPage(writer.Layout().Header, 0, std::vector<float32>(kPageSampleCount, 1.0f), 1.0f, 1.0f, encoded);
    ASSERT_TRUE(writer.WritePage(page, encoded));
    ASSERT_EQ(writer.Finish(), "");

    std::shared_ptr<HeightPageLayer> loaded;
    const std::string reason = LoadHeightLayer(path, loaded);
    EXPECT_NE(reason.find(path.string()), std::string::npos) << reason;
    EXPECT_NE(reason.find("format " + std::to_string(kHeightLayerFormatVersion + 1u)), std::string::npos) << reason;
    EXPECT_EQ(loaded, nullptr);
}

// A layer saved before a change of the caches' store version loads after it: the store's version word
// is set to the next cache version, and the layer still loads bit for bit. A cache of that version is
// refused (PageStoreFileTest).
TEST(HeightLayerStoreTest, ALayerLoadsAfterTheCachesStoreVersionChanges)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(257, 257);
    HeightPageLayer layer(kPageStoreMaxFaces, levels);
    const PageSampleRect rect{20, 30, 120, 90};
    layer.AddAtLevel(3, 0, rect, LayerDelta(rect, 1.5f));
    const std::filesystem::path directory = TestUtils::MakeUniqueTempDirectory("height_layer_cache_version");
    const std::filesystem::path saved = directory / "saved.gepage";
    ASSERT_EQ(SaveHeightLayer(layer, saved), "");

    const std::filesystem::path bumped = directory / "bumped.gepage";
    {
        std::vector<uint8> bytes = FileBytes(saved);
        const uint32 nextCacheVersion = kPageStoreFormatVersion + 1u;
        std::memcpy(bytes.data() + sizeof(uint32), &nextCacheVersion, sizeof(nextCacheVersion));
        std::ofstream(bumped, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::shared_ptr<HeightPageLayer> loaded;
    ASSERT_EQ(LoadHeightLayer(bumped, loaded), "");
    ExpectSameAuthoredPages(*loaded, layer);
}

TEST(HeightLayerStoreTest, ASparseLayoutIndexesItsPagesInOrderAndRefusesARepeatOrAnOutsideAddress)
{
    PageStoreHeader header;
    header.FaceCount = kPageStoreMaxFaces;
    header.FaceMap = PageFaceMap::EqualAngleCube;
    header.SamplesX = 513;
    header.SamplesZ = 513;
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(513, 513);
    std::fill_n(header.StepPerLevel.begin(), levels.size(), kHeightLayerStep);

    const std::array<PageAddress, 4> pages{PageAddress{3, 1, 1, 0}, PageAddress{0, 0, 2, 3}, PageAddress{3, 0, 0, 1},
                                           PageAddress{0, 0, 1, 1}};
    const std::optional<PageStoreLayout> layout = MakeSparsePageStoreLayout(header, pages);
    ASSERT_TRUE(layout.has_value());
    ASSERT_EQ(layout->Entries.size(), pages.size());
    for (const PageAddress& page : pages)
    {
        const std::optional<std::size_t> index = layout->PresentIndex(page);
        ASSERT_TRUE(index.has_value()) << "face " << int(page.Face) << " level " << int(page.Level);
        EXPECT_EQ(PageOfMortonCode(page.Face, page.Level, layout->Entries[*index].Morton), page);
    }
    EXPECT_FALSE(layout->PresentIndex(PageAddress{0, 0, 0, 0}).has_value());

    const std::array<PageAddress, 2> repeated{PageAddress{2, 0, 1, 1}, PageAddress{2, 0, 1, 1}};
    EXPECT_FALSE(MakeSparsePageStoreLayout(header, repeated).has_value());
    const PageAddress outside{0, 0, levels[0].PagesX, 0};
    EXPECT_FALSE(MakeSparsePageStoreLayout(header, std::span(&outside, 1)).has_value());
    const PageAddress pastFaces{kPageStoreMaxFaces, 0, 0, 0};
    EXPECT_FALSE(MakeSparsePageStoreLayout(header, std::span(&pastFaces, 1)).has_value());
}

namespace
{

// The largest gap, at level `level` of a planet layer of `faceSamples` samples a side, between the
// layer's pages composed on zeros and `expected` at the direction of each lattice point the face owns.
// Fails the test when no page of that level is held.
float32 LargestGapAtLevel(const HeightPageLayer& layer, uint32 faceSamples, uint32 level,
                          const std::function<float32(const Mathematics::Vector3&)>& expected)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(faceSamples, faceSamples);
    const PageStoreLevel& shape = levels[level];
    float32 gap = 0.0f;
    bool any = false;
    std::vector<float32> samples(kPageSampleCount);
    for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
        for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
            for (uint32 px = 0; px < shape.PagesX; ++px)
            {
                std::fill(samples.begin(), samples.end(), 0.0f);
                if (!layer.ApplyToPage(PageAddress{face, uint8(level), px, pz}, samples))
                    continue;
                any = true;
                for (uint32 j = 0; j < kPageOwnedSamples; ++j)
                    for (uint32 i = 0; i < kPageOwnedSamples; ++i)
                    {
                        const uint32 x = px * kPageOwnedSamples + i;
                        const uint32 z = pz * kPageOwnedSamples + j;
                        if (x >= shape.SamplesX || z >= shape.SamplesZ)
                            continue;
                        const Mathematics::Vector3 direction = EqualAngleSampleDirection(
                            face, std::min(x << level, faceSamples - 1u), std::min(z << level, faceSamples - 1u),
                            faceSamples);
                        const float32 held = samples[static_cast<std::size_t>(j + kPageApronSamples) * kPageStrideSamples +
                                                     (i + kPageApronSamples)];
                        gap = std::max(gap, std::abs(held - expected(direction)));
                    }
            }
    EXPECT_TRUE(any) << "the layer holds no page at level " << level;
    return gap;
}

float32 DabSumAt(std::span<const TerrainECS::PlanetSculptDab> dabs, const Mathematics::Vector3& direction)
{
    float32 sum = 0.0f;
    for (const TerrainECS::PlanetSculptDab& dab : dabs)
        sum += dab.HeightAt(direction);
    return sum;
}

// A dab rounds each sample it writes to kHeightLayerStep, so `dabCount` dabs drift at most half a step
// each from their closed forms, plus float32 noise.
float32 DabRoundingTolerance(std::size_t dabCount)
{
    return static_cast<float32>(dabCount) * 0.5f * kHeightLayerStep + 1e-6f;
}

float32 LargestGapToTheDabSum(const HeightPageLayer& layer, uint32 faceSamples, uint32 level,
                              std::span<const TerrainECS::PlanetSculptDab> dabs)
{
    return LargestGapAtLevel(layer, faceSamples, level,
                             [dabs](const Mathematics::Vector3& direction) { return DabSumAt(dabs, direction); });
}

} // namespace

// Three level-0 dabs, one across a cube edge, hold the sum of their closed forms at every lattice
// point of every face: no sample inside a cap is missed, on the dab's face or its neighbors.
TEST(PlanetSculptDabTest, LevelZeroDabsHoldTheSumOfTheirClosedFormsAtEveryLatticePoint)
{
    constexpr uint32 kFaceSamples = 1025;
    HeightPageLayer layer(kPageStoreMaxFaces, BuildPageStoreLevels(kFaceSamples, kFaceSamples));
    const std::array<TerrainECS::PlanetSculptDab, 3> dabs = {
        TerrainECS::PlanetSculptDab({0.2f, 1.0f, 0.1f}, 0.03f, 2.0f),
        TerrainECS::PlanetSculptDab({0.25f, 1.0f, 0.12f}, 0.04f, -1.25f),
        TerrainECS::PlanetSculptDab({1.0f, 1.0f, 0.2f}, 0.05f, 0.75f)};
    std::vector<PageFaceRect> changed;
    for (const TerrainECS::PlanetSculptDab& dab : dabs)
        dab.AddTo(layer, changed);
    EXPECT_GE(changed.size(), 4u) << "the dab across the edge writes both faces";
    EXPECT_LE(LargestGapToTheDabSum(layer, kFaceSamples, 0, dabs), DabRoundingTolerance(dabs.size()));
}

// A dab too wide for four pages at level 0 writes the level that holds it in four pages, and that
// level holds its closed form at every lattice point.
TEST(PlanetSculptDabTest, AWideDabHoldsItsClosedFormAtItsWriteLevel)
{
    constexpr uint32 kFaceSamples = 1025;
    HeightPageLayer layer(kPageStoreMaxFaces, BuildPageStoreLevels(kFaceSamples, kFaceSamples));
    const std::array<TerrainECS::PlanetSculptDab, 1> dabs = {
        TerrainECS::PlanetSculptDab({0.1f, 1.0f, -0.05f}, 0.5f, 3.0f)};
    std::vector<PageFaceRect> changed;
    dabs[0].AddTo(layer, changed);
    // The cap spans about 2 x 0.5 / (pi / 2) x 1024 = 652 level-0 samples: more than 512, at most 1024.
    EXPECT_EQ(layer.WriteLevel(652), 1u);
    EXPECT_LE(LargestGapToTheDabSum(layer, kFaceSamples, 1, dabs), DabRoundingTolerance(dabs.size()));
}

// A 50 m dab on a 600 km planet of 0.5 m samples writes level 0 at a cube corner and 3 km from a face
// side: the coarse levels' neighbor bands (2^L samples long) do not widen its write.
TEST(PlanetSculptDabTest, ADabNearAFaceSideWritesLevelZero)
{
    constexpr uint32 kFaceSamples = 1900545;
    constexpr float32 kAngularRadius = 50.0f / 600000.0f;
    const std::array<Mathematics::Vector3, 2> centers = {Mathematics::Vector3(1.0f, 1.0f, 1.0f),
                                                         Mathematics::Vector3(0.99f, 1.0f, 0.2f)};
    const std::array<std::size_t, 2> faceCounts = {3u, 1u};
    for (std::size_t index = 0; index < centers.size(); ++index)
    {
        HeightPageLayer layer(kPageStoreMaxFaces, BuildPageStoreLevels(kFaceSamples, kFaceSamples));
        std::vector<PageFaceRect> changed;
        TerrainECS::PlanetSculptDab(centers[index], kAngularRadius, 5.0f).AddTo(layer, changed);
        const std::vector<HeightPageLayerPage> pages = HeightLayerAuthoredPages::Export(layer);
        ASSERT_FALSE(pages.empty()) << "center " << index;
        std::array<bool, kPageStoreMaxFaces> faces = {};
        for (const HeightPageLayerPage& page : pages)
        {
            EXPECT_EQ(page.Address.Level, 0u) << "center " << index << " face " << int(page.Address.Face);
            faces[page.Address.Face] = true;
        }
        EXPECT_EQ(static_cast<std::size_t>(std::count(faces.begin(), faces.end(), true)), faceCounts[index])
            << "center " << index;
    }
}

// The cap's profile is the smooth step of the distance to its edge: the full height at the center,
// 0.15625 of it a quarter of the way in from the edge, and zero at the edge.
TEST(PlanetSculptDabTest, TheCapProfileIsTheSmoothStepOfTheDistanceToItsEdge)
{
    constexpr float32 kAngularRadius = 0.2f;
    const TerrainECS::PlanetSculptDab dab({0.0f, 1.0f, 0.0f}, kAngularRadius, 2.0f);
    const float32 quarterIn = 0.75f * std::sin(kAngularRadius);
    const float32 edge = std::sin(kAngularRadius);
    EXPECT_NEAR(dab.HeightAt({0.0f, 1.0f, 0.0f}), 2.0f, 1e-6f);
    EXPECT_NEAR(dab.HeightAt({quarterIn, std::sqrt(1.0f - quarterIn * quarterIn), 0.0f}), 2.0f * 0.15625f, 1e-5f);
    EXPECT_NEAR(dab.HeightAt({edge, std::sqrt(1.0f - edge * edge), 0.0f}), 0.0f, 1e-5f);
}

// The CPU readers' sample of a layer holds what the pages compose at every level-0 lattice point, coarse
// writes included, and blends the four samples around a point between them.
TEST(PlanetSculptDabTest, ALevelZeroSampleHoldsTheComposedPagesAndBlendsBetweenThem)
{
    constexpr uint32 kFaceSamples = 1025;
    HeightPageLayer layer(kPageStoreMaxFaces, BuildPageStoreLevels(kFaceSamples, kFaceSamples));
    std::vector<PageFaceRect> changed;
    TerrainECS::PlanetSculptDab({0.1f, 1.0f, -0.05f}, 0.5f, 3.0f).AddTo(layer, changed);
    TerrainECS::PlanetSculptDab({0.2f, 1.0f, 0.1f}, 0.03f, 2.0f).AddTo(layer, changed);
    const PageFacePoint center = EqualAngleFacePointOf(Mathematics::Vector3(0.2f, 1.0f, 0.1f).Normalize(), kFaceSamples);
    const uint32 pageX = static_cast<uint32>(center.X) / kPageOwnedSamples;
    const uint32 pageZ = static_cast<uint32>(center.Z) / kPageOwnedSamples;
    std::vector<float32> samples(kPageSampleCount, 0.0f);
    ASSERT_TRUE(layer.ApplyToPage(PageAddress{center.Face, 0, pageX, pageZ}, samples));
    const auto composed = [&](uint32 x, uint32 z) {
        return samples[static_cast<std::size_t>(z - pageZ * kPageOwnedSamples + kPageApronSamples) * kPageStrideSamples +
                       (x - pageX * kPageOwnedSamples + kPageApronSamples)];
    };
    float32 gap = 0.0f;
    for (uint32 j = 0; j + 1 < kPageOwnedSamples; ++j)
        for (uint32 i = 0; i + 1 < kPageOwnedSamples; ++i)
        {
            const uint32 x = pageX * kPageOwnedSamples + i;
            const uint32 z = pageZ * kPageOwnedSamples + j;
            gap = std::max(gap, std::abs(layer.SampleLevel0(center.Face, x, z) - composed(x, z)));
            const float32 blend =
                0.25f * (composed(x, z) + composed(x + 1, z) + composed(x, z + 1) + composed(x + 1, z + 1));
            gap = std::max(gap, std::abs(layer.SampleLevel0(center.Face, x + 0.5, z + 0.5) - blend));
        }
    EXPECT_LE(gap, 1e-5f);
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
    residency.Refresh(7, 0, edit);
    residency.Refresh(7, 0, second);
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
        EXPECT_TRUE(PageReadsLevel0Rect(page.Address, edit, kPageApronSamples))
            << "level " << int(page.Address.Level) << " page (" << page.Address.X << ", " << page.Address.Z
            << ") was rewritten though the edit is not under it";
    EXPECT_EQ(rewritten.size(), provider->Levels().size()) << "one page per level reads the edit";
    pool.Shutdown();
}

// A refreshed page keeps the bytes it was fetched with: its rewrite uploads them again, and no fetch starts.
TEST(PageResidencyTest, ARefreshedResidentPageIsRewrittenFromItsKeptBytesWithoutAFetch)
{
    JobSystem::WorkStealingThreadPool pool(2);
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const PageSource source{nullptr, provider};
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(7, 128, 2, 0.4f);
    residency.Configure(7, 7, source);
    const std::vector<PageWant> wanted = EveryPage(*provider);
    residency.Request(7, wanted);
    uint64 frame = RunFramesUntil(residency, 0, [&] { return residency.FieldCache(7).ResidentCount() == wanted.size(); });
    ASSERT_EQ(residency.FieldCache(7).ResidentCount(), wanted.size());
    for (int i = 0; i < 40; ++i)
    {
        residency.Request(7, wanted);
        residency.Update(++frame, 1.0f / 60.0f, false);
    }

    const PageSampleRect edit{10, 10, 20, 20};
    residency.Refresh(7, 0, edit);
    uint32 fetches = 0;
    uint32 rewrites = 0;
    for (int i = 0; i < 30; ++i)
    {
        residency.Request(7, wanted);
        residency.Update(++frame, 1.0f / 60.0f, false);
        fetches += residency.LoadsStartedThisFrame();
        for (const PageRewrite& rewrite : residency.FieldCache(7).Rewrites())
        {
            ++rewrites;
            const std::vector<uint8>* kept = residency.Bytes(7, rewrite.Page.Address);
            ASSERT_NE(kept, nullptr);
            EXPECT_EQ(*kept, *FetchPage(source, rewrite.Page.Address).Bytes) << "the kept bytes are a fresh fetch's";
        }
    }
    EXPECT_EQ(rewrites, provider->Levels().size()) << "one page a level reads the edit";
    EXPECT_EQ(fetches, 0u) << "a refresh fetched a page whose bytes the residency keeps";
    pool.Shutdown();
}

// A resident, unwanted page whose bytes the trim dropped is fetched again for each refresh, and only until
// the cache releases it, kPageReleaseHysteresisFrames after its last want.
TEST(PageResidencyTest, ARefreshedPageNoLongerWantedIsFetchedOnlyUntilItsRelease)
{
    JobSystem::WorkStealingThreadPool pool(2);
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    constexpr uint64 kHeightPage = 67600u;
    PageResidencyBudget budget;
    budget.CpuBytes = 5u * kHeightPage;
    PageResidencyManager residency(&pool, nullptr, budget);
    residency.ConfigureField(1, 128, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    const std::vector<PageWant> heights = EveryPage(*provider);
    const std::vector<PageWant> few(heights.end() - 5, heights.end()); // the top two levels, closed under parents
    uint64 frame = RunFramesUntil(residency, 0, [&] {
        residency.Request(1, heights);
        return residency.FieldCache(1).ResidentCount() == heights.size();
    });
    ASSERT_EQ(residency.FieldCache(1).ResidentCount(), heights.size());
    residency.Request(1, few);
    residency.Update(++frame, 1.0f / 60.0f, false);
    const CachedPage dropped{1, PageAddress{0, 0, 0, 0}};
    ASSERT_NE(residency.FieldCache(1).SlotOf(dropped), kNoResidentSlot);
    ASSERT_EQ(residency.Bytes(1, dropped.Address), nullptr) << "the trim kept the bytes of a page no longer wanted";

    const PageSampleRect edit{10, 10, 20, 20};
    uint32 fetches = 0;
    uint32 framesResident = 0;
    for (uint32 i = 0; i < 2u * kPageReleaseHysteresisFrames; ++i)
    {
        const bool resident = residency.FieldCache(1).SlotOf(dropped) != kNoResidentSlot;
        framesResident += resident ? 1u : 0u;
        residency.Refresh(1, 0, edit);
        residency.Request(1, few);
        residency.Update(++frame, 1.0f / 60.0f, false);
        if (!resident)
        {
            EXPECT_EQ(residency.LoadsStartedThisFrame(), 0u) << "a refresh fetched a released page";
        }
        fetches += residency.LoadsStartedThisFrame();
    }
    EXPECT_GT(fetches, 0u) << "a refresh rewrites the resident page from a fetch";
    EXPECT_EQ(residency.FieldCache(1).SlotOf(dropped), kNoResidentSlot);
    EXPECT_LE(framesResident, kPageReleaseHysteresisFrames + 1u) << "the page stayed past its release";
    pool.Shutdown();
}

// A planet stream: a refresh of one face's rect rewrites that face's pages, not the pages at the same place on
// another face.
TEST(PageResidencyTest, ARefreshRewritesThePagesOfItsFaceOnly)
{
    JobSystem::WorkStealingThreadPool pool(2);
    GeneratedPlanetLattice lattice;
    lattice.ReferenceRadius = 2000.0;
    lattice.FaceSamples = 257;
    lattice.HeightRow = [](std::span<const Mathematics::Vector3>, float32, std::span<float32> outHeights) {
        std::fill(outHeights.begin(), outHeights.end(), 0.0f);
    };
    lattice.HeightRange = [](const Mathematics::Vector3&, float32, float32) { return PageHeightRange{}; };
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(lattice);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(7, 128, 2, 0.4f);
    residency.Configure(7, 7, PageSource{nullptr, provider});
    std::vector<PageWant> wanted;
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(lattice.FaceSamples, lattice.FaceSamples);
    for (const uint8 face : {uint8{0}, uint8{3}})
        for (uint32 level = 0; level < levels.size(); ++level)
            for (uint32 z = 0; z < levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < levels[level].PagesX; ++x)
                    wanted.push_back({PageAddress{face, uint8(level), x, z}, float32(x + z), false});
    residency.Request(7, wanted);
    uint64 frame = RunFramesUntil(residency, 0, [&] { return residency.FieldCache(7).ResidentCount() == wanted.size(); });
    ASSERT_EQ(residency.FieldCache(7).ResidentCount(), wanted.size());
    for (int i = 0; i < 40; ++i)
    {
        residency.Request(7, wanted);
        residency.Update(++frame, 1.0f / 60.0f, false);
    }

    residency.Refresh(7, 3, PageSampleRect{10, 10, 20, 20});
    std::vector<CachedPage> rewritten;
    for (int i = 0; i < 60; ++i)
    {
        residency.Request(7, wanted);
        residency.Update(++frame, 1.0f / 60.0f, false);
        for (const PageRewrite& rewrite : residency.FieldCache(7).Rewrites())
            rewritten.push_back(rewrite.Page);
    }
    EXPECT_EQ(rewritten.size(), levels.size()) << "one page a level of face 3 reads the rect";
    for (const CachedPage& page : rewritten)
        EXPECT_EQ(int(page.Address.Face), 3) << "level " << int(page.Address.Level) << " page (" << page.Address.X
                                         << ", " << page.Address.Z << ")";
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
        const Mathematics::Vector3 shared = EqualAngleSampleDirection(face, x, z, request.FaceSamples);
        const Mathematics::Vector3 expected = LatticeDirection(face, x, z, request.FaceSamples);
        if (direction.x != shared.x || direction.y != shared.y || direction.z != shared.z ||
            Mathematics::Vector3::Dot(direction - expected, direction - expected) > 1e-12f)
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
            for (const auto& [x, z] : {std::pair<int64, int64>{0, 0}, {0, last}, {last, 0}, {last, last}})
                EXPECT_EQ(own.At(x == 0 ? -1 : last + 1, z == 0 ? -1 : last + 1), own.At(x, z));
        }
    }
}

TEST_F(PageStoreTest, APlanetStoresPagesLandOnTheirOwnFaceAndAStreamWithNoCpuReaderHoldsNoCpuDirectory)
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

    for (const PageWant& want : wanted)
    {
        const PageAddress& address = want.Address;
        const std::vector<float32> stored = PageSamples(*reader, address);
        const std::vector<uint8>* bytes = residency.Bytes(1, address);
        ASSERT_NE(bytes, nullptr);
        EXPECT_EQ(Heights(*bytes), stored) << int(address.Face) << " " << int(address.Level) << " " << address.X << "," << address.Z;
        const PageAddress faceZero{0, address.Level, address.X, address.Z};
        EXPECT_NE(PageSamples(*reader, faceZero), stored);
        EXPECT_EQ(residency.Bytes(1, faceZero), nullptr);
    }
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEBUG_DEATH(EXPECT_TRUE(residency.CpuPages(1).Empty()), "first RequestCpu");
    io.Stop();
    pool.Shutdown();
}

TEST_F(PageStoreTest, APlanetStreamKeepsItsCpuWindowPagesOnEveryFaceInADirectoryOfOnlyThosePages)
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

    // One level-0 page on each of three faces, as a body window spanning a cube corner asks; no GPU want.
    const uint32 levelCount = static_cast<uint32>(reader->Layout().Levels.size());
    const std::vector<PageAddress> window{{2, 0, 0, 0}, {4, 0, 1, 0}, {0, 0, 0, 1}};
    // The window lasts until the next Update, so each frame asks again while the reads land.
    bool kept = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (uint64 frame = 0; !kept && std::chrono::steady_clock::now() < deadline; ++frame)
    {
        residency.RequestCpu(1, levelCount, window);
        residency.Update(frame, 1.0f / 60.0f, false);
        kept = std::ranges::all_of(window, [&](const PageAddress& page) { return residency.Bytes(1, page) != nullptr; });
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(kept);
    for (const PageAddress& page : window)
        EXPECT_EQ(Heights(*residency.Bytes(1, page)), PageSamples(*reader, page)) << int(page.Face);
    const std::span<const PageAddress> kept1 = residency.CpuWindow(1);
    EXPECT_TRUE(std::ranges::equal(kept1, window));
    // A face-0 page of the same level and position as a window page on another face is not fetched.
    EXPECT_EQ(residency.Bytes(1, PageAddress{0, 0, 0, 0}), nullptr);
    // The directory holds the W window pages and no other page of any face or level: never a face's worth.
    const CpuPageDirectory directory = residency.CpuPages(1);
    for (const PageAddress& page : window)
        EXPECT_EQ(directory.BytesOf(directory.Page(page)).data(), residency.Bytes(1, page)->data()) << int(page.Face);
    std::size_t entries = 0;
    const PageStoreLayout& layout = reader->Layout();
    for (uint8 face = 0; face < layout.Header.FaceCount; ++face)
        for (uint32 level = 0; level < levelCount; ++level)
            for (uint32 z = 0; z < layout.Levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < layout.Levels[level].PagesX; ++x)
                    entries += directory.Page(PageAddress{face, static_cast<uint8>(level), x, z}) != CpuPageEntry::Missing;
    EXPECT_EQ(entries, window.size());
    io.Stop();
    pool.Shutdown();
}

// A planet's page bodies wait for their pages: its CPU window pages load on workers, never in the
// frame, even when the budget fetches a planar terrain's window pages synchronously.
TEST_F(PageStoreTest, APlanetStreamLoadsItsCpuWindowPagesOnWorkersWithASyncFetchBudget)
{
    const PlanetCookRequest request = SmallPlanetRequest(m_Dir / "planet.gepage");
    ASSERT_EQ(CookPlanetPageStore(request).Error, "");
    auto reader = std::make_shared<PageStoreReader>();
    ASSERT_EQ(reader->Open(request.Output, PageFieldKind::Height), "");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetIOService io;
    io.Start(pool, 1);
    PageResidencyBudget budget;
    budget.CpuSyncFetchPages = 8;
    PageResidencyManager residency(&pool, &io, budget);
    residency.ConfigureField(1, 128, 2, 0.4f);
    residency.Configure(1, 1, PageSource{reader, nullptr});

    const uint32 levelCount = static_cast<uint32>(reader->Layout().Levels.size());
    const std::vector<PageAddress> window{{2, 0, 0, 0}, {4, 0, 1, 0}, {0, 0, 0, 1}};
    residency.RequestCpu(1, levelCount, window);
    residency.Update(0, 1.0f / 60.0f, false);
    for (const PageAddress& page : window)
        EXPECT_EQ(residency.Bytes(1, page), nullptr) << "face " << int(page.Face) << " was fetched in the frame";

    bool kept = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (uint64 frame = 1; !kept && std::chrono::steady_clock::now() < deadline; ++frame)
    {
        residency.RequestCpu(1, levelCount, window);
        residency.Update(frame, 1.0f / 60.0f, false);
        kept = std::ranges::all_of(window, [&](const PageAddress& page) { return residency.Bytes(1, page) != nullptr; });
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(kept);
    io.Stop();
    pool.Shutdown();
}

// ---- The generated planet provider ---------------------------------------------------------------

namespace
{

// The default relief, band-limited at `minResolvedArc`.
float32 BandLimitedPlanetHeight(const Mathematics::Vector3& direction, float32 minResolvedArc)
{
    const Components::TerrainPlanetRelief relief{};
    return CBTTerrain::PlanetRelief(direction.x, direction.y, direction.z, relief.Amplitude, relief.Frequency,
                                    relief.Octaves, minResolvedArc);
}

// BandLimitedPlanetHeight at each direction of a row, as the planet lattice evaluates it.
void BandLimitedPlanetHeightRow(std::span<const Mathematics::Vector3> directions, float32 minResolvedArc,
                                std::span<float32> outHeights)
{
    const Components::TerrainPlanetRelief relief{};
    CBTTerrain::PlanetReliefRow(directions, relief.Amplitude, relief.Frequency, relief.Octaves, minResolvedArc,
                                outHeights);
}

// The default relief's range near a direction.
PageHeightRange DefaultPlanetHeightRange(const Mathematics::Vector3& direction, float32 chord, float32 minResolvedArc)
{
    const Components::TerrainPlanetRelief relief{};
    const CBTTerrain::PlanetReliefRange range = CBTTerrain::PlanetReliefRangeNear(
        direction.x, direction.y, direction.z, chord, relief.Amplitude, relief.Frequency, relief.Octaves, minResolvedArc);
    return PageHeightRange{range.Min, range.Max};
}

// The 2 km planet at 257 samples a face side, generated.
GeneratedHeightPageProvider SmallGeneratedPlanet()
{
    GeneratedPlanetLattice lattice;
    lattice.ReferenceRadius = kPlanetRadius;
    lattice.FaceSamples = 2u * kPageOwnedSamples + 1u;
    lattice.HeightRow = BandLimitedPlanetHeightRow;
    lattice.HeightRange = DefaultPlanetHeightRange;
    return GeneratedHeightPageProvider(std::move(lattice));
}

// One level's samples of a generated planet, (x, z) in [-1, samples], read from the page that
// stores them: an owned sample, or the apron one past a side. Pages are filled once.
float32 GeneratedSample(const GeneratedHeightPageProvider& provider, uint8 face, uint8 level, int64 x, int64 z)
{
    static std::map<PageAddress, std::vector<float32>> pages;
    const int64 samples = provider.Levels()[level].SamplesX;
    const auto pageOf = [samples](int64 index) {
        return static_cast<uint32>(std::clamp<int64>(index, 0, samples - 1) / kPageOwnedSamples);
    };
    const uint32 pageX = pageOf(x);
    const uint32 pageZ = pageOf(z);
    const PageAddress address{face, level, pageX, pageZ};
    std::vector<float32>& page = pages[address];
    if (page.empty())
    {
        page.resize(kPageSampleCount);
        EXPECT_TRUE(provider.FillPage(address, page));
    }
    const int64 column = x - static_cast<int64>(pageX) * kPageOwnedSamples + kPageApronSamples;
    const int64 row = z - static_cast<int64>(pageZ) * kPageOwnedSamples + kPageApronSamples;
    return page[static_cast<std::size_t>(row) * kPageStrideSamples + static_cast<std::size_t>(column)];
}

// The position along `side` of sample (x, z) on that side.
int64 AlongSide(PageFaceSide side, int64 x, int64 z)
{
    return side == PageFaceSide::MinX || side == PageFaceSide::MaxX ? z : x;
}

// The face coordinate a window of `margin` around `address` holds at window (column, row): a sample
// farther than `margin` past the face (a partial last page) repeats the one `margin` past.
std::pair<int64, int64> WindowFaceSample(const PageAddress& address, uint32 column, uint32 row, uint32 margin, int64 last)
{
    const int64 x = static_cast<int64>(address.X) * kPageOwnedSamples + column - margin;
    const int64 z = static_cast<int64>(address.Z) * kPageOwnedSamples + row - margin;
    return {std::clamp<int64>(x, -int64{margin}, last + margin), std::clamp<int64>(z, -int64{margin}, last + margin)};
}

// True when page sample (column, row) of `address` is on its face or its apron one past, the samples
// where FillPage and a wider window agree; FillPage repeats the apron farther out.
bool WithinFaceApron(const PageAddress& address, uint32 column, uint32 row, int64 last)
{
    const auto [x, z] = WindowFaceSample(address, column, row, kPageApronSamples, last);
    return x == static_cast<int64>(address.X) * kPageOwnedSamples + column - kPageApronSamples &&
           z == static_cast<int64>(address.Z) * kPageOwnedSamples + row - kPageApronSamples;
}

// True when page sample (column, row) of `address` is on its face.
bool OnFace(const PageAddress& address, uint32 column, uint32 row, int64 last)
{
    const int64 x = static_cast<int64>(address.X) * kPageOwnedSamples + column - kPageApronSamples;
    const int64 z = static_cast<int64>(address.Z) * kPageOwnedSamples + row - kPageApronSamples;
    return x >= 0 && x <= last && z >= 0 && z <= last;
}

// The sample of `face` on `side` at `along`, `inward` samples in from the side (-1 is the apron).
std::pair<int64, int64> SideSample(PageFaceSide side, int64 along, int64 inward, int64 samples)
{
    switch (side)
    {
    case PageFaceSide::MinX:
        return {inward, along};
    case PageFaceSide::MaxX:
        return {samples - 1 - inward, along};
    case PageFaceSide::MinZ:
        return {along, inward};
    case PageFaceSide::MaxZ:
        return {along, samples - 1 - inward};
    }
    return {0, 0};
}

} // namespace

TEST(GeneratedHeightPageProviderTest, APlanetPageIsTheReliefAtEveryLatticePointOnSixFaces)
{
    const GeneratedHeightPageProvider provider = SmallGeneratedPlanet();
    ASSERT_EQ(provider.FaceCount(), kPageStoreMaxFaces);
    const uint32 level0Samples = provider.Levels().front().SamplesX;
    std::vector<float32> page(kPageSampleCount);
    for (uint8 level : {uint8{0}, uint8{2}})
    {
        const PageStoreLevel& shape = provider.Levels()[level];
        const float32 minResolvedArc =
            level == 0 ? 0.0f
                       : static_cast<float32>(std::numbers::pi * static_cast<float64>(1u << level) / (level0Samples - 1u));
        for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
            for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
                for (uint32 px = 0; px < shape.PagesX; ++px)
                {
                    ASSERT_TRUE(provider.FillPage(PageAddress{face, level, px, pz}, page));
                    for (uint32 r = 0; r < kPageOwnedSamples && pz * kPageOwnedSamples + r < shape.SamplesZ; ++r)
                        for (uint32 c = 0; c < kPageOwnedSamples && px * kPageOwnedSamples + c < shape.SamplesX; ++c)
                        {
                            const uint32 x = (px * kPageOwnedSamples + c) << level;
                            const uint32 z = (pz * kPageOwnedSamples + r) << level;
                            const float32 expected =
                                BandLimitedPlanetHeight(LatticeDirection(face, x, z, level0Samples), minResolvedArc);
                            ASSERT_NEAR(page[static_cast<std::size_t>(r + 1u) * kPageStrideSamples + c + 1u], expected,
                                        1e-3f)
                                << "face " << int{face} << " level " << int{level} << " sample " << x << ", " << z;
                        }
                }
    }
    // A coarse level drops the octaves it cannot carry.
    const Mathematics::Vector3 direction = LatticeDirection(4, 77u, 190u, level0Samples);
    EXPECT_NE(BandLimitedPlanetHeight(direction, 0.0f), BandLimitedPlanetHeight(direction, 0.2f));
}

TEST(GeneratedHeightPageProviderTest, APlanetPageHeightRangeHoldsEverySampleOfThePageAndTheFullReliefUnderIt)
{
    // P-50's planet at 0.5 m: the first, middle and last pages of each level on six faces. The scene's
    // relief, and one at ten times its frequency so the relief changes inside a fine page.
    for (float32 frequency : {6.0f, 60.0f})
    {
        const Components::TerrainPlanetRelief relief{1300.0f, frequency, 4u};
        GeneratedPlanetLattice lattice;
        lattice.ReferenceRadius = 50000.0;
        lattice.FaceSamples = 77u * 2048u + 1u;
        lattice.HeightRow = [relief](std::span<const Mathematics::Vector3> directions, float32 minResolvedArc,
                                     std::span<float32> outHeights) {
            CBTTerrain::PlanetReliefRow(directions, relief.Amplitude, relief.Frequency, relief.Octaves,
                                        minResolvedArc, outHeights);
        };
        lattice.HeightRange = [relief](const Mathematics::Vector3& direction, float32 chord, float32 minResolvedArc) {
            const CBTTerrain::PlanetReliefRange range =
                CBTTerrain::PlanetReliefRangeNear(direction.x, direction.y, direction.z, chord, relief.Amplitude,
                                                  relief.Frequency, relief.Octaves, minResolvedArc);
            return PageHeightRange{range.Min, range.Max};
        };
        const GeneratedHeightPageProvider provider(std::move(lattice));
        const uint32 level0Samples = provider.Levels().front().SamplesX;
        const float64 envelope = 2.0 * CBTTerrain::kReliefEnvelope * relief.Amplitude;
        std::vector<float32> page(kPageSampleCount);
        for (uint8 level = 0; level < provider.Levels().size(); ++level)
        {
            const PageStoreLevel& shape = provider.Levels()[level];
            float64 widthSum = 0.0;
            uint32 pages = 0;
            for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
                for (uint32 pz : {0u, shape.PagesZ / 2u, shape.PagesZ - 1u})
                    for (uint32 px : {0u, shape.PagesX / 2u, shape.PagesX - 1u})
                    {
                        const PageAddress address{face, level, px, pz};
                        const PageHeightRange range = provider.PlanetPageHeightRange(address);
                        ASSERT_TRUE(provider.FillPage(address, page));
                        for (float32 height : page)
                            ASSERT_TRUE(height >= range.Min && height <= range.Max)
                                << "f " << frequency << " face " << int{face} << " level " << int{level} << " page "
                                << px << "," << pz << ": " << height << " outside [" << range.Min << ", "
                                << range.Max << "]";
                        // The full relief on 33 x 33 level-0 samples under the page.
                        const uint32 span = kPageOwnedSamples << level;
                        const uint32 x0 = px * span;
                        const uint32 z0 = pz * span;
                        const uint32 stride = std::max(span / 32u, 1u);
                        for (uint32 z = z0; z <= std::min(z0 + span, level0Samples - 1u); z += stride)
                            for (uint32 x = x0; x <= std::min(x0 + span, level0Samples - 1u); x += stride)
                            {
                                const Mathematics::Vector3 direction = LatticeDirection(face, x, z, level0Samples);
                                const float32 full = CBTTerrain::PlanetRelief(direction.x, direction.y, direction.z,
                                                                              relief.Amplitude, relief.Frequency,
                                                                              relief.Octaves);
                                ASSERT_TRUE(full >= range.Min && full <= range.Max)
                                    << "the full relief: f " << frequency << " face " << int{face} << " level "
                                    << int{level} << " sample " << x << "," << z;
                            }
                        widthSum += range.Max - range.Min;
                        ++pages;
                    }
            const float64 meanWidth = widthSum / pages / envelope;
            RecordProperty("f" + std::to_string(static_cast<int>(frequency)) + "_level" + std::to_string(level) +
                               "_mean_width_over_envelope",
                           std::to_string(meanWidth));
            if (frequency == 6.0f && level == 0)
            {
                EXPECT_LT(meanWidth, 0.05) << "a level-0 page's range is its own, far inside the planet's envelope";
            }
        }
    }
}

TEST(GeneratedHeightPageProviderTest, APlanetPageHeightRangeAsksForADiscThatHoldsEverySampleOfThePage)
{
    // The chord the range is asked for holds every direction FillPage evaluates, apron included, at
    // the face's center and corners, where the sample steps meet at 90, 60 and 120 degrees. An owned
    // sample has the shared direction.
    auto directions = std::make_shared<std::vector<Mathematics::Vector3>>();
    auto disc = std::make_shared<std::pair<Mathematics::Vector3, float32>>();
    GeneratedPlanetLattice lattice;
    lattice.ReferenceRadius = 50000.0;
    lattice.FaceSamples = 77u * 2048u + 1u;
    lattice.HeightRow = [directions](std::span<const Mathematics::Vector3> row, float32,
                                     std::span<float32> outHeights) {
        directions->insert(directions->end(), row.begin(), row.end());
        std::fill(outHeights.begin(), outHeights.end(), 0.0f);
    };
    lattice.HeightRange = [disc](const Mathematics::Vector3& direction, float32 chord, float32) {
        *disc = {direction, chord};
        return PageHeightRange{};
    };
    const GeneratedHeightPageProvider provider(std::move(lattice));
    std::vector<float32> page(kPageSampleCount);
    float32 fullest = 0.0f;
    for (uint8 level : {uint8{0}, uint8{2}, uint8{4}, uint8{6}, uint8{9}})
    {
        const PageStoreLevel& shape = provider.Levels()[level];
        for (uint8 face : {uint8{0}, uint8{4}})
            for (uint32 pz : {0u, 1u, shape.PagesZ / 2u, shape.PagesZ - 2u, shape.PagesZ - 1u})
                for (uint32 px : {0u, 1u, shape.PagesX / 2u, shape.PagesX - 2u, shape.PagesX - 1u})
                {
                    const PageAddress address{face, level, px, pz};
                    provider.PlanetPageHeightRange(address);
                    directions->clear();
                    ASSERT_TRUE(provider.FillPage(address, page));
                    ASSERT_EQ(directions->size(), kPageSampleCount);
                    const auto& [center, chord] = *disc;
                    for (std::size_t k = 0; k < directions->size(); ++k)
                    {
                        const Mathematics::Vector3& direction = (*directions)[k];
                        const float32 reach = (direction - center).Length();
                        fullest = std::max(fullest, reach / chord);
                        ASSERT_LE(reach, chord) << "face " << int{face} << " level " << int{level} << " page " << px
                                                << "," << pz;
                        // An owned sample's direction is the shared one, bit for bit.
                        const uint32 x = px * kPageOwnedSamples + static_cast<uint32>(k % kPageStrideSamples) - 1u;
                        const uint32 z = pz * kPageOwnedSamples + static_cast<uint32>(k / kPageStrideSamples) - 1u;
                        if (k % kPageStrideSamples == 0 || k / kPageStrideSamples == 0 || x >= shape.SamplesX ||
                            z >= shape.SamplesZ)
                            continue;
                        const Mathematics::Vector3 shared =
                            EqualAngleSampleDirection(face, x << level, z << level, provider.Levels().front().SamplesX);
                        ASSERT_TRUE(direction.x == shared.x && direction.y == shared.y && direction.z == shared.z)
                            << "face " << int{face} << " level " << int{level} << " sample " << x << "," << z;
                    }
                }
    }
    RecordProperty("largest_sample_reach_over_chord", std::to_string(fullest));
}

TEST(GeneratedHeightPageProviderTest, APlanetEdgeSampleIsOneValueFromBothFacesAndTheApronIsTheNeighborsRow)
{
    const GeneratedHeightPageProvider provider = SmallGeneratedPlanet();
    constexpr std::array<PageFaceSide, kPageFaceSides> kSides{PageFaceSide::MinZ, PageFaceSide::MaxZ, PageFaceSide::MinX,
                                                              PageFaceSide::MaxX};
    for (uint8 level = 0; level < provider.Levels().size(); ++level)
    {
        const int64 samples = provider.Levels()[level].SamplesX;
        for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
            for (PageFaceSide side : kSides)
            {
                const PageFaceEdge edge = PageFaceNeighbor(face, side);
                for (int64 along = 0; along < samples; along += 7)
                {
                    const int64 theirs = edge.Reversed ? samples - 1 - along : along;
                    const auto [ownX, ownZ] = SideSample(side, along, 0, samples);
                    const auto [edgeX, edgeZ] = SideSample(edge.Side, theirs, 0, samples);
                    EXPECT_EQ(GeneratedSample(provider, face, level, ownX, ownZ),
                              GeneratedSample(provider, edge.Face, level, edgeX, edgeZ))
                        << "face " << int{face} << " level " << int{level} << " along " << along;
                    if (along == 0 || along == samples - 1)
                        continue; // past a cube corner the apron repeats the corner sample
                    const auto [apronX, apronZ] = SideSample(side, along, -1, samples);
                    const auto [innerX, innerZ] = SideSample(edge.Side, theirs, 1, samples);
                    EXPECT_EQ(AlongSide(side, apronX, apronZ), along);
                    EXPECT_EQ(GeneratedSample(provider, face, level, apronX, apronZ),
                              GeneratedSample(provider, edge.Face, level, innerX, innerZ))
                        << "apron: face " << int{face} << " level " << int{level} << " along " << along;
                }
            }
    }
}

TEST(GeneratedHeightPageProviderTest, APlanetWindowHoldsThePageAndPastAFaceSideTheNeighborsSampleAtTheSameDistance)
{
    const GeneratedHeightPageProvider provider = SmallGeneratedPlanet();
    constexpr uint32 kMargin = kPageSurfaceWindowMarginSamples;
    constexpr uint32 kStride = kPageOwnedSamples + 2u * kMargin;
    std::vector<float32> window(static_cast<std::size_t>(kStride) * kStride);
    std::vector<float32> page(kPageSampleCount);
    uint32 pageMismatches = 0;
    uint32 sideMismatches = 0;
    uint32 sideSamples = 0;
    for (uint8 level : {uint8{0}, uint8{1}})
    {
        const PageStoreLevel& shape = provider.Levels()[level];
        const int64 last = static_cast<int64>(shape.SamplesX) - 1;
        for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
            for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
                for (uint32 px = 0; px < shape.PagesX; ++px)
                {
                    const PageAddress address{face, level, px, pz};
                    ASSERT_TRUE(provider.FillPlanetWindow(address, kMargin, window, {}));
                    ASSERT_TRUE(provider.FillPage(address, page));
                    for (uint32 r = 0; r < kPageStrideSamples; ++r)
                        for (uint32 c = 0; c < kPageStrideSamples; ++c)
                            pageMismatches += WithinFaceApron(address, c, r, last) &&
                                              window[(r + kMargin - kPageApronSamples) * kStride + c + kMargin - kPageApronSamples] !=
                                                  page[r * kPageStrideSamples + c];
                    for (uint32 r = 0; r < kStride; ++r)
                        for (uint32 c = 0; c < kStride; ++c)
                        {
                            const auto [x, z] = WindowFaceSample(address, c, r, kMargin, last);
                            const bool outsideX = x < 0 || x > last;
                            const bool outsideZ = z < 0 || z > last;
                            if (outsideX == outsideZ)
                                continue; // inside the face, or past a cube corner
                            const PageFaceSide side = outsideX ? (x < 0 ? PageFaceSide::MinX : PageFaceSide::MaxX)
                                                               : (z < 0 ? PageFaceSide::MinZ : PageFaceSide::MaxZ);
                            const int64 past = outsideX ? x : z;
                            const int64 depth = past < 0 ? -past : past - last;
                            const PageFaceEdge edge = PageFaceNeighbor(face, side);
                            const int64 along = AlongSide(side, x, z);
                            const auto [theirX, theirZ] =
                                SideSample(edge.Side, edge.Reversed ? last - along : along, depth, last + 1);
                            ++sideSamples;
                            sideMismatches += window[static_cast<std::size_t>(r) * kStride + c] !=
                                              GeneratedSample(provider, edge.Face, level, theirX, theirZ);
                        }
                }
    }
    EXPECT_EQ(pageMismatches, 0u) << "the window's inner 130 samples differ from the page";
    EXPECT_GT(sideSamples, 0u);
    EXPECT_EQ(sideMismatches, 0u) << "of " << sideSamples << " samples past a face side";
}

namespace
{
// The samples along side `side` of a face of `samples` samples at [alongMin, alongMax], [depthMin, depthMax]
// samples in from the side.
PageSampleRect SideBand(PageFaceSide side, uint32 alongMin, uint32 alongMax, uint32 depthMin, uint32 depthMax,
                        uint32 samples)
{
    const auto [x0, z0] = SideSample(side, alongMin, depthMin, samples);
    const auto [x1, z1] = SideSample(side, alongMax, depthMax, samples);
    return PageSampleRect{static_cast<uint32>(std::min(x0, x1)), static_cast<uint32>(std::min(z0, z1)),
                          static_cast<uint32>(std::max(x0, x1)), static_cast<uint32>(std::max(z0, z1))};
}

// Sample (x, z) of `face` at `level` as ApplyToPage adds it to a zero page; `pages` keeps each page it applies.
float32 AppliedPageSample(const HeightPageLayer& layer, std::map<PageAddress, std::vector<float32>>& pages, uint8 face,
                          uint8 level, int64 x, int64 z)
{
    const PageAddress address{face, level, static_cast<uint32>(x) / kPageOwnedSamples,
                              static_cast<uint32>(z) / kPageOwnedSamples};
    std::vector<float32>& page = pages[address];
    if (page.empty())
    {
        page.assign(kPageSampleCount, 0.0f);
        layer.ApplyToPage(address, page);
    }
    return page[(z % kPageOwnedSamples + kPageApronSamples) * kPageStrideSamples + x % kPageOwnedSamples +
                kPageApronSamples];
}
} // namespace

// A layer's window holds its page's deltas, and past a face's side the neighbor face's deltas at the
// same distance in: a level-0 write along face 0's side and a coarse write that prolongs across it.
TEST(HeightPageLayerTest, AWindowAddsThePageAndPastAFaceSideTheNeighborsDeltasAtTheSameDistance)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(257, 257);
    HeightPageLayer layer(kPageStoreMaxFaces, levels);
    const PageSampleRect rect{0, 100, 7, 140};
    layer.WriteLevel0(0, rect, RectDelta(rect, 0.5f));
    const PageSampleRect coarse{0, 40, 3, 60};
    layer.AddAtLevel(0, 1, coarse, RectDelta(coarse, 1.5f));
    std::map<PageAddress, std::vector<float32>> pages;
    constexpr uint32 kMargin = kPageSurfaceWindowMarginSamples;
    constexpr uint32 kStride = kPageOwnedSamples + 2u * kMargin;
    std::vector<float32> window(static_cast<std::size_t>(kStride) * kStride);
    std::vector<float32> page(kPageSampleCount);
    const PageFaceEdge edge = PageFaceNeighbor(0, PageFaceSide::MinX);
    uint32 pageMismatches = 0;
    uint32 sideMismatches = 0;
    uint32 sideWrites = 0;
    for (uint8 face : {uint8{0}, edge.Face})
        for (uint8 level : {uint8{0}, uint8{1}})
        {
            const int64 last = static_cast<int64>(levels[level].SamplesX) - 1;
            for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
                for (uint32 px = 0; px < levels[level].PagesX; ++px)
                {
                    const PageAddress address{face, level, px, pz};
                    std::fill(window.begin(), window.end(), 0.0f);
                    std::fill(page.begin(), page.end(), 0.0f);
                    const bool applied = layer.ApplyToWindow(address, kMargin, window);
                    EXPECT_EQ(applied, std::any_of(window.begin(), window.end(), [](float32 v) { return v != 0.0f; }));
                    layer.ApplyToPage(address, page);
                    // On the face only: past a side the check below reads the neighbor's samples.
                    for (uint32 r = 0; r < kPageStrideSamples; ++r)
                        for (uint32 c = 0; c < kPageStrideSamples; ++c)
                            pageMismatches += OnFace(address, c, r, last) &&
                                              window[(r + kMargin - kPageApronSamples) * kStride + c + kMargin - kPageApronSamples] !=
                                                  page[r * kPageStrideSamples + c];
                    for (uint32 r = 0; r < kStride; ++r)
                        for (uint32 c = 0; c < kStride; ++c)
                        {
                            const auto [x, z] = WindowFaceSample(address, c, r, kMargin, last);
                            const bool outsideX = x < 0 || x > last;
                            const bool outsideZ = z < 0 || z > last;
                            if (outsideX == outsideZ)
                                continue;
                            const PageFaceSide side = outsideX ? (x < 0 ? PageFaceSide::MinX : PageFaceSide::MaxX)
                                                               : (z < 0 ? PageFaceSide::MinZ : PageFaceSide::MaxZ);
                            const int64 past = outsideX ? x : z;
                            const PageFaceEdge across = PageFaceNeighbor(face, side);
                            const int64 along = AlongSide(side, x, z);
                            const auto [theirX, theirZ] = SideSample(across.Side, across.Reversed ? last - along : along,
                                                                     past < 0 ? -past : past - last, last + 1);
                            const float32 expected = AppliedPageSample(layer, pages, across.Face, level, theirX, theirZ);
                            sideWrites += expected != 0.0f;
                            sideMismatches += window[static_cast<std::size_t>(r) * kStride + c] != expected;
                        }
                }
        }
    EXPECT_EQ(pageMismatches, 0u) << "the window's samples on the face differ from ApplyToPage";
    EXPECT_GT(sideWrites, 0u) << "no window sample past a side reads a write";
    EXPECT_EQ(sideMismatches, 0u);
    // The face opposite face 0 shares no side with the writes; a span of the wrong size is refused.
    EXPECT_FALSE(layer.ApplyToWindow(PageAddress{1, 0, 1, 1}, kMargin, window));
    EXPECT_FALSE(layer.ApplyToWindow(PageAddress{0, 0, 0, 0}, kMargin, page));
}

namespace GameEngine::PageStreaming
{
struct HeightPageLayerTestAccess
{
    static bool WindowReached(const HeightPageLayer& layer, const PageAddress& address, uint32 margin)
    {
        return layer.WindowReached(address, margin);
    }
};
} // namespace GameEngine::PageStreaming

// A window that no layer page and no prolonged write reaches returns before the gather: a level-0 write
// and a coarse write near page (0, 0) of a flat layer reach its window and not the window of page (6, 6).
TEST(HeightPageLayerTest, AWindowThatNoPageOrProlongedWriteReachesReturnsBeforeTheGather)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(1025, 1025);
    HeightPageLayer written(1, levels);
    const PageSampleRect rect{10, 10, 20, 20};
    written.WriteLevel0(0, rect, RectDelta(rect, 0.5f));
    HeightPageLayer prolonged(1, levels);
    const PageSampleRect coarse{5, 5, 8, 8};
    prolonged.AddAtLevel(0, 1, coarse, RectDelta(coarse, 1.5f));
    constexpr uint32 kMargin = kPageSurfaceWindowMarginSamples;
    constexpr uint32 kStride = kPageOwnedSamples + 2u * kMargin;
    std::vector<float32> window(static_cast<std::size_t>(kStride) * kStride, 0.0f);
    for (const HeightPageLayer* layer : {&written, &prolonged})
    {
        EXPECT_TRUE(HeightPageLayerTestAccess::WindowReached(*layer, PageAddress{0, 0, 0, 0}, kMargin));
        EXPECT_TRUE(layer->ApplyToWindow(PageAddress{0, 0, 0, 0}, kMargin, window));
        std::fill(window.begin(), window.end(), 0.0f);
        EXPECT_FALSE(HeightPageLayerTestAccess::WindowReached(*layer, PageAddress{0, 0, 6, 6}, kMargin));
        EXPECT_FALSE(layer->ApplyToWindow(PageAddress{0, 0, 6, 6}, kMargin, window));
    }
}

// On a cube a window returns before the gather when the neighbor face past its side holds writes only
// outside the samples the window reads past that side.
TEST(HeightPageLayerTest, AWindowPastWhoseSideTheNeighborHoldsNoWriteItReadsReturnsBeforeTheGather)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(1025, 1025);
    HeightPageLayer layer(kPageStoreMaxFaces, levels);
    const uint8 neighbor = PageFaceNeighbor(0, PageFaceSide::MinX).Face;
    const PageSampleRect rect{500, 500, 510, 510};
    layer.WriteLevel0(neighbor, rect, RectDelta(rect, 0.5f));
    const PageSampleRect coarse{250, 250, 255, 255};
    layer.AddAtLevel(neighbor, 1, coarse, RectDelta(coarse, 1.5f));
    constexpr uint32 kMargin = kPageSurfaceWindowMarginSamples;
    constexpr uint32 kStride = kPageOwnedSamples + 2u * kMargin;
    std::vector<float32> window(static_cast<std::size_t>(kStride) * kStride, 0.0f);
    EXPECT_TRUE(HeightPageLayerTestAccess::WindowReached(layer, PageAddress{neighbor, 0, 3, 3}, kMargin));
    // Page (0, 3) of face 0 reads the neighbor's samples along face 0's MinX side.
    EXPECT_FALSE(HeightPageLayerTestAccess::WindowReached(layer, PageAddress{0, 0, 0, 3}, kMargin));
    EXPECT_FALSE(layer.ApplyToWindow(PageAddress{0, 0, 0, 3}, kMargin, window));
    // A layer with no write reaches no window.
    const HeightPageLayer empty(kPageStoreMaxFaces, levels);
    EXPECT_FALSE(HeightPageLayerTestAccess::WindowReached(empty, PageAddress{0, 0, 0, 3}, kMargin));
    EXPECT_FALSE(empty.ApplyToWindow(PageAddress{0, 0, 0, 3}, kMargin, window));
}

// A write that only the samples past a window's side read still reaches the window: a level-0 write and a
// coarse write that prolongs to level 0, both on face 0 a few samples in from its MinX side, where no
// page of the neighbor face is held.
TEST(HeightPageLayerTest, AWindowAddsANeighborsWriteThatOnlyItsSamplesPastTheSideRead)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(1025, 1025);
    HeightPageLayer written(kPageStoreMaxFaces, levels);
    const PageSampleRect rect{4, 400, 5, 420};
    written.WriteLevel0(0, rect, RectDelta(rect, 0.5f));
    HeightPageLayer prolonged(kPageStoreMaxFaces, levels);
    const PageSampleRect coarse{2, 200, 2, 210};
    prolonged.AddAtLevel(0, 1, coarse, RectDelta(coarse, 1.5f));
    const uint8 neighbor = PageFaceNeighbor(0, PageFaceSide::MinX).Face;
    constexpr uint32 kMargin = kPageSurfaceWindowMarginSamples;
    constexpr uint32 kStride = kPageOwnedSamples + 2u * kMargin;
    std::vector<float32> window(static_cast<std::size_t>(kStride) * kStride);
    for (const HeightPageLayer* layer : {&written, &prolonged})
    {
        uint32 heldPages = 0;
        uint32 appliedWindows = 0;
        uint32 changedSamples = 0;
        for (uint32 pz = 0; pz < levels[0].PagesZ; ++pz)
            for (uint32 px = 0; px < levels[0].PagesX; ++px)
            {
                const PageAddress address{neighbor, 0, px, pz};
                heldPages += !layer->PageDeltas(address).empty();
                std::fill(window.begin(), window.end(), 0.0f);
                appliedWindows += layer->ApplyToWindow(address, kMargin, window);
                changedSamples += static_cast<uint32>(
                    std::count_if(window.begin(), window.end(), [](float32 v) { return v != 0.0f; }));
            }
        EXPECT_EQ(heldPages, 0u) << "a neighbor page is held, so the window's own face reaches the write";
        EXPECT_GT(appliedWindows, 0u);
        EXPECT_GT(changedSamples, 0u);
    }
}

// Past each of face 0's four sides, at levels 0 and 1, its windows add the neighbor's level-0, coarse and
// strip-filling writes at the same distance in; face 0 holds no page. On a flat layer a window adds a write
// that only its margin columns read, on the page after it and on the page before it.
TEST(HeightPageLayerTest, AWindowAddsTheDeltasPastEachSideOfAFaceAtLevelsZeroAndOne)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(1025, 1025);
    constexpr uint32 kMargin = kPageSurfaceWindowMarginSamples;
    constexpr uint32 kStride = kPageOwnedSamples + 2u * kMargin;
    constexpr std::array kSides{PageFaceSide::MinZ, PageFaceSide::MaxZ, PageFaceSide::MinX, PageFaceSide::MaxX};
    HeightPageLayer written(kPageStoreMaxFaces, levels);
    HeightPageLayer prolonged(kPageStoreMaxFaces, levels);
    HeightPageLayer margin(kPageStoreMaxFaces, levels);
    for (PageFaceSide side : kSides)
    {
        const PageFaceEdge edge = PageFaceNeighbor(0, side);
        const PageSampleRect rect = SideBand(edge.Side, 300, 310, 4, 5, levels[0].SamplesX);
        written.WriteLevel0(edge.Face, rect, RectDelta(rect, 0.5f));
        const PageSampleRect coarse = SideBand(edge.Side, 150, 155, 2, 3, levels[1].SamplesX);
        prolonged.AddAtLevel(edge.Face, 1, coarse, RectDelta(coarse, 1.5f));
        const PageSampleRect band = SideBand(edge.Side, 300, 310, 1, kMargin, levels[0].SamplesX);
        margin.WriteLevel0(edge.Face, band, RectDelta(band, 0.25f));
    }
    std::vector<float32> window(static_cast<std::size_t>(kStride) * kStride);
    for (const HeightPageLayer* layer : {&written, &prolonged, &margin})
    {
        std::map<PageAddress, std::vector<float32>> pages;
        for (uint8 level : {uint8{0}, uint8{1}})
        {
            const int64 last = static_cast<int64>(levels[level].SamplesX) - 1;
            uint32 heldPages = 0;
            uint32 appliedMismatches = 0;
            std::array<uint32, kSides.size()> sideWrites{};
            std::array<uint32, kSides.size()> sideMismatches{};
            for (uint32 pz = 0; pz < levels[level].PagesZ; ++pz)
                for (uint32 px = 0; px < levels[level].PagesX; ++px)
                {
                    const PageAddress address{0, level, px, pz};
                    heldPages += !layer->PageDeltas(address).empty();
                    std::fill(window.begin(), window.end(), 0.0f);
                    const bool applied = layer->ApplyToWindow(address, kMargin, window);
                    appliedMismatches +=
                        applied != std::any_of(window.begin(), window.end(), [](float32 v)
                                               { return v != 0.0f; });
                    for (uint32 r = 0; r < kStride; ++r)
                        for (uint32 c = 0; c < kStride; ++c)
                        {
                            const auto [x, z] = WindowFaceSample(address, c, r, kMargin, last);
                            const bool outsideX = x < 0 || x > last;
                            const bool outsideZ = z < 0 || z > last;
                            if (outsideX == outsideZ)
                                continue;
                            const PageFaceSide past = outsideX ? (x < 0 ? PageFaceSide::MinX : PageFaceSide::MaxX)
                                                               : (z < 0 ? PageFaceSide::MinZ : PageFaceSide::MaxZ);
                            const std::size_t sideIndex = std::ranges::find(kSides, past) - kSides.begin();
                            const int64 distance = outsideX ? x : z;
                            const PageFaceEdge across = PageFaceNeighbor(0, past);
                            const int64 along = AlongSide(past, x, z);
                            const auto [theirX, theirZ] = SideSample(across.Side, across.Reversed ? last - along : along,
                                                                     distance < 0 ? -distance : distance - last, last + 1);
                            const float32 expected = AppliedPageSample(*layer, pages, across.Face, level, theirX, theirZ);
                            sideWrites[sideIndex] += expected != 0.0f;
                            sideMismatches[sideIndex] += window[static_cast<std::size_t>(r) * kStride + c] != expected;
                        }
                }
            if (layer != &margin)
                EXPECT_EQ(heldPages, 0u) << "level " << int{level} << ": face 0's own pages reach the writes";
            EXPECT_EQ(appliedMismatches, 0u) << "level " << int{level};
            for (std::size_t side = 0; side < kSides.size(); ++side)
            {
                EXPECT_GT(sideWrites[side], 0u) << "level " << int{level} << " side " << side << ": no sample past it reads a write";
                EXPECT_EQ(sideMismatches[side], 0u) << "level " << int{level} << " side " << side;
            }
        }
    }

    // Past page (0, 0)'s apron: only page (1, 0) holds the write.
    HeightPageLayer flat(1, levels);
    const PageSampleRect rect{kPageOwnedSamples + kPageApronSamples, 40, kPageOwnedSamples + kMargin - 1u, 60};
    flat.WriteLevel0(0, rect, RectDelta(rect, 0.5f));
    EXPECT_TRUE(flat.PageDeltas(PageAddress{0, 0, 0, 0}).empty());
    std::fill(window.begin(), window.end(), 0.0f);
    std::vector<float32> page(kPageSampleCount, 0.0f);
    ASSERT_TRUE(flat.ApplyToPage(PageAddress{0, 0, 1, 0}, page));
    EXPECT_TRUE(flat.ApplyToWindow(PageAddress{0, 0, 0, 0}, kMargin, window));
    uint32 marginWrites = 0;
    uint32 marginMismatches = 0;
    for (uint32 z = 0; z < kPageOwnedSamples; ++z)
        for (uint32 x = kPageOwnedSamples; x < kPageOwnedSamples + kMargin; ++x)
        {
            const float32 expected =
                page[(z + kPageApronSamples) * kPageStrideSamples + x - kPageOwnedSamples + kPageApronSamples];
            marginWrites += expected != 0.0f;
            marginMismatches += window[static_cast<std::size_t>(z + kMargin) * kStride + x + kMargin] != expected;
        }
    EXPECT_GT(marginWrites, 0u);
    EXPECT_EQ(marginMismatches, 0u);

    // Before page (1, 0)'s apron: only page (0, 0) holds the write.
    HeightPageLayer low(1, levels);
    const PageSampleRect lowRect{kPageOwnedSamples - kMargin, 40, kPageOwnedSamples - kPageApronSamples - 1u, 60};
    low.WriteLevel0(0, lowRect, RectDelta(lowRect, 0.5f));
    EXPECT_TRUE(low.PageDeltas(PageAddress{0, 0, 1, 0}).empty());
    std::fill(page.begin(), page.end(), 0.0f);
    ASSERT_TRUE(low.ApplyToPage(PageAddress{0, 0, 0, 0}, page));
    std::fill(window.begin(), window.end(), 0.0f);
    EXPECT_TRUE(low.ApplyToWindow(PageAddress{0, 0, 1, 0}, kMargin, window));
    const float32 lowExpected = page[(50 + kPageApronSamples) * kPageStrideSamples + kPageOwnedSamples - kMargin + kPageApronSamples];
    EXPECT_NE(lowExpected, 0.0f);
    EXPECT_EQ(window[(50 + kMargin) * kStride], lowExpected);
}

TEST(GeneratedHeightPageProviderTest, APlanetLatticeThatDoesNotHalveIsEmpty)
{
    GeneratedPlanetLattice lattice;
    lattice.ReferenceRadius = kPlanetRadius;
    lattice.FaceSamples = 2u * kPageOwnedSamples;
    lattice.HeightRow = BandLimitedPlanetHeightRow;
    lattice.HeightRange = DefaultPlanetHeightRange;
    const GeneratedHeightPageProvider provider(std::move(lattice));
    std::vector<float32> page(kPageSampleCount);
    EXPECT_TRUE(provider.Levels().empty());
    EXPECT_EQ(provider.FaceCount(), 1u);
    EXPECT_FALSE(provider.FillPage(PageAddress{0, 0, 0, 0}, page));
}

TEST(PageResidencyDeathTest, TheCpuDirectoryIsBuiltByTheFirstCpuRequestAndAReadBeforeItIsRefused)
{
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    JobSystem::WorkStealingThreadPool pool(2);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, 128, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    const std::vector<PageWant> wanted = EveryPage(*provider);
    residency.Request(1, wanted);
    RunFramesUntil(residency, 0, [&] { return residency.FieldCache(1).ResidentCount() == wanted.size(); });
    ASSERT_EQ(residency.FieldCache(1).ResidentCount(), wanted.size());
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEBUG_DEATH(EXPECT_TRUE(residency.CpuPages(1).Empty()), "first RequestCpu");

    residency.RequestCpu(1, static_cast<uint32>(provider->Levels().size()) - 1u, {});
    const CpuPageDirectory directory = residency.CpuPages(1);
    ASSERT_FALSE(directory.Empty());
    for (const PageWant& want : wanted)
    {
        const CpuPageEntry cpu = directory.Page(want.Address);
        ASSERT_EQ(State(cpu), CpuPageState::Resident);
        EXPECT_EQ(Bytes(cpu), residency.Bytes(1, want.Address)->data());
    }
    pool.Shutdown();
}

namespace
{

// The dense CPU directory as a test mirror: one entry for each page of every level, at the level's
// FirstEntry plus the page's row-major place.
std::size_t DenseDirectoryIndex(std::span<const PageStoreLevel> levels, const PageAddress& address)
{
    const PageStoreLevel& level = levels[address.Level];
    return static_cast<std::size_t>(level.FirstEntry) + static_cast<std::size_t>(address.Z) * level.PagesX + address.X;
}

// A page of face 0 drawn uniformly by level, then by column and row in that level.
PageAddress RandomPyramidPage(std::span<const PageStoreLevel> levels, std::mt19937& random)
{
    const uint32 level = std::uniform_int_distribution<uint32>(0u, static_cast<uint32>(levels.size()) - 1u)(random);
    const uint32 x = std::uniform_int_distribution<uint32>(0u, levels[level].PagesX - 1u)(random);
    const uint32 z = std::uniform_int_distribution<uint32>(0u, levels[level].PagesZ - 1u)(random);
    return PageAddress{0, static_cast<uint8>(level), x, z};
}

// Expects `entries` to hold expected[i] under keys[i] for every i.
void ExpectEveryEntry(const CpuPageEntries& entries, std::span<const uint64> keys,
                      std::span<const CpuPageEntry> expected, const char* when)
{
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        const CpuPageEntry entry = entries.Entry(keys[i]);
        ASSERT_EQ(State(entry), State(expected[i])) << when << ": key " << i << " of " << keys.size();
        ASSERT_EQ(Bytes(entry), Bytes(expected[i])) << when << ": key " << i << " of " << keys.size();
    }
}

} // namespace

TEST(CpuPageDirectoryTest, TheSparseDirectoryAnswersAsTheDenseOneOnRandomResidentAndAbsentSets)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(4097, 2049);
    const std::size_t pageCount = static_cast<std::size_t>(DensePageCount(levels));
    // Each page's bytes are one byte of this buffer, so every Resident page has its own address.
    const std::vector<uint8> bytes(pageCount);
    std::mt19937 random(5u);
    for (const uint32 residentPercent : {0u, 5u, 50u, 100u})
    {
        std::vector<CpuPageEntry> dense(pageCount);
        CpuPageEntries sparse;
        std::uniform_int_distribution<uint32> roll(0u, 99u);
        for (uint32 level = 0; level < levels.size(); ++level)
            for (uint32 z = 0; z < levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < levels[level].PagesX; ++x)
                {
                    const PageAddress address{0, static_cast<uint8>(level), x, z};
                    const std::size_t index = DenseDirectoryIndex(levels, address);
                    const uint32 draw = roll(random);
                    if (draw < residentPercent)
                        dense[index] = ResidentEntry(&bytes[index]);
                    else if (draw < residentPercent + (100u - residentPercent) / 4u)
                        dense[index] = CpuPageEntry::Absent;
                    if (dense[index] != CpuPageEntry::Missing)
                        sparse.InsertOrAssign(CpuPageDirectory::Key(address), dense[index]);
                    // A dropped page leaves both directories.
                    if (index % 7u == 0)
                    {
                        dense[index] = CpuPageEntry::Missing;
                        sparse.Erase(CpuPageDirectory::Key(address));
                    }
                }

        const CpuPageDirectory directory{&sparse, 1u, {}, nullptr, 1u};
        for (uint32 level = 0; level < levels.size(); ++level)
            for (uint32 z = 0; z < levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < levels[level].PagesX; ++x)
                {
                    const PageAddress address{0, static_cast<uint8>(level), x, z};
                    const CpuPageEntry expected = dense[DenseDirectoryIndex(levels, address)];
                    const CpuPageEntry page = directory.Page(address);
                    ASSERT_EQ(State(page), State(expected))
                        << residentPercent << "% resident: level " << level << " page " << x << ", " << z;
                    EXPECT_EQ(directory.BytesOf(page).data(), Bytes(expected));
                }
        EXPECT_EQ(directory.Page(PageAddress{0, static_cast<uint8>(levels.size()), 0, 0}), CpuPageEntry::Missing);
        EXPECT_EQ(directory.Page(PageAddress{1, 0, 0, 0}), CpuPageEntry::Missing) << "another face";
    }
}

TEST(CpuPageDirectoryTest, TheEntriesKeepEveryKeyThroughDisplacementsAndRehashesOnManyKeys)
{
    // Keys on every face and level, each with its own row: the table grows from its fewest buckets
    // to thousands, and a full bucket moves keys to their other bucket many times on the way.
    constexpr std::size_t kKeys = std::size_t{1} << 16u;
    const std::vector<uint8> bytes(kKeys);
    std::mt19937 random(13u);
    std::vector<uint64> keys(kKeys);
    std::vector<CpuPageEntry> expected(kKeys);
    CpuPageEntries entries;
    for (std::size_t i = 0; i < kKeys; ++i)
    {
        const PageAddress address{static_cast<uint8>(random() % 6u), static_cast<uint8>(random() % 24u),
                                  static_cast<uint32>(random() % (1u << 23u)), static_cast<uint32>(i)};
        keys[i] = CpuPageDirectory::Key(address);
        expected[i] = i % 4u == 0 ? CpuPageEntry::Absent : ResidentEntry(&bytes[i]);
        entries.InsertOrAssign(keys[i], expected[i]);
        if (std::has_single_bit(i + 1u))
            ExpectEveryEntry(entries, std::span(keys).first(i + 1u), std::span(expected).first(i + 1u), "growing");
    }
    for (std::size_t i = 0; i < kKeys; i += 3u)
    {
        entries.Erase(keys[i]);
        expected[i] = CpuPageEntry::Missing;
    }
    ExpectEveryEntry(entries, keys, expected, "after erasing every third key");
    // The erased keys come back with the other state: an Absent page lands, a Resident one is absent.
    for (std::size_t i = 0; i < kKeys; i += 3u)
    {
        expected[i] = i % 4u == 0 ? ResidentEntry(&bytes[i]) : CpuPageEntry::Absent;
        entries.InsertOrAssign(keys[i], expected[i]);
    }
    ExpectEveryEntry(entries, keys, expected, "after inserting them again");
    EXPECT_EQ(entries.Entry(CpuPageDirectory::Key(PageAddress{6, 0, 0, 0})), CpuPageEntry::Missing)
        << "a key never inserted";
}

TEST(CpuPageDirectoryTest, TheEntriesKeepAKeyThatFindsNoSlotWhenTheTableGrows)
{
    // CpuPageEntries' two hashes. Every key here is in bucket 0 under both hashes at 64 buckets or
    // fewer, so the fifth key finds no slot and the table grows with that key in hand.
    constexpr uint64 kFirstHash = Hashing::kGoldenRatio64;
    constexpr uint64 kSecondHash = 0xC2B2AE3D27D4EB4Full;
    constexpr uint32 kSharedBucketBits = 6;
    constexpr std::size_t kKeys = 9;
    const std::vector<uint8> bytes(kKeys);
    std::vector<uint64> keys;
    for (uint32 x = 0; keys.size() < kKeys; ++x)
    {
        const uint64 key = CpuPageDirectory::Key(PageAddress{0, 0, x, 0});
        if ((key * kFirstHash) >> (64u - kSharedBucketBits) == 0 && (key * kSecondHash) >> (64u - kSharedBucketBits) == 0)
            keys.push_back(key);
    }
    std::vector<CpuPageEntry> expected;
    CpuPageEntries entries;
    for (std::size_t i = 0; i < kKeys; ++i)
    {
        expected.push_back(ResidentEntry(&bytes[i]));
        entries.InsertOrAssign(keys[i], expected[i]);
        ExpectEveryEntry(entries, std::span(keys).first(i + 1u), expected, "after an insert");
    }
}

TEST(PageResidencyTest, AnEarthFaceStoreBuildsACpuDirectoryOfThePagesTheCpuLoaded)
{
    // The planar shape of an Earth face at 0.5 m: 20,185,089 samples a side, a pyramid of about 33
    // billion pages.
    constexpr uint32 kTiles = 19712;
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 512.0f, kTiles, kTiles));
    ASSERT_EQ(provider->Levels().front().SamplesX, 20185089u);
    JobSystem::WorkStealingThreadPool pool(2);
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, 128, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    const uint32 top = static_cast<uint32>(provider->Levels().size()) - 1u;
    const std::vector<PageAddress> window{PageAddress{0, 0, 100000, 100000}, PageAddress{0, 0, 100001, 100000}};
    const PageAddress topPage{0, static_cast<uint8>(top), 0, 0};
    const auto loaded = [&] {
        return residency.Bytes(1, window[0]) && residency.Bytes(1, window[1]) && residency.Bytes(1, topPage);
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (uint64 frame = 1; !loaded() && std::chrono::steady_clock::now() < deadline; ++frame)
    {
        residency.RequestCpu(1, top, window);
        residency.Update(frame, 1.0f / 60.0f, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(loaded());

    const CpuPageDirectory directory = residency.CpuPages(1);
    for (const PageAddress& address : {window[0], window[1], topPage})
    {
        const CpuPageEntry cpu = directory.Page(address);
        ASSERT_EQ(State(cpu), CpuPageState::Resident);
        EXPECT_EQ(Bytes(cpu), residency.Bytes(1, address)->data());
    }
    EXPECT_EQ(directory.Page(PageAddress{0, 0, 0, 0}), CpuPageEntry::Missing);
    ASSERT_FALSE(directory.Empty());
    pool.Shutdown();
}
