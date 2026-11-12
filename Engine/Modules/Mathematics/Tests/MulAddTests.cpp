#include "Mathematics/MulAdd.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

using GameEngine::Math::MulAdd;
using GameEngine::Math::MulAddDetail::SoftMulAdd;

namespace
{

// (1 + 2^-12)^2 - (1 + 2^-11) is 2^-24 exactly. Rounding the product first gives 1 + 2^-11 (a tie
// to even), so a multiply then an add gives 0.
constexpr float kFactor = 0x1.001p0f;
constexpr float kAddend = -0x1.002p0f;

static_assert(MulAdd(kFactor, kFactor, kAddend) == 0x1p-24f, "a constant MulAdd rounds once");

std::uint32_t Bits(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

// One step of a 32-bit linear congruential generator.
std::uint32_t Next(std::uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

} // namespace

TEST(MulAdd, RoundsOnce)
{
    EXPECT_EQ(MulAdd(kFactor, kFactor, kAddend), 0x1p-24f);
    // (1 + 2^-27)^2 - (1 + 2^-26) is 2^-54; the rounded product cancels to 0.
    EXPECT_EQ(MulAdd(0x1.0000002p0, 0x1.0000002p0, -0x1.0000004p0), 0x1p-54);
}

TEST(MulAdd, AMultiplyThenAnAddWrittenInCodeRoundsTwice)
{
    // volatile keeps the compiler from folding the expression at compile time.
    volatile float factor = kFactor;
    volatile float addend = kAddend;
    const float a = factor;
    const float c = addend;
    EXPECT_EQ(a * a + c, 0.0f) << "the compiler fused a * b + c: -ffp-contract=off is missing";
}

TEST(MulAdd, SoftwareFormGivesTheBitsOfTheHardwareFma)
{
    // The exact sum 1 + 2^-23 + 2^-24 - 2^-60 is just below the midpoint of two floats, and its
    // nearest double is the midpoint itself: rounding the double to float would round up.
    const float a = 0x1.00004p-24f;
    const float b = 0x1.ffff8p-1f;
    const float c = 0x1.000002p0f;
    EXPECT_EQ(SoftMulAdd(a, b, c), c);
    EXPECT_EQ(MulAdd(a, b, c), c);
    static_assert(MulAdd(0x1.00004p-24f, 0x1.ffff8p-1f, 0x1.000002p0f) == 0x1.000002p0f);

    const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, 0x1p-149f, -0x1p-126f, 0x1.fffffep127f,
                              std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()};
    for (float x : specials)
        for (float y : specials)
            for (float z : specials)
            {
                const float expected = std::fma(x, y, z);
                const float actual = SoftMulAdd(x, y, z);
                if (std::isnan(expected))
                    EXPECT_TRUE(std::isnan(actual)) << x << " " << y << " " << z;
                else
                    EXPECT_EQ(Bits(actual), Bits(expected)) << x << " " << y << " " << z;
            }

    // Random operands over a wide exponent range, with the addend often near -a * b so the sum
    // cancels and lands on halfway and subnormal cases.
    std::uint32_t state = 1u;
    int mismatches = 0;
    for (int i = 0; i < 1 << 20; ++i)
    {
        const float x = std::ldexp(1.0f + static_cast<float>(Next(state) >> 9) * 0x1p-23f,
                                   static_cast<int>(Next(state) % 120u) - 60);
        const float y = std::ldexp(1.0f + static_cast<float>(Next(state) >> 9) * 0x1p-23f,
                                   static_cast<int>(Next(state) % 120u) - 60);
        const float nearProduct = -(x * y);
        const float z = (i & 1) != 0 ? std::nextafter(nearProduct, static_cast<float>(Next(state) & 1u))
                                     : std::ldexp(static_cast<float>(static_cast<std::int32_t>(Next(state))),
                                                  static_cast<int>(Next(state) % 160u) - 180);
        if (Bits(SoftMulAdd(x, y, z)) != Bits(std::fma(x, y, z)))
            ++mismatches;
    }
    EXPECT_EQ(mismatches, 0);
}
