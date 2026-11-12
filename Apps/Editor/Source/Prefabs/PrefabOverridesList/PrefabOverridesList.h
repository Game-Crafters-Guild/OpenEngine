#pragma once

#include "UI/Interaction/DismissablePopup.h"
#include "UI/UIElement.h"

#include "ECS/Entity.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
class Button;
class Label;
class ScrollView;
} // namespace GameEngine

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Scene
{
struct PrefabOverrideTarget;
}

namespace GameEngine::UI::Layout
{
enum class PopupSide : uint8_t;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class SceneDocumentManager;
class UndoRedoService;
struct PrefabOverrideGroup;
struct PrefabOverrideRow;

/// The instance whose Overrides list is open. The list stays open across a rebuild of the Inspector: the next bar
/// on that instance opens its list again.
struct PrefabOverridesListReopen
{
    const ECS::World* World = nullptr;
    ECS::EntityHandle Root;
};

/// What the Overrides list uses besides the world: the open scene's document, whose load report gives the stale
/// rows, the Play mode test that makes the list read-only, and the open state that a rebuild keeps (may be null).
struct PrefabOverridesListContext
{
    std::function<SceneDocumentManager*()> Document;
    std::function<bool()> IsInPlayMode;
    PrefabOverridesListReopen* Reopen = nullptr;
};

/// The prefab bar's Overrides list: every override of the instance, grouped by entity, with the prefab's value
/// beside a changed field, Apply and Revert on each row, and Revert all and Apply all at the foot. A stale group
/// comes from the scene's load report: its rows are override lines whose address the prefab does not have, and
/// each offers Discard. Main thread only.
/// Layout and style: UI/controls/PrefabOverridesList/.
class PrefabOverridesList final : public UIElement, public DismissablePopup
{
  public:
    PrefabOverridesList(ECS::World& world, UndoRedoService* undo, EditorChangeNotifications* notifications,
                        PrefabOverridesListContext context);
    ~PrefabOverridesList() override;

    void OnOwnerManagerChanged(UIManager* owner) override { UpdatePopupRegistration(owner); }
    bool IsPopupOpen() const override { return m_Open; }
    void DismissPopup() override { Close(); }
    void OnPostLayout() override;

    /// Opens under `anchor` (over it when the window has no room below) on the instance that holds `entity`.
    /// `anchor` must stay alive while the list is open.
    void Open(const UIElement& anchor, ECS::EntityHandle entity);
    /// Closes the list; the next bar on the instance does not open it again.
    void Close();
    /// Reads the instance again at the next layout; does nothing while the list is closed. The rows are rebuilt
    /// after the layout, never inside the click of the row that changed the instance.
    void Refresh();

  private:
    SceneDocumentManager* Document() const;
    void BuildRows();
    void AppendGroupHeader(const PrefabOverrideGroup& group);
    void AppendRow(ECS::EntityHandle entity, const PrefabOverrideRow& row, const std::string& prefabName,
                   bool readOnly);
    void ApplyTargets(std::span<const Scene::PrefabOverrideTarget> targets, const std::string& undoName);
    void RevertTargets(std::span<const Scene::PrefabOverrideTarget> targets, const std::string& undoName);
    void Discard(std::size_t skipIndex);
    void FitWidthToBar();
    void Place();

    ECS::World& m_World;
    UndoRedoService* m_Undo = nullptr;
    EditorChangeNotifications* m_Notifications = nullptr;
    PrefabOverridesListContext m_Context;
    ECS::EntityHandle m_Entity;
    bool m_Open = false;
    bool m_RowsStale = false;
    const UIElement* m_Anchor = nullptr;
    UIElement* m_Panel = nullptr;
    Label* m_Title = nullptr;
    Label* m_PrefabLabel = nullptr;
    ScrollView* m_Scroll = nullptr;
    Label* m_Empty = nullptr;
    Label* m_Note = nullptr;
    Button* m_RevertAll = nullptr;
    Button* m_ApplyAll = nullptr;
    // The overrides of the rows that offer Apply and Revert, which Apply all and Revert all act on.
    std::vector<Scene::PrefabOverrideTarget> m_Targets;
    float m_PlacedX = -1.0f;
    float m_PlacedY = -1.0f;
    float m_MaxWidth = -1.0f;
    std::optional<UI::Layout::PopupSide> m_Side;
};

} // namespace GameEngine::Editor
