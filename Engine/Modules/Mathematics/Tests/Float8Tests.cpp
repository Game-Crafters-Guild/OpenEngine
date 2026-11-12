#include "Mathematics/Float8.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/ScalarFloat8.h"
#if defined(__AVX2__)
#include "Mathematics/Avx2Float8.h"
#endif

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace Mathematics = GameEngine::Mathematics;
using Mathematics::Float8;
using Mathematics::Float8Detail::ScalarFloat8;

namespace
{

constexpr std::size_t kLanes = Float8::kLaneCount;
using Lanes = std::array<float, kLanes>;

// One step of a 32-bit linear congruential generator.
std::uint32_t Next(std::uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

// Operands that reach every rounding case: random bit patterns (all exponents, subnormals, NaN and
// infinity), then the zeros, the extremes and values near 1.
std::vector<float> Operands()
{
    std::vector<float> values = {0.0f,
                                 -0.0f,
                                 1.0f,
                                 -1.0f,
                                 std::numeric_limits<float>::denorm_min(),
                                 std::numeric_limits<float>::min(),
                                 std::numeric_limits<float>::max(),
                                 -std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::infinity(),
                                 -std::numeric_limits<float>::infinity(),
                                 std::numeric_limits<float>::quiet_NaN(),
                                 1.0000001f,
                                 0.99999994f,
                                 12582912.0f};
    std::uint32_t state = 6808u;
    while (values.size() % kLanes != 0u || values.size() < 4096u)
        values.push_back(std::bit_cast<float>(Next(state)));
    return values;
}

// Bit equality, with every NaN equal to every NaN: NaN payloads are not part of the contract.
bool SameBits(float a, float b)
{
    if (std::isnan(a) || std::isnan(b))
        return std::isnan(a) && std::isnan(b);
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

template <typename Lanes8>
Lanes Stored(Lanes8 value)
{
    Lanes out{};
    value.Store(out);
    return out;
}

float FlipSignWhereLowBitSet(float value, float bits)
{
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) ^ (std::bit_cast<std::uint32_t>(bits) << 31u));
}

// The lanes of each operation of Lanes8 against the scalar operation; the count of lanes that differ.
template <typename Lanes8>
std::uint32_t LanesThatDifferFromScalar()
{
    const std::vector<float> values = Operands();
    std::uint32_t mismatches = 0;
    for (std::size_t first = 0; first + kLanes <= values.size(); first += kLanes)
    {
        // The second operand is the first rotated by three lanes, so every pair of kinds meets.
        Lanes a{}, b{};
        for (std::size_t lane = 0; lane < kLanes; ++lane)
        {
            a[lane] = values[first + lane];
            b[lane] = values[(first + lane * 3u + 1u) % values.size()];
        }
        const Lanes8 la = Lanes8::Load(a);
        const Lanes8 lb = Lanes8::Load(b);
        const Lanes sum = Stored(la + lb), difference = Stored(la - lb), product = Stored(la * lb),
                    quotient = Stored(la / lb), flipped = Stored(Lanes8::FlipSignWhereLowBitSet(la, lb)),
                    loaded = Stored(la), broadcast = Stored(Lanes8::Broadcast(a[0]));
        // Each comparison is seen through Select: lane i is a[i] where it holds, else b[i].
        const Lanes less = Stored(Lanes8::Select(la < lb, la, lb)),
                    lessOrEqual = Stored(Lanes8::Select(la <= lb, la, lb)),
                    greater = Stored(Lanes8::Select(la > lb, la, lb)),
                    greaterOrEqual = Stored(Lanes8::Select(la >= lb, la, lb));
        // Equal operands, where < and <= differ.
        const Lanes lessSelf = Stored(Lanes8::Select(la < la, la, lb)),
                    lessOrEqualSelf = Stored(Lanes8::Select(la <= la, la, lb)),
                    greaterSelf = Stored(Lanes8::Select(la > la, la, lb)),
                    greaterOrEqualSelf = Stored(Lanes8::Select(la >= la, la, lb));
        bool anyLess = false;
        for (std::size_t lane = 0; lane < kLanes; ++lane)
        {
            mismatches += SameBits(sum[lane], a[lane] + b[lane]) ? 0u : 1u;
            mismatches += SameBits(difference[lane], a[lane] - b[lane]) ? 0u : 1u;
            mismatches += SameBits(product[lane], a[lane] * b[lane]) ? 0u : 1u;
            mismatches += SameBits(quotient[lane], a[lane] / b[lane]) ? 0u : 1u;
            mismatches += SameBits(flipped[lane], FlipSignWhereLowBitSet(a[lane], b[lane])) ? 0u : 1u;
            mismatches += SameBits(loaded[lane], a[lane]) ? 0u : 1u;
            mismatches += SameBits(broadcast[lane], a[0]) ? 0u : 1u;
            mismatches += SameBits(less[lane], a[lane] < b[lane] ? a[lane] : b[lane]) ? 0u : 1u;
            mismatches += SameBits(lessOrEqual[lane], a[lane] <= b[lane] ? a[lane] : b[lane]) ? 0u : 1u;
            mismatches += SameBits(greater[lane], a[lane] > b[lane] ? a[lane] : b[lane]) ? 0u : 1u;
            mismatches += SameBits(greaterOrEqual[lane], a[lane] >= b[lane] ? a[lane] : b[lane]) ? 0u : 1u;
            const float same = a[lane];
            mismatches += SameBits(lessSelf[lane], a[lane] < same ? a[lane] : b[lane]) ? 0u : 1u;
            mismatches += SameBits(lessOrEqualSelf[lane], a[lane] <= same ? a[lane] : b[lane]) ? 0u : 1u;
            mismatches += SameBits(greaterSelf[lane], a[lane] > same ? a[lane] : b[lane]) ? 0u : 1u;
            mismatches += SameBits(greaterOrEqualSelf[lane], a[lane] >= same ? a[lane] : b[lane]) ? 0u : 1u;
            anyLess = anyLess || a[lane] < b[lane];
        }
        mismatches += (la < lb).Any() == anyLess ? 0u : 1u;
    }
    return mismatches;
}

} // namespace

TEST(Float8, EveryLaneOfADefaultFloat8IsPositiveZero)
{
    for (const float lane : Stored(Float8{}))
        EXPECT_EQ(std::bit_cast<std::uint32_t>(lane), 0u);
}

TEST(Float8, EachScalarOperationHasTheBitsOfTheScalarOperationInEachLane)
{
    EXPECT_EQ(LanesThatDifferFromScalar<ScalarFloat8>(), 0u);
}

TEST(Float8, EachAvx2OperationHasTheBitsOfTheScalarOperationInEachLane)
{
#if defined(__AVX2__)
    EXPECT_EQ(LanesThatDifferFromScalar<Mathematics::Float8Detail::Avx2Float8>(), 0u);
#else
    GTEST_SKIP() << "The build has no AVX2: Float8 is ScalarFloat8.";
#endif
}

// The two implementations against each other, through each operation and a chain of them.
TEST(Float8, TheAvx2AndTheScalarImplementationGiveTheSameBits)
{
#if defined(__AVX2__)
    using Mathematics::Float8Detail::Avx2Float8;
    const std::vector<float> values = Operands();
    std::uint32_t mismatches = 0;
    for (std::size_t first = 0; first + 2u * kLanes <= values.size(); first += kLanes)
    {
        const std::span<const float, kLanes> a(values.data() + first, kLanes);
        const std::span<const float, kLanes> b(values.data() + first + kLanes, kLanes);
        const Avx2Float8 wa = Avx2Float8::Load(a), wb = Avx2Float8::Load(b);
        const ScalarFloat8 sa = ScalarFloat8::Load(a), sb = ScalarFloat8::Load(b);
        const std::array<std::pair<Lanes, Lanes>, 10> results = {
            std::pair{Stored(wa + wb), Stored(sa + sb)},
            std::pair{Stored(wa - wb), Stored(sa - sb)},
            std::pair{Stored(wa * wb), Stored(sa * sb)},
            std::pair{Stored(wa / wb), Stored(sa / sb)},
            std::pair{Stored(Avx2Float8::FlipSignWhereLowBitSet(wa, wb)),
                      Stored(ScalarFloat8::FlipSignWhereLowBitSet(sa, sb))},
            std::pair{Stored((wa * wb + wa) / wb - wa), Stored((sa * sb + sa) / sb - sa)},
            std::pair{Stored(Avx2Float8::Select(wa < wb, wa, wb)), Stored(ScalarFloat8::Select(sa < sb, sa, sb))},
            std::pair{Stored(Avx2Float8::Select(wa <= wb, wa, wb)), Stored(ScalarFloat8::Select(sa <= sb, sa, sb))},
            std::pair{Stored(Avx2Float8::Select(wa > wb, wa, wb)), Stored(ScalarFloat8::Select(sa > sb, sa, sb))},
            std::pair{Stored(Avx2Float8::Select(wa >= wb, wa, wb)), Stored(ScalarFloat8::Select(sa >= sb, sa, sb))}};
        for (const auto& [wide, scalar] : results)
            for (std::size_t lane = 0; lane < kLanes; ++lane)
                mismatches += SameBits(wide[lane], scalar[lane]) ? 0u : 1u;
        mismatches += (wa < wb).Any() == (sa < sb).Any() ? 0u : 1u;
        mismatches += (wa >= wb).Any() == (sa >= sb).Any() ? 0u : 1u;
    }
    EXPECT_EQ(mismatches, 0u);
#else
    GTEST_SKIP() << "The build has no AVX2: there is one implementation.";
#endif
}

namespace
{

// Any is true when the comparison holds in one lane at each lane position, and false when it holds in none.
template <typename Lanes8>
std::uint32_t AnyMismatches()
{
    std::uint32_t mismatches = 0;
    const Lanes8 zeros = Lanes8::Broadcast(0.0f);
    for (std::size_t set = 0; set < kLanes; ++set)
    {
        Lanes values{};
        values[set] = 1.0f;
        mismatches += (Lanes8::Load(values) > zeros).Any() ? 0u : 1u;
    }
    mismatches += (zeros > zeros).Any() ? 1u : 0u;
    mismatches += (Lanes8::Broadcast(std::numeric_limits<float>::quiet_NaN()) <= zeros).Any() ? 1u : 0u;
    return mismatches;
}

// The lanes of Lanes8::Clamp01 that differ from the scalar Math::Clamp01, over the operands (NaN, the zeros,
// subnormals, the infinities) and the values at and beside 0 and 1.
template <typename Lanes8>
std::uint32_t Clamp01Mismatches()
{
    std::vector<float> values = Operands();
    for (const float edge : {0.0f, -0.0f, 1.0f, 0.5f, -1e-30f, 1e-30f, 1e-40f, -1e-40f, 0.99999994f, 1.0000001f,
                             2.0f, -2.0f})
        values.push_back(edge);
    while (values.size() % kLanes != 0u)
        values.push_back(0.25f);
    std::uint32_t mismatches = 0;
    for (std::size_t first = 0; first + kLanes <= values.size(); first += kLanes)
    {
        const std::span<const float, kLanes> lanes(values.data() + first, kLanes);
        const Lanes clamped = Stored(Lanes8::Clamp01(Lanes8::Load(lanes)));
        for (std::size_t lane = 0; lane < kLanes; ++lane)
            mismatches += SameBits(clamped[lane], GameEngine::Math::Clamp01(lanes[lane])) ? 0u : 1u;
    }
    return mismatches;
}

} // namespace

TEST(Float8, AnyHoldsWhenOneLaneHoldsAndNotWhenNoneDoes)
{
    EXPECT_EQ(AnyMismatches<ScalarFloat8>(), 0u);
#if defined(__AVX2__)
    EXPECT_EQ(AnyMismatches<Mathematics::Float8Detail::Avx2Float8>(), 0u);
#endif
}

TEST(Float8, Clamp01HasTheBitsOfTheScalarClamp01InEachLane)
{
    EXPECT_EQ(Clamp01Mismatches<ScalarFloat8>(), 0u);
#if defined(__AVX2__)
    EXPECT_EQ(Clamp01Mismatches<Mathematics::Float8Detail::Avx2Float8>(), 0u);
#endif
}
