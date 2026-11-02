// Tests for GUID::ToString and GUID::ToCompactString: the text forms that scene
// files, logs and the inspector show. Each call makes at most one allocation.

#include "AssetCore/GUID.h"
#include "Memory/AllocationCountScope.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

using GameEngine::GUID;
using GameEngine::String;
namespace Memory = GameEngine::Memory;

// Leading zeros, every hex letter and the 0xff edge.
GUID FixedGuid()
{
    return GUID(GUID::Data{0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f, 0x70,
                           0x81, 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xff});
}

TEST(GUIDToString, WritesLowercaseHexInBothForms)
{
    const GUID guid = FixedGuid();
    EXPECT_EQ(guid.ToString(), "001a2b3c-4d5e-6f70-8192-a3b4c5d6e7ff");
    EXPECT_EQ(guid.ToCompactString(), "001a2b3c4d5e6f708192a3b4c5d6e7ff");
    EXPECT_EQ(GUID::Null().ToString(), "00000000-0000-0000-0000-000000000000");
}

TEST(GUIDToString, MakesAtMostOneAllocationPerCall)
{
    const GUID guid = FixedGuid();
    String text;
    String compact;
    std::uint64_t textAllocations = 0;
    std::uint64_t compactAllocations = 0;
    {
        Memory::AllocationCountScope scope(Memory::CountWindow::ThisThread);
        text = guid.ToString();
        textAllocations = scope.Count();
    }
    {
        Memory::AllocationCountScope scope(Memory::CountWindow::ThisThread);
        compact = guid.ToCompactString();
        compactAllocations = scope.Count();
    }
    EXPECT_LE(textAllocations, 1u) << text;
    EXPECT_LE(compactAllocations, 1u) << compact;
}

} // namespace
