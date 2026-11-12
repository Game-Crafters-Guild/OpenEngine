#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <type_traits>

// The engine's only fused multiply-add. The build turns contraction off on every compiler, so
// `a * b + c` in code rounds twice and `MulAdd` rounds once, with the same bits on every platform.

namespace GameEngine::Math
{

namespace MulAddDetail
{

// a * b + c rounded once, from double + - * only. The product of two floats is exact in
// double. The double sum is rounded to odd (its last bit set when inexact), and a value
// rounded to odd with 29 spare bits rounds to float as the exact value does.
constexpr float SoftMulAdd(float a, float b, float c)
{
    const double product = static_cast<double>(a) * static_cast<double>(b);
    const double addend = static_cast<double>(c);
    const double sum = product + addend;
    if (!(sum - sum == 0.0))
        return static_cast<float>(sum); // infinity or NaN
    // The exact rounding error of the sum (Knuth's TwoSum).
    const double addendPart = sum - product;
    const double error = (product - (sum - addendPart)) + (addend - addendPart);
    std::uint64_t bits = std::bit_cast<std::uint64_t>(sum);
    if (error != 0.0 && (bits & 1u) == 0u)
        bits = ((error > 0.0) == (sum > 0.0)) ? bits + 1u : bits - 1u;
    return static_cast<float>(std::bit_cast<double>(bits));
}

} // namespace MulAddDetail

// a * b + c with one rounding: one instruction on arm64 and on x64 with FMA. WebAssembly has
// no fused instruction, so there the software form gives the same bits.
constexpr float MulAdd(float a, float b, float c)
{
#if defined(__EMSCRIPTEN__)
    return MulAddDetail::SoftMulAdd(a, b, c);
#else
    if (std::is_constant_evaluated())
        return MulAddDetail::SoftMulAdd(a, b, c);
    return std::fma(a, b, c);
#endif
}

// a * b + c with one rounding, in double. WebAssembly uses the C library's correctly rounded fma.
inline double MulAdd(double a, double b, double c)
{
    return std::fma(a, b, c);
}

} // namespace GameEngine::Math
