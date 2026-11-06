#pragma once

#include "Core/CpuProfiler.h"
#include "Events/Event.h"
#include "Types/FlatMap.h"
#include "UndoRedo/History/CompoundHistoryCommand.h"
#include "UndoRedo/History/HistoryGraph.h"
#include "UndoRedo/History/HistoryMutationScope.h"
#include "UndoRedo/History/HistoryReplayScope.h"
#include "UndoRedo/History/HistorySnapshotCommand.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
/// One pending logical edit followed by immutable, branch-preserving revisions.
class UndoRedoService
{
    struct PendingEdit
    {
        std::uint64_t Id = 0;
        std::string Name;
        HistorySnapshotTarget Target;
        HistorySnapshotTarget::Snapshot Before;
        HistorySnapshotTarget::Snapshot After;
        bool Active = true;
        bool GestureOpen = true;
        bool Previewed = false;
        std::vector<std::unique_ptr<IEditorCommand>> Consequences;
    };

public:
    using SnapshotTarget = HistorySnapshotTarget;
    using NodeId = HistoryGraph::NodeId;
    struct Config { std::size_t maxUndo = 1024; };
    struct History
    {
        HistoryGraph Graph;
        FlatMap<const void*, NodeId> SavedRevisions;
    };

    UndoRedoService() = default;
    explicit UndoRedoService(Config config) : m_Config(config) {}
    UndoRedoService(const UndoRedoService&) = delete;
    UndoRedoService& operator=(const UndoRedoService&) = delete;
    ~UndoRedoService()
    {
        if (m_Pending)
            m_Pending->Active = false;
    }

    bool CanUndo() const { return HasPendingCommand() || HasPendingEdit() || m_Graph.GetCurrent() != m_Graph.GetRoot(); }
    bool CanRedo() const { return !HasPendingCommand() && !HasPendingEdit() && !m_Graph.GetFuture().empty(); }
    // Includes an ended, unpublished logical action. GetHistoryNodes contains only committed revisions.
    std::size_t GetUndoCount() const { return m_Graph.GetAncestors().size() + (HasPendingStep() ? 1u : 0u); }
    std::size_t GetRedoCount() const { return m_Graph.GetFuture().size(); }
    NodeId GetCurrentNodeId() const { return m_Graph.GetCurrent(); }
    NodeId GetRootNodeId() const { return m_Graph.GetRoot(); }
    const std::vector<HistoryGraph::Node>& GetHistoryNodes() const { return m_Graph.GetNodes(); }
    const std::string& GetLastError() const { return m_LastError; }
    void MarkSaved(const void* document) { m_SavedRevisions.InsertOrAssign(document, GetCurrentNodeId()); }
    bool IsAtSavedRevision(const void* document) const
    {
        const NodeId* saved = m_SavedRevisions.Find(document);
        return saved && *saved == GetCurrentNodeId() && !HasPendingEdit() && !HasPendingCommand();
    }
    bool HasPendingEdit() const
    {
        return m_Pending && m_Pending->Active && (m_Pending->Previewed || !m_Pending->Consequences.empty());
    }
    bool HasOpenInteractiveEdit() const { return m_Pending && m_Pending->Active && m_Pending->GestureOpen; }
    const char* GetPendingName() const
    {
        return HasPendingCommand() ? m_PendingCommand->GetName() : HasPendingEdit() ? m_Pending->Name.c_str() : nullptr;
    }
    std::string GetPendingDescription() const
    {
        if (HasPendingCommand())
            return m_PendingCommand->GetDescription();
        if (!HasPendingEdit() || m_Pending->GestureOpen)
            return {};
        std::string result = m_Pending->Target.Describe
            ? m_Pending->Target.Describe(m_Pending->Before, m_Pending->After) : std::string{};
        for (const auto& command : m_Pending->Consequences)
        {
            const std::string description = command->GetDescription();
            if (description.empty())
                continue;
            if (!result.empty())
                result += '\n';
            result += description;
        }
        return result;
    }
    NodeId GetPendingActionId() const { return m_Pending ? m_Pending->Id : m_PendingCommandId; }
    void SetBeforeCommit(std::function<void()> callback) { m_BeforeCommit = std::move(callback); }
    bool AppendConsequence(NodeId action, std::unique_ptr<IEditorCommand> command)
    {
        if (!command || action == 0 || action != GetPendingActionId() || m_Replaying)
            return false;
        if (m_Pending)
            m_Pending->Consequences.push_back(std::move(command));
        else
        {
            auto compound = std::make_unique<CompoundHistoryCommand>(m_PendingCommand->GetName());
            compound->Add(std::move(m_PendingCommand));
            compound->Add(std::move(command));
            m_PendingCommand = std::move(compound);
        }
        return true;
    }
    const char* PeekUndoName() const
    {
        if (const char* pending = GetPendingName())
            return pending;
        const auto* node = m_Graph.Find(m_Graph.GetCurrent());
        return node ? node->GetName() : nullptr;
    }
    const char* PeekRedoName() const { return GetRedoNameAt(0); }
    NodeId GetUndoEntryIdAt(std::size_t index) const
    {
        const auto path = m_Graph.GetAncestors();
        return index < path.size() ? path[index] : index == path.size() && HasPendingStep() ? GetPendingActionId() : 0;
    }
    NodeId GetRedoEntryIdAt(std::size_t index) const
    {
        const auto path = m_Graph.GetFuture();
        return index < path.size() ? path[index] : 0;
    }
    const char* GetUndoNameAt(std::size_t index) const
    {
        const auto* node = m_Graph.Find(GetUndoEntryIdAt(index));
        return node ? node->GetName() : index == m_Graph.GetAncestors().size() ? GetPendingName() : nullptr;
    }
    const char* GetRedoNameAt(std::size_t index) const
    {
        const auto* node = m_Graph.Find(GetRedoEntryIdAt(index));
        return node ? node->GetName() : nullptr;
    }
    const char* GetUndoTypeNameAt(std::size_t index) const
    {
        const auto* node = m_Graph.Find(GetUndoEntryIdAt(index));
        return node ? node->GetTypeName() : index == m_Graph.GetAncestors().size() && HasPendingStep() ? "Pending" : nullptr;
    }
    const char* GetRedoTypeNameAt(std::size_t index) const
    {
        const auto* node = m_Graph.Find(GetRedoEntryIdAt(index));
        return node ? node->GetTypeName() : nullptr;
    }
    std::chrono::system_clock::time_point GetUndoTimestampAt(std::size_t index) const
    {
        const auto* node = m_Graph.Find(GetUndoEntryIdAt(index));
        return node ? node->Timestamp : std::chrono::system_clock::time_point{};
    }
    void SetOnHistoryChanged(std::function<void()> callback) { m_OnHistoryChanged = std::move(callback); }
    Event<>& HistoryChanged() { return m_HistoryChanged; }
    void SetBeforeMutation(std::function<void()> callback) { m_BeforeMutation = std::move(callback); }

    /// Resolve the logical action before saving, switching documents or starting a discrete operation.
    bool CommitPendingEdit()
    {
        m_LastError.clear();
        if (m_BeforeCommit && !m_Settling && GetPendingActionId() != 0)
        {
            const HistoryMutationScope mutationScope(this);
            m_Settling = true;
            m_BeforeCommit();
            m_Settling = false;
        }
        if (m_PendingCommand)
        {
            auto command = std::move(m_PendingCommand);
            const NodeId id = std::exchange(m_PendingCommandId, 0);
            if (!command->IsNoOp())
                CommitInternal(std::move(command), id);
        }
        if (!m_Pending)
            return true;
        const auto edit = m_Pending;
        if (edit->GestureOpen && !CaptureAfter(edit))
            return false;
        m_Pending.reset();
        edit->Active = false;
        if (edit->Before != edit->After || !edit->Consequences.empty())
        {
            std::unique_ptr<IEditorCommand> command = std::make_unique<HistorySnapshotCommand>(
                edit->Name, edit->Target, std::move(edit->Before), std::move(edit->After));
            if (!edit->Consequences.empty())
            {
                auto compound = std::make_unique<CompoundHistoryCommand>(edit->Name);
                compound->Add(std::move(command));
                for (auto& consequence : edit->Consequences)
                    compound->Add(std::move(consequence));
                command = std::move(compound);
            }
            CommitInternal(std::move(command), edit->Id);
        }
        NotifyHistoryChanged();
        return true;
    }
    History DetachHistory()
    {
        CommitPendingEdit();
        History result{std::move(m_Graph), std::move(m_SavedRevisions)};
        m_Graph = {};
        m_SavedRevisions = {};
        m_CompoundStack.clear();
        NotifyHistoryChanged();
        return result;
    }
    void AttachHistory(History&& history)
    {
        Clear();
        m_Graph = std::move(history.Graph);
        m_SavedRevisions = std::move(history.SavedRevisions);
        const auto& nodes = m_Graph.GetNodes();
        const NodeId greatest = nodes.empty() ? m_Graph.GetRoot() : nodes.back().Id;
        m_NextEntryId = std::max(m_NextEntryId, greatest + 1);
        NotifyHistoryChanged();
    }
    void Clear()
    {
        m_LastError.clear();
        const bool pending = HasPendingEdit() || HasPendingCommand();
        for (auto& [document, saved] : m_SavedRevisions)
        {
            (void)document;
            saved = !pending && saved == GetCurrentNodeId() ? 0 : std::numeric_limits<NodeId>::max();
        }
        DiscardPendingEdit();
        m_Graph = {};
        m_CompoundStack.clear();
        NotifyHistoryChanged();
    }
    /// Discard an unavailable edit only when its live world is about to be replaced.
    void DiscardPendingEdit()
    {
        if (m_Pending)
            m_Pending->Active = false;
        m_Pending.reset();
        m_PendingCommand.reset();
        m_PendingCommandId = 0;
    }
    void Execute(std::unique_ptr<IEditorCommand> command)
    {
        if (!command || m_Replaying || !CommitPendingEdit())
            return;
        FireBeforeMutation();
        m_PendingCommandId = m_NextEntryId++;
        m_PendingCommand = std::move(command);
        {
            const HistoryMutationScope mutationScope(this);
            m_PendingCommand->Do();
        }
        CommitPendingEdit();
    }
    void CommitAlreadyApplied(std::unique_ptr<IEditorCommand> command)
    {
        if (!command || m_Replaying)
            return;
        if (m_PendingCommand && m_PendingCommand->CanMergeWith(*command) && m_PendingCommand->MergeWith(*command))
        {
            NotifyHistoryChanged();
            return;
        }
        if (!CommitPendingEdit())
            return;
        FireBeforeMutation();
        m_PendingCommandId = m_NextEntryId++;
        const bool continuous = command->IsContinuous();
        m_PendingCommand = std::move(command);
        if (!continuous)
            CommitPendingEdit();
        else
            NotifyHistoryChanged();
    }
    bool Checkout(NodeId destination)
    {
        if (m_Replaying || !CommitPendingEdit())
            return false;
        FireBeforeMutation();
        const HistoryReplayScope replayScope;
        const HistoryMutationScope mutationScope(this, HistoryMutationScope::Phase::Restore);
        m_Replaying = true;
        const bool success = m_Graph.Checkout(destination);
        m_Replaying = false;
        if (!success)
            m_LastError = "Could not restore that revision. Its targets are unavailable.";
        NotifyHistoryChanged();
        return success;
    }
    void Undo()
    {
        if (m_Replaying || !CommitPendingEdit())
            return;
        FireBeforeMutation();
        const HistoryReplayScope replayScope;
        const HistoryMutationScope mutationScope(this, HistoryMutationScope::Phase::Restore);
        m_Replaying = true;
        if (!m_Graph.Undo() && CanUndo())
            m_LastError = "Undo failed. The current revision was kept.";
        m_Replaying = false;
        NotifyHistoryChanged();
    }
    void Redo()
    {
        if (m_Replaying || !CommitPendingEdit())
            return;
        FireBeforeMutation();
        const HistoryReplayScope replayScope;
        const HistoryMutationScope mutationScope(this, HistoryMutationScope::Phase::Restore);
        m_Replaying = true;
        if (!m_Graph.Redo() && CanRedo())
            m_LastError = "Redo failed. The current revision was kept.";
        m_Replaying = false;
        NotifyHistoryChanged();
    }
    void BeginCompound(std::string name)
    {
        CommitPendingEdit();
        m_CompoundStack.push_back(std::make_unique<CompoundHistoryCommand>(std::move(name)));
    }
    void EndCompound()
    {
        CommitPendingEdit();
        if (m_CompoundStack.empty())
            return;
        auto finished = std::move(m_CompoundStack.back());
        m_CompoundStack.pop_back();
        if (!finished->Empty())
            CommitInternal(std::move(finished));
    }

    class InteractiveEdit
    {
    public:
        InteractiveEdit() = default;
        InteractiveEdit(const InteractiveEdit&) = delete;
        InteractiveEdit& operator=(const InteractiveEdit&) = delete;
        InteractiveEdit(InteractiveEdit&& other) noexcept { *this = std::move(other); }
        InteractiveEdit& operator=(InteractiveEdit&& other) noexcept
        {
            if (this != &other)
            {
                Cancel();
                m_Service = other.m_Service;
                m_Edit = std::move(other.m_Edit);
                other.m_Service = nullptr;
            }
            return *this;
        }
        ~InteractiveEdit() { Cancel(); }
        explicit operator bool() const { return m_Edit && m_Edit->Active && m_Edit->GestureOpen; }
        template<typename ApplyFn>
        void Preview(ApplyFn&& apply)
        {
            GE_CPU_PROFILE_SCOPE("UndoRedo.InteractiveEdit.Preview");
            if (!*this)
                return;
            const auto edit = m_Edit;
            UndoRedoService* service = m_Service;
            if (edit->Target.Validate && !edit->Target.Validate())
                return;
            const HistoryMutationScope mutationScope(service);
            const bool firstPreview = !edit->Previewed;
            apply();
            edit->Previewed = true;
            if (edit->Target.Notify)
                edit->Target.Notify(EditChangeKind::Preview);
            if (firstPreview)
                service->NotifyHistoryChanged();
        }
        void Commit()
        {
            if (!*this)
                return;
            const auto edit = m_Edit;
            UndoRedoService* service = m_Service;
            if (!service->CaptureAfter(edit))
                return;
            edit->GestureOpen = false;
            edit->Previewed = edit->Before != edit->After;
            m_Edit.reset();
            const HistoryMutationScope mutationScope(service, edit->Previewed
                ? HistoryMutationScope::Phase::Edit : HistoryMutationScope::Phase::Restore);
            if (edit->Target.Notify)
                edit->Target.Notify(edit->Previewed ? EditChangeKind::Commit : EditChangeKind::UndoRedo);
            service->NotifyHistoryChanged();
        }
        void Cancel()
        {
            if (!*this)
            {
                m_Edit.reset();
                return;
            }
            const auto edit = m_Edit;
            UndoRedoService* service = m_Service;
            const HistoryMutationScope mutationScope(service, HistoryMutationScope::Phase::Restore);
            std::size_t undone = 0;
            for (auto command = edit->Consequences.rbegin(); command != edit->Consequences.rend(); ++command)
            {
                if (!(*command)->TryUndo())
                {
                    for (std::size_t restore = edit->Consequences.size() - undone; restore < edit->Consequences.size(); ++restore)
                        edit->Consequences[restore]->TryRedo();
                    return;
                }
                ++undone;
            }
            if (!edit->Target.Apply || !edit->Target.Apply(edit->Before))
            {
                for (auto& command : edit->Consequences)
                    command->TryRedo();
                return;
            }
            edit->Active = false;
            edit->GestureOpen = false;
            if (service->m_Pending == edit)
                service->m_Pending.reset();
            m_Edit.reset();
            for (auto& command : edit->Consequences)
                command->NotifyReplay();
            if (edit->Target.Notify)
                edit->Target.Notify(EditChangeKind::UndoRedo);
            service->NotifyHistoryChanged();
        }
    private:
        friend class UndoRedoService;
        InteractiveEdit(UndoRedoService* service, std::shared_ptr<PendingEdit> edit)
            : m_Service(service), m_Edit(std::move(edit)) {}
        UndoRedoService* m_Service = nullptr;
        std::shared_ptr<PendingEdit> m_Edit;
    };

    InteractiveEdit BeginInteractiveEdit(std::string name, SnapshotTarget target)
    {
        if (m_Replaying)
            return {};
        FireBeforeMutation();
        if (m_Pending && !m_Pending->GestureOpen && !target.Identity.empty() &&
            target.Identity == m_Pending->Target.Identity && name == m_Pending->Name)
        {
            m_Pending->GestureOpen = true;
            return InteractiveEdit(this, m_Pending);
        }
        if (!CommitPendingEdit())
            return {};
        SnapshotTarget::Snapshot before;
        if (!target.Capture || !target.Capture(before))
            return {};
        m_Pending = std::make_shared<PendingEdit>();
        m_Pending->Id = m_NextEntryId++;
        m_Pending->Name = std::move(name);
        m_Pending->Target = std::move(target);
        m_Pending->Before = std::move(before);
        m_Pending->After = m_Pending->Before;
        return InteractiveEdit(this, m_Pending);
    }

private:
    bool HasPendingCommand() const { return m_PendingCommand && !m_PendingCommand->IsNoOp(); }
    bool HasPendingStep() const
    {
        return HasPendingCommand() || (HasPendingEdit() && !m_Pending->GestureOpen);
    }
    bool CaptureAfter(const std::shared_ptr<PendingEdit>& edit)
    {
        SnapshotTarget::Snapshot after;
        if (!edit->Target.Capture || !edit->Target.Capture(after))
        {
            m_LastError = "Could not finish the pending action. Its targets are unavailable.";
            NotifyHistoryChanged();
            return false;
        }
        edit->After = std::move(after);
        return true;
    }
    void CommitInternal(std::unique_ptr<IEditorCommand> command, NodeId id = 0)
    {
        if (!m_CompoundStack.empty())
            m_CompoundStack.back()->Add(std::move(command));
        else
            m_Graph.Append(id ? id : m_NextEntryId++, std::move(command), m_Config.maxUndo);
        NotifyHistoryChanged();
    }
    void NotifyHistoryChanged()
    {
        if (m_OnHistoryChanged)
            m_OnHistoryChanged();
        m_HistoryChanged.Invoke();
    }
    void FireBeforeMutation() { if (m_BeforeMutation) m_BeforeMutation(); }
    Config m_Config{};
    HistoryGraph m_Graph;
    FlatMap<const void*, NodeId> m_SavedRevisions;
    NodeId m_NextEntryId = 1;
    std::shared_ptr<PendingEdit> m_Pending;
    std::unique_ptr<IEditorCommand> m_PendingCommand;
    NodeId m_PendingCommandId = 0;
    bool m_Settling = false;
    std::function<void()> m_BeforeCommit;
    std::vector<std::unique_ptr<CompoundHistoryCommand>> m_CompoundStack;
    bool m_Replaying = false;
    std::function<void()> m_OnHistoryChanged;
    std::function<void()> m_BeforeMutation;
    std::string m_LastError;
    Event<> m_HistoryChanged;
};
} // namespace GameEngine::Editor
