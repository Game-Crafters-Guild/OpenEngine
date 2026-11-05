#include <gtest/gtest.h>

#include "EditorChangeNotifications.h"
#include "Scene/SceneDocumentManager.h"
#include "UndoRedo/Spline/SplineUndoHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
class SceneDocumentHistoryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
        auto& splines = SplineECS::SplineService::Get();
        m_Spline = splines.CreateSpline(Spline::SplineType::Linear, false);
        auto* data = splines.GetSplineData(m_Spline);
        data->AddPoint({0.0f, 0.0f, 0.0f});
        data->AddPoint({10.0f, 0.0f, 0.0f});
        splines.RebuildCache(m_Spline);
        m_Entity = m_World.CreateEntity();
        Components::SplineComponent component{};
        component.SplineDataIndex = m_Spline.Index();
        component.SplineDataGeneration = m_Spline.Generation();
        m_World.AddComponentImmediate(m_Entity, component);
        m_Document.SetWorld(&m_World);
        m_Document.SetUndoRedoService(&m_History);
        m_Document.SetChangeNotifications(&m_Notifications);
        m_Document.MarkClean();
    }

    void TearDown() override
    {
        m_Document.SetChangeNotifications(nullptr);
        SplineECS::SplineService::Get().DestroySpline(m_Spline);
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
    }

    void MovePoint(float x)
    {
        SplineUndo::CommitSplineDataOneShot(&SplineECS::SplineService::Get(), m_Spline, &m_World,
            m_Entity, &m_Notifications, &m_History, "Move Spline Point",
            [x](Spline::SplineData* data) { data->SetPointPosition(0, {x, 0.0f, 0.0f}); });
    }

    void RemoveNameOutsideHistory()
    {
        m_World.RemoveComponentImmediate<Components::Name>(m_Entity);
        m_Document.MarkDirty();
    }

    void ExecuteMoveWithDirtyCallback(UndoRedoService& history)
    {
        auto target = SplineUndo::MakeSplineEditableSnapshotTarget(&SplineECS::SplineService::Get(),
            m_Spline, &m_World, m_Entity, nullptr, "Move Spline Point");
        target.Notify = [this](EditChangeKind) { m_Document.MarkDirty(); };
        HistorySnapshotTarget::Snapshot before;
        HistorySnapshotTarget::Snapshot after;
        ASSERT_TRUE(target.Capture(before));
        SplineECS::SplineService::Get().GetSplineData(m_Spline)->SetPointPosition(0, {3.0f, 0.0f, 0.0f});
        ASSERT_TRUE(target.Capture(after));
        ASSERT_TRUE(target.Apply(before));
        history.Execute(std::make_unique<HistorySnapshotCommand>("Move Spline Point", target, before, after));
    }

    ECS::World m_World;
    ECS::EntityHandle m_Entity;
    SplineECS::SplineHandle m_Spline;
    EditorChangeNotifications m_Notifications;
    UndoRedoService m_History;
    SceneDocumentManager m_Document;

private:
    bool m_OwnsSplines = false;
};

TEST_F(SceneDocumentHistoryTest, SplineRedoStaysDirtyWhenComponentBytesMatchTheSavedState)
{
    std::vector<std::uint8_t> savedComponent;
    const auto type = ECS::GetComponentTypeId<Components::SplineComponent>();
    ASSERT_TRUE(m_World.CaptureComponentBytes(m_Entity, type, savedComponent));
    MovePoint(3.0f);
    EXPECT_TRUE(m_Document.IsDirty());
    ASSERT_TRUE(m_History.CommitPendingEdit());
    const auto firstRevision = m_History.GetCurrentNodeId();

    m_History.Undo();
    EXPECT_FALSE(m_Document.IsDirty());
    m_History.Redo();
    EXPECT_TRUE(m_Document.IsDirty());
    EXPECT_FLOAT_EQ(SplineECS::SplineService::Get().GetSplineData(m_Spline)->Points[0].Position.x, 3.0f);
    std::vector<std::uint8_t> editedComponent;
    ASSERT_TRUE(m_World.CaptureComponentBytes(m_Entity, type, editedComponent));
    EXPECT_EQ(editedComponent, savedComponent);

    m_History.Undo();
    MovePoint(7.0f);
    ASSERT_TRUE(m_History.CommitPendingEdit());
    ASSERT_TRUE(m_History.Checkout(firstRevision));
    EXPECT_TRUE(m_Document.IsDirty());
    EXPECT_FLOAT_EQ(SplineECS::SplineService::Get().GetSplineData(m_Spline)->Points[0].Position.x, 3.0f);
}

TEST_F(SceneDocumentHistoryTest, UndoingASavedSplineEditIsDirtyAndRedoRestoresTheSavedState)
{
    MovePoint(3.0f);
    ASSERT_TRUE(m_History.CommitPendingEdit());
    m_Document.MarkClean();
    EXPECT_FALSE(m_Document.IsDirty());

    m_History.Undo();
    EXPECT_TRUE(m_Document.IsDirty());
    EXPECT_FLOAT_EQ(SplineECS::SplineService::Get().GetSplineData(m_Spline)->Points[0].Position.x, 0.0f);
    m_History.Redo();
    EXPECT_FALSE(m_Document.IsDirty());
    EXPECT_FLOAT_EQ(SplineECS::SplineService::Get().GetSplineData(m_Spline)->Points[0].Position.x, 3.0f);
}

TEST_F(SceneDocumentHistoryTest, ReplayWithoutHistoryCannotProveTheSceneIsClean)
{
    m_Document.SetUndoRedoService(nullptr);
    SplineECS::SplineService::Get().GetSplineData(m_Spline)->SetPointPosition(0, {3.0f, 0.0f, 0.0f});
    m_Notifications.NotifyComponentChange<Components::SplineComponent>(
        &m_World, m_Entity, EditChangeKind::UndoRedo);
    EXPECT_TRUE(m_Document.IsDirty());
}

TEST_F(SceneDocumentHistoryTest, UntrackedRemovalStaysDirtyAfterUndoingALaterRecordedEdit)
{
    m_World.AddComponentImmediate(m_Entity, Components::Name{});
    m_Document.MarkClean();
    RemoveNameOutsideHistory();
    MovePoint(3.0f);
    m_History.Undo();
    EXPECT_FALSE(m_World.HasComponent<Components::Name>(m_Entity));
    EXPECT_FLOAT_EQ(SplineECS::SplineService::Get().GetSplineData(m_Spline)->Points[0].Position.x, 0.0f);
    EXPECT_TRUE(m_Document.IsDirty());
    m_History.Redo();
    EXPECT_TRUE(m_Document.IsDirty());
    m_History.Undo();
    EXPECT_TRUE(m_Document.IsDirty());
}

TEST_F(SceneDocumentHistoryTest, AnUntrackedEditDoesNotJoinAnEndedPendingAction)
{
    m_World.AddComponentImmediate(m_Entity, Components::Name{});
    m_Document.MarkClean();
    MovePoint(3.0f);
    ASSERT_TRUE(m_History.HasPendingEdit());
    RemoveNameOutsideHistory();
    m_History.Undo();
    EXPECT_FALSE(m_World.HasComponent<Components::Name>(m_Entity));
    EXPECT_TRUE(m_Document.IsDirty());
}

TEST_F(SceneDocumentHistoryTest, BranchCheckoutCannotClearUntrackedChanges)
{
    MovePoint(3.0f);
    ASSERT_TRUE(m_History.CommitPendingEdit());
    const auto first = m_History.GetCurrentNodeId();
    m_History.Undo();
    m_Document.MarkDirty();
    MovePoint(7.0f);
    ASSERT_TRUE(m_History.CommitPendingEdit());
    ASSERT_TRUE(m_History.Checkout(first));
    EXPECT_TRUE(m_Document.IsDirty());
    ASSERT_TRUE(m_History.Checkout(m_History.GetRootNodeId()));
    EXPECT_TRUE(m_Document.IsDirty());
}

TEST_F(SceneDocumentHistoryTest, MarkCleanClearsUntrackedDirtinessForLaterHistoryEdits)
{
    m_Document.MarkDirty();
    m_Document.MarkClean();
    EXPECT_FALSE(m_Document.IsDirty());
    MovePoint(3.0f);
    m_History.Undo();
    EXPECT_FALSE(m_Document.IsDirty());
    m_Document.MarkDirty();
    MovePoint(7.0f);
    m_History.Undo();
    EXPECT_TRUE(m_Document.IsDirty());
}

TEST_F(SceneDocumentHistoryTest, UntrackedStructureNotificationsRemainDirtyAfterReplay)
{
    m_World.AddComponentImmediate(m_Entity, Components::Name{});
    m_Document.MarkClean();
    m_World.RemoveComponentImmediate<Components::Name>(m_Entity);
    m_Notifications.NotifyWorldStructureChanged({&m_World, EditChangeKind::Commit});
    MovePoint(3.0f);
    m_History.Undo();
    EXPECT_TRUE(m_Document.IsDirty());
}

TEST_F(SceneDocumentHistoryTest, RecordedCommandsCanUseTheDirectDirtyCallbackAndUndoCleanly)
{
    ExecuteMoveWithDirtyCallback(m_History);
    EXPECT_TRUE(m_Document.IsDirty());
    m_History.Undo();
    EXPECT_FALSE(m_Document.IsDirty());
    m_History.Redo();
    EXPECT_TRUE(m_Document.IsDirty());
}

TEST_F(SceneDocumentHistoryTest, ACommandFromAnotherHistoryCannotOwnThisDocumentsDirtyState)
{
    UndoRedoService otherHistory;
    ExecuteMoveWithDirtyCallback(otherHistory);
    MovePoint(7.0f);
    m_History.Undo();
    EXPECT_TRUE(m_Document.IsDirty());
}

TEST_F(SceneDocumentHistoryTest, CancellingAPreviewPreservesOnlyTheUntrackedDirtiness)
{
    auto target = SplineUndo::MakeSplineEditableSnapshotTarget(&SplineECS::SplineService::Get(),
        m_Spline, &m_World, m_Entity, &m_Notifications, "Move Spline Point");
    m_Document.MarkDirty();
    auto first = m_History.BeginInteractiveEdit("Move Spline Point", target);
    first.Preview([this]
    {
        SplineECS::SplineService::Get().GetSplineData(m_Spline)->SetPointPosition(0, {3.0f, 0.0f, 0.0f});
    });
    first.Cancel();
    EXPECT_TRUE(m_Document.IsDirty());
    m_Document.MarkClean();
    auto second = m_History.BeginInteractiveEdit("Move Spline Point", target);
    second.Preview([this]
    {
        SplineECS::SplineService::Get().GetSplineData(m_Spline)->SetPointPosition(0, {7.0f, 0.0f, 0.0f});
    });
    second.Cancel();
    EXPECT_FALSE(m_Document.IsDirty());
}

TEST_F(SceneDocumentHistoryTest, CommittingAPreviewAtItsStartingValueLeavesTheSceneClean)
{
    auto target = SplineUndo::MakeSplineEditableSnapshotTarget(&SplineECS::SplineService::Get(),
        m_Spline, &m_World, m_Entity, &m_Notifications, "Move Spline Point");
    auto edit = m_History.BeginInteractiveEdit("Move Spline Point", target);
    edit.Preview([this]
    {
        SplineECS::SplineService::Get().GetSplineData(m_Spline)->SetPointPosition(0, {3.0f, 0.0f, 0.0f});
    });
    edit.Preview([this]
    {
        SplineECS::SplineService::Get().GetSplineData(m_Spline)->SetPointPosition(0, {0.0f, 0.0f, 0.0f});
    });
    edit.Commit();
    EXPECT_FALSE(m_Document.IsDirty());
}
} // namespace
} // namespace GameEngine::Editor
