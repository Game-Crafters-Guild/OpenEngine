#include "Panels/LogChannelPopover.h"

#include "Logger/LogChannel.h"
#include "Logger/Logger.h"
#include "Panels/LogView.h"
#include "Types/StringUtils.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/SearchFieldWithFilter.h"
#include "UI/Controls/Toggle.h"
#include "UI/Layout/PopupPlacement.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace GameEngine {

namespace {

constexpr const char* kLayoutAssetPath = "UI/panels/LogPanel/LogChannelPopover.uxml";
constexpr const char* kRowLayoutAssetPath = "UI/panels/LogPanel/LogChannelRow.uxml";
constexpr std::array<std::string_view, 6> kRequiredIds = {"LogChannelsScroll",  "LogChannelsSearchHost",
                                                         "LogChannelsColumns", "LogChannelsShowAll",
                                                         "LogChannelsHideAll", "LogChannelsCount"};
constexpr std::array<std::string_view, 3> kRowRequiredIds = {"LogChannelShown", "LogChannelName", "LogChannelLevel"};
constexpr const char* kStyleAssetPath = "UI/panels/LogPanel/LogChannelPopover.css";
constexpr const char* kSearchFieldId = "log-channel-search-field";
constexpr const char* kHiddenClass = "hidden";
/// A hidden channel's row: its name and its level dim.
constexpr const char* kRowOffClass = "log-channel-row-off";
/// The search field has the focus but no focus ring until the user types or presses in it.
constexpr const char* kAwaitingInputClass = "awaiting-input";

/// The levels a row offers after its Default item, in list order: item i + 1 sets kLevels[i].
constexpr std::array<Logger::LogLevel, 6> kLevels{Logger::LogLevel::Trace, Logger::LogLevel::Debug,
                                                  Logger::LogLevel::Info, Logger::LogLevel::Warning,
                                                  Logger::LogLevel::Error, Logger::LogLevel::Off};

/// Logical px between the Channels button and the list, and between the list and the window edge.
constexpr float kAnchorGap = 4.0f;
constexpr float kEdgeMargin = 8.0f;
/// A move smaller than this keeps the current position, so layout does not run again for nothing.
constexpr float kPlacementEpsilon = 0.25f;

/// A channel's place in the list: Core first, then the other channels by name, ignoring case.
struct ListOrder
{
    bool IsNotCore = true;
    std::string LowerName;
    uint16_t Channel = 0;

    auto operator<=>(const ListOrder&) const = default;
};

/// The items of a row's level list: "Default (<level>)" first, then kLevels.
std::vector<std::string> LevelLabels(uint16_t channel)
{
    std::vector<std::string> labels;
    labels.reserve(kLevels.size() + 1);
    std::string defaultLabel = "Default (";
    defaultLabel += Logger::LogLevelDisplayName(Logger::Log::GetChannelLevelWithoutSetting(channel));
    defaultLabel += ')';
    labels.push_back(std::move(defaultLabel));
    for (const Logger::LogLevel level : kLevels)
        labels.emplace_back(Logger::LogLevelDisplayName(level));
    return labels;
}

/// The level list item that shows the channel's setting: 0 when it follows the default.
int LevelIndex(uint16_t channel)
{
    const std::optional<Logger::LogLevel> setting = Logger::Log::GetChannelSetting(channel);
    if (!setting)
        return 0;
    const auto found = std::ranges::find(kLevels, *setting);
    return found == kLevels.end() ? 0 : static_cast<int>(found - kLevels.begin()) + 1;
}

void SetClass(UIElement& element, const char* name, bool on)
{
    if (on == element.HasClass(name))
        return;
    if (on)
        element.AddClass(name);
    else
        element.RemoveClass(name);
}

} // namespace

LogChannelPopover::LogChannelPopover(LogView& view)
    : DismissablePopup(this)
    , m_View(view)
{
    AddClass("log-channels-anchor");
    AddClass(kHiddenClass);
    // The list draws over the panels around the Log panel, as a context menu does.
    SetOverlayLayer(OverlayLayer::Dropdown);
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> panel = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!panel)
        return;
    auto* scroll = dynamic_cast<ScrollView*>(panel->FindById(HashStringId("LogChannelsScroll")));
    UIElement* searchHost = panel->FindById(HashStringId("LogChannelsSearchHost"));
    m_Columns = panel->FindById(HashStringId("LogChannelsColumns"));
    m_ShowAll = dynamic_cast<Button*>(panel->FindById(HashStringId("LogChannelsShowAll")));
    m_HideAll = dynamic_cast<Button*>(panel->FindById(HashStringId("LogChannelsHideAll")));
    m_Count = dynamic_cast<Label*>(panel->FindById(HashStringId("LogChannelsCount")));
    if (!scroll || !searchHost || !m_Columns || !m_ShowAll || !m_HideAll || !m_Count)
    {
        Logger::Log::Error("Log panel: {} has an element of the wrong type for the Channels list; check its "
                           "element types against LogChannelPopover.cpp.",
                           kLayoutAssetPath);
        m_Count = nullptr;
        return;
    }

    m_Scroll = scroll;

    // The field of the searchable context menus (UIContextMenu::AddSearchRow).
    auto search = std::make_unique<SearchFieldWithFilter>();
    m_Search = search.get();
    m_Search->AddClass("context-menu-search");
    m_Search->AddClass("log-channels-search");
    m_Search->SetFieldId(kSearchFieldId);
    m_Search->SetPlaceholder("Search channels");
    m_Search->SetRefocusAfterClear(true);
    m_Search->SetOnQueryChanging([this](const std::string& query) { OnQueryChanged(query); });
    m_Search->SetOnQueryChanged([this](const std::string& query) { OnQueryChanged(query); });
    m_Search->RegisterEventHandler(kEventMouseDown, [this](UIEvent&) { m_Search->RemoveClass(kAwaitingInputClass); });
    searchHost->AddChild(std::move(search));

    m_ShowAll->SetTooltip("Show the lines of every channel the list shows. With a search, only the channels that match.");
    m_ShowAll->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { SetListedChannelsShown(true); });
    m_HideAll->SetTooltip("Hide the lines of every channel the list shows. With a search, only the channels that match.");
    m_HideAll->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { SetListedChannelsShown(false); });

    m_Panel = panel.get();
    AddChild(std::move(panel));
}

LogChannelPopover::~LogChannelPopover() = default;

void LogChannelPopover::Open(const UIElement& anchor)
{
    if (!m_Panel)
        return;
    m_Anchor = &anchor;
    m_Open = true;
    m_PlacedX = -1.0f;
    m_PlacedY = -1.0f;
    m_Side.reset();
    BuildRows();
    m_Search->SetValue("");
    m_Search->AddClass(kAwaitingInputClass);
    ApplyQuery("");
    m_Scroll->SetScrollY(0.0f);
    RemoveClass(kHiddenClass);
    if (UIManager* manager = GetOwnerManager())
        manager->SetFocusById(kSearchFieldId);
}

void LogChannelPopover::Close()
{
    if (!m_Open)
        return;
    m_Open = false;
    m_Anchor = nullptr;
    AddClass(kHiddenClass);
}

void LogChannelPopover::OnPostLayout()
{
    if (m_Open)
        Place();
}

void LogChannelPopover::BuildRows()
{
    m_Scroll->GetViewport()->RemoveAllChildren();
    m_Rows.clear();
    const uint16_t count = Logger::Log::GetChannelCount();
    const uint16_t core = Logger::CoreLog().Index;
    std::vector<ListOrder> order;
    order.reserve(count);
    for (uint16_t channel = 0; channel < count; ++channel)
        order.push_back({channel != core, ToLowerAscii(Logger::Log::GetChannelName(channel)), channel});
    std::ranges::sort(order);
    m_Rows.reserve(count);
    for (const ListOrder& entry : order)
        AppendRow(entry.Channel);
}

void LogChannelPopover::AppendRow(uint16_t channel)
{
    std::unique_ptr<UIElement> element = Editor::ControlLayoutLoader::Load(kRowLayoutAssetPath, kRowRequiredIds);
    if (!element)
        return;
    Row row;
    row.Channel = channel;
    row.Name = Logger::Log::GetChannelName(channel);
    row.Element = element.get();
    row.Shown = dynamic_cast<Toggle*>(element->FindById(HashStringId("LogChannelShown")));
    row.NameLabel = dynamic_cast<Label*>(element->FindById(HashStringId("LogChannelName")));
    row.Level = dynamic_cast<Dropdown*>(element->FindById(HashStringId("LogChannelLevel")));
    if (!row.Shown || !row.NameLabel || !row.Level)
    {
        Logger::Log::Error("Log panel: {} needs the LogChannelShown toggle, the LogChannelName label and the "
                           "LogChannelLevel list.",
                           kRowLayoutAssetPath);
        return;
    }
    row.Shown->SetTooltip("Show or hide the lines of this channel in the Log panel.");
    row.Level->SetTooltip("The lowest level this channel logs. Default uses the editor's log level, or this "
                           "channel's own default level when that is higher.");
    row.NameLabel->SetText(row.Name);
    row.Level->SetOptionsFromLabels(LevelLabels(channel), LevelIndex(channel));
    SyncRow(row);
    row.Shown->SetOnValueChanged([this, channel](const bool& shown) { OnShownChanged(channel, shown); });
    row.Level->SetOnValueChanged(
        [this, channel, level = row.Level](const std::string&) { OnLevelSelected(channel, level->GetSelectedIndex()); });
    // A scroll view takes its rows through AddContent, which a layout file cannot name.
    m_Scroll->AddContent(std::move(element));
    m_Rows.push_back(std::move(row));
}

void LogChannelPopover::SyncRow(const Row& row)
{
    const bool shown = m_View.GetModel().IsChannelShown(row.Channel);
    if (row.Shown->GetValue() != shown)
        row.Shown->SetValueWithoutNotify(shown);
    SetClass(*row.Element, kRowOffClass, !shown);
}

void LogChannelPopover::OnQueryChanged(const std::string& query)
{
    m_Search->RemoveClass(kAwaitingInputClass);
    ApplyQuery(query);
}

void LogChannelPopover::ApplyQuery(const std::string& query)
{
    // Escape reverts the field through its value-changed path, which leaves the clear button and the placeholder
    // as they were.
    m_Search->RefreshVisualState();
    size_t listed = 0;
    for (const Row& row : m_Rows)
    {
        const bool matches = ContainsIgnoreCase(row.Name, query);
        SetClass(*row.Element, kHiddenClass, !matches);
        listed += matches ? 1 : 0;
    }
    if (!m_Count)
        return;
    SetClass(*m_Columns, kHiddenClass, listed == 0);
    m_ShowAll->SetEnabled(listed > 0);
    m_HideAll->SetEnabled(listed > 0);
    SetClass(*m_Count, kHiddenClass, query.empty());
    if (query.empty())
        return;
    m_Count->SetText(listed == 0 ? std::string("No channel matches")
                                 : std::to_string(listed) + " of " + std::to_string(m_Rows.size()) + " channels");
}

void LogChannelPopover::SetListedChannelsShown(bool shown)
{
    std::vector<uint16_t> channels;
    channels.reserve(m_Rows.size());
    for (const Row& row : m_Rows)
    {
        if (!row.Element->HasClass(kHiddenClass))
            channels.push_back(row.Channel);
    }
    m_View.SetChannelsShown(channels, shown);
    for (const Row& row : m_Rows)
        SyncRow(row);
}

void LogChannelPopover::OnShownChanged(uint16_t channel, bool shown)
{
    m_View.SetChannelsShown({&channel, 1}, shown);
    for (const Row& row : m_Rows)
    {
        if (row.Channel == channel)
            SyncRow(row);
    }
}

void LogChannelPopover::OnLevelSelected(uint16_t channel, int index)
{
    if (index <= 0)
        Logger::Log::SetChannelLevel(channel, std::nullopt);
    else if (static_cast<size_t>(index) <= kLevels.size())
        Logger::Log::SetChannelLevel(channel, kLevels[static_cast<size_t>(index) - 1]);
}

void LogChannelPopover::Place()
{
    UIManager* manager = GetOwnerManager();
    UIElement* root = manager ? manager->GetRootElement() : nullptr;
    if (!m_Anchor || !root)
        return;
    const Mathematics::Rect anchor = m_Anchor->GetWindowRect();
    const Mathematics::Rect window = root->GetWindowRect();
    const Mathematics::Rect origin = GetWindowRect();
    const float width = m_Panel->GetLayoutWidth();
    const float height = m_Panel->GetLayoutHeight();

    // The side is chosen once per Open, so the list does not jump when a search shrinks it.
    const UI::Layout::AnchoredPopupPosition placed = UI::Layout::PlacePopupBelowOrAbove(
        {window.X, window.Y, window.Width, window.Height}, {anchor.X, anchor.Y, anchor.Width, anchor.Height}, width,
        height, kEdgeMargin, kAnchorGap, m_Side);
    if (height > 0.0f)
        m_Side = placed.Side;

    const float localX = placed.Position.X - origin.X;
    const float localY = placed.Position.Y - origin.Y;
    if (std::fabs(localX - m_PlacedX) < kPlacementEpsilon && std::fabs(localY - m_PlacedY) < kPlacementEpsilon)
        return;
    m_PlacedX = localX;
    m_PlacedY = localY;
    m_Panel->Overrides()
        .Set(Style::PositionLeft, StyleLength::Px(localX))
        .Set(Style::PositionTop, StyleLength::Px(localY));
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

} // namespace GameEngine
