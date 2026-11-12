#include "Mathematics/Transcendental.h"

#include "Types/Fnv1a.h"
#include "Types/FormatNumber.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <vector>

namespace Math = GameEngine::Math;
namespace Hashing = GameEngine::Hashing;

namespace
{

constexpr float kFloatMax = std::numeric_limits<float>::max();
constexpr float kInfinity = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kHalfPi = 1.57079637f;
constexpr float kPi = 3.14159274f;

// One step of a 32-bit linear congruential generator.
std::uint32_t Next(std::uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

// A float in [low, high).
float Uniform(std::uint32_t& state, float low, float high)
{
    return low + (high - low) * (static_cast<float>(Next(state) >> 8) * 0x1p-24f);
}

// A finite float of any sign and exponent.
float AnyFinite(std::uint32_t& state)
{
    for (;;)
    {
        const float value = std::bit_cast<float>(Next(state));
        if (std::isfinite(value))
            return value;
    }
}

// |value - reference| in units of the float spacing at the reference.
double FloatUlps(float value, double reference)
{
    int exponent = 0;
    std::frexp(reference, &exponent);
    return std::fabs(static_cast<double>(value) - reference) / std::ldexp(1.0, std::max(exponent - 24, -149));
}

double DoubleUlps(double value, double reference)
{
    int exponent = 0;
    std::frexp(reference, &exponent);
    return std::fabs(value - reference) / std::ldexp(1.0, std::max(exponent - 53, -1074));
}

std::uint32_t Bits(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

// The largest error over the sweep, with the input that gave it.
struct WorstCase
{
    double Ulps = 0.0;
    double Input = 0.0;

    void Add(double ulps, double input)
    {
        if (!(ulps <= Ulps))
        {
            Ulps = ulps;
            Input = input;
        }
    }
};

} // namespace

TEST(Transcendental, SinAndCosOfAFloatAreWithinOneUlpOfTheExactValue)
{
    WorstCase sin;
    WorstCase cos;
    std::uint32_t state = 1u;
    for (int i = 0; i < 300000; ++i)
    {
        const float x = i < 100000 ? Uniform(state, -25.0f, 25.0f) : (i < 150000 ? Uniform(state, -0x1p21f, 0x1p21f) : AnyFinite(state));
        const float s = Math::Sin(x);
        const float c = Math::Cos(x);
        sin.Add(FloatUlps(s, std::sin(static_cast<double>(x))), x);
        cos.Add(FloatUlps(c, std::cos(static_cast<double>(x))), x);
        ASSERT_EQ(Bits(Math::Sin(-x)), Bits(-s)) << x;
        ASSERT_EQ(Bits(Math::Cos(-x)), Bits(c)) << x;
    }
    RecordProperty("SinMaxUlps", std::to_string(sin.Ulps));
    RecordProperty("CosMaxUlps", std::to_string(cos.Ulps));
    EXPECT_LE(sin.Ulps, 1.0) << "at " << sin.Input;
    EXPECT_LE(cos.Ulps, 1.0) << "at " << cos.Input;
}

TEST(Transcendental, SinOfADoubleIsWithinOneUlpInItsDomain)
{
    // The reference is the C library's double sin, which is within 1 ulp itself.
    WorstCase sin;
    std::uint32_t state = 2u;
    for (int i = 0; i < 200000; ++i)
    {
        const double scale = i < 100000 ? 25.0 : 0x1.921fbp+20;
        const double x = scale * (static_cast<double>(Next(state)) * 0x1p-31 - 1.0);
        sin.Add(DoubleUlps(Math::Sin(x), std::sin(x)), x);
    }
    RecordProperty("SinMaxUlps", std::to_string(sin.Ulps));
    EXPECT_LE(sin.Ulps, 2.0) << "at " << sin.Input;
    // At a multiple of pi the first reduction round cancels most bits. The second round gives these bits.
    EXPECT_EQ(std::bit_cast<std::uint64_t>(Math::Sin(std::numbers::pi)), std::bit_cast<std::uint64_t>(0x1.1a62633145c07p-53));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(Math::Sin(2.0 * std::numbers::pi)), std::bit_cast<std::uint64_t>(-0x1.1a62633145c07p-52));
    EXPECT_TRUE(std::isnan(Math::Sin(0x1.921fcp+20)));
    EXPECT_TRUE(std::isnan(Math::Sin(-1.0e300)));
}

// The bake sites call SinCos; its bits are those of Sin and Cos on every branch.
TEST(Transcendental, SinCosGivesTheBitsOfSinAndCos)
{
    std::uint32_t state = 6u;
    for (int i = 0; i < 300000; ++i)
    {
        const float x = i < 100000 ? Uniform(state, -25.0f, 25.0f) : (i < 150000 ? Uniform(state, -0x1p21f, 0x1p21f) : AnyFinite(state));
        const Math::SinAndCos both = Math::SinCos(x);
        ASSERT_EQ(Bits(both.Sin), Bits(Math::Sin(x))) << x;
        ASSERT_EQ(Bits(both.Cos), Bits(Math::Cos(x))) << x;
    }
    for (const float special : {0.0f, -0.0f, 0x1p-149f, 0x1p-12f, kFloatMax, -kFloatMax, kInfinity, -kInfinity, kNaN})
    {
        const Math::SinAndCos both = Math::SinCos(special);
        EXPECT_EQ(Bits(both.Sin), Bits(Math::Sin(special))) << special;
        EXPECT_EQ(Bits(both.Cos), Bits(Math::Cos(special))) << special;
    }
}

TEST(Transcendental, AtanAndAtan2AreWithinOneUlpOfTheExactValue)
{
    WorstCase atan;
    WorstCase atan2;
    std::uint32_t state = 3u;
    for (int i = 0; i < 200000; ++i)
    {
        const float x = i < 100000 ? Uniform(state, -4.0f, 4.0f) : AnyFinite(state);
        atan.Add(FloatUlps(Math::Atan(x), std::atan(static_cast<double>(x))), x);
        const float y = i < 100000 ? Uniform(state, -4.0f, 4.0f) : AnyFinite(state);
        const float a = Math::Atan2(y, x);
        atan2.Add(FloatUlps(a, std::atan2(static_cast<double>(y), static_cast<double>(x))), y);
        ASSERT_EQ(Bits(Math::Atan2(-y, x)), Bits(-a)) << y << ", " << x;
    }
    RecordProperty("AtanMaxUlps", std::to_string(atan.Ulps));
    RecordProperty("Atan2MaxUlps", std::to_string(atan2.Ulps));
    EXPECT_LE(atan.Ulps, 1.0) << "at " << atan.Input;
    EXPECT_LE(atan2.Ulps, 1.0) << "at y = " << atan2.Input;
}

TEST(Transcendental, PowIsWithinOneUlpOfTheExactValue)
{
    WorstCase pow;
    std::uint32_t state = 4u;
    for (int i = 0; i < 200000; ++i)
    {
        float x = 0.0f;
        float y = 0.0f;
        if (i < 100000)
        {
            x = Uniform(state, 0.0f, 16.0f);
            y = Uniform(state, -30.0f, 30.0f);
        }
        else if (i < 150000)
        {
            x = -Uniform(state, 0.25f, 4.0f);
            y = static_cast<float>(static_cast<int>(Next(state) % 41u) - 20);
        }
        else
        {
            x = std::fabs(AnyFinite(state));
            y = Uniform(state, -2.0f, 2.0f);
        }
        const double reference = std::pow(static_cast<double>(x), static_cast<double>(y));
        if (std::fabs(reference) > kFloatMax)
            continue;
        pow.Add(FloatUlps(Math::Pow(x, y), reference), x);
        ASSERT_EQ(Math::Pow(x, 1.0f), x) << "a power of 1 is the identity";
    }
    RecordProperty("PowMaxUlps", std::to_string(pow.Ulps));
    EXPECT_LE(pow.Ulps, 1.0) << "at x = " << pow.Input;
}

// The bake fixtures rest on these: an identity rotation has yaw Atan2(+0, 1) = +0, and the
// sites take the cosine and sine of yaw and of -yaw.
TEST(Transcendental, ZerosInfinitiesAndNaNsFollowTheCRules)
{
    EXPECT_EQ(Bits(Math::Atan2(0.0f, 1.0f)), Bits(0.0f));
    EXPECT_EQ(Bits(Math::Sin(0.0f)), Bits(0.0f));
    EXPECT_EQ(Bits(Math::Sin(-0.0f)), Bits(-0.0f));
    EXPECT_EQ(Math::Cos(0.0f), 1.0f);
    EXPECT_EQ(Math::Cos(-0.0f), 1.0f);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(Math::Sin(-0.0)), std::bit_cast<std::uint64_t>(-0.0));
    for (const float denormal : {0x1p-149f, -0x1.8p-140f, 0x1.fffffcp-127f})
    {
        EXPECT_EQ(Bits(Math::Sin(denormal)), Bits(denormal));
        EXPECT_EQ(Math::Cos(denormal), 1.0f);
        EXPECT_EQ(Bits(Math::Atan(denormal)), Bits(denormal));
    }
    for (const float large : {kFloatMax, -kFloatMax, 0x1p100f, 1.0e10f, 0x1.921fb6p+21f, 0x1p-12f})
    {
        EXPECT_LE(FloatUlps(Math::Sin(large), std::sin(static_cast<double>(large))), 1.0) << large;
        EXPECT_LE(FloatUlps(Math::Cos(large), std::cos(static_cast<double>(large))), 1.0) << large;
    }
    for (const float special : {kInfinity, -kInfinity, kNaN})
    {
        EXPECT_EQ(Bits(Math::Sin(special)), Bits(kNaN));
        EXPECT_EQ(Bits(Math::Cos(special)), Bits(kNaN));
    }

    EXPECT_EQ(Bits(Math::Atan(-0.0f)), Bits(-0.0f));
    EXPECT_EQ(Math::Atan(kInfinity), kHalfPi);
    EXPECT_EQ(Math::Atan(-kInfinity), -kHalfPi);
    EXPECT_EQ(Bits(Math::Atan(kNaN)), Bits(kNaN));
    EXPECT_EQ(Bits(Math::Atan2(-0.0f, 1.0f)), Bits(-0.0f));
    EXPECT_EQ(Bits(Math::Atan2(0.0f, 0.0f)), Bits(0.0f));
    EXPECT_EQ(Math::Atan2(0.0f, -1.0f), kPi);
    EXPECT_EQ(Math::Atan2(-0.0f, -0.0f), -kPi);
    EXPECT_EQ(Math::Atan2(1.0f, 0.0f), kHalfPi);
    EXPECT_EQ(Math::Atan2(-1.0f, -0.0f), -kHalfPi);
    EXPECT_EQ(Math::Atan2(kInfinity, 5.0f), kHalfPi);
    EXPECT_EQ(Math::Atan2(kInfinity, kInfinity), 0.785398185f);
    EXPECT_EQ(Math::Atan2(-kInfinity, -kInfinity), -2.3561945f);
    EXPECT_EQ(Math::Atan2(1.0f, -kInfinity), kPi);
    EXPECT_EQ(Bits(Math::Atan2(-1.0f, kInfinity)), Bits(-0.0f));
    EXPECT_EQ(Bits(Math::Atan2(kNaN, 1.0f)), Bits(kNaN));

    EXPECT_EQ(Math::Pow(kNaN, 0.0f), 1.0f);
    EXPECT_EQ(Math::Pow(1.0f, kNaN), 1.0f);
    EXPECT_EQ(Math::Pow(-1.0f, kInfinity), 1.0f);
    EXPECT_EQ(Bits(Math::Pow(2.0f, kNaN)), Bits(kNaN));
    EXPECT_EQ(Bits(Math::Pow(-2.0f, 0.5f)), Bits(kNaN));
    EXPECT_EQ(Bits(Math::Pow(0.0f, 2.5f)), Bits(0.0f));
    EXPECT_EQ(Bits(Math::Pow(-0.0f, 3.0f)), Bits(-0.0f));
    EXPECT_EQ(Bits(Math::Pow(-0.0f, 2.0f)), Bits(0.0f));
    EXPECT_EQ(Math::Pow(-0.0f, -3.0f), -kInfinity);
    EXPECT_EQ(Math::Pow(0.0f, -2.0f), kInfinity);
    EXPECT_EQ(Math::Pow(0.5f, kInfinity), 0.0f);
    EXPECT_EQ(Math::Pow(0.5f, -kInfinity), kInfinity);
    EXPECT_EQ(Math::Pow(2.0f, -kInfinity), 0.0f);
    EXPECT_EQ(Math::Pow(kInfinity, -1.0f), 0.0f);
    EXPECT_EQ(Math::Pow(-kInfinity, 3.0f), -kInfinity);
    EXPECT_EQ(Bits(Math::Pow(-kInfinity, -3.0f)), Bits(-0.0f));
    EXPECT_EQ(Math::Pow(-2.0f, 3.0f), -8.0f);
    EXPECT_EQ(Math::Pow(2.0f, 200.0f), kInfinity);
    EXPECT_EQ(Math::Pow(2.0f, -200.0f), 0.0f);
    EXPECT_EQ(Math::Pow(2.0f, -149.0f), 0x1p-149f);
    EXPECT_EQ(Math::Pow(0x1p-148f, 0.5f), 0x1p-74f);
    EXPECT_EQ(Math::Pow(0.0f, 0.7f), 0.0f) << "PowInv's mask reaches 0 and 1";
}

// One hash of every result bit over a fixed sweep of normal inputs: the float results, and the
// double results of the kernels under Atan, Atan2, Pow and the large reduction, so one double
// operation that rounds differently changes the hash. The value is the same on every platform;
// a change to it is a change to every bake that calls these functions.
TEST(Transcendental, TheSweepHashIsTheSameOnEveryPlatform)
{
    namespace Detail = Math::TranscendentalDetail;
    std::uint64_t hash = Hashing::kFnv1a64OffsetBasis;
    std::uint32_t state = 5u;
    // Each draw is a separate statement: C++ does not set the order of function arguments.
    for (int i = 0; i < 4096; ++i)
    {
        const float x = i < 3072 ? Uniform(state, -64.0f, 64.0f) : Uniform(state, 0x1p21f, 0x1p60f);
        hash = Hashing::Fnv1a64Value(hash, Bits(Math::Sin(x)));
        hash = Hashing::Fnv1a64Value(hash, Bits(Math::Cos(x)));
        if (i >= 3072)
        {
            double reduced = 0.0;
            Detail::ReduceLarge(x, reduced);
            hash = Hashing::Fnv1a64Value(hash, std::bit_cast<std::uint64_t>(reduced));
        }
        const float y = Uniform(state, -64.0f, 64.0f);
        const float atan2X = Uniform(state, -64.0f, 64.0f);
        hash = Hashing::Fnv1a64Value(hash, Bits(Math::Atan(y)));
        hash = Hashing::Fnv1a64Value(hash, Bits(Math::Atan2(y, atan2X)));
        const double ratio = static_cast<double>(y) / static_cast<double>(atan2X);
        hash = Hashing::Fnv1a64Value(hash, std::bit_cast<std::uint64_t>(Detail::Atan(ratio)));
        const float base = Uniform(state, 0.001f, 8.0f);
        const float exponent = Uniform(state, -8.0f, 8.0f);
        hash = Hashing::Fnv1a64Value(hash, Bits(Math::Pow(base, exponent)));
        const double log2Base = Detail::Log2(base);
        hash = Hashing::Fnv1a64Value(hash, std::bit_cast<std::uint64_t>(log2Base));
        const double power = Detail::Exp2(static_cast<double>(exponent) * log2Base);
        hash = Hashing::Fnv1a64Value(hash, std::bit_cast<std::uint64_t>(power));
        const double d = static_cast<double>(Uniform(state, -1000.0f, 1000.0f)) * 1.0000001;
        hash = Hashing::Fnv1a64Value(hash, std::bit_cast<std::uint64_t>(Math::Sin(d)));
    }
    EXPECT_EQ(hash, 0x4de591c84c85cae5ull) << std::hex << "now 0x" << hash;
}

namespace
{

// The largest error of FastSin and FastCos against the double sine and cosine, for |x| <= 2e5.
constexpr double kFastTrigBound = 2.5e-7;

// The arguments where the reduction of FastSin changes: zeros, subnormals, multiples of pi/2, the
// ties of x / pi between two integers, the ends of the bound's range, and inputs outside it.
std::vector<float> FastSinEdgeInputs()
{
    std::vector<float> inputs = {0.0f, -0.0f, std::numeric_limits<float>::denorm_min(),
                                 -std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(),
                                 kHalfPi, -kHalfPi, kPi, -kPi, 2.0e5f, -2.0e5f, 1.0e6f, -1.0e6f, 1.0e30f, kFloatMax,
                                 kInfinity, -kInfinity, kNaN};
    for (const double k : {0.5, 1.5, 1000.5, 63661.5})
        for (const double sign : {1.0, -1.0})
        {
            const float tie = static_cast<float>(sign * k * std::numbers::pi);
            const float away = static_cast<float>(sign) * kInfinity;
            inputs.insert(inputs.end(), {std::nextafter(tie, 0.0f), tie, std::nextafter(tie, away)});
        }
    return inputs;
}

} // namespace

// FastSin and FastCos over the arguments eight octaves of a high planet relief frequency reach.
TEST(Transcendental, FastSinAndFastCosAreWithinTheirBound)
{
    std::uint32_t state = 6808u;
    double worstSin = 0.0, worstCos = 0.0;
    for (int i = 0; i < 400000; ++i)
    {
        const float x = (i & 1) != 0 ? Uniform(state, -2.0e4f, 2.0e4f) : Uniform(state, -8.0f, 8.0f);
        worstSin = std::max(worstSin, std::fabs(Math::FastSin(x) - std::sin(static_cast<double>(x))));
        worstCos = std::max(worstCos, std::fabs(Math::FastCos(x) - std::cos(static_cast<double>(x))));
    }
    for (const float x : FastSinEdgeInputs())
        if (std::fabs(x) <= 2.0e5f)
        {
            worstSin = std::max(worstSin, std::fabs(Math::FastSin(x) - std::sin(static_cast<double>(x))));
            worstCos = std::max(worstCos, std::fabs(Math::FastCos(x) - std::cos(static_cast<double>(x))));
        }
    RecordProperty("worstSin", GameEngine::FormatDouble(worstSin));
    RecordProperty("worstCos", GameEngine::FormatDouble(worstCos));
    EXPECT_LE(worstSin, kFastTrigBound);
    EXPECT_LE(worstCos, kFastTrigBound);
}

// FastSin of either zero is +0 (the final add drops the sign of -0), and a NaN or an infinity gives a NaN.
TEST(Transcendental, FastSinOfZerosInfinitiesAndNaNs)
{
    EXPECT_EQ(Bits(Math::FastSin(0.0f)), Bits(0.0f));
    EXPECT_EQ(Bits(Math::FastSin(-0.0f)), Bits(0.0f));
    EXPECT_TRUE(std::isnan(Math::FastSin(kInfinity)));
    EXPECT_TRUE(std::isnan(Math::FastSin(-kInfinity)));
    EXPECT_TRUE(std::isnan(Math::FastSin(kNaN)));
}

// Each lane of FastSin(Float8) has the bits of FastSin of that lane, at the edge inputs and over a sweep.
TEST(Transcendental, FastSinInEightLanesHasTheBitsOfFastSin)
{
    using GameEngine::Mathematics::Float8;
    std::vector<float> inputs = FastSinEdgeInputs();
    std::uint32_t state = 6823u;
    while (inputs.size() % Float8::kLaneCount != 0u || inputs.size() < 80000u)
        inputs.push_back(inputs.size() % 3u == 0u ? AnyFinite(state) : Uniform(state, -2.0e5f, 2.0e5f));
    std::uint32_t mismatches = 0;
    for (std::size_t first = 0; first < inputs.size(); first += Float8::kLaneCount)
    {
        std::array<float, Float8::kLaneCount> lanes{};
        Math::FastSin(Float8::Load(std::span<const float, Float8::kLaneCount>(inputs.data() + first,
                                                                               Float8::kLaneCount)))
            .Store(lanes);
        for (std::size_t lane = 0; lane < lanes.size(); ++lane)
        {
            const float one = Math::FastSin(inputs[first + lane]);
            const bool same = std::isnan(one) ? std::isnan(lanes[lane]) : Bits(one) == Bits(lanes[lane]);
            mismatches += same ? 0u : 1u;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

// The bits of FastSin and FastCos over the planet relief's range, hashed. The value is the same on
// every platform; a change to it changes every planet relief bake.
TEST(Transcendental, TheFastSinHashIsTheSameOnEveryPlatform)
{
    std::uint64_t hash = Hashing::kFnv1a64OffsetBasis;
    std::uint32_t state = 7244u;
    for (int i = 0; i < 4096; ++i)
    {
        const float x = (i & 1) != 0 ? Uniform(state, -2.0e5f, 2.0e5f) : Uniform(state, -8.0f, 8.0f);
        hash = Hashing::Fnv1a64Value(hash, Bits(Math::FastSin(x)));
        hash = Hashing::Fnv1a64Value(hash, Bits(Math::FastCos(x)));
    }
    EXPECT_EQ(hash, 0x610401686f3d023eull) << std::hex << "now 0x" << hash;
}
