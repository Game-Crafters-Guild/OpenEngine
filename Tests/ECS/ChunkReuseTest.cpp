// An archetype table under structural churn reuses the chunks that removals empty: every chunk but
// the last is full, a frame of steady churn allocates no chunk after warm-up, and a drained table
// holds no chunk for iteration to walk. The World patterns are the shapes counted in GE-366.

#include "ECS/Archetype/ArchetypeTable.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Memory/AllocationCountScope.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::ECS;

struct ChurnPosition { float X = 0.0f; float Y = 0.0f; };
struct ChurnVelocity { float X = 0.0f; float Y = 0.0f; };
struct ChurnA { float A = 0.0f; float B = 0.0f; };
struct ChurnB { float A = 0.0f; float B = 0.0f; };
struct ChurnC { float A = 0.0f; float B = 0.0f; };
struct ChurnPayload { int32_t Value = 0; float Scale = 0.0f; };
struct ChurnDamage { int32_t Amount = 10; };

// The chunks a table needs when every chunk but the last is full.
std::size_t ChunksNeeded(const Archetype& archetype)
{
    const uint32_t capacity = archetype.GetTable().GetLayout().Capacity;
    return (archetype.GetEntityCount() + capacity - 1) / capacity;
}

struct ChunkTally
{
    std::size_t Chunks = 0;
    std::size_t Needed = 0;
};

ChunkTally TallyChunks(World& world, const std::vector<EntityHandle>& handles)
{
    std::set<const Archetype*> archetypes;
    for (const EntityHandle handle : handles)
        archetypes.insert(world.GetEntityArchetype(handle));
    ChunkTally tally;
    for (const Archetype* archetype : archetypes)
    {
        tally.Chunks += archetype->GetChunkCount();
        tally.Needed += ChunksNeeded(*archetype);
    }
    return tally;
}

EntityHandle CreateWithMask(World& world, uint8_t mask)
{
    const ChurnPosition p{};
    const ChurnVelocity v{};
    switch (mask)
    {
    case 0: return world.CreateHandle(p, v);
    case 1: return world.CreateHandle(p, v, ChurnA{});
    case 2: return world.CreateHandle(p, v, ChurnB{});
    case 3: return world.CreateHandle(p, v, ChurnA{}, ChurnB{});
    case 4: return world.CreateHandle(p, v, ChurnC{});
    case 5: return world.CreateHandle(p, v, ChurnA{}, ChurnC{});
    case 6: return world.CreateHandle(p, v, ChurnB{}, ChurnC{});
    default: return world.CreateHandle(p, v, ChurnA{}, ChurnB{}, ChurnC{});
    }
}

// Moves the entity to the archetype that differs from its own by one component.
void ToggleComponent(World& world, EntityHandle handle, uint8_t& mask, int bit)
{
    const bool has = ((mask >> bit) & 1u) != 0;
    if (bit == 0)
        has ? world.RemoveComponentImmediate<ChurnA>(handle) : world.AddComponentImmediate(handle, ChurnA{});
    else if (bit == 1)
        has ? world.RemoveComponentImmediate<ChurnB>(handle) : world.AddComponentImmediate(handle, ChurnB{});
    else
        has ? world.RemoveComponentImmediate<ChurnC>(handle) : world.AddComponentImmediate(handle, ChurnC{});
    mask = static_cast<uint8_t>(mask ^ (1u << bit));
}

// Table rows named by entity index, so that a removal can find any entity while rows move.
void AddRow(ArchetypeTable& table, std::vector<EntityLocation>& locations, uint32_t index)
{
    locations[index] = table.AddEntity(EntityHandle(index, 1));
}

void RemoveRow(ArchetypeTable& table, std::vector<EntityLocation>& locations, uint32_t index)
{
    const EntityLocation location = locations[index];
    const EntityHandle moved = table.RemoveEntity(location);
    if (moved.IsValid())
        locations[moved.index] = location;
}

// 10,000 entities over 8 archetypes, 1250 archetype moves a frame.
TEST(ChunkReuse, ArchetypeMovesAllocateNoChunkAfterWarmUp)
{
    constexpr std::size_t kEntities = 10000;
    constexpr std::size_t kMovesPerFrame = 1250;
    constexpr int kWarmUpFrames = 100;
    constexpr int kFrames = 300;
    World world(nullptr);
    world.RegisterComponents<ChurnPosition, ChurnVelocity, ChurnA, ChurnB, ChurnC>();
    std::vector<EntityHandle> handles(kEntities);
    std::vector<uint8_t> masks(kEntities);
    for (std::size_t i = 0; i < kEntities; ++i)
    {
        masks[i] = static_cast<uint8_t>(i % 8);
        handles[i] = CreateWithMask(world, masks[i]);
    }

    uint64_t steadyAllocations = 0;
    for (int frame = 1; frame <= kFrames; ++frame)
    {
        const Memory::AllocationCountScope scope(Memory::CountWindow::Process);
        for (std::size_t k = 0; k < kMovesPerFrame; ++k)
        {
            const std::size_t i = (static_cast<std::size_t>(frame) * kMovesPerFrame + k) % kEntities;
            ToggleComponent(world, handles[i], masks[i], static_cast<int>((static_cast<std::size_t>(frame) + i) % 3));
        }
        if (frame > kWarmUpFrames)
            steadyAllocations += scope.Count();
    }

    EXPECT_EQ(steadyAllocations, 0u);
    const ChunkTally tally = TallyChunks(world, handles);
    EXPECT_EQ(tally.Chunks, tally.Needed);
}

// 16,384 entities gain one component and lose it again, once per run.
TEST(ChunkReuse, AddRemoveComponentAllocatesNoChunkAfterWarmUp)
{
    constexpr std::size_t kEntities = 16384;
    constexpr int kRuns = 4;
    World world(nullptr);
    world.RegisterComponents<ChurnPosition, ChurnVelocity, ChurnPayload>();
    std::vector<EntityHandle> handles(kEntities);
    for (std::size_t i = 0; i < kEntities; ++i)
        handles[i] = world.CreateHandle(ChurnPosition{static_cast<float>(i), 0.5f}, ChurnVelocity{0.1f, 0.1f});
    const Archetype* base = world.GetEntityArchetype(handles[0]);
    const Archetype* withPayload = nullptr;

    uint64_t steadyAllocations = 0;
    for (int run = 0; run < kRuns; ++run)
    {
        const Memory::AllocationCountScope scope(Memory::CountWindow::Process);
        for (const EntityHandle handle : handles)
            world.AddComponentImmediate(handle, ChurnPayload{1, 1.0f});
        if (run == 0)
            withPayload = world.GetEntityArchetype(handles[0]);
        for (const EntityHandle handle : handles)
            world.RemoveComponentImmediate<ChurnPayload>(handle);
        if (run > 0)
            steadyAllocations += scope.Count();
    }

    EXPECT_EQ(steadyAllocations, 0u);
    ASSERT_NE(withPayload, nullptr);
    EXPECT_EQ(base->GetChunkCount(), ChunksNeeded(*base));
    EXPECT_EQ(withPayload->GetChunkCount(), 0u);
}

// 10,000 static entities; 100 projectiles spawn a frame and each lives 10 frames.
TEST(ChunkReuse, SpawnDespawnAllocatesNoChunkAfterWarmUp)
{
    constexpr std::size_t kStatic = 10000;
    constexpr std::size_t kPerFrame = 100;
    constexpr int kLifetime = 10;
    constexpr int kWarmUpFrames = 100;
    constexpr int kFrames = 300;
    World world(nullptr);
    world.RegisterComponents<ChurnPosition, ChurnVelocity, ChurnDamage>();
    for (std::size_t i = 0; i < kStatic; ++i)
        world.CreateHandle(ChurnPosition{}, ChurnVelocity{});
    // A ring of live projectiles: frame f reuses the slots of frame f - kLifetime.
    std::vector<EntityHandle> live(kPerFrame * kLifetime);

    uint64_t steadyAllocations = 0;
    for (int frame = 0; frame < kFrames; ++frame)
    {
        const std::size_t first = static_cast<std::size_t>(frame % kLifetime) * kPerFrame;
        const Memory::AllocationCountScope scope(Memory::CountWindow::Process);
        for (std::size_t k = 0; k < kPerFrame; ++k)
        {
            EntityHandle& slot = live[first + k];
            if (frame >= kLifetime)
                world.DestroyEntityImmediate(slot);
            slot = world.CreateHandle(ChurnPosition{}, ChurnVelocity{}, ChurnDamage{});
        }
        if (frame >= kWarmUpFrames)
            steadyAllocations += scope.Count();
    }

    EXPECT_EQ(steadyAllocations, 0u);
    const Archetype* projectiles = world.GetEntityArchetype(live[0]);
    ASSERT_NE(projectiles, nullptr);
    EXPECT_EQ(projectiles->GetChunkCount(), ChunksNeeded(*projectiles));
}

TEST(ChunkReuse, DrainedTableHoldsNoChunkToIterate)
{
    const std::array metas{ComponentMeta{1, 12, 4}, ComponentMeta{2, 16, 16}};
    ArchetypeTable table(metas);
    const uint32_t rows = table.GetLayout().Capacity * 3;
    std::vector<EntityLocation> locations(rows + 1);
    for (uint32_t index = 1; index <= rows; ++index)
        AddRow(table, locations, index);
    ASSERT_EQ(table.GetChunkCount(), 3u);

    for (uint32_t index = 1; index <= rows; ++index)
        RemoveRow(table, locations, index);

    EXPECT_EQ(table.GetEntityCount(), 0u);
    EXPECT_TRUE(table.GetChunks().empty());
}

TEST(ChunkReuse, ReservedTableFillsWithoutAllocating)
{
    const std::array metas{ComponentMeta{1, 12, 4}, ComponentMeta{2, 16, 16}};
    ArchetypeTable table(metas);
    const uint32_t rows = table.GetLayout().Capacity * 3;
    std::vector<EntityLocation> locations(rows + 1);
    table.Reserve(rows);

    uint64_t allocations = 0;
    {
        const Memory::AllocationCountScope scope(Memory::CountWindow::Process);
        for (uint32_t index = 1; index <= rows; ++index)
            AddRow(table, locations, index);
        allocations = scope.Count();
    }

    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(table.GetChunkCount(), 3u);
}

// Rows of 4 KiB, so that a chunk holds a few rows.
TEST(ChunkReuse, RemovalMovesTheTableLastRowIntoTheHole)
{
    constexpr uint32_t kRowBytes = 4096;
    const std::array metas{ComponentMeta{1, kRowBytes, 16}};
    ArchetypeTable table(metas);
    const uint32_t capacity = table.GetLayout().Capacity;
    ASSERT_GE(capacity, 2u);
    const uint32_t rows = capacity * 2 + 1;
    std::vector<EntityLocation> locations(rows + 1);
    for (uint32_t index = 1; index <= rows; ++index)
    {
        AddRow(table, locations, index);
        auto& chunk = table.GetChunks()[locations[index].ChunkIndex];
        std::memset(static_cast<std::byte*>(chunk.GetColumnRaw(0)) + std::size_t{locations[index].IndexInChunk} * kRowBytes,
                    static_cast<int>(index), kRowBytes);
    }
    ASSERT_EQ(table.GetChunkCount(), 3u);

    const EntityHandle moved = table.RemoveEntity(locations[1]);

    EXPECT_EQ(moved, EntityHandle(rows, 1));
    EXPECT_EQ(table.GetChunkCount(), 2u);
    const auto& hole = table.GetChunks()[0];
    EXPECT_EQ(hole.GetEntityHandles()[0], EntityHandle(rows, 1));
    const auto* bytes = static_cast<const std::byte*>(hole.GetColumnRaw(0));
    EXPECT_EQ(bytes[0], std::byte(rows));
    EXPECT_EQ(bytes[kRowBytes - 1], std::byte(rows));
    for (const auto& chunk : table.GetChunks())
        EXPECT_EQ(chunk.GetCount(), capacity);
}

// A destroy in chunk 0 moves the table's last row out of chunk 1; the World finds it at its new row.
TEST(ChunkReuse, WorldFindsTheRowMovedFromTheLastChunk)
{
    World world(nullptr);
    world.RegisterComponents<ChurnPayload>();
    std::vector<EntityHandle> handles{world.CreateHandle(ChurnPayload{0, 0.0f})};
    while (world.GetEntityArchetype(handles[0])->GetChunkCount() < 2)
        handles.push_back(world.CreateHandle(ChurnPayload{static_cast<int32_t>(handles.size()), 0.0f}));
    world.DestroyEntityImmediate(handles[0]);
    for (std::size_t i = 1; i < handles.size(); ++i)
        EXPECT_EQ(world.GetComponent<ChurnPayload>(handles[i]) ? world.GetComponent<ChurnPayload>(handles[i])->Value : -1,
                  static_cast<int32_t>(i));
}

} // namespace
