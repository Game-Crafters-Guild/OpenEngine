#pragma once

#include "ECS/Entity.h"
#include "UI/Controls/TextField.h"
#include "UndoRedo/History/HistorySnapshotTarget.h"

#include <functional>
#include <string>

namespace GameEngine
{
struct InspectorContext;

namespace Editor
{
class UndoRedoService;
}

/// Edits the primary entity's name and preserves its component presence in history.
class InspectorEntityNameField final : public TextField
{
public:
    InspectorEntityNameField(const InspectorContext& context, std::function<void()> componentSetChanged);

private:
    void PreviewName(const std::string& text);
    void CommitName();
    void ApplyName(const Editor::HistorySnapshotTarget::Snapshot& bytes);

    ECS::World* m_World;
    ECS::EntityHandle m_Entity;
    Editor::UndoRedoService* m_Undo;
    Editor::HistorySnapshotTarget m_Target;
    Editor::HistorySnapshotTarget::Snapshot m_InitialName;
    std::string m_InitialText;
    std::string m_SelectionSuffix;
    std::function<void()> m_ComponentSetChanged;
};
}
