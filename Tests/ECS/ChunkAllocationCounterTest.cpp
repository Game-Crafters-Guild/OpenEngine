// Archetype chunks are allocated through the global aligned operator new, so the
// allocation counter sees them: a chunk allocated with std::aligned_alloc or
// _aligned_malloc reads as zero allocations, and a test or lab that counts a structural
// change would miss the dominant allocation of the path.

#include "ECS/ArchetypeTable.h"
#include "Memory/AllocationCountScope.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace
{
using namespace GameEngine;
using namespace GameEngine::ECS;

constexpr std::uint32_t kChunks = 4;

std::uint32_t FreeEveryChunk(ArchetypeTable& table)
{
    return table.CompactChunks([](std::uint32_t, std::uint32_t, ArchetypeChunk&) {});
}

// The table's side arrays are grown to kChunks first and keep their capacity when the
// empty chunks are freed, so the measured reserve allocates the chunks and nothing else.
TEST(ChunkAllocation, ChunkAllocationIsSeenByTheCounter)
{
    const std::array metas{ComponentMeta{1, 12, 4}, ComponentMeta{2, 16, 16}};
    ArchetypeTable table(metas);
    const std::uint32_t entitiesForAllChunks = table.GetLayout().Capacity * kChunks;
    table.Reserve(entitiesForAllChunks);
    ASSERT_EQ(table.GetChunkCount(), kChunks);
    ASSERT_EQ(FreeEveryChunk(table), kChunks);
    ASSERT_EQ(table.GetChunkCount(), 0u);

    std::uint64_t allocations = 0;
    {
        const Memory::AllocationCountScope scope(Memory::CountWindow::Process);
        table.Reserve(entitiesForAllChunks);
        allocations = scope.Count();
    }

    EXPECT_EQ(table.GetChunkCount(), kChunks);
    EXPECT_EQ(allocations, kChunks);
}

} // namespace
