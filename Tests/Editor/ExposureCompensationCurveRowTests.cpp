// The camera inspector's Compensation Curve row, driven the way an author drives it: an edit of the
// graph is one undo step on the camera, mirrored to every other selected camera, and the selected
// key's fields edit the key they show.

#include <gtest/gtest.h>

#include "Components/Rendering/Camera.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "InspectorRegistry.h"
#include "Inspectors/ExposureCompensationCurveRow.h"
#include "Mathematics/Curve.h"
#include "UI/Controls/CurveField.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <vector>

using namespace GameEngine;

namespace
{
class ExposureCompensationCurveRowTests : public ::testing::Test
{
  protected:
    ECS::World World;
    Editor::UndoRedoService Undo;
    UIElement Root;
    ECS::EntityHandle Camera;

    // A camera with three keys, so a key can be deleted above the graph's two-key minimum.
    static Components::Camera ThreeKeyCamera()
    {
        Components::Camera camera{};
        camera.AutoExposureCompensationCurve = Math::Curve{};
        camera.AutoExposureCompensationCurve.TryInsert(Math::CurveKey{-10.0f, -4.0f});
        camera.AutoExposureCompensationCurve.TryInsert(Math::CurveKey{-2.0f, -2.75f});
        camera.AutoExposureCompensationCurve.TryInsert(Math::CurveKey{6.0f, 0.0f});
        return camera;
    }

    CurveField* Build(const std::vector<ECS::EntityHandle>& alsoSelected = {},
                      const Components::Camera& authored = ThreeKeyCamera())
    {
        Camera = World.CreateEntity();
        World.AddComponentImmediate(Camera, authored);
        InspectorContext ctx{};
        ctx.Parent = &Root;
        ctx.World = &World;
        ctx.Entity = Camera;
        if (!alsoSelected.empty())
        {
            ctx.Entities = {Camera};
            ctx.Entities.insert(ctx.Entities.end(), alsoSelected.begin(), alsoSelected.end());
        }
        ctx.Undo = &Undo;
        return AddExposureCompensationCurveRow(ctx);
    }

    const Math::Curve& Stored(ECS::EntityHandle camera)
    {
        return World.GetComponent<Components::Camera>(camera)->AutoExposureCompensationCurve;
    }
};
} // namespace

// The graph shows the camera's keys: a default camera's.
TEST_F(ExposureCompensationCurveRowTests, TheGraphShowsTheCamerasKeys)
{
    CurveField* graph = Build({}, Components::Camera{});
    ASSERT_NE(graph, nullptr);
    const Math::Curve defaults = Components::DefaultExposureCompensationCurve();
    ASSERT_EQ(graph->GetKeys().size(), defaults.KeyCount);
    for (uint8 i = 0; i < defaults.KeyCount; ++i)
    {
        EXPECT_FLOAT_EQ(graph->GetKeys()[i].Time, defaults.Keys[i].Time) << i;
        EXPECT_FLOAT_EQ(graph->GetKeys()[i].Value, defaults.Keys[i].Value) << i;
    }
}

// Deleting a key writes the camera's curve, and one undo puts the key back.
TEST_F(ExposureCompensationCurveRowTests, AnEditIsOneUndoStepOnTheCamera)
{
    CurveField* graph = Build();
    ASSERT_NE(graph, nullptr);
    graph->DeleteKey(1);
    ASSERT_EQ(Stored(Camera).KeyCount, 2u);
    EXPECT_FLOAT_EQ(Stored(Camera).Keys[1].Time, 6.0f);

    Undo.Undo();
    ASSERT_EQ(Stored(Camera).KeyCount, 3u);
    EXPECT_FLOAT_EQ(Stored(Camera).Keys[1].Value, -2.75f);
}

// With several cameras selected an edit reaches all of them, as one undo step.
TEST_F(ExposureCompensationCurveRowTests, AnEditReachesEverySelectedCamera)
{
    const ECS::EntityHandle other = World.CreateEntity();
    World.AddComponentImmediate(other, ThreeKeyCamera());
    CurveField* graph = Build({other});
    ASSERT_NE(graph, nullptr);
    graph->DeleteKey(0);
    EXPECT_EQ(Stored(Camera).KeyCount, 2u);
    EXPECT_EQ(Stored(other).KeyCount, 2u);

    Undo.Undo();
    EXPECT_EQ(Stored(Camera).KeyCount, 3u);
    EXPECT_EQ(Stored(other).KeyCount, 3u);
}

// A context with no camera builds no row.
TEST_F(ExposureCompensationCurveRowTests, NoCameraNoRow)
{
    const ECS::EntityHandle bare = World.CreateEntity();
    InspectorContext ctx{};
    ctx.Parent = &Root;
    ctx.World = &World;
    ctx.Entity = bare;
    EXPECT_EQ(AddExposureCompensationCurveRow(ctx), nullptr);
}
