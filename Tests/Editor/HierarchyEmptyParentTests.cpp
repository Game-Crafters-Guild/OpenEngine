#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "Editor/Hierarchy/HierarchyEmptyParent.h"
#include "Editor/Hierarchy/HierarchyOrdering.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"

using GameEngine::Components::HierarchyOrder;
using GameEngine::Components::Name;
using GameEngine::Components::Parent;
using GameEngine::Components::RuntimeOnlyEntity;
using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Editor::ChildrenInHierarchyOrder;
using GameEngine::Editor::CreateEmptyParent;
using GameEngine::Editor::EmptyParentResult;
using GameEngine::Engine::Renderer::TransformHierarchySystem;

namespace
{

using Matrix = std::array<float, 16>;

Name MakeName(const char* text)
{
    Name n{};
    std::strncpy(n.value, text, sizeof(n.value) - 1);
    return n;
}

// A yaw, a non-uniform scale and an offset: a parent that a "subtract the
// position" shortcut gets wrong.
Transform ObliqueTransform(float x, float y, float z, float yaw, float scale)
{
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    Transform t{};
    const float m[16] = {c * scale, 0, -s * scale, 0, 0, scale * 0.5f, 0, 0, s * scale, 0, c * scale, 0, x, y, z, 1};
    std::memcpy(t.matrix, m, sizeof(m));
    return t;
}

// The Parent, HierarchyOrder and Transform an undo has to restore.
struct HierarchyRecord
{
    bool HasParent = false;
    EntityHandle ParentEntity{};
    bool HasOrder = false;
    int Order = 0;
    Matrix Local{};
};

class HierarchyEmptyParentTest : public ::testing::Test
{
  protected:
    EntityHandle Spawn(const Transform& local, EntityHandle parent, int order)
    {
        const EntityHandle e = m_World.CreateEntity();
        m_World.AddComponentImmediate(e, local);
        m_World.AddComponentImmediate(e, HierarchyOrder{order});
        if (parent.IsValid())
            m_World.AddComponentImmediate(e, Parent{parent});
        return e;
    }

    void Tick() { m_Hierarchy.Update(m_World, 0.0f); }

    Matrix WorldOf(EntityHandle e)
    {
        Matrix m{};
        const auto* wt = m_World.GetComponent<WorldTransform>(e);
        EXPECT_NE(wt, nullptr);
        if (wt)
            std::memcpy(m.data(), wt->matrix, sizeof(wt->matrix));
        return m;
    }

    EntityHandle ParentOf(EntityHandle e)
    {
        const auto* p = m_World.GetComponent<Parent>(e);
        return p ? p->parent : EntityHandle{};
    }

    HierarchyRecord Record(EntityHandle e)
    {
        HierarchyRecord r{};
        if (const auto* p = m_World.GetComponent<Parent>(e))
        {
            r.HasParent = true;
            r.ParentEntity = p->parent;
        }
        if (const auto* o = m_World.GetComponent<HierarchyOrder>(e))
        {
            r.HasOrder = true;
            r.Order = o->order;
        }
        std::memcpy(r.Local.data(), m_World.GetComponent<Transform>(e)->matrix, sizeof(r.Local));
        return r;
    }

    EmptyParentResult Group(std::vector<EntityHandle> entities)
    {
        EmptyParentResult created = CreateEmptyParent(m_World, nullptr, entities, MakeName("Empty Parent"));
        Tick();
        return created;
    }

    static void ExpectNear(const Matrix& actual, const Matrix& expected, const char* what)
    {
        for (int i = 0; i < 16; ++i)
            EXPECT_NEAR(actual[i], expected[i], 1e-4f) << what << " element " << i;
    }

    World m_World;
    TransformHierarchySystem m_Hierarchy;
};

// Three siblings: one new parent under their parent, in the first one's place,
// holding all three in their order, at the centre of their world positions
// with identity rotation and unit scale, every child keeping its world pose.
TEST_F(HierarchyEmptyParentTest, SiblingsGoUnderOneParentAtTheirCentreKeepingWorldPoses)
{
    const EntityHandle root = Spawn(ObliqueTransform(10, 2, -4, 0.6f, 2.0f), {}, 0);
    const EntityHandle before = Spawn(ObliqueTransform(0, 0, 0, 0, 1), root, 0);
    const EntityHandle a = Spawn(ObliqueTransform(1, 0, 0, 0.3f, 1), root, 10);
    const EntityHandle unselected = Spawn(ObliqueTransform(0, 5, 0, 0, 1), root, 20);
    const EntityHandle b = Spawn(ObliqueTransform(0, 3, 2, -0.4f, 1.5f), root, 30);
    const EntityHandle c = Spawn(ObliqueTransform(-2, 1, 6, 1.1f, 0.5f), root, 40);
    Tick();
    const Matrix worldA = WorldOf(a);
    const Matrix worldB = WorldOf(b);
    const Matrix worldC = WorldOf(c);

    const EmptyParentResult created = Group({c, a, b});
    ASSERT_TRUE(created.NewParent.IsValid());
    ASSERT_NE(created.Command, nullptr);
    const EntityHandle group = created.NewParent;

    EXPECT_EQ(ParentOf(group).id, root.id);
    const std::vector<EntityHandle> rootChildren = ChildrenInHierarchyOrder(m_World, root);
    ASSERT_EQ(rootChildren.size(), 3u);
    EXPECT_EQ(rootChildren[0].id, before.id);
    EXPECT_EQ(rootChildren[1].id, group.id);
    EXPECT_EQ(rootChildren[2].id, unselected.id);

    const std::vector<EntityHandle> grouped = ChildrenInHierarchyOrder(m_World, group);
    ASSERT_EQ(grouped.size(), 3u);
    EXPECT_EQ(grouped[0].id, a.id);
    EXPECT_EQ(grouped[1].id, b.id);
    EXPECT_EQ(grouped[2].id, c.id);

    ExpectNear(WorldOf(a), worldA, "a world");
    ExpectNear(WorldOf(b), worldB, "b world");
    ExpectNear(WorldOf(c), worldC, "c world");

    const Matrix centre = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                           (worldA[12] + worldB[12] + worldC[12]) / 3.0f,
                           (worldA[13] + worldB[13] + worldC[13]) / 3.0f,
                           (worldA[14] + worldB[14] + worldC[14]) / 3.0f, 1};
    ExpectNear(WorldOf(group), centre, "new parent world");
    EXPECT_EQ(m_World.GetComponent<Name>(group)->View(), "Empty Parent");
}

// Entities under different parents go under their lowest common ancestor, in
// the place of the branch that holds the first of them in tree order.
TEST_F(HierarchyEmptyParentTest, MixedParentsGoUnderTheirLowestCommonAncestor)
{
    const EntityHandle top = Spawn(ObliqueTransform(3, 0, 0, 0.2f, 1), {}, 0);
    const EntityHandle left = Spawn(ObliqueTransform(-5, 0, 0, 0.9f, 2), top, 0);
    const EntityHandle right = Spawn(ObliqueTransform(5, 0, 0, -0.7f, 0.5f), top, 10);
    const EntityHandle a = Spawn(ObliqueTransform(1, 1, 1, 0, 1), left, 0);
    const EntityHandle b = Spawn(ObliqueTransform(-1, 2, 0, 0.5f, 1), right, 0);
    Tick();
    const Matrix worldA = WorldOf(a);
    const Matrix worldB = WorldOf(b);

    const EntityHandle group = Group({b, a}).NewParent;
    ASSERT_TRUE(group.IsValid());

    EXPECT_EQ(ParentOf(group).id, top.id);
    const std::vector<EntityHandle> topChildren = ChildrenInHierarchyOrder(m_World, top);
    ASSERT_EQ(topChildren.size(), 3u);
    EXPECT_EQ(topChildren[0].id, group.id);
    EXPECT_EQ(topChildren[1].id, left.id);
    EXPECT_EQ(topChildren[2].id, right.id);

    const std::vector<EntityHandle> grouped = ChildrenInHierarchyOrder(m_World, group);
    ASSERT_EQ(grouped.size(), 2u);
    EXPECT_EQ(grouped[0].id, a.id);
    EXPECT_EQ(grouped[1].id, b.id);
    ExpectNear(WorldOf(a), worldA, "a world");
    ExpectNear(WorldOf(b), worldB, "b world");

    // Entities with no common ancestor go under the scene root.
    const EntityHandle loose = Spawn(ObliqueTransform(0, 0, 9, 0, 1), {}, 10);
    Tick();
    const EntityHandle rootGroup = Group({loose, a}).NewParent;
    ASSERT_TRUE(rootGroup.IsValid());
    EXPECT_FALSE(m_World.HasComponent<Parent>(rootGroup));
}

// A selected entity below another selected one stays under it.
TEST_F(HierarchyEmptyParentTest, AnEntityBelowAnotherGivenOneStaysWithIt)
{
    const EntityHandle a = Spawn(ObliqueTransform(1, 0, 0, 0, 1), {}, 0);
    const EntityHandle child = Spawn(ObliqueTransform(0, 2, 0, 0.4f, 1), a, 0);
    const EntityHandle b = Spawn(ObliqueTransform(-3, 0, 0, 0, 1), {}, 10);
    Tick();
    const Matrix worldChild = WorldOf(child);

    const EntityHandle group = Group({child, a, b}).NewParent;
    ASSERT_TRUE(group.IsValid());

    EXPECT_EQ(ParentOf(child).id, a.id);
    const std::vector<EntityHandle> grouped = ChildrenInHierarchyOrder(m_World, group);
    ASSERT_EQ(grouped.size(), 2u);
    EXPECT_EQ(grouped[0].id, a.id);
    EXPECT_EQ(grouped[1].id, b.id);
    ExpectNear(WorldOf(child), worldChild, "child world");
}

// Undo restores every touched entity's Parent, order and local Transform
// exactly and removes the new parent; redo brings back the same result.
TEST_F(HierarchyEmptyParentTest, UndoRestoresTheHierarchyExactlyAndRedoReappliesIt)
{
    const EntityHandle p = Spawn(ObliqueTransform(4, 1, 0, 0.8f, 3), {}, 0);
    const EntityHandle q = Spawn(ObliqueTransform(-4, 0, 2, -0.2f, 1), {}, 10);
    const EntityHandle a = Spawn(ObliqueTransform(1, 0, 1, 0.1f, 1), p, 0);
    const EntityHandle sibling = Spawn(ObliqueTransform(2, 0, 0, 0, 1), p, 5);
    const EntityHandle b = Spawn(ObliqueTransform(0, 1, -1, 0.3f, 2), q, 7);
    const EntityHandle unordered = Spawn(ObliqueTransform(0, 0, 3, 0, 1), {}, 0);
    m_World.RemoveComponentImmediate<HierarchyOrder>(unordered);
    Tick();

    const std::vector<EntityHandle> everyone = {p, q, a, sibling, b, unordered};
    std::vector<HierarchyRecord> original;
    for (const EntityHandle e : everyone)
        original.push_back(Record(e));

    EmptyParentResult created = Group({a, b, unordered});
    ASSERT_TRUE(created.NewParent.IsValid());
    std::vector<HierarchyRecord> grouped;
    for (const EntityHandle e : everyone)
        grouped.push_back(Record(e));

    const auto expectRecords = [&](const std::vector<HierarchyRecord>& expected, const char* when) {
        for (std::size_t i = 0; i < everyone.size(); ++i)
        {
            const HierarchyRecord now = Record(everyone[i]);
            EXPECT_EQ(now.HasParent, expected[i].HasParent) << when << " entity " << i;
            EXPECT_EQ(now.ParentEntity.id, expected[i].ParentEntity.id) << when << " entity " << i;
            EXPECT_EQ(now.HasOrder, expected[i].HasOrder) << when << " entity " << i;
            EXPECT_EQ(now.Order, expected[i].Order) << when << " entity " << i;
            EXPECT_EQ(std::memcmp(now.Local.data(), expected[i].Local.data(), sizeof(now.Local)), 0) << when << " entity " << i;
        }
    };

    created.Command->Undo();
    EXPECT_FALSE(m_World.IsValid(created.NewParent));
    expectRecords(original, "after undo");

    created.Command->Redo();
    ASSERT_TRUE(m_World.IsValid(created.NewParent));
    EXPECT_EQ(m_World.GetComponent<Name>(created.NewParent)->View(), "Empty Parent");
    expectRecords(grouped, "after redo");
}

// The new parent's place is a generated entity: nothing is created, as a
// hierarchy drop onto it is refused.
TEST_F(HierarchyEmptyParentTest, NothingIsCreatedUnderAGeneratedEntity)
{
    const EntityHandle generated = Spawn(ObliqueTransform(0, 0, 0, 0, 1), {}, 0);
    m_World.AddComponentImmediate(generated, RuntimeOnlyEntity{});
    const EntityHandle a = Spawn(ObliqueTransform(1, 0, 0, 0, 1), generated, 0);
    const EntityHandle b = Spawn(ObliqueTransform(2, 0, 0, 0, 1), generated, 10);
    Tick();
    const std::size_t aliveBefore = m_World.GetEntityCount();

    const EmptyParentResult created = Group({a, b});

    EXPECT_FALSE(created.NewParent.IsValid());
    EXPECT_EQ(created.Command, nullptr);
    EXPECT_EQ(m_World.GetEntityCount(), aliveBefore);
    EXPECT_EQ(ParentOf(a).id, generated.id);
}

} // namespace
