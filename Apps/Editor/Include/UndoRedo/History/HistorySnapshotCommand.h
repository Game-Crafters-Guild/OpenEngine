#pragma once

#include "UndoRedo/History/HistorySnapshotTarget.h"
#include "UndoRedo/IEditorCommand.h"

#include <string>
#include <utility>

namespace GameEngine::Editor
{
class HistorySnapshotCommand final : public IEditorCommand
{
public:
    HistorySnapshotCommand(std::string name, HistorySnapshotTarget target,
                           HistorySnapshotTarget::Snapshot before, HistorySnapshotTarget::Snapshot after)
        : m_Name(std::move(name)), m_Target(std::move(target)), m_Before(std::move(before)), m_After(std::move(after))
    {
        if (m_Target.Describe)
            m_Description = m_Target.Describe(m_Before, m_After);
    }

    const char* GetName() const override { return m_Name.c_str(); }
    std::string GetDescription() const override { return m_Description; }
    const char* GetTypeName() const override { return "Snapshot"; }
    void Do() override { Redo(); }
    void Undo() override { if (TryUndo()) NotifyReplay(); }
    void Redo() override { if (TryRedo()) NotifyReplay(); }
    bool TryUndo() override { return Apply(m_Before); }
    bool TryRedo() override { return Apply(m_After); }
    void NotifyReplay() override
    {
        if (m_Target.Notify)
            m_Target.Notify(EditChangeKind::UndoRedo);
    }

private:
    bool Apply(const HistorySnapshotTarget::Snapshot& snapshot)
    {
        if (!m_Target.Apply || !m_Target.Apply(snapshot))
            return false;
        return true;
    }
    std::string m_Name;
    std::string m_Description;
    HistorySnapshotTarget m_Target;
    HistorySnapshotTarget::Snapshot m_Before;
    HistorySnapshotTarget::Snapshot m_After;
};
} // namespace GameEngine::Editor
