#pragma once

#include "Panels/Script/ScriptHistoryDocument.h"
#include "UndoRedo/IEditorCommand.h"

#include <memory>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
class ScriptTextEditCommand final : public IEditorCommand
{
public:
    ScriptTextEditCommand(std::shared_ptr<ScriptHistoryDocument> document, std::string before, std::string after)
        : m_Document(std::move(document)), m_Before(std::move(before)), m_After(std::move(after)) {}

    const char* GetName() const override { return "Script Edit"; }
    void Do() override {}
    void Undo() override { TryUndo(); NotifyReplay(); }
    void Redo() override { TryRedo(); NotifyReplay(); }
    bool TryUndo() override { m_Document->SetText(m_Before); return true; }
    bool TryRedo() override { m_Document->SetText(m_After); return true; }
    void NotifyReplay() override { m_Document->Changed().Invoke(); }
    bool IsContinuous() const override { return true; }
    bool IsNoOp() const override { return m_Before == m_After; }

    bool CanMergeWith(const IEditorCommand& other) const override
    {
        const auto* command = dynamic_cast<const ScriptTextEditCommand*>(&other);
        return command && command->m_Document == m_Document && m_After == command->m_Before;
    }
    bool MergeWith(const IEditorCommand& other) override
    {
        if (!CanMergeWith(other))
            return false;
        m_After = static_cast<const ScriptTextEditCommand&>(other).m_After;
        return true;
    }

private:
    std::shared_ptr<ScriptHistoryDocument> m_Document;
    std::string m_Before;
    std::string m_After;
};
} // namespace GameEngine::Editor
