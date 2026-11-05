#pragma once

#include "UndoRedo/IEditorCommand.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
class CompoundHistoryCommand final : public IEditorCommand
{
public:
    explicit CompoundHistoryCommand(std::string name) : m_Name(std::move(name)) {}
    const char* GetName() const override { return m_Name.c_str(); }
    std::string GetDescription() const override
    {
        std::string result;
        for (const auto& command : m_Commands)
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
    const char* GetTypeName() const override { return "Compound"; }
    void Do() override { Redo(); }
    void Undo() override { if (TryUndo()) NotifyReplay(); }
    void Redo() override { if (TryRedo()) NotifyReplay(); }
    void NotifyReplay() override
    {
        for (auto& command : m_Commands)
            command->NotifyReplay();
    }

    bool TryUndo() override
    {
        for (std::size_t i = m_Commands.size(); i > 0; --i)
        {
            if (m_Commands[i - 1]->TryUndo())
                continue;
            for (std::size_t restore = i; restore < m_Commands.size(); ++restore)
                m_Commands[restore]->TryRedo();
            return false;
        }
        return true;
    }
    bool TryRedo() override
    {
        for (std::size_t i = 0; i < m_Commands.size(); ++i)
        {
            if (m_Commands[i]->TryRedo())
                continue;
            while (i > 0)
                m_Commands[--i]->TryUndo();
            return false;
        }
        return true;
    }
    void Add(std::unique_ptr<IEditorCommand> command)
    {
        if (command)
            m_Commands.push_back(std::move(command));
    }
    bool Empty() const { return m_Commands.empty(); }

private:
    std::string m_Name;
    std::vector<std::unique_ptr<IEditorCommand>> m_Commands;
};
} // namespace GameEngine::Editor
