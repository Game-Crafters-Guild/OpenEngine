#include "SmartShape2D/ShapeDestructionHistory.h"
#include "SmartShape2D/ShapeExplosion.h"
#include "SmartShape2D/SmartShape.h"
#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <gtest/gtest.h>

namespace GameEngine::SmartShape2D
{
class ShapeDestructionHistoryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto terrain = world.CreateEntity();
        Components::SmartShape shape;
        shape.Destructible = true;
        world.AddComponentImmediate(terrain, shape);
        world.AddComponentImmediate(terrain, Components::WorldTransform{});
    }
    size_t CutCount() { return world.Query<ECS::Read<Components::SmartShapeCut>>().Count(); }
    ECS::World world;
    ShapeDestructionHistory history;
};

TEST_F(ShapeDestructionHistoryTest, MultipleBlastsUndoAndRedoOneAtATime)
{
    EXPECT_FALSE(history.Undo(world));
    EXPECT_FALSE(history.Redo(world));
    history.Explode(world, {1, 2, 0}, 1);
    history.Explode(world, {4, 5, 0}, 2);
    EXPECT_EQ(CutCount(), 2u);
    EXPECT_TRUE(history.Undo(world));
    EXPECT_EQ(CutCount(), 1u);
    world.Query<ECS::Read<Components::SmartShapeCut>>()
        .Each([](const Components::SmartShapeCut& cut) { EXPECT_FLOAT_EQ(cut.Center.x, 1); });
    EXPECT_TRUE(history.Undo(world));
    EXPECT_EQ(CutCount(), 0u);
    EXPECT_FALSE(history.Undo(world));
    EXPECT_TRUE(history.Redo(world));
    EXPECT_TRUE(history.Redo(world));
    EXPECT_EQ(CutCount(), 2u);
    EXPECT_FALSE(history.Redo(world));
}

TEST_F(ShapeDestructionHistoryTest, ResetIsUndoableAndNewBlastReplacesRedo)
{
    history.Explode(world, {}, 1);
    history.Reset(world);
    EXPECT_EQ(CutCount(), 0u);
    EXPECT_TRUE(history.Undo(world));
    EXPECT_EQ(CutCount(), 1u);
    EXPECT_TRUE(history.Redo(world));
    EXPECT_EQ(CutCount(), 0u);
    history.Undo(world);
    history.Explode(world, {2, 0, 0}, 1);
    EXPECT_EQ(CutCount(), 2u);
    EXPECT_FALSE(history.Redo(world));
}

TEST_F(ShapeDestructionHistoryTest, NoOpDoesNotEraseRedoAndExternalDamageInvalidatesHistory)
{
    history.Explode(world, {}, 1);
    history.Undo(world);
    history.Explode(world, {}, -1);
    history.Reset(world);
    EXPECT_TRUE(history.Redo(world));
    ExplodeShapes(world, {3, 0, 0}, 1);
    EXPECT_FALSE(history.Undo(world));
    EXPECT_EQ(CutCount(), 2u);
}

TEST_F(ShapeDestructionHistoryTest, SceneRestoreDiscardsOldHistory)
{
    const auto snapshot = world.SerializeWorld();
    history.Explode(world, {}, 1);
    world.DeserializeWorld(snapshot);
    EXPECT_FALSE(history.Undo(world));
    EXPECT_FALSE(history.Redo(world));
    EXPECT_EQ(CutCount(), 0u);
}

TEST_F(ShapeDestructionHistoryTest, HistoryRetainsAtMost128UndoSteps)
{
    for (int index = 0; index < 130; ++index)
        history.Explode(world, {static_cast<float>(index), 0, 0}, 1);
    size_t count = 0;
    while (history.Undo(world))
        ++count;
    EXPECT_EQ(count, 128u);
    EXPECT_EQ(CutCount(), 2u);
}
}
