#pragma once

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace GameEngine::Hashing
{

inline constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ull;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ull;

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
    // 2^64 divided by the golden ratio: spreads the bits of a small value across the word.
    constexpr std::uint64_t kGoldenRatio = 0x9e3779b97f4a7c15ull;
    seed ^= value + kGoldenRatio + (seed << 12) + (seed >> 4);
    return seed;
}

/// A well-mixed 64-bit hash of a pointer's address (the SplitMix64 finalizer), for keys built from
/// object identity. The value differs between runs, so it never goes into anything stored.
inline std::uint64_t HashPointer(const void* pointer) noexcept
{
    std::uint64_t bits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
    bits ^= bits >> 30;
    bits *= 0xbf58476d1ce4e5b9ull;
    bits ^= bits >> 27;
    bits *= 0x94d049bb133111ebull;
    bits ^= bits >> 31;
    return bits;
}

} // namespace GameEngine::Hashing
