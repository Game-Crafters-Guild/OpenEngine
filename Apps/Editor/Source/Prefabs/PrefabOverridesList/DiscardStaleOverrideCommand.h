#pragma once

#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"

#include "ECS/Entity.h"
#include "Scene/SceneIO.h"

#include <cstddef>
#include <optional>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class SceneDocumentManager;

/// The undo step of a Discard in the Overrides list: the load report's row of an override line whose target the
/// prefab does not have leaves the document's record, so the save guard stops counting it. Undo puts the row back.
/// Do and Undo report a change on the instance's root, so the prefab bar and its list read the report again.
class DiscardStaleOverrideCommand final : public IEditorCommand
{
  public:
    DiscardStaleOverrideCommand(SceneDocumentManager& document, ECS::World& world,
                                EditorChangeNotifications* notifications, ECS::EntityHandle root,
                                std::size_t skipIndex);

    const char* GetName() const override { return "Discard stale override"; }
    const char* GetTypeName() const override { return "DiscardStaleOverrideCommand"; }
    void Do() override;
    void Undo() override;

  private:
    void NotifyRoot(EditorChangeNotifications::ChangeKind kind) const;

    SceneDocumentManager& m_Document;
    ECS::World& m_World;
    EditorChangeNotifications* m_Notifications = nullptr;
    ECS::EntityHandle m_Root;
    std::size_t m_SkipIndex = 0;
    std::optional<Scene::SceneDegradation::Skip> m_Skip;
};

} // namespace GameEngine::Editor
