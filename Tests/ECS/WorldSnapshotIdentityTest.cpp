#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"

#include <gtest/gtest.h>

#include <functional>
#include <vector>

namespace
{
using namespace GameEngine::ECS;
struct SnapshotValue { int Value = 0; };
struct SnapshotReference { EntityHandle Target; };

TEST(WorldSnapshotIdentity, PreservesSparseHandlesAndReusedGenerations)
{
    World world(nullptr);
    const auto old = world.CreateHandle(SnapshotValue{1});
    const auto hole = world.CreateHandle(SnapshotValue{2});
    const auto sibling = world.CreateHandle(SnapshotValue{3});
    world.DestroyEntityImmediate(old);
    const auto reused = world.CreateHandle(SnapshotValue{4});
    ASSERT_EQ(reused.index, old.index);
    ASSERT_NE(reused.version, old.version);
    world.DestroyEntityImmediate(hole);
    const auto snapshot = world.SerializeWorld();
    const auto generation = world.GetLifecycleResetGeneration();

    world.DestroyEntityImmediate(reused);
    world.DeserializeWorld(snapshot);

    ASSERT_TRUE(world.IsValid(reused));
    ASSERT_TRUE(world.IsValid(sibling));
    EXPECT_FALSE(world.IsValid(old));
    EXPECT_FALSE(world.IsValid(hole));
    EXPECT_EQ(world.GetComponent<SnapshotValue>(reused)->Value, 4);
    EXPECT_EQ(world.GetComponent<SnapshotValue>(sibling)->Value, 3);
    EXPECT_NE(world.GetLifecycleResetGeneration(), generation);
}

TEST(WorldSnapshotIdentity, ARestoreKeepsTheRunItWasTakenInAndAnyOtherStartsANewOne)
{
    World world(nullptr);
    world.CreateHandle(SnapshotValue{1});
    const auto epoch = world.GetEntityHandleEpoch();
    const auto snapshot = world.SerializeWorld();

    world.DeserializeWorld(snapshot);
    EXPECT_EQ(world.GetEntityHandleEpoch(), epoch) << "an in-place restore kept the handles but left their run";

    world.Clear();
    const auto replaced = world.GetEntityHandleEpoch();
    EXPECT_NE(replaced, epoch);
    world.DeserializeWorld(snapshot);
    EXPECT_NE(world.GetEntityHandleEpoch(), epoch) << "a run the world had left came back with handles it may reissue";
    EXPECT_NE(world.GetEntityHandleEpoch(), replaced);
}

// Within one run no handle is issued twice: a restore keeps every slot past the versions it
// issued since the snapshot, whether the slot is free in the snapshot, new since it, or holds
// an entity the snapshot brings back.
TEST(WorldSnapshotIdentity, ARestoreNeverReissuesAHandleOfItsRun)
{
    World world(nullptr);
    const auto kept = world.CreateHandle(SnapshotValue{1});
    const auto freed = world.CreateHandle(SnapshotValue{2});
    world.DestroyEntityImmediate(freed);
    const auto snapshot = world.SerializeWorld();

    const auto reused = world.CreateHandle(SnapshotValue{3}); // the snapshot's free slot
    const auto appended = world.CreateHandle(SnapshotValue{4}); // a slot the snapshot does not have
    world.DestroyEntityImmediate(kept);
    const auto replacing = world.CreateHandle(SnapshotValue{5}); // the slot of an entity the snapshot holds
    ASSERT_EQ(reused.index, freed.index);
    ASSERT_EQ(replacing.index, kept.index);
    const std::vector<EntityHandle> issuedAfter = {reused, appended, replacing};

    world.DeserializeWorld(snapshot);
    ASSERT_TRUE(world.IsValid(kept));
    for (const auto handle : issuedAfter)
        EXPECT_FALSE(world.IsValid(handle));
    world.DestroyEntityImmediate(kept);
    for (int i = 0; i < 4; ++i)
    {
        const auto created = world.CreateEntity();
        for (const auto handle : issuedAfter)
            EXPECT_NE(created, handle) << "a handle issued before the restore was issued again";
    }
}

// The editor's redo of a create revives the handle its undo parked. After an undo and a redo of
// an earlier snapshot command, that handle's slot is on the free list again: the revive takes it
// off, so the next create takes another slot.
TEST(WorldSnapshotIdentity, RedoingACreateAfterASnapshotRedoKeepsItsSlot)
{
    World world(nullptr);
    world.CreateHandle(SnapshotValue{1});
    const auto before = world.SerializeWorld();
    world.CreateHandle(SnapshotValue{2}); // the stroke
    const auto after = world.SerializeWorld();
    const auto created = world.CreateHandle(SnapshotValue{3}); // a create command
    world.DestroyEntityImmediatePreserveHandle(created); // its undo
    world.DeserializeWorld(before); // the snapshot command's undo
    world.DeserializeWorld(after); // its redo

    ASSERT_TRUE(world.ReviveEntityImmediatePreserveHandle(created)); // the create's redo
    const auto next = world.CreateEntity();

    EXPECT_TRUE(world.IsValid(created));
    EXPECT_NE(next.index, created.index) << "a create took the slot of a live entity";
    EXPECT_EQ(world.GetEntityCount(), 4u);
}

// A handle issued after a snapshot, parked by a preserve-handle destroy and revived after the
// snapshot is restored, is the only entity its slot ever held in the run: it comes back alive
// and its slot leaves the free list.
TEST(WorldSnapshotIdentity, ReviveOfAHandleIssuedAfterTheSnapshotLeavesTheFreeList)
{
    World world(nullptr);
    const auto kept = world.CreateHandle(SnapshotValue{1});
    const auto snapshot = world.SerializeWorld();
    const auto issued = world.CreateHandle(SnapshotValue{2});
    world.DestroyEntityImmediatePreserveHandle(issued);
    world.DeserializeWorld(snapshot);

    ASSERT_TRUE(world.ReviveEntityImmediatePreserveHandle(issued));
    const auto next = world.CreateEntity();

    EXPECT_TRUE(world.IsValid(kept));
    EXPECT_TRUE(world.IsValid(issued));
    EXPECT_TRUE(world.IsValid(next));
    EXPECT_NE(next.index, issued.index) << "a create took the slot of a live entity";
    EXPECT_EQ(world.GetEntityCount(), 3u);
}

// Whichever way a slot reaches the free list (a destroy, a restore that frees it, a restore after
// a Clear) and leaves it (a create, a revive), a revive takes exactly that slot and no create
// takes a live one, while the other free slots keep their order.
TEST(WorldSnapshotIdentity, ARevivedSlotLeavesTheFreeListWhicheverWayItGotThere)
{
    World world(nullptr);
    const auto expectNoCreateTakes = [&world](EntityHandle live, int creates) {
        for (int i = 0; i < creates; ++i)
            EXPECT_NE(world.CreateEntity().index, live.index) << "a create took the slot of a live entity";
        EXPECT_TRUE(world.IsValid(live));
    };

    // A destroy pushes the slot; the revive takes it back.
    const auto destroyed = world.CreateEntity();
    const auto other = world.CreateEntity();
    world.DestroyEntityImmediate(other);
    world.DestroyEntityImmediate(destroyed);
    ASSERT_TRUE(world.ReviveEntityImmediatePreserveHandle(destroyed));
    EXPECT_EQ(world.CreateEntity().index, other.index) << "the revive disturbed the other free slot";
    expectNoCreateTakes(destroyed, 2);

    // Every create path pops the slot off the free list; the editor's delete and its undo then
    // leave the list alone. A pop that kept the slot marked would send the revive searching for
    // an index the list no longer holds.
    const std::function<EntityHandle()> creates[] = {
        [&world] { return world.CreateEntity(); },
        [&world] { return world.CreateHandle(SnapshotValue{3}); },
        [&world] { return world.CreateBatchHandle(1).front(); },
        [&world] {
            // CreateDirect takes a slot itself only in bulk mode; otherwise it is CreateEntity.
            world.BeginBulkOperations();
            const auto created = world.CreateDirect();
            world.EndBulkOperations();
            return created;
        },
    };
    for (const auto& create : creates)
    {
        const auto freed = world.CreateEntity();
        world.DestroyEntityImmediate(freed);
        const auto popped = create();
        ASSERT_EQ(popped.index, freed.index) << "the create did not take the free slot";
        world.DestroyEntityImmediatePreserveHandle(popped);
        ASSERT_TRUE(world.ReviveEntityImmediatePreserveHandle(popped));
        expectNoCreateTakes(popped, 2);
    }

    // A restore frees a slot past the snapshot's, and a create pops it.
    const auto beforeLater = world.SerializeWorld();
    world.CreateEntity();
    world.DeserializeWorld(beforeLater);
    const auto poppedAfterRestore = world.CreateEntity();
    world.DestroyEntityImmediatePreserveHandle(poppedAfterRestore);
    ASSERT_TRUE(world.ReviveEntityImmediatePreserveHandle(poppedAfterRestore));
    expectNoCreateTakes(poppedAfterRestore, 2);

    // A restore frees the slot of an entity created after the snapshot.
    const auto snapshot = world.SerializeWorld();
    const auto afterSnapshot = world.CreateEntity();
    world.DestroyEntityImmediatePreserveHandle(afterSnapshot);
    world.DeserializeWorld(snapshot);
    ASSERT_TRUE(world.ReviveEntityImmediatePreserveHandle(afterSnapshot));
    expectNoCreateTakes(afterSnapshot, 2);

    // A Clear empties the list; a restore after it brings back the snapshot's free slots.
    const auto freedInSnapshot = world.CreateEntity();
    world.DestroyEntityImmediate(freedInSnapshot);
    const auto withFreeSlot = world.SerializeWorld();
    world.Clear();
    world.DeserializeWorld(withFreeSlot);
    ASSERT_TRUE(world.ReviveEntityImmediatePreserveHandle(freedInSnapshot));
    expectNoCreateTakes(freedInSnapshot, 2);
}

TEST(WorldSnapshotIdentity, PreservesEntityReferencesAndComponentlessEntities)
{
    World world(nullptr);
    const auto hole = world.CreateEntity();
    const auto empty = world.CreateEntity();
    const auto parent = world.CreateHandle(SnapshotValue{7});
    const auto child = world.CreateHandle(SnapshotReference{parent});
    world.DestroyEntityImmediate(hole);
    const auto snapshot = world.SerializeWorld();

    world.DeserializeWorld(snapshot);

    ASSERT_TRUE(world.IsValid(empty));
    ASSERT_TRUE(world.IsValid(parent));
    ASSERT_TRUE(world.IsValid(child));
    ASSERT_NE(world.GetComponent<SnapshotReference>(child), nullptr);
    EXPECT_EQ(world.GetComponent<SnapshotReference>(child)->Target, parent);
    EXPECT_EQ(world.GetComponent<SnapshotValue>(parent)->Value, 7);
}

TEST(WorldSnapshotIdentity, PreservesReservedUndoHandlesAndFreeIndexOrder)
{
    World world(nullptr);
    const auto firstFree = world.CreateEntity();
    const auto reserved = world.CreateHandle(SnapshotValue{9});
    const auto lastFree = world.CreateEntity();
    world.DestroyEntityImmediate(firstFree);
    world.DestroyEntityImmediatePreserveHandle(reserved);
    world.DestroyEntityImmediate(lastFree);
    const auto snapshot = world.SerializeWorld();
    const auto expectedFirst = world.CreateEntity();
    const auto expectedSecond = world.CreateEntity();

    world.DeserializeWorld(snapshot);

    EXPECT_TRUE(world.ReviveEntityImmediatePreserveHandle(reserved));
    // The free indices come back in their order; the handles issued after the snapshot are
    // not issued again (ARestoreNeverReissuesAHandleOfItsRun).
    const auto first = world.CreateEntity();
    const auto second = world.CreateEntity();
    EXPECT_EQ(first.index, expectedFirst.index);
    EXPECT_EQ(second.index, expectedSecond.index);
    EXPECT_NE(first, expectedFirst);
    EXPECT_NE(second, expectedSecond);
    EXPECT_TRUE(world.IsValid(reserved));
}

TEST(WorldSnapshotIdentity, DeletionReplayAndUndoSurviveAnotherSnapshotCycle)
{
    World world(nullptr);
    const auto gap = world.CreateEntity();
    const auto target = world.CreateHandle(SnapshotValue{11});
    const auto sibling = world.CreateHandle(SnapshotValue{12});
    std::vector<std::uint8_t> targetBytes;
    ASSERT_TRUE(world.CaptureComponentBytes(target, GetComponentTypeId<SnapshotValue>(), targetBytes));
    world.DestroyEntityImmediate(gap);
    const auto beforePlay = world.SerializeWorld();
    world.DestroyEntityImmediatePreserveHandle(target);
    world.DeserializeWorld(beforePlay);
    ASSERT_TRUE(world.IsValid(target));

    // The existing delete command replays the exact handle captured during Play.
    world.DestroyEntityImmediatePreserveHandle(target);
    ASSERT_FALSE(world.IsValid(target));
    ASSERT_TRUE(world.IsValid(sibling));
    world.DeserializeWorld(world.SerializeWorld());
    EXPECT_FALSE(world.IsValid(target));
    EXPECT_TRUE(world.IsValid(sibling));
    EXPECT_TRUE(world.ReviveEntityImmediatePreserveHandle(target));
    EXPECT_TRUE(world.ApplyComponentBytesImmediate(target, GetComponentTypeId<SnapshotValue>(),
                                                   targetBytes));
    EXPECT_EQ(world.GetComponent<SnapshotValue>(target)->Value, 11);
    EXPECT_EQ(world.GetComponent<SnapshotValue>(sibling)->Value, 12);
}

TEST(WorldSnapshotIdentity, RejectsTruncatedAndInvalidEnvelopesWithoutClearingTheWorld)
{
    World world(nullptr);
    const auto firstFree = world.CreateEntity();
    const auto secondFree = world.CreateEntity();
    const auto live = world.CreateHandle(SnapshotValue{19});
    world.DestroyEntityImmediate(firstFree);
    world.DestroyEntityImmediate(secondFree);
    const auto snapshot = world.SerializeWorld();
    const auto generation = world.GetLifecycleResetGeneration();
    const auto expectUnchanged = [&] {
        ASSERT_TRUE(world.IsValid(live));
        EXPECT_EQ(world.GetComponent<SnapshotValue>(live)->Value, 19);
        EXPECT_EQ(world.GetLifecycleResetGeneration(), generation);
    };
    for (std::size_t length = 0; length < snapshot.size(); ++length)
    {
        SCOPED_TRACE(length);
        world.DeserializeWorld({snapshot.begin(), snapshot.begin() + length});
        expectUnchanged();
    }
    const auto corrupt = [&](std::size_t offset, auto value) {
        auto invalid = snapshot;
        std::memcpy(invalid.data() + offset, &value, sizeof(value));
        world.DeserializeWorld(invalid);
        expectUnchanged();
    };
    corrupt(4, uint32_t{99}); // Unsupported format version.
    corrupt(8, uint32_t{kEntityIndexMask + 2}); // Unrepresentable slot count.
    corrupt(24, EntityVersion{kEntityVersionMask + 1}); // Invalid generation bits.
    corrupt(26, uint32_t{0xFFFFFFFF}); // Invalid entity record length.
    corrupt(snapshot.size() - 4, EntityIndex{firstFree.index}); // Duplicate free index.
    corrupt(snapshot.size() - 4, EntityIndex{live.index}); // Live/free overlap.
    corrupt(snapshot.size() - 4, EntityIndex{kEntityIndexMask}); // Free index out of range.
    auto trailing = snapshot;
    trailing.push_back(0);
    world.DeserializeWorld(trailing);
    expectUnchanged();
}
} // namespace
