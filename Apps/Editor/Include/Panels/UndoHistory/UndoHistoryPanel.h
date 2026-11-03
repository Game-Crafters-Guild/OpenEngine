#pragma once

#include "UI/Controls/DockPanel.h"
#include "Events/Event.h"
#include <cstddef>
#include <cstdint>

namespace GameEngine {

class ScrollView;
class Label;
class UIElement;
class Button;

namespace Editor { class UndoRedoService; }

class UndoHistoryPanel : public DockPanel
{
  public:
    std::string_view DeclaredTabIconClass() const override { return "undo-icon"; }

    UndoHistoryPanel();
    ~UndoHistoryPanel() override;

    void SetUndoRedoService(Editor::UndoRedoService* undo);
    void Rebuild();
    void OnOwnerManagerChanged(UIManager* owner) override;

  private:
    void BuildUI();
    void AddRevision(std::uint64_t id, const std::string& label, std::size_t depth, const std::string& tooltip);

    Editor::UndoRedoService* m_Undo = nullptr;
    EventSubscription m_HistoryChanged;
    bool m_RebuildQueued = false;

    ScrollView* m_ScrollView = nullptr;
    UIElement* m_List = nullptr;
    Label* m_CountLabel = nullptr;
    Button* m_BackBtn = nullptr;
    Button* m_ForwardBtn = nullptr;
};

} // namespace GameEngine
