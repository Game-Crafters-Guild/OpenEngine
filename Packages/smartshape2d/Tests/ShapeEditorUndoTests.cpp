#include "CreateSmartShapeCommand.h"
#include "Components/Spline/SplineComponent.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "SplineECS/SplineService.h"
#include "UndoRedo/SplineUndoHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeEditorUndo, PointEditRedoFollowsTheSplineRecreatedByCreationRedo)
{
    if (!SplineECS::SplineService::IsInitialized())
        SplineECS::SplineService::Initialize();
    auto* service = SplineECS::SplineService::TryGet();
    for (const auto kind : {ShapeCreationKind::Closed, ShapeCreationKind::Open})
    {
        ECS::World world;
        Editor::UndoRedoService undo;
        ECS::EntityHandle owner;
        Editor::HierarchyCommandContext context;
        context.World = &world;
        int selections = 0;
        context.SelectEntity = [&selections](ECS::EntityHandle) { ++selections; };
        context.NotifyEntityCreated = [&owner](ECS::EntityHandle entity) { owner = entity; };
        undo.Execute(std::make_unique<CreateSmartShapeCommand>(context, kind));
        const auto reference = *world.GetComponent<Components::SplineComponent>(owner);
        const SplineECS::SplineHandle original{reference.SplineDataIndex, reference.SplineDataGeneration};
        Editor::SplineUndo::CommitSplineDataOneShot(service, original, &world, owner, nullptr, &undo,
            "Move Smart Shape Point", [&world, owner](Spline::SplineData* data)
            {
                data->SetPointPosition(0, {5, 6, 0});
                auto* settings = world.GetComponentForWrite<Components::SplineComponent>(owner);
                settings->DefaultRadius = 3;
                settings->ShowWidthEnvelope = true;
            });
        undo.Undo();
        undo.Undo();
        EXPECT_FALSE(world.IsValid(owner));
        undo.Redo();
        ASSERT_TRUE(world.IsValid(owner));
        const auto recreated = *world.GetComponent<Components::SplineComponent>(owner);
        const SplineECS::SplineHandle current{recreated.SplineDataIndex, recreated.SplineDataGeneration};
        ASSERT_NE(current, original);
        undo.Redo();
        auto* data = service->GetSplineData(current);
        ASSERT_NE(data, nullptr);
        EXPECT_FLOAT_EQ(data->Points[0].Position.x, 5);
        EXPECT_FLOAT_EQ(data->Points[0].Position.y, 6);
        EXPECT_FLOAT_EQ(world.GetComponent<Components::SplineComponent>(owner)->DefaultRadius, 3);
        EXPECT_TRUE(world.GetComponent<Components::SplineComponent>(owner)->ShowWidthEnvelope);
        undo.Undo();
        EXPECT_FLOAT_EQ(world.GetComponent<Components::SplineComponent>(owner)->DefaultRadius, 1);
        EXPECT_FALSE(world.GetComponent<Components::SplineComponent>(owner)->ShowWidthEnvelope);
        EXPECT_FLOAT_EQ(data->Points[0].Position.x, -2);
        EXPECT_EQ(selections, 0) << "Selection belongs to the plugin compound, never command replay";
        service->DestroySpline(current);
    }
}

TEST(SmartShapeEditorUndo, MissingOwnerRefusesSplineSnapshotRestore)
{
    if (!SplineECS::SplineService::IsInitialized())
        SplineECS::SplineService::Initialize();
    auto* service = SplineECS::SplineService::TryGet();
    ECS::World world;
    const auto entity = world.CreateEntity();
    const auto handle = service->CreateSpline();
    service->GetSplineData(handle)->AddPoint({1, 2, 0}, 1);
    Components::SplineComponent reference;
    reference.SplineDataIndex = handle.Index();
    reference.SplineDataGeneration = handle.Generation();
    world.AddComponentImmediate(entity, reference);
    auto target = Editor::SplineUndo::MakeSplineEditableSnapshotTarget(service, handle, &world, entity, nullptr, "Edit");
    Editor::UndoRedoService::SnapshotTarget::Snapshot snapshot;
    ASSERT_TRUE(target.Capture(snapshot));
    world.DestroyEntityImmediate(entity);
    EXPECT_FALSE(target.Apply(snapshot));
    service->DestroySpline(handle);
}
}
