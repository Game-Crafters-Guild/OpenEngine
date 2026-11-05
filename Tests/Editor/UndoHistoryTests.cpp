#include "UndoRedo/UndoRedoService.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"
#include "Panels/Script/ScriptTextEditCommand.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>

using GameEngine::Editor::UndoRedoService;

namespace
{
UndoRedoService::SnapshotTarget IntegerTarget(int& value)
{
    UndoRedoService::SnapshotTarget target;
    target.debugLabel = "Value";
    target.Identity = {reinterpret_cast<std::uintptr_t>(&value)};
    target.Capture = [&value](auto& bytes)
    {
        bytes.resize(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        return true;
    };
    target.Apply = [&value](const auto& bytes)
    {
        if (bytes.size() != sizeof(value))
            return false;
        std::memcpy(&value, bytes.data(), sizeof(value));
        return true;
    };
    return target;
}
}

TEST(UndoHistory, ReleasingAGestureLeavesItsValuePending)
{
    UndoRedoService history;
    int value = 0;
    auto edit = history.BeginInteractiveEdit("Value", IntegerTarget(value));
    edit.Preview([&value] { value = 3; });
    edit.Commit();
    EXPECT_TRUE(history.GetHistoryNodes().empty());
    EXPECT_TRUE(history.CanUndo());
    history.Undo();
    EXPECT_EQ(value, 0);
    history.Redo();
    EXPECT_EQ(value, 3);
}

TEST(UndoHistory, StartingAnotherActionCommitsBeforeItsCapture)
{
    UndoRedoService history;
    int value = 0;
    auto edit = history.BeginInteractiveEdit("Position", IntegerTarget(value));
    edit.Preview([&value] { value = 3; });
    edit.Commit();
    auto rotation = history.BeginInteractiveEdit("Rotation", IntegerTarget(value));
    EXPECT_EQ(history.GetUndoCount(), 1u);
    rotation.Preview([&value] { value = 7; });
    rotation.Commit();
    history.Undo();
    EXPECT_EQ(value, 3);
    history.Undo();
    EXPECT_EQ(value, 0);
}

TEST(UndoHistory, RepeatedScrubsShareTheOriginalBaseline)
{
    UndoRedoService history;
    int value = 0;
    for (int gesture = 1; gesture <= 4; ++gesture)
    {
        auto edit = history.BeginInteractiveEdit("Position", IntegerTarget(value));
        for (int sample = 0; sample < 100; ++sample)
            edit.Preview([&value, gesture, sample] { value = gesture * 100 + sample; });
        edit.Commit();
        EXPECT_TRUE(history.GetHistoryNodes().empty());
    }
    history.Undo();
    EXPECT_EQ(value, 0);
    EXPECT_EQ(history.GetHistoryNodes().size(), 1u);
    history.Redo();
    EXPECT_EQ(value, 499);
}

TEST(UndoHistory, ReturningToTheOriginalValueCreatesNoRevision)
{
    UndoRedoService history;
    int value = 5;
    auto first = history.BeginInteractiveEdit("Scale", IntegerTarget(value));
    first.Preview([&value] { value = 10; });
    first.Commit();
    auto second = history.BeginInteractiveEdit("Scale", IntegerTarget(value));
    second.Preview([&value] { value = 5; });
    second.Commit();
    EXPECT_FALSE(history.CanUndo());
    ASSERT_TRUE(history.CommitPendingEdit());
    EXPECT_TRUE(history.GetHistoryNodes().empty());
}

TEST(UndoHistory, CancelRestoresTheWholeLogicalAction)
{
    UndoRedoService history;
    int value = 5;
    auto first = history.BeginInteractiveEdit("Scale", IntegerTarget(value));
    first.Preview([&value] { value = 10; });
    first.Commit();
    auto second = history.BeginInteractiveEdit("Scale", IntegerTarget(value));
    second.Preview([&value] { value = 15; });
    second.Cancel();
    EXPECT_EQ(value, 5);
    EXPECT_FALSE(history.CanUndo());
}

TEST(UndoHistory, StartingAnotherActionInvalidatesThePreviousHandle)
{
    UndoRedoService history;
    int value = 0;
    auto first = history.BeginInteractiveEdit("Position", IntegerTarget(value));
    first.Preview([&value] { value = 10; });
    auto second = history.BeginInteractiveEdit("Rotation", IntegerTarget(value));
    EXPECT_FALSE(static_cast<bool>(first));
    first.Preview([&value] { value = 999; });
    first.Cancel();
    EXPECT_EQ(value, 10);
    second.Cancel();
    history.Undo();
    EXPECT_EQ(value, 0);
}

TEST(UndoHistory, AnEditMayOutliveItsService)
{
    UndoRedoService::InteractiveEdit edit;
    int value = 0;
    {
        UndoRedoService history;
        edit = history.BeginInteractiveEdit("Value", IntegerTarget(value));
        edit.Preview([&value] { value = 3; });
    }
    EXPECT_FALSE(static_cast<bool>(edit));
    edit.Cancel();
    EXPECT_EQ(value, 3);
}

namespace
{
void SetValue(UndoRedoService& history, int& value, int after, const char* name)
{
    auto edit = history.BeginInteractiveEdit(name, IntegerTarget(value));
    edit.Preview([&value, after] { value = after; });
    edit.Commit();
    ASSERT_TRUE(history.CommitPendingEdit());
}
}

TEST(UndoHistory, EditingAfterUndoPreservesBothFutures)
{
    UndoRedoService history;
    int value = 0;
    SetValue(history, value, 1, "A");
    const auto fork = history.GetCurrentNodeId();
    SetValue(history, value, 2, "B");
    const auto original = history.GetCurrentNodeId();
    history.Undo();
    SetValue(history, value, 3, "C");
    const auto alternate = history.GetCurrentNodeId();
    EXPECT_EQ(history.GetHistoryNodes().size(), 3u);
    ASSERT_TRUE(history.Checkout(original));
    EXPECT_EQ(value, 2);
    ASSERT_TRUE(history.Checkout(alternate));
    EXPECT_EQ(value, 3);
    ASSERT_TRUE(history.Checkout(fork));
    history.Redo();
    EXPECT_EQ(value, 3);
    history.Redo();
    EXPECT_EQ(value, 3) << "Redo at a leaf must not return to the root";
}

TEST(UndoHistory, ReplayingDoesNotRecordNewCommands)
{
    UndoRedoService history;
    int value = 0;
    auto target = IntegerTarget(value);
    target.Notify = [&history](auto kind)
    {
        if (kind == GameEngine::Editor::EditChangeKind::UndoRedo)
            EXPECT_FALSE(static_cast<bool>(history.BeginInteractiveEdit("Recursive", {})));
    };
    auto edit = history.BeginInteractiveEdit("Value", target);
    edit.Preview([&value] { value = 1; });
    edit.Commit();
    history.Undo();
    history.Redo();
    EXPECT_EQ(history.GetHistoryNodes().size(), 1u);
}

TEST(UndoHistory, FailedUndoDoesNotMoveTheRevision)
{
    UndoRedoService history;
    int value = 0;
    bool refuse = false;
    auto target = IntegerTarget(value);
    auto apply = target.Apply;
    target.Apply = [&refuse, apply](const auto& bytes) { return !refuse && apply(bytes); };
    auto edit = history.BeginInteractiveEdit("Value", target);
    edit.Preview([&value] { value = 1; });
    edit.Commit();
    ASSERT_TRUE(history.CommitPendingEdit());
    const auto revision = history.GetCurrentNodeId();
    refuse = true;
    history.Undo();
    EXPECT_EQ(value, 1);
    EXPECT_EQ(history.GetCurrentNodeId(), revision);
    refuse = false;
    history.Undo();
    EXPECT_EQ(value, 0);
}

TEST(UndoHistory, FailedBranchTraversalRestoresTheStartingState)
{
    UndoRedoService history;
    int value = 0;
    bool refuse = false;
    auto target = IntegerTarget(value);
    auto apply = target.Apply;
    target.Apply = [&refuse, apply](const auto& bytes) { return !refuse && apply(bytes); };
    auto edit = history.BeginInteractiveEdit("Old branch", target);
    edit.Preview([&value] { value = 1; });
    edit.Commit();
    ASSERT_TRUE(history.CommitPendingEdit());
    const auto original = history.GetCurrentNodeId();
    history.Undo();
    SetValue(history, value, 2, "New branch");
    SetValue(history, value, 3, "New branch child");
    const auto current = history.GetCurrentNodeId();
    refuse = true;
    EXPECT_FALSE(history.Checkout(original));
    EXPECT_EQ(value, 3);
    EXPECT_EQ(history.GetCurrentNodeId(), current);
}

TEST(UndoHistory, DetachedHistoryPreservesBranchesAndNodeIds)
{
    UndoRedoService history;
    int value = 0;
    SetValue(history, value, 1, "A");
    const auto original = history.GetCurrentNodeId();
    history.Undo();
    SetValue(history, value, 2, "B");
    auto detached = history.DetachHistory();
    EXPECT_TRUE(history.GetHistoryNodes().empty());
    SetValue(history, value, 99, "Play edit");
    history.AttachHistory(std::move(detached));
    value = 2; // Play Mode restores the authored world before attaching its history.
    ASSERT_TRUE(history.Checkout(original));
    EXPECT_EQ(value, 1);
    EXPECT_EQ(history.GetHistoryNodes().size(), 2u);
}

TEST(UndoHistory, RetentionKeepsEveryRemainingRevisionReachable)
{
    UndoRedoService history({3});
    int value = 0;
    SetValue(history, value, 1, "A");
    SetValue(history, value, 2, "B");
    history.Undo();
    SetValue(history, value, 3, "C");
    SetValue(history, value, 4, "D");
    SetValue(history, value, 5, "E");
    EXPECT_EQ(history.GetHistoryNodes().size(), 3u);
    while (history.CanUndo())
        history.Undo();
    EXPECT_EQ(value, 1);
    while (history.CanRedo())
        history.Redo();
    EXPECT_EQ(value, 5);
}

TEST(UndoHistory, ExactAuthoredValuesSurviveUndoAndRedo)
{
    struct Values
    {
        GameEngine::Mathematics::Vector3 Position;
        GameEngine::Mathematics::Quaternion Rotation;
        GameEngine::Mathematics::Vector3 Scale;
        GameEngine::ColorLinear Color;
        std::int32_t Count = 0;
        bool Enabled = false;
    } values{};
    const Values before = values;
    UndoRedoService history;
    UndoRedoService::SnapshotTarget target;
    target.Capture = [&values](auto& bytes)
    {
        bytes.resize(sizeof(values));
        std::memcpy(bytes.data(), &values, sizeof(values));
        return true;
    };
    target.Apply = [&values](const auto& bytes)
    {
        if (bytes.size() != sizeof(values))
            return false;
        std::memcpy(&values, bytes.data(), sizeof(values));
        return true;
    };
    auto edit = history.BeginInteractiveEdit("Compound values", target);
    edit.Preview([&values]
    {
        values.Position = {0.000001f, -2.0f, 10.0f};
        values.Rotation = {0.0f, 0.5f, 0.0f, 0.8660254f};
        values.Scale = {-1.0f, 2.0f, 0.5f};
        values.Color = {2.0f, 0.4f, 0.3f, 0.25f};
        values.Count = 42;
        values.Enabled = true;
    });
    const Values after = values;
    edit.Commit();
    history.Undo();
    EXPECT_EQ(std::memcmp(&values, &before, sizeof(values)), 0);
    history.Redo();
    EXPECT_EQ(std::memcmp(&values, &after, sizeof(values)), 0);
}

TEST(UndoHistory, ConsequencesBelongOnlyToTheirOriginatingAction)
{
    UndoRedoService history;
    int value = 0;
    int dependent = 0;
    auto edit = history.BeginInteractiveEdit("Position", IntegerTarget(value));
    const auto action = history.GetPendingActionId();
    edit.Preview([&value] { value = 1; });
    edit.Commit();
    dependent = 10;
    using GameEngine::Editor::HistorySnapshotCommand;
    auto before = IntegerTarget(dependent);
    UndoRedoService::SnapshotTarget::Snapshot zero(sizeof(dependent), 0);
    UndoRedoService::SnapshotTarget::Snapshot ten;
    ASSERT_TRUE(before.Capture(ten));
    EXPECT_FALSE(history.AppendConsequence(action + 1,
        std::make_unique<HistorySnapshotCommand>("Dependent", before, zero, ten)));
    ASSERT_TRUE(history.AppendConsequence(action,
        std::make_unique<HistorySnapshotCommand>("Dependent", before, zero, ten)));
    history.Undo();
    EXPECT_EQ(value, 0);
    EXPECT_EQ(dependent, 0);
    history.Redo();
    EXPECT_EQ(value, 1);
    EXPECT_EQ(dependent, 10);
    EXPECT_EQ(history.GetHistoryNodes().size(), 1u);
}

TEST(UndoHistory, CancelAlsoRestoresAuthoredConsequences)
{
    UndoRedoService history;
    int value = 0;
    int dependent = 0;
    auto edit = history.BeginInteractiveEdit("Position", IntegerTarget(value));
    edit.Preview([&value] { value = 1; });
    dependent = 10;
    auto target = IntegerTarget(dependent);
    UndoRedoService::SnapshotTarget::Snapshot zero(sizeof(dependent), 0);
    UndoRedoService::SnapshotTarget::Snapshot ten;
    ASSERT_TRUE(target.Capture(ten));
    ASSERT_TRUE(history.AppendConsequence(history.GetPendingActionId(),
        std::make_unique<GameEngine::Editor::HistorySnapshotCommand>("Dependent", target, zero, ten)));
    edit.Cancel();
    EXPECT_EQ(value, 0);
    EXPECT_EQ(dependent, 0);
    EXPECT_FALSE(history.CanUndo());
}

TEST(UndoHistory, ObserversSeeOnlyTheCompletedTraversal)
{
    UndoRedoService history;
    int first = 0;
    int second = 0;
    auto target = IntegerTarget(first);
    target.Notify = [&first, &second](auto kind)
    {
        if (kind == GameEngine::Editor::EditChangeKind::UndoRedo)
            EXPECT_EQ(first, second);
    };
    auto edit = history.BeginInteractiveEdit("First", target);
    edit.Preview([&first] { first = 1; });
    edit.Commit();
    SetValue(history, second, 1, "Second");
    ASSERT_TRUE(history.Checkout(history.GetRootNodeId()));
    EXPECT_EQ(first, 0);
    EXPECT_EQ(second, 0);
}

TEST(UndoHistory, FailedCaptureRetainsThePendingAction)
{
    UndoRedoService history;
    int value = 0;
    bool refuse = false;
    auto target = IntegerTarget(value);
    const auto capture = target.Capture;
    target.Capture = [&refuse, capture](auto& bytes) { return !refuse && capture(bytes); };
    auto edit = history.BeginInteractiveEdit("Position", target);
    edit.Preview([&value] { value = 3; });
    refuse = true;
    EXPECT_FALSE(history.CommitPendingEdit());
    EXPECT_EQ(value, 3);
    EXPECT_TRUE(history.GetHistoryNodes().empty());
    EXPECT_FALSE(static_cast<bool>(history.BeginInteractiveEdit("Rotation", IntegerTarget(value))));
    refuse = false;
    edit.Cancel();
    EXPECT_EQ(value, 0);
}

namespace
{
void EditScript(UndoRedoService& history, const std::shared_ptr<GameEngine::Editor::ScriptHistoryDocument>& document,
                const std::string& text)
{
    const std::string before = document->GetText();
    document->SetText(text);
    history.CommitAlreadyApplied(std::make_unique<GameEngine::Editor::ScriptTextEditCommand>(document, before, text));
}
}

TEST(UndoHistory, ScriptHistoryRestoresItsDocumentWithoutAnOpenView)
{
    using GameEngine::Editor::ScriptHistoryDocument;
    UndoRedoService history;
    auto first = ScriptHistoryDocument::Open("history-first.cs", "first");
    auto second = ScriptHistoryDocument::Open("history-second.cs", "second");
    EditScript(history, first, "first edited");
    EditScript(history, second, "second edited");
    history.Undo();
    EXPECT_EQ(second->GetText(), "second");
    history.Undo();
    EXPECT_EQ(first->GetText(), "first");
    history.Redo();
    EXPECT_EQ(first->GetText(), "first edited");
    EXPECT_EQ(second->GetText(), "second");
    auto reopened = ScriptHistoryDocument::Open("history-first.cs", "first");
    EXPECT_EQ(reopened, first);
    EXPECT_EQ(reopened->GetText(), "first edited");
}

TEST(UndoHistory, ReopeningACleanScriptRetainedByHistoryLoadsExternalChanges)
{
    using GameEngine::Editor::ScriptHistoryDocument;
    UndoRedoService history;
    auto document = ScriptHistoryDocument::Open("history-external-clean.cs", "original");
    EditScript(history, document, "saved");
    ASSERT_TRUE(history.CommitPendingEdit());
    document->MarkSaved();
    int changes = 0;
    auto subscription = document->Changed().Subscribe([&changes] { ++changes; });
    std::weak_ptr<ScriptHistoryDocument> retained = document;
    document.reset();

    auto reopened = ScriptHistoryDocument::Open("history-external-clean.cs", "external");
    EXPECT_EQ(reopened, retained.lock());
    EXPECT_EQ(reopened->GetText(), "external");
    EXPECT_EQ(reopened->GetSavedText(), "external");
    EXPECT_FALSE(reopened->IsDirty());
    EXPECT_EQ(changes, 1);
}

TEST(UndoHistory, ReopeningADirtyScriptPreservesEditsAndTracksTheExternalDiskVersion)
{
    using GameEngine::Editor::ScriptHistoryDocument;
    UndoRedoService history;
    auto document = ScriptHistoryDocument::Open("history-external-dirty.cs", "original");
    EditScript(history, document, "local");
    ASSERT_TRUE(history.CommitPendingEdit());

    auto reopened = ScriptHistoryDocument::Open("history-external-dirty.cs", "external");
    EXPECT_EQ(reopened, document);
    EXPECT_EQ(reopened->GetText(), "local");
    EXPECT_EQ(reopened->GetSavedText(), "external");
    EXPECT_TRUE(reopened->IsDirty());
    history.Undo();
    EXPECT_TRUE(reopened->IsDirty());
    EXPECT_EQ(reopened->GetText(), "original");
}

TEST(UndoHistory, ReopeningAScriptWhoseEditsMatchDiskMarksItClean)
{
    using GameEngine::Editor::ScriptHistoryDocument;
    UndoRedoService history;
    auto document = ScriptHistoryDocument::Open("history-external-matching.cs", "original");
    EditScript(history, document, "matching");
    auto reopened = ScriptHistoryDocument::Open("history-external-matching.cs", "matching");
    EXPECT_EQ(reopened, document);
    EXPECT_EQ(reopened->GetSavedText(), "matching");
    EXPECT_FALSE(reopened->IsDirty());
}

TEST(UndoHistory, ReopeningACleanScriptAcceptsAnEmptyDiskFile)
{
    using GameEngine::Editor::ScriptHistoryDocument;
    auto document = ScriptHistoryDocument::Open("history-external-empty.cs", "original");
    auto reopened = ScriptHistoryDocument::Open("history-external-empty.cs", "");
    EXPECT_EQ(reopened, document);
    EXPECT_TRUE(reopened->GetText().empty());
    EXPECT_TRUE(reopened->GetSavedText().empty());
    EXPECT_FALSE(reopened->IsDirty());
}

TEST(UndoHistory, ScriptTypingDoesNotMutateAnAlreadyCommittedNode)
{
    UndoRedoService history;
    auto document = GameEngine::Editor::ScriptHistoryDocument::Open("history-immutable.cs", "a");
    EditScript(history, document, "ab");
    ASSERT_TRUE(history.CommitPendingEdit());
    const auto first = history.GetCurrentNodeId();
    EditScript(history, document, "abc");
    history.Undo();
    EXPECT_EQ(document->GetText(), "ab");
    EXPECT_EQ(history.GetCurrentNodeId(), first);
    history.Undo();
    EXPECT_EQ(document->GetText(), "a");
}

TEST(UndoHistory, ScriptTypingBackToTheOriginalTextCreatesNoNode)
{
    UndoRedoService history;
    auto document = GameEngine::Editor::ScriptHistoryDocument::Open("history-noop.cs", "a");
    EditScript(history, document, "ab");
    EditScript(history, document, "a");
    EXPECT_FALSE(history.CanUndo());
    ASSERT_TRUE(history.CommitPendingEdit());
    EXPECT_TRUE(history.GetHistoryNodes().empty());
}

TEST(UndoHistory, SavedRevisionsFollowTheirDetachedDocumentHistory)
{
    UndoRedoService history;
    int value = 0;
    int document = 0;
    history.MarkSaved(&document);
    SetValue(history, value, 1, "Position");
    EXPECT_FALSE(history.IsAtSavedRevision(&document));
    auto authored = history.DetachHistory();
    SetValue(history, value, 99, "Play edit");
    history.MarkSaved(&document);
    history.AttachHistory(std::move(authored));
    value = 1;
    EXPECT_FALSE(history.IsAtSavedRevision(&document));
    history.Undo();
    EXPECT_EQ(value, 0);
    EXPECT_TRUE(history.IsAtSavedRevision(&document));
}

TEST(UndoHistory, ClearingHistoryDoesNotSavePendingValues)
{
    UndoRedoService history;
    int value = 0;
    int document = 0;
    history.MarkSaved(&document);
    auto edit = history.BeginInteractiveEdit("Position", IntegerTarget(value));
    edit.Preview([&value] { value = 1; });
    edit.Commit();
    history.Clear();
    EXPECT_EQ(value, 1);
    EXPECT_FALSE(history.IsAtSavedRevision(&document));
    EXPECT_FALSE(history.CanUndo());
    history.MarkSaved(&document);
    history.Clear();
    EXPECT_TRUE(history.IsAtSavedRevision(&document));
}

TEST(UndoHistory, ChangeDescriptionsKeepTheirCommittedValues)
{
    UndoRedoService history;
    int value = 1;
    auto target = IntegerTarget(value);
    target.Describe = [](const auto& before, const auto& after)
    {
        int oldValue = 0;
        int newValue = 0;
        std::memcpy(&oldValue, before.data(), sizeof(oldValue));
        std::memcpy(&newValue, after.data(), sizeof(newValue));
        return std::to_string(oldValue) + " -> " + std::to_string(newValue);
    };
    auto edit = history.BeginInteractiveEdit("Value", target);
    edit.Preview([&value] { value = 3; });
    edit.Commit();
    ASSERT_TRUE(history.CommitPendingEdit());
    value = 8;
    EXPECT_EQ(history.GetHistoryNodes().front().GetDescription(), "1 -> 3");
    history.Undo();
    EXPECT_EQ(value, 1);
    history.Redo();
    EXPECT_EQ(value, 3);
    EXPECT_EQ(history.GetHistoryNodes().front().GetDescription(), "1 -> 3");
}
