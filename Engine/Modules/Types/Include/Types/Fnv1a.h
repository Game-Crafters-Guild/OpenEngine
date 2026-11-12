#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace GameEngine::Hashing
{

inline constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ull;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ull;

/// 2^64 divided by the golden ratio: spreads the bits of a small value across the word.
inline constexpr std::uint64_t kGoldenRatio64 = 0x9e3779b97f4a7c15ull;

inline std::uint64_t Fnv1a64(const void* data,
                             std::size_t size,
                             std::uint64_t seed = kFnv1a64OffsetBasis) noexcept
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = seed;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= kFnv1a64Prime;
    }
    return hash;
}

constexpr std::uint64_t Fnv1a64(std::string_view text,
                                std::uint64_t seed = kFnv1a64OffsetBasis) noexcept
{
    std::uint64_t hash = seed;
    for (const char character : text)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= kFnv1a64Prime;
    }
    return hash;
}

template <typename T>
inline std::uint64_t Fnv1a64Value(std::uint64_t hash, const T& value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    return Fnv1a64(&value, sizeof(T), hash);
}

/// Folds a contiguous run of trivially copyable values (a vector, an array, a span) into `hash`:
/// the count as a 64-bit value first, then the bytes, so {1, 2} followed by {} hashes apart from
/// {1} followed by {2}.
template <std::ranges::contiguous_range Range>
    requires std::ranges::sized_range<const Range&> &&
             std::is_trivially_copyable_v<std::ranges::range_value_t<Range>>
inline std::uint64_t Fnv1a64Values(std::uint64_t hash, const Range& values) noexcept
{
    const std::size_t count = std::ranges::size(values);
    hash = Fnv1a64Value(hash, static_cast<std::uint64_t>(count));
    return Fnv1a64(std::ranges::data(values), count * sizeof(std::ranges::range_value_t<Range>), hash);
}

/// Mixes the finished hash `value` into `seed`, for combining two hashes already computed (a
/// pipeline's hash with its format key's). To fold raw values in, use Fnv1a64Value.
inline std::uint64_t HashCombine(std::uint64_t seed, std::uint64_t value) noexcept
{
    seed ^= value + kGoldenRatio64 + (seed << 12) + (seed >> 4);
    return seed;
}

/// The SplitMix64 output function: adds the golden-ratio increment to `value`, then avalanches every bit.
/// SplitMix64(x) is the first output of a SplitMix64 generator with the state x. The value is the same
/// on every platform and toolchain, so it can go into authored and stored data.
constexpr std::uint64_t SplitMix64(std::uint64_t value) noexcept
{
    value += kGoldenRatio64;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

/// The number of low bits that a unit draw drops from a 64-bit hash. The draw keeps the top 24 bits, one float significand.
inline constexpr int kUnitDrawDiscardedBits =
    std::numeric_limits<std::uint64_t>::digits - std::numeric_limits<float>::digits;

/// 1 / 2^24: scales the kept bits into [0, 1). Every quotient is exact in a float, so the draw is
/// bit-identical on every IEEE platform and never reaches 1.
inline constexpr float kUnitDrawScale = 1.0f / static_cast<float>(1u << std::numeric_limits<float>::digits);

/// A random draw in [0, 1) as a pure function of its four inputs: the same inputs give the same
/// value on every platform, and the draw reads no other state. `seed` names the group, `agent` the
/// actor, `tick` the simulation step and `index` the draw within that step.
constexpr float UnitDraw(std::uint64_t seed, std::uint64_t agent, std::uint64_t tick, std::uint64_t index) noexcept
{
    std::uint64_t hash = SplitMix64(seed);
    hash = SplitMix64(hash ^ agent);
    hash = SplitMix64(hash ^ tick);
    hash = SplitMix64(hash ^ index);
    return static_cast<float>(hash >> kUnitDrawDiscardedBits) * kUnitDrawScale;
}

/// A well-mixed 64-bit hash of a pointer's address (SplitMix64), for keys built from object
/// identity. The value differs between runs, so it never goes into anything stored.
inline std::uint64_t HashPointer(const void* pointer) noexcept
{
    return SplitMix64(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)));
}

} // namespace GameEngine::Hashing
