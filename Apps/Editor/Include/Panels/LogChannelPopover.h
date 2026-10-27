#pragma once

#include "UI/Interaction/DismissablePopup.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine {

namespace UI::Layout {
enum class PopupSide : uint8_t;
}

class Button;
class Dropdown;
class Label;
class LogView;
class ScrollView;
class SearchFieldWithFilter;
class Toggle;

/// The Log panel's Channels list: a search field, Show All, Hide All and one row per channel (show toggle, name,
/// level). Each Open rebuilds the rows, Core first. Main thread only. Layout and style: UI/panels/LogPanel/
/// LogChannelPopover.uxml, LogChannelRow.uxml and LogChannelPopover.css.
class LogChannelPopover : public UIElement, public DismissablePopup {
public:
    explicit LogChannelPopover(LogView& view);
    ~LogChannelPopover() override;

    void OnOwnerManagerChanged(UIManager* owner) override { UpdatePopupRegistration(owner); }
    bool IsPopupOpen() const override { return m_Open; }
    void DismissPopup() override { Close(); }

    /// Opens under `anchor` (over it when the window has no room below), with an empty search field that has
    /// the focus. `anchor` must stay alive while the list is open.
    void Open(const UIElement& anchor);
    void Close();

    void OnPostLayout() override;

private:
    struct Row {
        uint16_t Channel = 0;
        std::string Name;
        UIElement* Element = nullptr;
        Toggle* Shown = nullptr;
        Label* NameLabel = nullptr;
        Dropdown* Level = nullptr;
    };

    void BuildRows();
    void AppendRow(uint16_t channel);
    void SyncRow(const Row& row);
    void OnQueryChanged(const std::string& query);
    void ApplyQuery(const std::string& query);
    void SetListedChannelsShown(bool shown);
    void OnShownChanged(uint16_t channel, bool shown);
    void OnLevelSelected(uint16_t channel, int index);
    /// Moves the list next to the anchor and inside the window. Applies a position only when it moves.
    void Place();

    LogView& m_View;
    bool m_Open = false;
    const UIElement* m_Anchor = nullptr;
    UIElement* m_Panel = nullptr;
    ScrollView* m_Scroll = nullptr;
    UIElement* m_Columns = nullptr;
    Button* m_ShowAll = nullptr;
    Button* m_HideAll = nullptr;
    Label* m_Count = nullptr;
    SearchFieldWithFilter* m_Search = nullptr;
    std::vector<Row> m_Rows;
    float m_PlacedX = -1.0f;
    float m_PlacedY = -1.0f;
    std::optional<UI::Layout::PopupSide> m_Side;
};

} // namespace GameEngine
