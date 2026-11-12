#include "PageStreaming/CpuPage/CpuPageEntries.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <utility>

namespace GameEngine::PageStreaming
{

namespace
{

// The fewest buckets: a shift below 64 bits.
constexpr std::size_t kMinBuckets = 4;
// The displacements a Place tries before the table grows. More than eight keys that share one
// bucket pair move in a cycle with no end: the bound stops it, and the table doubles until every
// key has a slot.
constexpr uint32 kMaxDisplacements = 64;

} // namespace

CpuPageEntries::CpuPageEntries()
{
    Rehash(kMinBuckets, kEmptyKey, 0u);
}

std::uintptr_t* CpuPageEntries::FindValue(uint64 key, uint64*& outKey)
{
    for (const uint64 hash : {kFirstHash, kSecondHash})
    {
        Bucket& bucket = m_Buckets[BucketOf(key, hash)];
        for (std::size_t slot = 0; slot < kSlots; ++slot)
            if (bucket.Keys[slot] == key)
            {
                outKey = &bucket.Keys[slot];
                return &bucket.Values[slot];
            }
    }
    return nullptr;
}

void CpuPageEntries::InsertOrAssign(uint64 key, CpuPageEntry entry)
{
    assert(key != kEmptyKey && "not a CpuPageDirectory::Key");
    assert(entry != CpuPageEntry::Missing && "a Missing page has no entry: Erase it");
    std::uintptr_t value = static_cast<std::uintptr_t>(entry);
    uint64* heldKey = nullptr;
    if (std::uintptr_t* held = FindValue(key, heldKey))
    {
        *held = value;
        return;
    }
    if ((m_Size + 1u) * 2u > m_Buckets.size() * kSlots)
        Rehash(m_Buckets.size() * 2u, kEmptyKey, 0u);
    // A Place that fails leaves a displaced key in hand: the rehash takes it.
    if (!Place(key, value))
        Rehash(m_Buckets.size() * 2u, key, value);
    ++m_Size;
}

bool CpuPageEntries::Place(uint64& key, std::uintptr_t& value)
{
    std::size_t bucketIndex = BucketOf(key, kFirstHash);
    for (uint32 displacement = 0; displacement < kMaxDisplacements; ++displacement)
    {
        // The key's other bucket: its two buckets XOR the one it is in.
        const std::size_t otherIndex = BucketOf(key, kFirstHash) ^ BucketOf(key, kSecondHash) ^ bucketIndex;
        for (const std::size_t candidate : {bucketIndex, otherIndex})
        {
            Bucket& bucket = m_Buckets[candidate];
            for (std::size_t slot = 0; slot < kSlots; ++slot)
                if (bucket.Keys[slot] == kEmptyKey)
                {
                    bucket.Keys[slot] = key;
                    bucket.Values[slot] = value;
                    return true;
                }
        }
        // Both buckets are full: the key takes a slot of its other bucket, and that slot's key moves on.
        Bucket& bucket = m_Buckets[otherIndex];
        const std::size_t slot = displacement % kSlots;
        std::swap(key, bucket.Keys[slot]);
        std::swap(value, bucket.Values[slot]);
        bucketIndex = otherIndex;
    }
    return false;
}

void CpuPageEntries::Erase(uint64 key)
{
    uint64* heldKey = nullptr;
    std::uintptr_t* held = FindValue(key, heldKey);
    if (!held)
        return;
    *heldKey = kEmptyKey;
    *held = 0u;
    --m_Size;
}

void CpuPageEntries::Clear()
{
    std::fill(m_Buckets.begin(), m_Buckets.end(), Bucket{});
    m_Size = 0;
}

void CpuPageEntries::Reserve(std::size_t count)
{
    const std::size_t bucketCount = std::bit_ceil(std::max(kMinBuckets, (count * 2u + kSlots - 1u) / kSlots));
    if (bucketCount > m_Buckets.size())
        Rehash(bucketCount, kEmptyKey, 0u);
}

void CpuPageEntries::Rehash(std::size_t bucketCount, uint64 homelessKey, std::uintptr_t homelessValue)
{
    std::vector<std::pair<uint64, std::uintptr_t>> held;
    held.reserve(m_Size + 1u);
    for (const Bucket& bucket : m_Buckets)
        for (std::size_t slot = 0; slot < kSlots; ++slot)
            if (bucket.Keys[slot] != kEmptyKey)
                held.emplace_back(bucket.Keys[slot], bucket.Values[slot]);
    if (homelessKey != kEmptyKey)
        held.emplace_back(homelessKey, homelessValue);
    // A key that finds no slot doubles the table and starts again.
    for (bool placed = false; !placed; bucketCount *= 2u)
    {
        m_Buckets.assign(bucketCount, Bucket{});
        m_Shift = 64u - static_cast<uint32>(std::countr_zero(bucketCount));
        placed = true;
        for (auto [key, value] : held)
            if (!Place(key, value))
            {
                placed = false;
                break;
            }
    }
}

} // namespace GameEngine::PageStreaming
