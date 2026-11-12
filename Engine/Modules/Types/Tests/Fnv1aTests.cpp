#include "Types/Fnv1a.h"
#include "Types/StringId.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace
{

using GameEngine::Hashing::Fnv1a64;
using GameEngine::Hashing::Fnv1a64Values;
using GameEngine::Hashing::HashCombine;
using GameEngine::Hashing::HashPointer;
using GameEngine::Hashing::kFnv1a64OffsetBasis;
using GameEngine::Hashing::kGoldenRatio64;
using GameEngine::Hashing::SplitMix64;
using GameEngine::Hashing::UnitDraw;

static_assert(Fnv1a64(std::string_view{}) == 0xcbf29ce484222325ull);
static_assert(Fnv1a64("a") == 0xaf63dc4c8601ec8cull);
static_assert(Fnv1a64("foobar") == 0x85944171f73967e8ull);
static_assert(GameEngine::HashStringId("foobar") == 0x85944171f73967e8ull);
// One pinned draw: a change to the mix or to the bits the draw keeps changes every seeded result.
static_assert(UnitDraw(7, 11, 13, 17) == 0.469842135906219482421875f);

TEST(Fnv1aTests, MatchesStandardKnownVectors)
{
    EXPECT_EQ(Fnv1a64(std::string_view{}), 0xcbf29ce484222325ull);
    EXPECT_EQ(Fnv1a64("a"), 0xaf63dc4c8601ec8cull);
    EXPECT_EQ(Fnv1a64("foobar"), 0x85944171f73967e8ull);
}

TEST(Fnv1aTests, ByteHashSupportsChaining)
{
    constexpr std::array<std::uint8_t, 6> bytes{'f', 'o', 'o', 'b', 'a', 'r'};
    const std::uint64_t first = Fnv1a64(bytes.data(), 3, kFnv1a64OffsetBasis);
    const std::uint64_t chained = Fnv1a64(bytes.data() + 3, 3, first);

    EXPECT_EQ(chained, Fnv1a64(bytes.data(), bytes.size(), kFnv1a64OffsetBasis));
    EXPECT_EQ(chained, 0x85944171f73967e8ull);
}

TEST(Fnv1aTests, ValuesHashTheCountThenTheBytes)
{
    constexpr std::array<std::uint32_t, 2> values{1, 2};
    EXPECT_EQ(Fnv1a64Values(kFnv1a64OffsetBasis, values), 0xc72c2c2fcfced1a4ull);
    EXPECT_EQ(Fnv1a64Values(kFnv1a64OffsetBasis, std::vector<std::uint32_t>{1, 2}), 0xc72c2c2fcfced1a4ull)
        << "any contiguous range of the same values hashes the same";
    EXPECT_EQ(Fnv1a64Values(kFnv1a64OffsetBasis, std::span<const std::uint32_t>{}), 0xa8c7f832281a39c5ull)
        << "an empty run still folds its count";
}

TEST(Fnv1aTests, HashPointerMixesTheAddress)
{
    // SplitMix64 of the address: these values pin the mix the UI's identity keys use.
    EXPECT_EQ(HashPointer(reinterpret_cast<const void*>(std::uintptr_t{1})), 0x910a2dec89025cc1ull);
    EXPECT_EQ(HashPointer(reinterpret_cast<const void*>(std::uintptr_t{0x1000})), 0xd73a9a3d941e7ec7ull);
    EXPECT_EQ(HashPointer(nullptr), 0xe220a8397b1dcdafull);
}

TEST(Fnv1aTests, SplitMix64MatchesTheReferenceGenerator)
{
    // The first three outputs of the reference splitmix64.c (Vigna, prng.di.unimi.it/splitmix64.c)
    // with the seed 1234567. The generator adds the increment to its state before each output.
    constexpr std::uint64_t kSeed = 1234567;
    EXPECT_EQ(SplitMix64(kSeed), 6457827717110365317ull);
    EXPECT_EQ(SplitMix64(kSeed + kGoldenRatio64), 3203168211198807973ull);
    EXPECT_EQ(SplitMix64(kSeed + 2 * kGoldenRatio64), 9817491932198370423ull);
}

TEST(Fnv1aTests, UnitDrawIsAPureFunctionOfEachInput)
{
    const float draw = UnitDraw(7, 11, 13, 17);
    EXPECT_EQ(UnitDraw(7, 11, 13, 17), draw);
    EXPECT_NE(UnitDraw(8, 11, 13, 17), draw) << "seed";
    EXPECT_NE(UnitDraw(7, 12, 13, 17), draw) << "agent";
    EXPECT_NE(UnitDraw(7, 11, 14, 17), draw) << "tick";
    EXPECT_NE(UnitDraw(7, 11, 13, 18), draw) << "index";
}

TEST(Fnv1aTests, UnitDrawIsUniformOverTheUnitInterval)
{
    constexpr std::uint64_t kDrawCount = 100000;
    constexpr std::size_t kBinCount = 16;
    constexpr double kMeanTolerance = 0.01;
    std::array<std::uint64_t, kBinCount> bins{};
    double sum = 0.0;
    for (std::uint64_t index = 0; index < kDrawCount; ++index)
    {
        const float draw = UnitDraw(42, 3, 5, index);
        ASSERT_GE(draw, 0.0f);
        ASSERT_LT(draw, 1.0f);
        sum += draw;
        ++bins[static_cast<std::size_t>(draw * kBinCount)];
    }
    EXPECT_NEAR(sum / static_cast<double>(kDrawCount), 0.5, kMeanTolerance);
    EXPECT_EQ(std::ranges::count(bins, std::uint64_t{0}), 0) << "every sixteenth of [0, 1) gets a draw";
}

TEST(Fnv1aTests, HashCombineKeepsItsValuesAndTheirOrder)
{
    // Pipeline cache keys are built with HashCombine; these values pin the mix.
    EXPECT_EQ(HashCombine(0, 0), 0x9e3779b97f4a7c15ull);
    EXPECT_EQ(HashCombine(1, 2), 0x9e3779b97f4a8c16ull);
    EXPECT_EQ(HashCombine(2, 1), 0x9e3779b97f4a9c14ull);
}

} // namespace
