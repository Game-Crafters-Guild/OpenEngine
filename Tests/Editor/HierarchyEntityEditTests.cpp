// The Hierarchy's entity edits: its context menu offers Rename, Duplicate and Delete under the
// same command ids its key map produces, and a delete takes each selected entity's subtree once,
// as one undoable step that tells the Hierarchy to rebuild its rows and restores the selection on
// undo.

#include <gtest/gtest.h>

#include "Editor/Hierarchy/HierarchyEntityCommands.h"
#include "Editor/Hierarchy/HierarchyEntityDelete.h"
#include "EditorChangeNotifications.h"
#include "RecordingContextMenu.h"
#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Platform/ContextMenu.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace GameEngine;
using Editor::EditorChangeNotifications;

TEST(HierarchyEntityEdits, TheEntityMenuOffersRenameDuplicateAndDeleteAsTheKeyMapsCommands)
{
    ContextMenuBuilder builder;
    Editor::AddHierarchyEntityEditItems(builder);
    EditorTesting::RecordingContextMenu menu;
    builder.Build(&menu);

    EXPECT_EQ(menu.CommandOf("Delete"), Editor::kHierarchyCmdDelete)
        << "the entity context menu has no Delete, or its Delete is not the Delete key's command";
    EXPECT_EQ(menu.CommandOf("Duplicate"), Editor::kHierarchyCmdDuplicate);
    EXPECT_EQ(menu.CommandOf("Rename"), Editor::kHierarchyCmdRename)
        << "the entity context menu has no Rename, or its Rename is not F2's command";
}

namespace
{

class HierarchyEntityDeleteTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Token = m_Notifications.SubscribeWorldStructureChanged(
            [this](const EditorChangeNotifications::WorldStructureChangedEvent& e)
            {
                if (e.world == &m_World && e.kind == EditorChangeNotifications::ChangeKind::Commit)
                    ++m_StructureCommits;
            });
    }

    void TearDown() override { m_Notifications.Unsubscribe(m_Token); }

    ECS::EntityHandle Spawn(ECS::EntityHandle parent = {})
    {
        // Every editor entity carries components; a bare one has no archetype to snapshot.
        const ECS::EntityHandle e = m_World.CreateEntity();
        m_World.AddComponentImmediate(e, Components::Name{});
        if (parent.IsValid())
            m_World.AddComponentImmediate(e, Components::Parent{parent});
        return e;
    }

    ECS::World m_World;
    EditorChangeNotifications m_Notifications;
    EditorChangeNotifications::SubscriptionToken m_Token{};
    int m_StructureCommits = 0;
};

} // namespace

TEST_F(HierarchyEntityDeleteTest, TakesEachSelectedSubtreeOnceAndLeavesTheRest)
{
    const ECS::EntityHandle parent = Spawn();
    const ECS::EntityHandle child = Spawn(parent);
    const ECS::EntityHandle grandchild = Spawn(child);
    const ECS::EntityHandle other = Spawn();
    const ECS::EntityHandle bystander = Spawn();

    // The child is selected beside its parent: it must not be taken twice.
    const std::vector<ECS::EntityHandle> selected{child, parent, other};
    const std::vector<ECS::EntityHandle> deletion = Editor::CollectHierarchyDeletion(m_World, selected);

    std::vector<ECS::EntityHandle> expected{parent, child, grandchild, other};
    std::sort(expected.begin(), expected.end(),
              [](const ECS::EntityHandle& a, const ECS::EntityHandle& b) { return a.id < b.id; });
    EXPECT_EQ(deletion, expected);
    EXPECT_EQ(std::count(deletion.begin(), deletion.end(), bystander), 0);
}

TEST_F(HierarchyEntityDeleteTest, OneUndoableStepThatTellsTheHierarchyToRebuild)
{
    const ECS::EntityHandle parent = Spawn();
    const ECS::EntityHandle child = Spawn(parent);
    const std::vector<ECS::EntityHandle> selected{parent};
    std::vector<ECS::EntityHandle> deletion = Editor::CollectHierarchyDeletion(m_World, selected);
    ASSERT_EQ(deletion.size(), 2u);

    const std::string undoName = Editor::HierarchyDeletionUndoName(deletion.size());
    Editor::DeleteEntitiesCommand command(undoName, &m_World, &m_Notifications, std::move(deletion));
    command.Do();

    EXPECT_FALSE(m_World.IsValid(parent));
    EXPECT_FALSE(m_World.IsValid(child));
    EXPECT_EQ(m_StructureCommits, 1)
        << "the delete did not post the structure commit the Hierarchy rebuilds its rows on";
    EXPECT_STREQ(command.GetName(), "Delete Entities");

    command.Undo();
    EXPECT_TRUE(m_World.IsValid(parent));
    EXPECT_TRUE(m_World.IsValid(child));
    EXPECT_EQ(m_StructureCommits, 2);
}

// The path the Delete key and the menu's Delete both run (kHierarchyCmdDelete): one entry on the
// undo stack, and Undo brings the entities back and reselects them.
TEST_F(HierarchyEntityDeleteTest, TheHierarchyDeleteIsOneUndoStepThatRestoresTheEntitiesAndTheSelection)
{
    const ECS::EntityHandle parent = Spawn();
    const ECS::EntityHandle child = Spawn(parent);
    const std::vector<ECS::EntityHandle> selected{parent};
    Editor::UndoRedoService undo;
    int deselects = 0;
    int reselects = 0;

    ASSERT_TRUE(Editor::DeleteHierarchySelection(m_World, &undo, &m_Notifications, selected, [&]() { ++deselects; },
                                                 [&]() { ++reselects; }));
    EXPECT_FALSE(m_World.IsValid(parent));
    EXPECT_FALSE(m_World.IsValid(child));
    ASSERT_EQ(undo.GetUndoCount(), 1u) << "the delete is not one step on the undo stack";
    EXPECT_STREQ(undo.PeekUndoName(), "Delete Entities");
    EXPECT_EQ(deselects, 1);

    undo.Undo();
    EXPECT_TRUE(m_World.IsValid(parent));
    EXPECT_TRUE(m_World.IsValid(child));
    const Components::Parent* restoredParent = m_World.GetComponent<Components::Parent>(child);
    ASSERT_NE(restoredParent, nullptr) << "undo brought the child back without its parent";
    EXPECT_EQ(restoredParent->Entity, parent) << "undo brought the child back under another parent";
    EXPECT_EQ(reselects, 1) << "undo did not restore the selection";
}

// A Hierarchy with no undo service still deletes, once, and still deselects.
TEST_F(HierarchyEntityDeleteTest, WithoutAnUndoServiceTheDeleteRunsOnceAndDeselects)
{
    const ECS::EntityHandle parent = Spawn();
    const ECS::EntityHandle child = Spawn(parent);
    const std::vector<ECS::EntityHandle> selected{parent};
    int deselects = 0;

    ASSERT_TRUE(Editor::DeleteHierarchySelection(m_World, nullptr, &m_Notifications, selected, [&]() { ++deselects; },
                                                 [] {}));
    EXPECT_FALSE(m_World.IsValid(parent));
    EXPECT_FALSE(m_World.IsValid(child));
    EXPECT_EQ(m_StructureCommits, 1);
    EXPECT_EQ(deselects, 1) << "the delete left the deleted entities selected";
}
