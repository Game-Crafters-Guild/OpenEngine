#include "Panels/LogView.h"

#include "Input/KeyCodes.h"
#include "Logger/LogSink.h"
#include "Logger/Logger.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ListView.h"
#include "UI/Interaction/Selection.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine {

namespace {

constexpr const char* kRowLayoutAssetPath = "UI/panels/LogPanel/LogRow.uxml";
constexpr const char* kGlyphId = "LogRowGlyph";
constexpr const char* kTimeId = "LogRowTime";
constexpr const char* kChannelId = "LogRowChannel";
/// The channel cell of a Core line: an en dash (UTF-8), so the cell reads as part of the column.
/// Copies and the file log write no channel for Core.
constexpr const char* kNoChannelText = "\xE2\x80\x93";
constexpr const char* kMessageId = "LogRowMessage";
constexpr std::array<std::string_view, 4> kRowRequiredIds = {kGlyphId, kTimeId, kChannelId, kMessageId};
/// Only on rows whose entry names a source: elsewhere a double-click only selects.
constexpr const char* kOpenSourceTooltip = "Double-click to open the source.";

/// A burst (an asset scan) can queue thousands of lines; the list takes this many per frame and
/// keeps the rest queued, so no single frame pays for the burst.
constexpr size_t kMaxFlushPerFrame = 2000;
/// Messages queued while the panel is not flushing (hidden, paused) are capped; the oldest go first,
/// as the model would drop them anyway.
constexpr size_t kMaxPending = 16384;

/// One class per severity for each part of a row, indexed by LogModel::Severity.
using SeverityClasses = std::array<const char*, LogModel::kSeverityCount>;
constexpr SeverityClasses kGlyphSeverityClasses{"log-row-glyph-debug", "log-row-glyph-info", "log-row-glyph-warning",
                                                "log-row-glyph-error"};
constexpr SeverityClasses kMessageSeverityClasses{"log-row-message-debug", "log-row-message-info",
                                                  "log-row-message-warning", "log-row-message-error"};

void SetSeverityClass(UIElement* part, const SeverityClasses& classes, LogModel::Severity severity)
{
    if (!part)
        return;
    for (const char* name : classes)
        part->RemoveClass(name);
    part->AddClass(classes[static_cast<size_t>(severity)]);
}

std::string_view FirstLine(std::string_view text)
{
    const size_t end = text.find_first_of("\r\n");
    return end == std::string_view::npos ? text : text.substr(0, end);
}

} // namespace

/// One pooled row: the parts of LogRow.uxml, found once when the row is built and set on every bind.
class LogView::Row : public UIElement {
public:
    Row()
    {
        std::unique_ptr<UIElement> layout = Editor::ControlLayoutLoader::Load(kRowLayoutAssetPath, kRowRequiredIds);
        if (!layout)
            return;
        // The row takes the layout root's place: its classes and its parts.
        for (const std::string& name : layout->GetClasses())
            AddClass(name);
        m_Glyph = layout->FindById(HashStringId(kGlyphId));
        m_Time = dynamic_cast<Label*>(layout->FindById(HashStringId(kTimeId)));
        m_Channel = dynamic_cast<Label*>(layout->FindById(HashStringId(kChannelId)));
        m_Message = dynamic_cast<Label*>(layout->FindById(HashStringId(kMessageId)));
        while (!layout->GetChildren().empty())
            AddChild(layout->TakeChild(layout->GetChildren().front().get()));
    }

    void Bind(const LogModel::Entry& entry)
    {
        const LogModel::Severity severity = LogModel::SeverityOf(entry.Level);
        SetTooltip(LogModel::FindSourceToOpen(entry) ? kOpenSourceTooltip : "");
        SetSeverityClass(m_Glyph, kGlyphSeverityClasses, severity);
        if (m_Time)
            m_Time->SetText(LogModel::FormatTime(entry));
        if (m_Channel)
        {
            const std::string_view channel = LogModel::ChannelLabel(entry);
            m_Channel->SetText(channel.empty() ? std::string(kNoChannelText) : std::string(channel));
        }
        if (m_Message)
        {
            SetSeverityClass(m_Message, kMessageSeverityClasses, severity);
            m_Message->SetText(std::string(FirstLine(entry.Text)));
        }
    }

private:
    UIElement* m_Glyph = nullptr;
    Label* m_Time = nullptr;
    Label* m_Channel = nullptr;
    Label* m_Message = nullptr;
};

class LogView::DataProvider : public ListChangeTrackingProvider {
public:
    explicit DataProvider(const LogModel& model) : m_Model(model) {}

    int GetItemCount() const override { return static_cast<int>(m_Model.GetVisibleCount()); }
    ListId GetItemId(int index) const override
    {
        const LogModel::Entry* entry = index >= 0 ? m_Model.GetVisibleEntry(static_cast<size_t>(index)) : nullptr;
        return entry ? entry->Sequence : 0;
    }
    float GetItemHeight(int /*index*/) const override { return kRowHeight; }

private:
    const LogModel& m_Model;
};

LogView::LogView()
{
    AddClass("log-view");
    SetId("LogView");

    m_DataProvider = std::make_unique<DataProvider>(m_Model);
    m_SelectionModel = std::make_unique<UI::Interaction::SelectionModel>();

    auto listView = std::make_unique<ListView>();
    m_ListView = listView.get();
    m_ListView->SetId("LogListView");
    m_ListView->AddClass("log-list");
    m_ListView->SetFocusable(true);
    m_ListView->SetDataProvider(m_DataProvider.get());
    m_ListView->SetSelectionModel(m_SelectionModel.get());
    m_SelectionModel->SetOnChanged([this]() { OnSelectionModelChanged(); });
    m_ListView->SetItemFactory([this](ListId, IListDataProvider*) { return MakeRow(); });
    m_ListView->SetItemBinder(
        [this](UIElement* row, ListId, int index, IListDataProvider*)
        {
            if (row)
                BindRow(*row, index);
        });
    m_ListView->SetOnItemActivated([this](ListId id) { OnItemActivated(id); });
    m_ListView->SetOnContextMenu([this](ListId id, float x, float y) { OnListContextMenu(id, x, y); });
    m_ListView->RegisterEventHandler(kEventKeyDown, [this](UIEvent& event) { OnListKeyDown(event); });
    AddChild(std::move(listView));
}

LogView::~LogView() = default;

std::unique_ptr<UIElement> LogView::MakeRow()
{
    return std::make_unique<Row>();
}

void LogView::BindRow(UIElement& row, int index)
{
    const LogModel::Entry* entry = index >= 0 ? m_Model.GetVisibleEntry(static_cast<size_t>(index)) : nullptr;
    // Every row comes from MakeRow.
    if (entry)
        static_cast<Row&>(row).Bind(*entry);
}

void LogView::AddMessage(const Logger::LogMessage& message)
{
    LogModel::Entry entry = LogModel::MakeEntry(message);
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    if (m_Pending.size() >= kMaxPending)
        m_Pending.pop_front();
    m_Pending.push_back(std::move(entry));
}

void LogView::FlushPendingMessages()
{
    if (m_Paused)
        return;

    std::deque<LogModel::Entry> batch;
    {
        std::lock_guard<std::mutex> lock(m_PendingMutex);
        if (m_Pending.empty())
            return;
        if (m_Pending.size() <= kMaxFlushPerFrame)
        {
            batch.swap(m_Pending);
        }
        else
        {
            const auto end = m_Pending.begin() + static_cast<std::ptrdiff_t>(kMaxFlushPerFrame);
            batch.assign(std::make_move_iterator(m_Pending.begin()), std::make_move_iterator(end));
            m_Pending.erase(m_Pending.begin(), end);
        }
    }

    const LogModel::AppendResult result = m_Model.Append(batch);
    if (result.Trimmed || m_Model.IsFiltered())
        m_ListView->RefreshFromProvider();
    else
        m_ListView->NotifyItemsAppended(static_cast<int>(result.Appended));
    if (result.Trimmed)
        DropSelectionNotShown();
    if (result.AppendedVisible > 0)
        m_ListView->ScrollToEnd();
    NotifyEntriesChanged();
}

void LogView::Clear()
{
    {
        std::lock_guard<std::mutex> lock(m_PendingMutex);
        m_Pending.clear();
    }
    m_Model.Clear();
    m_SelectionModel->Clear();
    m_ListView->RefreshFromProvider();
    NotifyEntriesChanged();
}

void LogView::SetSeverityShown(LogModel::Severity severity, bool shown)
{
    m_Model.SetSeverityShown(severity, shown);
    AfterFilterChange();
}

void LogView::SetChannelsShown(std::span<const uint16_t> channels, bool shown)
{
    m_Model.SetChannelsShown(channels, shown);
    AfterFilterChange();
}

void LogView::SetSearch(std::string_view text)
{
    m_Model.SetSearch(text);
    AfterFilterChange();
}

void LogView::SetSearchField(LogModel::SearchField field)
{
    m_Model.SetSearchField(field);
    AfterFilterChange();
}

const LogModel::Entry* LogView::GetSelectedEntry() const
{
    const UI::Interaction::ItemId anchor = m_SelectionModel->GetAnchor();
    return anchor != 0 ? m_Model.FindEntry(anchor) : nullptr;
}

std::vector<const LogModel::Entry*> LogView::GetSelectedEntries() const
{
    std::vector<UI::Interaction::ItemId> ids = m_SelectionModel->GetSelection();
    // Sequences increase in arrival order, so sorted ids are oldest first.
    std::sort(ids.begin(), ids.end());
    std::vector<const LogModel::Entry*> entries;
    entries.reserve(ids.size());
    for (const UI::Interaction::ItemId id : ids)
    {
        if (const LogModel::Entry* entry = m_Model.FindEntry(id))
            entries.push_back(entry);
    }
    return entries;
}

std::optional<Mathematics::Rect> LogView::GetSelectedRowWindowRect() const
{
    const UI::Interaction::ItemId anchor = m_SelectionModel->GetAnchor();
    const std::optional<size_t> row = anchor != 0 ? m_Model.FindVisibleRow(anchor) : std::nullopt;
    if (!row)
        return std::nullopt;
    const Mathematics::Rect list = m_ListView->GetWindowRect();
    const float top = list.Y + static_cast<float>(*row) * kRowHeight - m_ListView->GetScrollOffset();
    if (top < list.Y || top + kRowHeight > list.Bottom())
        return std::nullopt;
    return Mathematics::Rect{list.X, top, list.Width, kRowHeight};
}

void LogView::OnPostLayout()
{
    UIElement::OnPostLayout();
    if (!m_RevealSelectionAfterLayout)
        return;
    m_RevealSelectionAfterLayout = false;
    const UI::Interaction::ItemId anchor = m_SelectionModel->GetAnchor();
    if (const std::optional<size_t> row = anchor != 0 ? m_Model.FindVisibleRow(anchor) : std::nullopt)
        m_ListView->ScrollIndexIntoView(static_cast<int>(*row));
}

void LogView::OnSelectionModelChanged()
{
    if (m_OnSelectionChanged)
        m_OnSelectionChanged(GetSelectedEntry());
}

void LogView::OnItemActivated(uint64_t id)
{
    const LogModel::Entry* entry = m_Model.FindEntry(id);
    if (entry && m_OnEntryActivated)
        m_OnEntryActivated(*entry);
}

void LogView::OnListContextMenu(uint64_t id, float x, float y)
{
    if (id == 0 || !m_Model.FindEntry(id))
        return;
    if (!m_SelectionModel->IsSelected(id))
        m_SelectionModel->SetSingle(id);
    if (m_OnContextMenu)
        m_OnContextMenu(x, y);
}

void LogView::OnListKeyDown(UIEvent& event)
{
    if (event.Key != Input::kKeyCode_C || !Input::IsPrimaryShortcutModifier(event.Mods) || !m_OnCopyShortcut)
        return;
    m_OnCopyShortcut();
    event.Stop();
}

void LogView::AfterFilterChange()
{
    m_ListView->RefreshFromProvider();
    DropSelectionNotShown();
    NotifyEntriesChanged();
}

void LogView::DropSelectionNotShown()
{
    const std::vector<UI::Interaction::ItemId> selection = m_SelectionModel->GetSelection();
    std::vector<UI::Interaction::ItemId> shown;
    shown.reserve(selection.size());
    for (const UI::Interaction::ItemId id : selection)
    {
        const LogModel::Entry* entry = m_Model.FindEntry(id);
        if (entry && m_Model.IsShown(*entry))
            shown.push_back(id);
    }
    if (shown.empty())
    {
        m_SelectionModel->Clear();
        return;
    }
    if (shown.size() == selection.size())
        return;
    // A hidden anchor moves to the newest line still selected: sequences increase in arrival order.
    const UI::Interaction::ItemId anchor = m_SelectionModel->GetAnchor();
    const bool anchorShown = std::find(shown.begin(), shown.end(), anchor) != shown.end();
    m_SelectionModel->SetSelection(shown, anchorShown ? anchor : *std::max_element(shown.begin(), shown.end()));
}

void LogView::NotifyEntriesChanged()
{
    if (m_OnEntriesChanged)
        m_OnEntriesChanged();
}

} // namespace GameEngine

namespace RegisterWidgets {
static auto s_RegisterLogView = GameEngine::UIRegistration::RegisterWithFactory<GameEngine::LogView>(
    "LogView", []() { return std::make_unique<GameEngine::LogView>(); });
}
