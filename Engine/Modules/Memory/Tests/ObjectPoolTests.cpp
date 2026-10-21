// Memory::ObjectPool<T>'s slots and lifetimes: stable addresses across growth, last-in
// first-out reuse, a Delete of null, alignment, the pool's destruction of its live objects
// (across many blocks, when one object's destructor deletes another and when two delete each
// other), a throwing constructor, the stats, and that a warmed pool allocates nothing from the
// heap.

#include "Memory/AllocationCountScope.h"
#include "Memory/Arena.h"
#include "Memory/ObjectPool.h"
#include "Memory/StatsSnapshot.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
using GameEngine::Memory::AllocationCountScope;
using GameEngine::Memory::CountWindow;
using GameEngine::Memory::ObjectPool;
using GameEngine::Memory::SnapshotStats;
using GameEngine::Memory::StatsEntry;
using GameEngine::Memory::StatsKind;

bool IsAligned(const void* pointer, std::size_t alignment)
{
    return (reinterpret_cast<std::uintptr_t>(pointer) & (alignment - 1)) == 0;
}

std::optional<StatsEntry> FindEntry(std::string_view name)
{
    for (StatsEntry& entry : SnapshotStats())
    {
        if (entry.Name == name)
            return std::move(entry);
    }
    return std::nullopt;
}

/// A 64-byte object that counts its destructions; its first block holds 256 slots.
struct Tracked
{
    static int s_Destroyed;
    int Value = 0;
    std::array<std::byte, 60> Payload{};
    explicit Tracked(int value) : Value(value) {}
    ~Tracked() { ++s_Destroyed; }
};
int Tracked::s_Destroyed = 0;
static_assert(sizeof(Tracked) == 64);

/// A 4 KiB object: blocks of 16, 32, ... 1024 slots, then 1024 per block (the 4 MiB cap).
struct Page
{
    static int s_Destroyed;
    std::array<std::byte, 4096 - sizeof(int)> Payload{};
    int Value = 0;
    explicit Page(int value) : Value(value) {}
    ~Page() { ++s_Destroyed; }
};
int Page::s_Destroyed = 0;
static_assert(sizeof(Page) == 4096);

/// A node whose destructor deletes its child from the same pool.
struct Node
{
    static std::array<int, 2> s_Destroyed;
    ObjectPool<Node>& Pool;
    int Id = 0;
    Node* Child = nullptr;
    Node(ObjectPool<Node>& pool, int id) : Pool(pool), Id(id) {}
    ~Node()
    {
        ++s_Destroyed[static_cast<std::size_t>(Id)];
        if (Child != nullptr)
            Pool.Delete(Child);
    }
};
std::array<int, 2> Node::s_Destroyed{};

struct ThrowsWhenAsked
{
    explicit ThrowsWhenAsked(bool fail)
    {
        if (fail)
            throw std::runtime_error("constructor failed");
    }
};

/// Nine bytes at one-byte alignment: a slot of 16 bytes, so the freelist link is aligned.
struct NineBytes
{
    std::array<char, 9> Bytes{};
};

// Explicitly padded to its alignment, so no compiler pads it implicitly (MSVC warns C4324 on implicit padding).
struct alignas(256) OverAligned
{
    int Value = 0;
    std::array<std::byte, 256 - sizeof(int)> Padding{};
};

/// One slot larger than the 4 MiB block cap.
struct LargerThanTheCap
{
    std::array<std::byte, GameEngine::Memory::kArenaMaxBlockBytes + 64> Payload;
};
} // namespace

TEST(ObjectPoolTests, NewReturnsStableAddressesAcrossGrowth)
{
    ObjectPool<Tracked> pool("Tests.ObjectPool.Stable");
    std::vector<Tracked*> addresses;
    for (int index = 0; index < 1000; ++index) // 256 + 512 + 1024 slots: three blocks
        addresses.push_back(pool.New(index));
    std::vector<Tracked*> grown;
    for (int index = 0; index < 1000; ++index)
        grown.push_back(pool.New(-index)); // a fourth block
    // Through the addresses recorded before growth: the objects are still there and still
    // owned by the pool, so writes land in them and nowhere else (AddressSanitizer reports
    // these accesses if growth freed or moved a block).
    for (int index = 0; index < 1000; ++index)
    {
        Tracked* object = addresses[static_cast<std::size_t>(index)];
        EXPECT_EQ(object->Value, index);
        object->Value = index + 5000;
    }
    for (int index = 0; index < 1000; ++index)
    {
        EXPECT_EQ(addresses[static_cast<std::size_t>(index)]->Value, index + 5000);
        EXPECT_EQ(grown[static_cast<std::size_t>(index)]->Value, -index);
    }
    EXPECT_EQ(pool.LiveCount(), 2000u);
}

TEST(ObjectPoolTests, DeleteRunsTheDestructorAndReusesTheSlotLifo)
{
    Tracked::s_Destroyed = 0;
    ObjectPool<Tracked> pool("Tests.ObjectPool.Lifo");
    Tracked* first = pool.New(1);
    Tracked* second = pool.New(2);
    pool.Delete(first);
    pool.Delete(second);
    EXPECT_EQ(Tracked::s_Destroyed, 2);
    EXPECT_EQ(pool.LiveCount(), 0u);

    EXPECT_EQ(pool.New(3), second) << "the slot freed last is reused first";
    EXPECT_EQ(pool.New(4), first);
    EXPECT_EQ(pool.LiveCount(), 2u);
}

TEST(ObjectPoolTests, DeleteOfNullDoesNothing)
{
    Tracked::s_Destroyed = 0;
    ObjectPool<Tracked> pool("Tests.ObjectPool.Null");
    Tracked* kept = pool.New(1);
    pool.Delete(nullptr);
    EXPECT_EQ(Tracked::s_Destroyed, 0);
    EXPECT_EQ(pool.LiveCount(), 1u);
    EXPECT_EQ(pool.New(2), kept + 1) << "the next New takes the next unused slot";
}

TEST(ObjectPoolTests, PoolDestructionDestroysEveryLiveObject)
{
    Tracked::s_Destroyed = 0;
    {
        ObjectPool<Tracked> pool("Tests.ObjectPool.Destruction");
        std::vector<Tracked*> objects;
        for (int index = 0; index < 50; ++index)
            objects.push_back(pool.New(index));
        for (std::size_t index = 0; index < objects.size(); index += 5)
            pool.Delete(objects[index]);
        ASSERT_EQ(Tracked::s_Destroyed, 10);
    }
    EXPECT_EQ(Tracked::s_Destroyed, 50) << "each object destroyed once: 10 by Delete, 40 by the pool";
}

// The pool derives which slots are live from every block's used mark and the freelist; in
// Release no per-operation bit exists to fall back on.
TEST(ObjectPoolTests, PoolDestructionDestroysEveryLiveObjectAcrossManyBlocks)
{
    Page::s_Destroyed = 0;
    {
        ObjectPool<Page> pool("Tests.ObjectPool.ManyBlocks");
        std::vector<Page*> pages;
        for (int index = 0; index < 4096; ++index)
            pages.push_back(pool.New(index));
        const std::optional<StatsEntry> entry = FindEntry("Tests.ObjectPool.ManyBlocks");
        ASSERT_TRUE(entry.has_value());
        ASSERT_EQ(entry->Blocks, 10u) << "2,032 slots in seven blocks, then 1,024 per block";
        for (std::size_t index = 0; index < pages.size(); index += 3)
            pool.Delete(pages[index]);
        ASSERT_EQ(Page::s_Destroyed, 1366);
    }
    EXPECT_EQ(Page::s_Destroyed, 4096);
}

// A parent whose destructor deletes its child, at the lower and then at the higher address,
// the pool destroyed with both live: each is destroyed exactly once.
TEST(ObjectPoolTests, ADestructorThatDeletesASiblingIsDestroyedOnce)
{
    for (const bool parentFirst : {true, false})
    {
        Node::s_Destroyed = {};
        {
            ObjectPool<Node> pool("Tests.ObjectPool.Siblings");
            Node* first = pool.New(pool, 0);
            Node* second = pool.New(pool, 1);
            ASSERT_LT(first, second);
            Node* parent = parentFirst ? first : second;
            parent->Child = parentFirst ? second : first;
        }
        EXPECT_EQ(Node::s_Destroyed[0], 1) << (parentFirst ? "parent at the lower address" : "parent at the higher");
        EXPECT_EQ(Node::s_Destroyed[1], 1) << (parentFirst ? "parent at the lower address" : "parent at the higher");
    }
}

// Two objects whose destructors delete each other, the pool destroyed with both live: the
// walk marks an object destroyed before its destructor runs, so the Delete that comes back
// to it is a no-op.
TEST(ObjectPoolTests, ADeleteCycleIsDestroyedOnceEach)
{
    Node::s_Destroyed = {};
    {
        ObjectPool<Node> pool("Tests.ObjectPool.Cycle");
        Node* first = pool.New(pool, 0);
        Node* second = pool.New(pool, 1);
        first->Child = second;
        second->Child = first;
    }
    EXPECT_EQ(Node::s_Destroyed[0], 1);
    EXPECT_EQ(Node::s_Destroyed[1], 1);
}

TEST(ObjectPoolTests, AConstructorThatThrowsReturnsItsSlot)
{
    ObjectPool<ThrowsWhenAsked> pool("Tests.ObjectPool.Throws");
    ThrowsWhenAsked* kept = pool.New(false);
    EXPECT_THROW(pool.New(true), std::runtime_error);
    EXPECT_EQ(pool.LiveCount(), 1u);
    // A one-byte type takes a pointer-sized slot; the failed constructor had the second one.
    EXPECT_EQ(pool.New(false), reinterpret_cast<ThrowsWhenAsked*>(reinterpret_cast<std::byte*>(kept) + sizeof(void*)))
        << "the failed constructor's slot was not the one reused";
    EXPECT_EQ(pool.LiveCount(), 2u);
}

TEST(ObjectPoolTests, AByteAlignedNineByteTypeGetsAlignedLinks)
{
    ObjectPool<NineBytes> pool("Tests.ObjectPool.NineBytes");
    std::vector<NineBytes*> objects;
    for (int index = 0; index < 64; ++index)
        objects.push_back(pool.New());
    for (std::size_t index = 0; index < objects.size(); ++index)
    {
        EXPECT_TRUE(IsAligned(objects[index], alignof(void*)));
        if (index > 0)
            EXPECT_EQ(reinterpret_cast<std::byte*>(objects[index]) - reinterpret_cast<std::byte*>(objects[index - 1]),
                      16);
    }
    for (std::size_t index = 0; index < objects.size(); index += 2)
        pool.Delete(objects[index]);
    for (std::size_t index = 0; index < objects.size(); index += 2)
        EXPECT_TRUE(IsAligned(pool.New(), alignof(void*)));
}

TEST(ObjectPoolTests, AnOverAlignedTypeIsServed)
{
    ObjectPool<OverAligned> pool("Tests.ObjectPool.OverAligned");
    for (int index = 0; index < 200; ++index) // the first block holds 64 slots
        ASSERT_TRUE(IsAligned(pool.New(), 256)) << "object " << index;
}

TEST(ObjectPoolTests, ASlotLargerThanTheBlockCapGetsABlockOfItsOwn)
{
    ObjectPool<LargerThanTheCap> pool("Tests.ObjectPool.LargerThanTheCap");
    LargerThanTheCap* first = pool.New();
    LargerThanTheCap* second = pool.New();
    EXPECT_NE(first, second);
    const std::optional<StatsEntry> entry = FindEntry("Tests.ObjectPool.LargerThanTheCap");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->Blocks, 2u);
}

TEST(ObjectPoolTests, TheSnapshotReportsThePoolUnderItsName)
{
    ObjectPool<Tracked> pool("Tests.ObjectPool.Snapshot");
    std::vector<Tracked*> objects;
    for (int index = 0; index < 257; ++index) // a second block at the 257th
        objects.push_back(pool.New(index));
    std::optional<StatsEntry> entry = FindEntry("Tests.ObjectPool.Snapshot");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->Kind, StatsKind::ObjectPool);
    EXPECT_EQ(entry->Blocks, 2u);
    EXPECT_EQ(entry->Bytes, 257u * 64u);
    EXPECT_EQ(entry->HighWaterBytes, 257u * 64u);
    EXPECT_EQ(entry->ReservedBytes, 256u * 64u + 32u + 512u * 64u + 64u) << "slots plus one bit per slot";

    for (std::size_t index = 0; index < 100; ++index)
        pool.Delete(objects[index]);
    for (int index = 0; index < 100 + 512; ++index) // the 100 free slots, the rest of block 2, one more
        pool.New(index);
    entry = FindEntry("Tests.ObjectPool.Snapshot");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->Blocks, 3u);
    EXPECT_EQ(entry->HighWaterBytes, (256u + 512u + 1u) * 64u) << "the most slots live, as of the third block";
    EXPECT_EQ(pool.LiveCount(), 769u);
}

TEST(ObjectPoolTests, SteadyNewDeleteAllocatesNothingAfterWarmup)
{
    ObjectPool<Tracked> pool("Tests.ObjectPool.Steady");
    std::vector<Tracked*> live;
    live.reserve(1024);
    for (int index = 0; index < 1024; ++index)
        live.push_back(pool.New(index));
    for (Tracked* object : live)
        pool.Delete(object);
    live.clear();

    const AllocationCountScope steady(CountWindow::ThisThread);
    for (int round = 0; round < 16; ++round)
    {
        for (int index = 0; index < 1024; ++index)
            live.push_back(pool.New(index));
        for (std::size_t index = 0; index < live.size(); index += 2)
            pool.Delete(live[index]);
        for (std::size_t index = 1; index < live.size(); index += 2)
            pool.Delete(live[index]);
        live.clear();
    }
    EXPECT_EQ(steady.Count(), 0u) << "a warmed pool reached the heap";
}

#if GE_DEBUG_INSTRUMENTATION && !defined(NDEBUG)
// Debug and DebugFast check, before the pool derives its live set, that the bits written at
// each New and Delete agree with the freelist and the live count; a disagreement asserts.
TEST(ObjectPoolTests, TheDebugBitsAgreeWithTheFreelistBeforeDerivation)
{
    Tracked::s_Destroyed = 0;
    int deleted = 0;
    {
        ObjectPool<Tracked> pool("Tests.ObjectPool.DebugBits");
        std::vector<Tracked*> live;
        for (int index = 0; index < 1000; ++index) // three blocks
            live.push_back(pool.New(index));
        for (std::size_t index = 0; index < live.size(); index += 3)
        {
            pool.Delete(live[index]);
            ++deleted;
        }
        for (int index = 0; index < 100; ++index)
            pool.New(index);
        ASSERT_EQ(pool.LiveCount(), 1100u - static_cast<std::size_t>(deleted));
    }
    EXPECT_EQ(Tracked::s_Destroyed, 1100);
}
#endif
