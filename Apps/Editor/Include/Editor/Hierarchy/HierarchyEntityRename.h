#pragma once

#include "UI/InlineRenameSession.h"

#include "ECS/Entity.h"

namespace GameEngine
{
class Label;
class UIElement;
namespace ECS
{
class World;
}
} // namespace GameEngine

namespace GameEngine::Editor
{

class EditorChangeNotifications;
class UndoRedoService;

// The Hierarchy's inline rename of one entity on its row.
class HierarchyEntityRename
{
public:
    // Starts editing the entity's name in place of its row's `title`; keyboard focus returns to
    // `tree` when the session ends with Enter or Escape. The row's icon is its own element and
    // stays.
    void Begin(Label* title, UIElement* tree, ECS::World& world, ECS::EntityHandle entity, UndoRedoService* undo,
               EditorChangeNotifications* notifications);

    // Ends the session without renaming: every row is about to be rebuilt.
    void Cancel() { m_Session.Cancel(); }

    // Called when the tree binds `row` to show `shown` (an invalid handle for a row with no
    // entity). The session ends without renaming when `row` is the one it edits and now shows
    // another entity; a row bound again to the entity being renamed keeps its field, as it is
    // on every selection change.
    void OnRowBound(const UIElement& row, ECS::EntityHandle shown);

    // The title label the active session edits, or null.
    const Label* GetTitle() const { return m_Session.GetTitle(); }

private:
    InlineRenameSession m_Session;
    ECS::EntityHandle m_Entity;
};

} // namespace GameEngine::Editor
