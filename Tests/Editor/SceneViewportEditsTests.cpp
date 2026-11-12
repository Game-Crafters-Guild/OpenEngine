// A Scene View viewport bound to a second world edits that world: the measure tool's entity and a drop's
// undo step land in the bound world and the bound undo service, and the primary world stays unchanged.

#include <gtest/gtest.h>

#include "SceneView/SceneViewport/SceneViewportEdits.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Measure/MeasureComponent.h"
#include "Components/Name.h"
#include "Core/Engine.h"
#include "ECS/Query.h"
#include "ECS/World.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
using namespace GameEngine;

constexpr std::uint32_t kMeasureColor = 0xFFFFC838u;

std::size_t CountMeasures(ECS::World& world)
{
    std::size_t count = 0;
    world.Query<ECS::Read<Components::MeasureComponent>>().Each(
        [&count](ECS::EntityHandle, const Components::MeasureComponent&) { ++count; });
    return count;
}

TEST(SceneViewportEditsTests, MeasureLandsInTheBoundWorldAndTheBoundUndo)
{
    ECS::World& primary = *EngineCore::GetInstance().EnsurePrimaryWorld();
    const std::size_t primaryMeasures = CountMeasures(primary);

    ECS::World second;
    Editor::UndoRedoService secondUndo;
    int dirtyCount = 0;
    const SceneViewportBinding binding{nullptr, &secondUndo, nullptr, [&dirtyCount]() { ++dirtyCount; }};
    SceneViewportEdits edits(second, binding);

    const ECS::EntityHandle root =
        edits.CreateMeasure(Mathematics::Vector3(0.0f, 0.0f, 0.0f), Mathematics::Vector3(2.0f, 0.0f, 0.0f), false, kMeasureColor);

    ASSERT_TRUE(root.IsValid());
    ASSERT_TRUE(second.IsValid(root));
    const auto* measure = second.GetComponent<Components::MeasureComponent>(root);
    ASSERT_NE(measure, nullptr);
    EXPECT_TRUE(second.IsValid(measure->StartEntity));
    EXPECT_TRUE(second.IsValid(measure->EndEntity));
    EXPECT_FLOAT_EQ(measure->End[0], 2.0f);
    EXPECT_EQ(CountMeasures(second), 1u);
    EXPECT_EQ(CountMeasures(primary), primaryMeasures);
    EXPECT_EQ(secondUndo.GetUndoCount(), 1u);
    EXPECT_STREQ(secondUndo.PeekUndoName(), "Create Measure");
    EXPECT_EQ(dirtyCount, 1);

    secondUndo.Undo();
    EXPECT_FALSE(second.IsValid(root));
    EXPECT_EQ(CountMeasures(primary), primaryMeasures);
}

TEST(SceneViewportEditsTests, ZeroLengthMeasureCreatesNothing)
{
    ECS::World second;
    Editor::UndoRedoService secondUndo;
    const SceneViewportBinding binding{nullptr, &secondUndo, nullptr, {}};
    SceneViewportEdits edits(second, binding);

    const Mathematics::Vector3 point(1.0f, 2.0f, 3.0f);
    EXPECT_FALSE(edits.CreateMeasure(point, point, false, kMeasureColor).IsValid());
    EXPECT_EQ(CountMeasures(second), 0u);
    EXPECT_EQ(secondUndo.GetUndoCount(), 0u);
}

TEST(SceneViewportEditsTests, DropRecordsItsUndoStepInTheBoundUndo)
{
    ECS::World second;
    Editor::UndoRedoService secondUndo;
    const SceneViewportBinding binding{nullptr, &secondUndo, nullptr, {}};
    SceneViewportEdits edits(second, binding);

    const ECS::EntityHandle dropped = second.CreateEntity();
    Components::Name name{};
    std::strncpy(name.value, "Duck", sizeof(name.value) - 1);
    second.AddComponentImmediate(dropped, name);

    edits.RecordDrop({dropped}, {}, {}, {});

    EXPECT_EQ(secondUndo.GetUndoCount(), 1u);
    secondUndo.Undo();
    EXPECT_FALSE(second.IsValid(dropped));
}

} // namespace
