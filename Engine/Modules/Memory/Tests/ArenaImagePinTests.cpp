// Memory as an image-pin source: which address ranges an arena's recorded destructors pin
// (Memory::Detail::DescribeArenasPinningImage), and when the pin is released.
//
// A recorded destructor's address is Arena::Destroy<T>, instantiated here. The tests find it
// through the query itself: a binary search over ranges that exclude the arena's own storage
// (an arena stored inside a range never pins it), so no test depends on where the loader put
// this executable's code or the stack.

#include "Memory/Arena.h"
#include "Memory/ImagePins.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using GameEngine::Memory::Arena;
using GameEngine::Memory::Detail::DescribeArenasPinningImage;

constexpr std::uint64_t kAddressEnd = std::numeric_limits<std::uint64_t>::max();

bool Pins(std::string_view arena, std::uint64_t base, std::uint64_t size)
{
    const std::vector<std::string> names = DescribeArenasPinningImage(base, size);
    return std::find(names.begin(), names.end(), arena) != names.end();
}

struct Storage
{
    std::uint64_t Begin;
    std::uint64_t End;
};

Storage StorageOf(const Arena& arena)
{
    const auto begin = reinterpret_cast<std::uint64_t>(&arena);
    return {begin, begin + sizeof(Arena)};
}

/// True when the arena pins some range that does not hold its own storage.
bool PinsAnyRangeBesideItsStorage(const Arena& arena, std::string_view name)
{
    const Storage storage = StorageOf(arena);
    return Pins(name, 1, storage.Begin - 1) || Pins(name, storage.End, kAddressEnd - storage.End);
}

/// The one destructor address the arena holds, found by bisecting the side of its storage
/// that it pins; 0 when it pins neither side.
std::uint64_t FindDestructorAddress(const Arena& arena, std::string_view name)
{
    const Storage storage = StorageOf(arena);
    std::uint64_t low = 1;
    std::uint64_t high = storage.Begin;
    if (!Pins(name, low, high - low))
    {
        low = storage.End;
        high = kAddressEnd;
        if (!Pins(name, low, high - low))
            return 0;
    }
    while (high - low > 1)
    {
        const std::uint64_t middle = low + (high - low) / 2;
        if (Pins(name, low, middle - low))
            high = middle;
        else
            low = middle;
    }
    return low;
}

struct Pinned
{
    ~Pinned() { ++s_Destroyed; }
    static inline int s_Destroyed = 0;
};

struct Trivial
{
    int Value;
};

/// Asks, from inside its own destructor, whether its arena still pins its destructor.
struct QueriesItsPin
{
    ~QueriesItsPin() { s_PinnedWhileRunning = Pins(s_ArenaName, s_Address, 1); }
    static inline std::string_view s_ArenaName;
    static inline std::uint64_t s_Address = 0;
    static inline bool s_PinnedWhileRunning = false;
};
} // namespace

TEST(ArenaImagePinTests, AnArenaHoldingADestructorInARangePinsThatRange)
{
    Arena arena("Tests.Pin.Holding");
    arena.Allocate(64);
    EXPECT_FALSE(PinsAnyRangeBesideItsStorage(arena, "Tests.Pin.Holding")) << "an arena with no destructor pinned";

    arena.New<Pinned>();
    const std::uint64_t address = FindDestructorAddress(arena, "Tests.Pin.Holding");
    ASSERT_NE(address, 0u) << "an arena holding a destructor pinned no range";
    EXPECT_TRUE(Pins("Tests.Pin.Holding", address, 1));
}

TEST(ArenaImagePinTests, ResetReleasesThePin)
{
    Arena arena("Tests.Pin.Reset");
    arena.New<Pinned>();
    const std::uint64_t address = FindDestructorAddress(arena, "Tests.Pin.Reset");
    ASSERT_NE(address, 0u);

    arena.Reset();
    EXPECT_FALSE(Pins("Tests.Pin.Reset", address, 1));
    EXPECT_FALSE(PinsAnyRangeBesideItsStorage(arena, "Tests.Pin.Reset"));
}

TEST(ArenaImagePinTests, AnArenaStoredInsideTheRangeDoesNotPinIt)
{
    Arena arena("Tests.Pin.StoredInside");
    arena.New<Pinned>();
    const std::uint64_t address = FindDestructorAddress(arena, "Tests.Pin.StoredInside");
    ASSERT_NE(address, 0u);

    const Storage storage = StorageOf(arena);
    const std::uint64_t begin = std::min(address, storage.Begin);
    const std::uint64_t end = std::max(address + 1, storage.End);
    EXPECT_FALSE(Pins("Tests.Pin.StoredInside", begin, end - begin))
        << "an arena inside the unloading image is that image's to destroy, and must not pin it";
}

TEST(ArenaImagePinTests, RangesAreHalfOpen)
{
    Arena arena("Tests.Pin.HalfOpen");
    arena.New<Pinned>();
    const std::uint64_t address = FindDestructorAddress(arena, "Tests.Pin.HalfOpen");
    ASSERT_GT(address, 16u);

    EXPECT_TRUE(Pins("Tests.Pin.HalfOpen", address, 1));
    EXPECT_TRUE(Pins("Tests.Pin.HalfOpen", address - 16, 17));
    EXPECT_FALSE(Pins("Tests.Pin.HalfOpen", address - 16, 16)) << "a range ending at the address holds it";
    EXPECT_FALSE(Pins("Tests.Pin.HalfOpen", address + 1, 16));
    EXPECT_FALSE(Pins("Tests.Pin.HalfOpen", address, 0)) << "an empty range holds nothing";
}

// The bounds clear after the destructors ran, never before: a destructor that asks sees its
// arena still pinning it.
TEST(ArenaImagePinTests, ResetClearsTheBoundsOnlyAfterTheDestructorsRan)
{
    Arena arena("Tests.Pin.ClearOrder");
    arena.New<QueriesItsPin>();
    QueriesItsPin::s_ArenaName = "Tests.Pin.ClearOrder";
    QueriesItsPin::s_Address = FindDestructorAddress(arena, "Tests.Pin.ClearOrder");
    QueriesItsPin::s_PinnedWhileRunning = false;
    ASSERT_NE(QueriesItsPin::s_Address, 0u);

    arena.Reset();
    EXPECT_TRUE(QueriesItsPin::s_PinnedWhileRunning)
        << "the pin was released before the destructor in the image had finished running";
    EXPECT_FALSE(Pins("Tests.Pin.ClearOrder", QueriesItsPin::s_Address, 1));
}

TEST(ArenaImagePinTests, ATriviallyDestructibleNewRecordsNoBound)
{
    Arena arena("Tests.Pin.Trivial");
    arena.New<int>(1);
    arena.New<Trivial>(Trivial{2});
    EXPECT_FALSE(PinsAnyRangeBesideItsStorage(arena, "Tests.Pin.Trivial"))
        << "an object with no destructor to run pinned code";
}
