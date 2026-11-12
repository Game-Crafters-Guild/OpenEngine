#pragma once

#include "Types/Fnv1a.h"
#include "Types/Types.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace GameEngine::PageStreaming
{

/// Where a page of a stream stands on the CPU.
enum class CpuPageState : uint8
{
    Missing,  ///< not loaded yet, in flight, or its load failed and waits for a retry
    Resident, ///< its samples are on the CPU
    Absent,   ///< not in its source: its parent is the finest data there
};

/// One page's entry in a stream's CPU page directory, in one word: Missing, Absent, or else the
/// address of its CpuPageDirectory::PageBytes bytes (Resident). A scalar, so a lookup returns it in
/// a register: MSVC returns a class from a member function through memory.
enum class CpuPageEntry : std::uintptr_t
{
    Missing = 0,
    Absent = 1,
};

/// The Resident entry of a page whose bytes are at `bytes`.
inline CpuPageEntry ResidentEntry(const uint8* bytes)
{
    const auto value = reinterpret_cast<std::uintptr_t>(bytes);
    assert(value > static_cast<std::uintptr_t>(CpuPageEntry::Absent) && "not a page's bytes");
    return static_cast<CpuPageEntry>(value);
}

/// Where the page of `entry` stands on the CPU.
inline CpuPageState State(CpuPageEntry entry)
{
    if (entry == CpuPageEntry::Missing)
        return CpuPageState::Missing;
    return entry == CpuPageEntry::Absent ? CpuPageState::Absent : CpuPageState::Resident;
}

/// The page's bytes while `entry` is Resident, else null.
inline const uint8* Bytes(CpuPageEntry entry)
{
    const auto value = static_cast<std::uintptr_t>(entry);
    return value > static_cast<std::uintptr_t>(CpuPageEntry::Absent) ? reinterpret_cast<const uint8*>(value) : nullptr;
}

/// A stream's CPU page entries under their CpuPageDirectory::Key, one for each Resident or Absent page.
/// A key lives in one of two buckets of four slots (cuckoo hashing), a bucket in one cache line; Entry
/// reads both buckets with no branch on their contents. At most half full: 32 to 64 bytes a page.
class CpuPageEntries
{
public:
    CpuPageEntries();

    /// The entry under `key`, or Missing when there is none.
    CpuPageEntry Entry(uint64 key) const
    {
        const Bucket& first = m_Buckets[BucketOf(key, kFirstHash)];
        const Bucket& second = m_Buckets[BucketOf(key, kSecondHash)];
        // At most one slot holds the key: OR the masked entries together. Missing (0) when no slot holds it.
        std::uintptr_t value = 0;
        for (std::size_t slot = 0; slot < kSlots; ++slot)
        {
            value |= (0u - static_cast<std::uintptr_t>(first.Keys[slot] == key)) & first.Values[slot];
            value |= (0u - static_cast<std::uintptr_t>(second.Keys[slot] == key)) & second.Values[slot];
        }
        return static_cast<CpuPageEntry>(value);
    }

    /// Sets the entry under `key`: Absent, or a ResidentEntry.
    void InsertOrAssign(uint64 key, CpuPageEntry entry);
    /// Removes the entry under `key`, if any: the page becomes Missing.
    void Erase(uint64 key);
    void Clear();
    /// Makes room for `count` entries without a rehash.
    void Reserve(std::size_t count);

private:
    static constexpr std::size_t kSlots = 4;
    // No key has this value: CpuPageDirectory::Key keeps bit 23 clear.
    static constexpr uint64 kEmptyKey = ~0ull;
    // Two odd multipliers: the golden ratio and the second prime of xxHash64. The homeless-key test in
    // PageStoreTests copies both: change them together.
    static constexpr uint64 kFirstHash = Hashing::kGoldenRatio64;
    static constexpr uint64 kSecondHash = 0xC2B2AE3D27D4EB4Full;

    // A slot's value: its key's CpuPageEntry, Missing (0) when the slot is empty.
    struct alignas(64) Bucket
    {
        std::array<uint64, kSlots> Keys{kEmptyKey, kEmptyKey, kEmptyKey, kEmptyKey};
        std::array<std::uintptr_t, kSlots> Values{};
    };

    std::size_t BucketOf(uint64 key, uint64 hash) const { return static_cast<std::size_t>((key * hash) >> m_Shift); }
    // The value slot of `key` (its key slot to `outKey`), or null when the table has no `key`.
    std::uintptr_t* FindValue(uint64 key, uint64*& outKey);
    // Puts a key that is not in the table into one of its buckets, moving other keys to their other
    // bucket. False when the moves run out: `key` and `value` then hold the key left without a slot.
    bool Place(uint64& key, std::uintptr_t& value);
    // Moves every entry, and the homeless key unless it is kEmptyKey, into `bucketCount` buckets
    // (a power of two), or more if a key finds no slot.
    void Rehash(std::size_t bucketCount, uint64 homelessKey, std::uintptr_t homelessValue);

    std::vector<Bucket> m_Buckets;
    std::size_t m_Size = 0;
    uint32 m_Shift = 64; ///< 64 - log2(bucket count)
};

} // namespace GameEngine::PageStreaming
