#include "Types/Fnv1a.h"
#include "Types/StringId.h"

#include <gtest/gtest.h>

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

static_assert(Fnv1a64(std::string_view{}) == 0xcbf29ce484222325ull);
static_assert(Fnv1a64("a") == 0xaf63dc4c8601ec8cull);
static_assert(Fnv1a64("foobar") == 0x85944171f73967e8ull);
static_assert(GameEngine::HashStringId("foobar") == 0x85944171f73967e8ull);

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
    // SplitMix64's finalizer of the address: these values pin the mix the UI's identity keys use.
    EXPECT_EQ(HashPointer(reinterpret_cast<const void*>(std::uintptr_t{1})), 0x5692161d100b05e5ull);
    EXPECT_EQ(HashPointer(reinterpret_cast<const void*>(std::uintptr_t{0x1000})), 0xf06041805d06b780ull);
    EXPECT_EQ(HashPointer(nullptr), 0u);
}

TEST(Fnv1aTests, HashCombineKeepsItsValuesAndTheirOrder)
{
    // Pipeline cache keys are built with HashCombine; these values pin the mix.
    EXPECT_EQ(HashCombine(0, 0), 0x9e3779b97f4a7c15ull);
    EXPECT_EQ(HashCombine(1, 2), 0x9e3779b97f4a8c16ull);
    EXPECT_EQ(HashCombine(2, 1), 0x9e3779b97f4a9c14ull);
}

} // namespace
