#pragma once

#include "Components/AssetRef.h"
#include "Types/Fnv1a.h"
#include "Types/Types.h"

#include <cstddef>

namespace GameEngine::Components
{

// Which pool a pick is drawn from. Salts the selection hash so roles sharing a
// station index draw independently: hash(Seed, role, stationIndex) %
// activePoolSize. Values are an on-disk-adjacent contract — every authored
// scene's picks reshuffle if one is renumbered, so APPEND ONLY.
enum class SplinePoolRole : uint32
{
    // SplinePlacement (surface tiles).
    Straight = 0,
    Curve = 1,
    Scatter = 2,
    // SplineFence (posts and spans).
    Post = 3,
    Span = 4,
    Gate = 5,
    // The crest row's cells. Caps are not a role: the piece that closes a
    // remainder is chosen by the length that fits it, never by a seeded pick.
    Crest = 6,
};

// Active entries are the leading non-empty slots. Entries after the first
// empty slot never select (the inspector compacts on edit; a hole from a
// hand-edited scene is reported by PoolHasHole, not silently reshuffled).
// Templated on the array bound because the recipes size their pools from their
// own kits.
template <std::size_t kCapacity>
inline uint32 ActivePoolCount(const ModelRef (&pool)[kCapacity])
{
    uint32 count = 0;
    while (count < static_cast<uint32>(kCapacity) && !pool[count].IsNull())
        ++count;
    return count;
}

// True when a non-empty slot follows an empty one — authored data the
// selection contract ignores. Inspector validation surfaces it by name.
template <std::size_t kCapacity>
inline bool PoolHasHole(const ModelRef (&pool)[kCapacity])
{
    for (uint32 i = ActivePoolCount(pool); i < static_cast<uint32>(kCapacity); ++i)
    {
        if (!pool[i].IsNull())
            return true;
    }
    return false;
}

// The pool pick for one station: a pure function of (seed, role, stationIndex), so every rebuild
// on every platform makes the same picks. stationIndex is the global tile ordinal along the spline.
// Returns an index in [0, activeCount); activeCount must be >= 1.
constexpr uint32 SplinePoolSelect(uint32 seed, SplinePoolRole role, uint32 stationIndex,
                                  uint32 activeCount)
{
    uint64 h = Hashing::SplitMix64(seed);
    h = Hashing::SplitMix64(h ^ static_cast<uint64>(role));
    h = Hashing::SplitMix64(h ^ static_cast<uint64>(stationIndex));
    return static_cast<uint32>(h % static_cast<uint64>(activeCount));
}

} // namespace GameEngine::Components
