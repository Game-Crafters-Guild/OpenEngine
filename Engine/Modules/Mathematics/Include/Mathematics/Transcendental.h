#pragma once

#include "Mathematics/Float8.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

// Sin, Cos, Atan, Atan2 and Pow with the same bits on every platform. The C library's versions
// round differently per platform. These use the range reduction and polynomials of Sun's fdlibm
// 5.3 (as kept in FreeBSD msun), in double, with only + - * / and exact operations.
// Sun's notice and the upstream revision: ThirdParty/fdlibm/{LICENSE.txt,UPSTREAM.md}.
// FastSin and FastCos are float polynomials with the same property. FastSin has an eight-lane form.

namespace GameEngine::Math
{

namespace TranscendentalDetail
{

// fdlibm e_rem_pio2.c: pi/2 in three 33-bit parts, each with the double nearest the rest.
inline constexpr double kInvPio2 = 6.36619772367581382433e-01;
inline constexpr double kPio2Part1 = 1.57079632673412561417e+00;
inline constexpr double kPio2Tail1 = 6.07710050650619224932e-11;
inline constexpr double kPio2Part2 = 6.07710050630396597660e-11;
inline constexpr double kPio2Tail2 = 2.02226624879595063154e-21;
inline constexpr double kPio2Part3 = 2.02226624871116645580e-21;
inline constexpr double kPio2Tail3 = 8.47842766036889956997e-32;
// The medium reduction is exact while n * kPio2Part1 is exact: |x| <= 2^20 * pi/2.
inline constexpr double kMediumLimit = 0x1.921fbp+20;
// Below this |x| (about pi/4) the kernels take x as it is.
inline constexpr double kKernelLimit = 0x1.921fbp-1;
inline constexpr double kPi = 3.14159265358979311600e+00;
inline constexpr double kPiLow = 1.22464679914735317720e-16;
// pi/2 * 2^-64: scales a 64-bit fixed-point fraction of a quadrant to radians.
inline constexpr double kPio2Scaled = 8.515303950216386e-20;
// The bits of 2/pi after the binary point, 32 at a time (Payne-Hanek reduction of a float).
inline constexpr std::uint32_t kTwoOverPiBits[8] = {0xA2F9836Eu, 0x4E441529u, 0xFC2757D1u, 0xF534DDC0u,
                                                    0xDB629599u, 0x3C439041u, 0xFE5163ABu, 0xDEBBC561u};

// x rounded to the nearest integer, ties to even, for |x| < 2^51. It is exact and odd.
inline double RoundToInteger(double x)
{
    return (x + 0x1.8p52) - 0x1.8p52;
}

inline std::int32_t BiasedExponent(double x)
{
    return static_cast<std::int32_t>((std::bit_cast<std::uint64_t>(x) >> 52) & 0x7FFu);
}

// fdlibm k_sin.c: sin(x + y) for |x + y| <= pi/4, where y is the tail of x. The polynomial
// is within 2^-58 of sin(x)/x.
inline double KernelSin(double x, double y)
{
    constexpr double kS1 = -1.66666666666666324348e-01;
    constexpr double kS2 = 8.33333333332248946124e-03;
    constexpr double kS3 = -1.98412698298579493134e-04;
    constexpr double kS4 = 2.75573137070700676789e-06;
    constexpr double kS5 = -2.50507602534068634195e-08;
    constexpr double kS6 = 1.58969099521155010221e-10;
    const double z = x * x;
    const double w = z * z;
    const double r = kS2 + z * (kS3 + z * kS4) + z * w * (kS5 + z * kS6);
    const double v = z * x;
    if (y == 0.0)
        return x + v * (kS1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * kS1);
}

// fdlibm k_cos.c (the FreeBSD form): cos(x + y) for |x + y| <= pi/4, where y is the tail of x.
inline double KernelCos(double x, double y)
{
    constexpr double kC1 = 4.16666666666666019037e-02;
    constexpr double kC2 = -1.38888888888741095749e-03;
    constexpr double kC3 = 2.48015872894767294178e-05;
    constexpr double kC4 = -2.75573143513906633035e-07;
    constexpr double kC5 = 2.08757232129817482790e-09;
    constexpr double kC6 = -1.13596475577881948265e-11;
    const double z = x * x;
    const double w = z * z;
    const double r = z * (kC1 + z * (kC2 + z * kC3)) + w * w * (kC4 + z * (kC5 + z * kC6));
    const double halfZ = 0.5 * z;
    const double head = 1.0 - halfZ;
    return head + (((1.0 - head) - halfZ) + (z * r - x * y));
}

// fdlibm e_rem_pio2.c, medium path: x = n * pi/2 + (y0 + y1) with |y0 + y1| <= pi/4, for
// |x| <= kMediumLimit. Each extra round runs only when the first ones cancel too many bits.
inline std::int32_t ReduceMedium(double x, double& y0, double& y1)
{
    const double n = RoundToInteger(x * kInvPio2);
    double r = x - n * kPio2Part1;
    double w = n * kPio2Tail1;
    y0 = r - w;
    const std::int32_t exponent = BiasedExponent(x);
    if (exponent - BiasedExponent(y0) > 16)
    {
        double t = r;
        w = n * kPio2Part2;
        r = t - w;
        w = n * kPio2Tail2 - ((t - r) - w);
        y0 = r - w;
        if (exponent - BiasedExponent(y0) > 49)
        {
            t = r;
            w = n * kPio2Part3;
            r = t - w;
            w = n * kPio2Tail3 - ((t - r) - w);
            y0 = r - w;
        }
    }
    y1 = (r - y0) - w;
    return static_cast<std::int32_t>(n);
}

// 32 bits of 2/pi from bit `first` on. Bit 1 is the first bit after the binary point; the
// bits at 0 and below are zero.
inline std::uint32_t TwoOverPiWindow(std::int32_t first)
{
    if (first < 1)
        return kTwoOverPiBits[0] >> (1 - first);
    const std::int32_t index = (first - 1) >> 5;
    const std::int32_t shift = (first - 1) & 31;
    const std::uint64_t pair = (static_cast<std::uint64_t>(kTwoOverPiBits[index]) << 32) | kTwoOverPiBits[index + 1];
    return static_cast<std::uint32_t>(pair >> (32 - shift));
}

// Payne-Hanek: x = n * pi/2 + y with |y| <= pi/4, for a finite float with |x| > kMediumLimit.
// x = m * 2^e with a 24-bit m. Bits of 2/pi before e - 1 add multiples of 4 to x * 2/pi, so a
// 128-bit window from there gives n mod 4 and at least 35 significant bits of the fraction.
inline std::int32_t ReduceLarge(float x, double& y)
{
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(x) & 0x7FFFFFFFu;
    const std::uint32_t mantissa = (bits & 0x7FFFFFu) | 0x800000u;
    const std::int32_t first = static_cast<std::int32_t>(bits >> 23) - 150 - 1;
    const std::uint32_t window[4] = {TwoOverPiWindow(first + 96), TwoOverPiWindow(first + 64),
                                     TwoOverPiWindow(first + 32), TwoOverPiWindow(first)};
    // mantissa * window, 160 bits, least significant limb first. x * 2/pi = product * 2^-126.
    std::uint32_t product[5] = {};
    std::uint64_t carry = 0;
    for (std::int32_t limb = 0; limb < 4; ++limb)
    {
        const std::uint64_t term = static_cast<std::uint64_t>(mantissa) * window[limb] + carry;
        product[limb] = static_cast<std::uint32_t>(term);
        carry = term >> 32;
    }
    const std::uint64_t fraction = (static_cast<std::uint64_t>(product[3] & 0x3FFFFFFFu) << 34) |
                                   (static_cast<std::uint64_t>(product[2]) << 2) | (product[1] >> 30);
    // A fraction of one half or more rounds n up; the fraction then reads as a negative int64.
    const std::int32_t n = static_cast<std::int32_t>((product[3] >> 30) + (fraction >> 63));
    y = static_cast<double>(std::bit_cast<std::int64_t>(fraction)) * kPio2Scaled;
    if (x < 0.0f)
    {
        y = -y;
        return -n;
    }
    return n;
}

// x = n * pi/2 + (y0 + y1) with |y0 + y1| <= pi/4, for a finite float.
inline std::int32_t Reduce(float x, double& y0, double& y1)
{
    if (std::fabs(x) <= kMediumLimit)
        return ReduceMedium(x, y0, y1);
    y1 = 0.0;
    return ReduceLarge(x, y0);
}

// sin(x + y) for x + y = n * pi/2 + (y0 + y1), from the quadrant n mod 4.
inline double SinOfQuadrant(std::int32_t n, double y0, double y1)
{
    switch (n & 3)
    {
    case 0:
        return KernelSin(y0, y1);
    case 1:
        return KernelCos(y0, y1);
    case 2:
        return -KernelSin(y0, y1);
    default:
        return -KernelCos(y0, y1);
    }
}

inline double CosOfQuadrant(std::int32_t n, double y0, double y1)
{
    return SinOfQuadrant(n + 1, y0, y1);
}

// fdlibm s_atan.c: atan(x) for a double that is not NaN. Reduction to |t| <= 7/16 around
// atan(0.5), atan(1), atan(1.5) or atan(inf); the polynomial is within 2^-57 of atan(t)/t.
inline double Atan(double x)
{
    constexpr double kAtanHigh[4] = {4.63647609000806093515e-01, 7.85398163397448278999e-01,
                                     9.82793723247329054082e-01, 1.57079632679489655800e+00};
    constexpr double kAtanLow[4] = {2.26987774529616870924e-17, 3.06161699786838301793e-17,
                                    1.39033110312309984516e-17, 6.12323399573676603587e-17};
    constexpr double kT[11] = {3.33333333333329318027e-01,  -1.99999999998764832476e-01, 1.42857142725034663711e-01,
                               -1.11111104054623557880e-01, 9.09088713343650656196e-02,  -7.69187620504482999495e-02,
                               6.66107313738753120669e-02,  -5.83357013379057348645e-02, 4.97687799461593236017e-02,
                               -3.65315727442169155270e-02, 1.62858201153657823623e-02};
    const double ax = std::fabs(x);
    if (ax >= 0x1p66)
        return x > 0.0 ? kAtanHigh[3] + kAtanLow[3] : -kAtanHigh[3] - kAtanLow[3];
    if (ax < 0x1p-27)
        return x;
    std::int32_t id = -1;
    double t = x;
    if (ax >= 0.4375)
    {
        if (ax < 0.6875)
        {
            id = 0;
            t = (2.0 * ax - 1.0) / (2.0 + ax);
        }
        else if (ax < 1.1875)
        {
            id = 1;
            t = (ax - 1.0) / (ax + 1.0);
        }
        else if (ax < 2.4375)
        {
            id = 2;
            t = (ax - 1.5) / (1.0 + 1.5 * ax);
        }
        else
        {
            id = 3;
            t = -1.0 / ax;
        }
    }
    const double z = t * t;
    const double w = z * z;
    const double odd = z * (kT[0] + w * (kT[2] + w * (kT[4] + w * (kT[6] + w * (kT[8] + w * kT[10])))));
    const double even = w * (kT[1] + w * (kT[3] + w * (kT[5] + w * (kT[7] + w * kT[9]))));
    if (id < 0)
        return t - t * (odd + even);
    const double result = kAtanHigh[id] - ((t * (odd + even) - kAtanLow[id]) - t);
    return x < 0.0 ? -result : result;
}

// fdlibm e_log.c: log2(x) for a finite x > 0. x = m * 2^k with m in [sqrt(1/2), sqrt(2)); the
// polynomial in s = (m - 1)/(m + 1) is within 2^-58 of log(m). log2(1) is 0 exactly.
inline double Log2(double x)
{
    constexpr double kLg1 = 6.666666666666735130e-01;
    constexpr double kLg2 = 3.999999999940941908e-01;
    constexpr double kLg3 = 2.857142874366239149e-01;
    constexpr double kLg4 = 2.222219843214978396e-01;
    constexpr double kLg5 = 1.818357216161805012e-01;
    constexpr double kLg6 = 1.531383769920937332e-01;
    constexpr double kLg7 = 1.479819860511658591e-01;
    constexpr double kInvLn2 = 1.4426950408889634;
    int exponent = 0;
    double m = std::frexp(x, &exponent);
    if (m < 0.70710678118654752440)
    {
        m *= 2.0;
        --exponent;
    }
    const double f = m - 1.0;
    const double s = f / (2.0 + f);
    const double z = s * s;
    const double w = z * z;
    const double r = z * (kLg1 + w * (kLg3 + w * (kLg5 + w * kLg7))) + w * (kLg2 + w * (kLg4 + w * kLg6));
    const double halfSquare = 0.5 * f * f;
    const double logM = f - (halfSquare - s * (halfSquare + r));
    return static_cast<double>(exponent) + logM * kInvLn2;
}

// fdlibm e_exp.c: 2^t for -1000 < t < 1000. 2^t = 2^k * e^r with |r| <= ln2/2; the rational
// form of e^r has a polynomial within 2^-58 of its exact value.
inline double Exp2(double t)
{
    constexpr double kP1 = 1.66666666666666019037e-01;
    constexpr double kP2 = -2.77777777770155933842e-03;
    constexpr double kP3 = 6.61375632143793436117e-05;
    constexpr double kP4 = -1.65339022054652515390e-06;
    constexpr double kP5 = 4.13813679705723846039e-08;
    constexpr double kLn2 = 6.93147180559945286227e-01;
    const double k = RoundToInteger(t);
    const double r = (t - k) * kLn2;
    const double rr = r * r;
    const double c = r - rr * (kP1 + rr * (kP2 + rr * (kP3 + rr * (kP4 + rr * kP5))));
    const double expR = 1.0 - ((r * c) / (c - 2.0) - r);
    return std::ldexp(expR, static_cast<int>(k));
}

} // namespace TranscendentalDetail

// A NaN result is always the quiet NaN constant, so that NaN bits do not depend on the CPU.
// Float results are within 1 ulp of the exact value for every input.

inline float Sin(float x)
{
    namespace D = TranscendentalDetail;
    if (std::isnan(x) || std::isinf(x))
        return std::numeric_limits<float>::quiet_NaN();
    const float ax = std::fabs(x);
    if (ax < 0x1p-12f)
        return x;
    if (ax <= D::kKernelLimit)
        return static_cast<float>(D::KernelSin(x, 0.0));
    double y0 = 0.0;
    double y1 = 0.0;
    const std::int32_t n = D::Reduce(x, y0, y1);
    return static_cast<float>(D::SinOfQuadrant(n, y0, y1));
}

inline float Cos(float x)
{
    namespace D = TranscendentalDetail;
    if (std::isnan(x) || std::isinf(x))
        return std::numeric_limits<float>::quiet_NaN();
    const float ax = std::fabs(x);
    if (ax < 0x1p-12f)
        return 1.0f;
    if (ax <= D::kKernelLimit)
        return static_cast<float>(D::KernelCos(x, 0.0));
    double y0 = 0.0;
    double y1 = 0.0;
    const std::int32_t n = D::Reduce(x, y0, y1);
    return static_cast<float>(D::CosOfQuadrant(n, y0, y1));
}

struct SinAndCos
{
    float Sin = 0.0f;
    float Cos = 1.0f;
};

// Sin(x) and Cos(x) with one range reduction. The bits are those of Sin(x) and Cos(x).
inline SinAndCos SinCos(float x)
{
    namespace D = TranscendentalDetail;
    if (std::isnan(x) || std::isinf(x))
        return {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
    const float ax = std::fabs(x);
    if (ax < 0x1p-12f)
        return {x, 1.0f};
    if (ax <= D::kKernelLimit)
        return {static_cast<float>(D::KernelSin(x, 0.0)), static_cast<float>(D::KernelCos(x, 0.0))};
    double y0 = 0.0;
    double y1 = 0.0;
    const std::int32_t n = D::Reduce(x, y0, y1);
    return {static_cast<float>(D::SinOfQuadrant(n, y0, y1)), static_cast<float>(D::CosOfQuadrant(n, y0, y1))};
}

// Sin of a double is within 1 ulp for |x| <= 2^20 * pi/2 (about 1.6e6). Beyond that it
// returns NaN: the reduction there needs more bits of pi than this one keeps.
inline double Sin(double x)
{
    namespace D = TranscendentalDetail;
    const double ax = std::fabs(x);
    if (!(ax <= D::kMediumLimit))
        return std::numeric_limits<double>::quiet_NaN();
    if (ax < 0x1p-27)
        return x;
    if (ax <= D::kKernelLimit)
        return D::KernelSin(x, 0.0);
    double y0 = 0.0;
    double y1 = 0.0;
    const std::int32_t n = D::ReduceMedium(x, y0, y1);
    return D::SinOfQuadrant(n, y0, y1);
}

inline float Atan(float x)
{
    if (std::isnan(x))
        return std::numeric_limits<float>::quiet_NaN();
    return static_cast<float>(TranscendentalDetail::Atan(x));
}

// The C rules for zeros and infinities: Atan2(+-0, +x) is +-0 and Atan2(+-0, -x) is +-pi, for
// x >= 0 including +-0; Atan2(y, +-0) is +-pi/2 for y != 0.
inline float Atan2(float y, float x)
{
    namespace D = TranscendentalDetail;
    if (std::isnan(x) || std::isnan(y))
        return std::numeric_limits<float>::quiet_NaN();
    const bool yNegative = std::signbit(y);
    const bool xNegative = std::signbit(x);
    double result = 0.0;
    if (y == 0.0f)
    {
        if (!xNegative)
            return y;
        result = D::kPi;
    }
    else if (x == 0.0f || (std::isinf(y) && !std::isinf(x)))
        result = 0.5 * D::kPi;
    else if (std::isinf(x))
        result = std::isinf(y) ? (xNegative ? 0.75 : 0.25) * D::kPi : (xNegative ? D::kPi : 0.0);
    else
    {
        result = D::Atan(std::fabs(static_cast<double>(y) / static_cast<double>(x)));
        if (xNegative)
            result = D::kPi - (result - D::kPiLow);
    }
    const float magnitude = static_cast<float>(result);
    return yNegative ? -magnitude : magnitude;
}

// The C rules for special operands: Pow(x, +-0) and Pow(1, y) are 1 (even for NaN), a negative
// x needs an integer y, and the sign of a negative x survives an odd integer y.
inline float Pow(float x, float y)
{
    if (y == 0.0f || x == 1.0f)
        return 1.0f;
    if (std::isnan(x) || std::isnan(y))
        return std::numeric_limits<float>::quiet_NaN();
    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    const float ax = std::fabs(x);
    if (std::isinf(y))
    {
        if (ax == 1.0f)
            return 1.0f;
        return (ax > 1.0f) == (y > 0.0f) ? kInfinity : 0.0f;
    }
    // Every float of magnitude 2^24 or more is an even integer.
    const float ay = std::fabs(y);
    const bool yInteger = ay >= 0x1p24f || std::floor(ay) == ay;
    const bool yOdd = ay < 0x1p24f && yInteger && (static_cast<std::int32_t>(ay) & 1) != 0;
    const bool negative = std::signbit(x) && yOdd;
    float magnitude = 0.0f;
    if (ax == 0.0f)
        magnitude = y < 0.0f ? kInfinity : 0.0f;
    else if (std::isinf(x))
        magnitude = y < 0.0f ? 0.0f : kInfinity;
    else if (x < 0.0f && !yInteger)
        return std::numeric_limits<float>::quiet_NaN();
    else
    {
        // Outside these bounds the float result is infinity or zero.
        const double t = static_cast<double>(y) * TranscendentalDetail::Log2(ax);
        if (t >= 129.0)
            magnitude = kInfinity;
        else if (t >= -151.0)
            magnitude = static_cast<float>(TranscendentalDetail::Exp2(t));
    }
    return negative ? -magnitude : magnitude;
}

namespace FastTrigDetail
{

// x = n pi + r with |r| <= pi/2. n x kPiHigh and n x kPiMid are exact for |n| < 2^16.
inline constexpr float kInvPi = std::numbers::inv_pi_v<float>;
inline constexpr float kPiHigh = 3.140625f;
inline constexpr float kPiMid = 9.65118408203125e-4f;
inline constexpr float kPiLow = 2.53518169301969465e-6f;
// 1.5 x 2^23: (x / pi + kRoundShift) - kRoundShift is x / pi rounded half to even, and bit 0 of
// the sum is the parity of n.
inline constexpr float kRoundShift = 12582912.0f;
// sin(r) / r and cos(r) on [-pi/2, pi/2], Abramowitz and Stegun 4.3.97 and 4.3.99 (error 2e-9).
inline constexpr std::array<float, 5> kSinTerms = {-0.1666666664f, 0.0083333315f, -0.0001984090f, 0.0000027526f,
                                                   -0.0000000239f};
inline constexpr std::array<float, 5> kCosTerms = {-0.4999999963f, 0.0416666418f, -0.0013888397f, 0.0000247609f,
                                                   -0.0000002605f};

// The reduced argument r of x and the sign bit of (-1)^n (bit 31).
struct Reduced
{
    float R = 0.0f;
    std::uint32_t Sign = 0u;
};

inline Reduced Reduce(float x)
{
    const float shifted = x * kInvPi + kRoundShift;
    const float n = shifted - kRoundShift;
    const float r = ((x - n * kPiHigh) - n * kPiMid) - n * kPiLow;
    return {r, std::bit_cast<std::uint32_t>(shifted) << 31u};
}

inline float SinOfReduced(float r)
{
    const float r2 = r * r;
    const float p = (((kSinTerms[4] * r2 + kSinTerms[3]) * r2 + kSinTerms[2]) * r2 + kSinTerms[1]) * r2 + kSinTerms[0];
    return r + r * (r2 * p);
}

inline float CosOfReduced(float r)
{
    const float r2 = r * r;
    const float p = (((kCosTerms[4] * r2 + kCosTerms[3]) * r2 + kCosTerms[2]) * r2 + kCosTerms[1]) * r2 + kCosTerms[0];
    return 1.0f + r2 * p;
}

} // namespace FastTrigDetail

/// A float sine within 2.5e-7 of sin(x) for |x| <= 2e5, with the same bits on every platform and in
/// each lane of FastSin(Float8). Sin is within 1 ulp but has no lane form. FastSin(-0) is +0, NaN and
/// infinity give a NaN, and beyond |x| = 2e5 the result has no bound (it leaves [-1, 1] by 1e9).
inline float FastSin(float x)
{
    const FastTrigDetail::Reduced reduced = FastTrigDetail::Reduce(x);
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(FastTrigDetail::SinOfReduced(reduced.R)) ^
                                reduced.Sign);
}

/// The cosine of FastSin: within 2.5e-7 of cos(x) for |x| <= 2e5, the same bits on every platform.
inline float FastCos(float x)
{
    const FastTrigDetail::Reduced reduced = FastTrigDetail::Reduce(x);
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(FastTrigDetail::CosOfReduced(reduced.R)) ^
                                reduced.Sign);
}

/// FastSin in eight lanes: each lane has the bits of FastSin of that lane.
inline Mathematics::Float8 GE_VECTORCALL FastSin(Mathematics::Float8 x)
{
    using Mathematics::Float8;
    namespace D = FastTrigDetail;
    const Float8 shift = Float8::Broadcast(D::kRoundShift);
    const Float8 shifted = x * Float8::Broadcast(D::kInvPi) + shift;
    const Float8 n = shifted - shift;
    const Float8 r = ((x - n * Float8::Broadcast(D::kPiHigh)) - n * Float8::Broadcast(D::kPiMid)) -
                     n * Float8::Broadcast(D::kPiLow);
    const Float8 r2 = r * r;
    const Float8 p = (((Float8::Broadcast(D::kSinTerms[4]) * r2 + Float8::Broadcast(D::kSinTerms[3])) * r2 +
                       Float8::Broadcast(D::kSinTerms[2])) * r2 + Float8::Broadcast(D::kSinTerms[1])) * r2 +
                     Float8::Broadcast(D::kSinTerms[0]);
    return Float8::FlipSignWhereLowBitSet(r + r * (r2 * p), shifted);
}

} // namespace GameEngine::Math
