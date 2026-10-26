#pragma once

#include "Mathematics/Rect.h"
#include "Panels/LogModel.h"
#include "UI/UIElement.h"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Logger {
struct LogMessage;
}

namespace GameEngine {

class ListView;

namespace UI::Interaction {
class SelectionModel;
}

/**
 * @brief The Log panel's list: one row per entry the model's filters show, newest at the bottom.
 *
 * Messages arrive on any thread through AddMessage and reach the model on the main thread in
 * FlushPendingMessages. A row shows the severity glyph, the time and the message's first line; each
 * row is declared in UI/panels/LogPanel/LogRow.uxml. The list's item id is the entry's sequence, so a
 * selection stays on its entry when old entries are dropped. Main thread only, except AddMessage.
 */
class LogView : public UIElement {
public:
    /// Row height in logical pixels: the Hierarchy's row height.
    static constexpr float kRowHeight = 20.0f;

    LogView();
    ~LogView() override;

    /// Queues a message for the next FlushPendingMessages. Thread-safe.
    void AddMessage(const Logger::LogMessage& message);
    /// Moves queued messages into the model and updates the list. Does nothing while paused.
    void FlushPendingMessages();
    /// Drops every entry and every queued message.
    void Clear();

    /// While paused the list stops changing; messages keep queuing and land when resumed.
    void SetPaused(bool paused) { m_Paused = paused; }
    bool IsPaused() const { return m_Paused; }

    const LogModel& GetModel() const { return m_Model; }
    /// Changes the model's filters, rebinds the rows and drops a selection the filters now hide.
    void SetSeverityShown(LogModel::Severity severity, bool shown);
    void SetSearch(std::string_view text);
    void SetSearchField(LogModel::SearchField field);

    /// The entry the selection is anchored on, or null.
    const LogModel::Entry* GetSelectedEntry() const;
    /// Every selected entry the model still holds, oldest first.
    std::vector<const LogModel::Entry*> GetSelectedEntries() const;
    /// Scrolls the anchored entry fully into view after the next layout: the list shrinks when the
    /// detail pane opens, which can hide a row selected near its bottom.
    void RevealSelectionAfterLayout() { m_RevealSelectionAfterLayout = true; }

    /// The window rectangle of the row the selection is anchored on, or nothing when it is out of view.
    std::optional<Mathematics::Rect> GetSelectedRowWindowRect() const;

    void OnPostLayout() override;

    /// Called with the anchored entry (or null) whenever the selection changes.
    void SetOnSelectionChanged(std::function<void(const LogModel::Entry*)> callback)
    {
        m_OnSelectionChanged = std::move(callback);
    }
    /// Called on a double-click on a row.
    void SetOnEntryActivated(std::function<void(const LogModel::Entry&)> callback)
    {
        m_OnEntryActivated = std::move(callback);
    }
    /// Called on a right-click on a row, after the row is selected; x and y are window coordinates.
    void SetOnContextMenu(std::function<void(float x, float y)> callback) { m_OnContextMenu = std::move(callback); }
    /// Called on the copy shortcut while the list has focus.
    void SetOnCopyShortcut(std::function<void()> callback) { m_OnCopyShortcut = std::move(callback); }
    /// Called after entries arrive, are cleared or the filters change: the per-severity counts moved.
    void SetOnEntriesChanged(std::function<void()> callback) { m_OnEntriesChanged = std::move(callback); }

private:
    class DataProvider;
    class Row;

    std::unique_ptr<UIElement> MakeRow();
    void BindRow(UIElement& row, int index);
    void OnSelectionModelChanged();
    void OnItemActivated(uint64_t id);
    void OnListContextMenu(uint64_t id, float x, float y);
    void OnListKeyDown(UIEvent& event);
    void AfterFilterChange();
    void DropSelectionNotShown();
    void NotifyEntriesChanged();

    LogModel m_Model;
    ListView* m_ListView = nullptr;
    std::unique_ptr<DataProvider> m_DataProvider;
    std::unique_ptr<UI::Interaction::SelectionModel> m_SelectionModel;

    std::deque<LogModel::Entry> m_Pending;
    mutable std::mutex m_PendingMutex;
    bool m_Paused = false;
    bool m_RevealSelectionAfterLayout = false;

    std::function<void(const LogModel::Entry*)> m_OnSelectionChanged;
    std::function<void(const LogModel::Entry&)> m_OnEntryActivated;
    std::function<void(float, float)> m_OnContextMenu;
    std::function<void()> m_OnCopyShortcut;
    std::function<void()> m_OnEntriesChanged;
};

} // namespace GameEngine
