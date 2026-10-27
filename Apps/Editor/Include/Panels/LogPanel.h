#pragma once

#include "Logger/CallbackSink.h"
#include "Panels/LogModel.h"
#include "UI/Controls/DockPanel.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace GameEngine {

class Button;
class INativeContextMenu;
class Label;
class LogChannelPopover;
class LogView;
class Splitter;
class TextArea;
class UIElement;
class WeightedPane;

namespace EditorUI {
class TransientNotice;
}

/**
 * @brief The Log panel: the engine's log as a filterable list with a detail pane for the selected entry.
 *
 * The layout is UI/panels/LogPanel/LogPanel.uxml and the style LogPanel.css beside it. A click
 * selects a row and shows its whole message, channel, source, time and stack trace in the detail pane; a
 * double-click opens the source, Ctrl+C copies, the right-click menu copies, opens the source or
 * clears, and every copy is confirmed by a notice over the list. The Channels button shows or hides each
 * channel's lines and sets the level a channel logs at. Main thread only.
 */
class LogPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "dock-log-icon"; }

    LogPanel();
    ~LogPanel() override;

    /// Flushes queued messages and runs the notice's clock. Called each frame from the main thread.
    void Update();

    /// Opens a source file at a 1-based line (0 opens the file): the menu's Open Source, the detail
    /// pane's source button and a click on a stack frame.
    void SetOnOpenSourceFile(std::function<void(const std::string&, int)> callback)
    {
        m_OnOpenSourceFile = std::move(callback);
    }

private:
    void BuildFromLayout();
    void BuildSearchBar(UIElement& host);
    void WireFilterToggle(UIElement& root, LogModel::Severity severity, const char* id);
    void ToggleSeverity(LogModel::Severity severity);
    void SyncFilterToggles();
    void ShowChannelList();
    void TogglePause();
    void ClearLog();

    void OnLogMessage(const Logger::LogMessage& message);
    void OnSelectionChanged(const LogModel::Entry* entry);
    void ShowDetail(const LogModel::Entry& entry);
    void HideDetail();
    void SyncDetailLayoutMode();

    void CopySelected();
    void CopyAllVisible();
    /// Copies `text` and confirms it: on the selected row's right end when `besideSelection` and the
    /// row is in view, else at the bottom of the list.
    void CopyText(const std::string& text, const std::string& confirmation, bool besideSelection);
    std::optional<float> FindNoticeTopBesideSelection() const;
    void OpenSelectedSource();
    void OpenSource(const std::string& file, int line);
    void ShowContextMenu(float x, float y);
    void OnContextMenuCommand(uint32_t commandId);

    LogView* m_LogView = nullptr;
    EditorUI::TransientNotice* m_Notice = nullptr;
    UIElement* m_SplitBody = nullptr;
    WeightedPane* m_ListPane = nullptr;
    Splitter* m_DetailSplitter = nullptr;
    WeightedPane* m_DetailPane = nullptr;
    UIElement* m_DetailGlyph = nullptr;
    Label* m_DetailSeverity = nullptr;
    Label* m_DetailChannel = nullptr;
    Label* m_DetailTime = nullptr;
    Button* m_DetailSource = nullptr;
    TextArea* m_DetailMessage = nullptr;
    UIElement* m_DetailTraceTitle = nullptr;
    UIElement* m_DetailFrames = nullptr;
    UIElement* m_SearchBar = nullptr;
    Button* m_PauseButton = nullptr;
    Button* m_ChannelsButton = nullptr;
    std::array<Button*, LogModel::kSeverityCount> m_FilterToggles{};

    std::unique_ptr<INativeContextMenu> m_ContextMenu;
    LogChannelPopover* m_ChannelPopover = nullptr;
    std::function<void(const std::string&, int)> m_OnOpenSourceFile;

    std::uint64_t m_CallbackId = 0;

    bool m_DetailLayoutInitialized = false;
    bool m_DetailSideBySide = false;
    float m_VisibleListWeight = 0.65f;
    float m_VisibleDetailWeight = 0.35f;
};

} // namespace GameEngine
