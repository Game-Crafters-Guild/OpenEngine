#include "Panels/LogPanel.h"

#include "Editor/EditorPaths.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Logger/Logger.h"
#include "Panels/BacktraceFrameView.h"
#include "Panels/LogView.h"
#include "Platform/Clipboard.h"
#include "Platform/ContextMenu.h"
#include "ScriptEditor/SourceLocation.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Splitter.h"
#include "UI/Controls/TextArea.h"
#include "UI/Controls/TransientNotice.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/EditorIcons.h"
#include "UI/EditorSearchBars.h"
#include "UI/PanelSearchBar.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UIPlatform.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

namespace GameEngine {

namespace {

constexpr const char* kLayoutAssetPath = "UI/panels/LogPanel/LogPanel.uxml";
constexpr const char* kStyleAssetPath = "UI/panels/LogPanel/LogPanel.css";
constexpr const char* kHiddenClass = "hidden";
constexpr const char* kFilterOnClass = "active";
constexpr const char* kFilterOffClass = "log-filter-off";
constexpr const char* kFilterEmptyClass = "log-filter-empty";
constexpr const char* kSideBySideClass = "log-split--side-by-side";
constexpr const char* kSearchBarClass = "log-search-bar";

constexpr uint32_t kCopyCommandId = 1;
constexpr uint32_t kOpenSourceCommandId = 3;
constexpr uint32_t kClearCommandId = 4;

/// The detail pane sits beside the list when the panel is wider than tall by this aspect, and goes
/// back below it under the lower one: the gap keeps a resize at the boundary from flipping each frame.
constexpr float kSideBySideEnterAspect = 1.1f;
constexpr float kSideBySideExitAspect = 0.9f;

/// Indexed by LogModel::Severity: the toggle's name, the lines its tooltip names and its glyph class
/// in the detail header.
constexpr std::array<const char*, LogModel::kSeverityCount> kSeverityNames{"Debug", "Info", "Warnings", "Errors"};
constexpr std::array<const char*, LogModel::kSeverityCount> kSeverityLineNames{"debug", "info", "warning", "error"};
constexpr std::array<const char*, LogModel::kSeverityCount> kSeverityTitles{"Debug", "Info", "Warning", "Error"};
constexpr std::array<const char*, LogModel::kSeverityCount> kDetailGlyphClasses{
    "log-row-glyph-debug", "log-row-glyph-info", "log-row-glyph-warning", "log-row-glyph-error"};

// One sink shared by every Log panel; the logger has no way to remove a sink, so it stays added and
// a closed panel only unregisters its callback.
Logger::CallbackSink* s_CallbackSink = nullptr;

Logger::CallbackSink& GetOrCreateCallbackSink()
{
    if (!s_CallbackSink)
    {
        Logger::CallbackSink::Config config;
        config.MinLevel = Logger::LogLevel::Trace;
        auto sink = std::make_unique<Logger::CallbackSink>(config);
        s_CallbackSink = sink.get();
        Logger::Log::AddSink(std::move(sink));
    }
    return *s_CallbackSink;
}

template <typename T>
T* FindIn(UIElement& root, const char* id)
{
    return dynamic_cast<T*>(root.FindById(HashStringId(id)));
}

LogModel::SearchField SearchFieldFromKey(const std::string& key)
{
    if (key == "message")
        return LogModel::SearchField::Message;
    if (key == "level")
        return LogModel::SearchField::Level;
    if (key == "source")
        return LogModel::SearchField::Source;
    if (key == "function")
        return LogModel::SearchField::Function;
    return LogModel::SearchField::All;
}

std::string LinesCopied(size_t count)
{
    return count == 1 ? std::string("Copied 1 line") : "Copied " + std::to_string(count) + " lines";
}

/// An entry is the line with its source and stack trace, which is what Copy puts on the clipboard.
std::string EntriesCopied(size_t count)
{
    return count == 1 ? std::string("Copied 1 entry") : "Copied " + std::to_string(count) + " entries";
}

/// The detail header's button text: the file name and line the button opens.
std::string SourceButtonLabel(const LogModel::SourceToOpen& source)
{
    std::string label = std::filesystem::path(source.File).filename().string();
    if (source.Line > 0)
        label += ":" + std::to_string(source.Line);
    return label;
}

/// What a click on the source does: a C# file opens in the script editor at the line; any other file
/// opens in the external editor, which receives no line.
std::string SourceButtonTooltip(const LogModel::SourceToOpen& source)
{
    if (Editor::IsCSharpSource(source.File))
        return "Open " + source.File + " at line " + std::to_string(source.Line) + " in the script editor.";
    return "Open " + source.File + " in the external editor. It opens at the top of the file, not at the line.";
}

} // namespace

LogPanel::LogPanel()
    : DockPanel("Log")
{
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    BuildFromLayout();
    m_CallbackId =
        GetOrCreateCallbackSink().RegisterCallback([this](const Logger::LogMessage& message) { OnLogMessage(message); });
}

LogPanel::~LogPanel()
{
    if (m_SearchBar)
        EditorSearchBars::Unregister(m_SearchBar);
    if (s_CallbackSink && m_CallbackId != 0)
        s_CallbackSink->UnregisterCallback(m_CallbackId);
}

void LogPanel::BuildFromLayout()
{
    const std::filesystem::path path = Editor::GetEditorGlobalPaths().installAssetsRoot / kLayoutAssetPath;
    std::unique_ptr<UIElement> root;
    if (!UIParsing::XMLParser::ParseLayoutFromFile(path.string(), root) || !root)
    {
        Logger::Log::Error("Log panel: {} did not load, so the panel is empty. Check that it is staged under the "
                           "editor asset mount.",
                           path.generic_string());
        return;
    }

    m_LogView = FindIn<LogView>(*root, "LogView");
    m_Notice = FindIn<EditorUI::TransientNotice>(*root, "LogCopyNotice");
    m_SplitBody = FindIn<UIElement>(*root, "LogSplit");
    m_ListPane = FindIn<WeightedPane>(*root, "LogListPane");
    m_DetailSplitter = FindIn<Splitter>(*root, "LogDetailSplitter");
    m_DetailPane = FindIn<WeightedPane>(*root, "LogDetailPane");
    m_DetailGlyph = FindIn<UIElement>(*root, "LogDetailGlyph");
    m_DetailSeverity = FindIn<Label>(*root, "LogDetailSeverity");
    m_DetailTime = FindIn<Label>(*root, "LogDetailTime");
    m_DetailSource = FindIn<Button>(*root, "LogDetailSource");
    m_DetailMessage = FindIn<TextArea>(*root, "LogDetailMessage");
    m_DetailTraceTitle = FindIn<UIElement>(*root, "LogDetailTraceTitle");
    m_DetailFrames = FindIn<UIElement>(*root, "LogDetailFrames");
    m_PauseButton = FindIn<Button>(*root, "LogPause");
    auto* detailScroll = FindIn<ScrollView>(*root, "LogDetailScroll");
    UIElement* detailBody = FindIn<UIElement>(*root, "LogDetailBody");
    UIElement* searchHost = FindIn<UIElement>(*root, "LogSearchHost");
    if (!m_LogView || !m_Notice || !m_SplitBody || !m_ListPane || !m_DetailSplitter || !m_DetailPane ||
        !m_DetailMessage || !m_DetailFrames || !m_PauseButton || !detailScroll || !detailBody || !searchHost)
    {
        Logger::Log::Error("Log panel: {} is missing an element the panel needs; check its ids against LogPanel.cpp.",
                           kLayoutAssetPath);
        m_LogView = nullptr;
        return;
    }

    // A scroll view takes its content through its viewport, which a layout file cannot name.
    if (UIElement* bodyParent = detailBody->GetParent())
        detailScroll->GetViewport()->AddChild(bodyParent->TakeChild(detailBody));
    m_DetailMessage->SetReadOnly(true);

    WireFilterToggle(*root, LogModel::Severity::Error, "LogFilterError");
    WireFilterToggle(*root, LogModel::Severity::Warning, "LogFilterWarning");
    WireFilterToggle(*root, LogModel::Severity::Info, "LogFilterInfo");
    WireFilterToggle(*root, LogModel::Severity::Debug, "LogFilterDebug");
    if (auto* clear = FindIn<Button>(*root, "LogClear"))
    {
        clear->SetTooltip("Remove every line from the log.");
        clear->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ClearLog(); });
    }
    m_PauseButton->SetTooltip("Stop the list from moving while you read; new lines wait until you resume.");
    m_PauseButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { TogglePause(); });
    if (auto* copyAll = FindIn<Button>(*root, "LogCopyAll"))
    {
        copyAll->SetTooltip("Copy every line the filters show.");
        copyAll->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { CopyAllVisible(); });
    }
    if (m_DetailSource)
        m_DetailSource->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OpenSelectedSource(); });
    BuildSearchBar(*searchHost);

    m_LogView->SetOnSelectionChanged([this](const LogModel::Entry* entry) { OnSelectionChanged(entry); });
    m_LogView->SetOnEntryActivated([this](const LogModel::Entry&) { OpenSelectedSource(); });
    m_LogView->SetOnContextMenu([this](float x, float y) { ShowContextMenu(x, y); });
    m_LogView->SetOnCopyShortcut([this]() { CopySelected(); });
    m_LogView->SetOnEntriesChanged([this]() { SyncFilterToggles(); });

    AddChild(std::move(root));
    SyncFilterToggles();
}

void LogPanel::BuildSearchBar(UIElement& host)
{
    PanelSearchBarBuilt built = BuildPanelSearchBar(
        "log-search-field",
        [this](const std::string& text)
        {
            if (m_LogView)
                m_LogView->SetSearch(text);
        },
        {},
        {{"all", "All fields"}, {"message", "Message"}, {"level", "Severity"}, {"source", "Source"},
         {"function", "Function"}},
        [this](const std::string& key)
        {
            if (m_LogView)
                m_LogView->SetSearchField(SearchFieldFromKey(key));
        });
    m_SearchBar = built.RootPtr;
    m_SearchBar->AddClass(kSearchBarClass);
    host.AddChild(std::move(built.Root));
    // The search stays in the toolbar wherever panel searches sit; it takes only the focus outline.
    EditorSearchBars::RegisterFocusTarget(m_SearchBar);
}

void LogPanel::WireFilterToggle(UIElement& root, LogModel::Severity severity, const char* id)
{
    auto* toggle = FindIn<Button>(root, id);
    if (!toggle)
        return;
    m_FilterToggles[static_cast<size_t>(severity)] = toggle;
    toggle->SetTooltip(std::string("Show or hide ") + kSeverityLineNames[static_cast<size_t>(severity)] +
                       " lines. The number counts the lines that match the search.");
    toggle->RegisterEventHandler(kEventButtonClick, [this, severity](UIEvent&) { ToggleSeverity(severity); });
}

void LogPanel::ToggleSeverity(LogModel::Severity severity)
{
    if (!m_LogView)
        return;
    m_LogView->SetSeverityShown(severity, !m_LogView->GetModel().IsSeverityShown(severity));
}

void LogPanel::SyncFilterToggles()
{
    if (!m_LogView)
        return;
    const LogModel& model = m_LogView->GetModel();
    for (size_t index = 0; index < LogModel::kSeverityCount; ++index)
    {
        Button* toggle = m_FilterToggles[index];
        if (!toggle)
            continue;
        const auto severity = static_cast<LogModel::Severity>(index);
        std::string text = kSeverityNames[index];
        text += "  ";
        const size_t count = model.CountMatchingSearch(severity);
        text += std::to_string(count);
        if (toggle->GetText() != text)
            toggle->SetText(text);
        if ((count == 0) != toggle->HasClass(kFilterEmptyClass))
        {
            if (count == 0)
                toggle->AddClass(kFilterEmptyClass);
            else
                toggle->RemoveClass(kFilterEmptyClass);
        }
        const bool shown = model.IsSeverityShown(severity);
        if (shown == toggle->HasClass(kFilterOnClass))
            continue;
        toggle->RemoveClass(shown ? kFilterOffClass : kFilterOnClass);
        toggle->AddClass(shown ? kFilterOnClass : kFilterOffClass);
    }
}

void LogPanel::TogglePause()
{
    if (!m_LogView)
        return;
    const bool paused = !m_LogView->IsPaused();
    m_LogView->SetPaused(paused);
    m_PauseButton->SetText(paused ? "Resume" : "Pause");
}

void LogPanel::Update()
{
    SyncDetailLayoutMode();
    if (m_LogView)
        m_LogView->FlushPendingMessages();
    if (m_Notice)
        m_Notice->Tick(std::chrono::steady_clock::now());
}

void LogPanel::ClearLog()
{
    if (m_LogView)
        m_LogView->Clear();
}

void LogPanel::OnLogMessage(const Logger::LogMessage& message)
{
    if (m_LogView)
        m_LogView->AddMessage(message);
}

void LogPanel::OnSelectionChanged(const LogModel::Entry* entry)
{
    // Selection changes inside the list's pointer dispatch, where the frame rows cannot be replaced:
    // the pane is rebuilt after it, from the entry's sequence, which outlives a dropped entry.
    const uint64_t sequence = entry ? entry->Sequence : 0;
    PostSafeAction(
        [this, sequence]()
        {
            const LogModel::Entry* selected = m_LogView ? m_LogView->GetModel().FindEntry(sequence) : nullptr;
            if (selected)
                ShowDetail(*selected);
            else
                HideDetail();
        });
}

void LogPanel::ShowDetail(const LogModel::Entry& entry)
{
    const size_t severity = static_cast<size_t>(LogModel::SeverityOf(entry.Level));
    if (m_DetailGlyph)
    {
        for (const char* name : kDetailGlyphClasses)
            m_DetailGlyph->RemoveClass(name);
        m_DetailGlyph->AddClass(kDetailGlyphClasses[severity]);
    }
    if (m_DetailSeverity)
        m_DetailSeverity->SetText(kSeverityTitles[severity]);
    if (m_DetailTime)
        m_DetailTime->SetText(LogModel::FormatTime(entry));
    if (m_DetailSource)
    {
        if (const std::optional<LogModel::SourceToOpen> source = LogModel::FindSourceToOpen(entry))
        {
            m_DetailSource->SetText(SourceButtonLabel(*source));
            m_DetailSource->SetTooltip(SourceButtonTooltip(*source));
            m_DetailSource->RemoveClass(kHiddenClass);
        }
        else
        {
            m_DetailSource->AddClass(kHiddenClass);
        }
    }
    m_DetailMessage->SetValue(entry.Text);

    m_DetailFrames->RemoveAllChildren();
    const bool hasTrace = entry.Backtrace && !entry.Backtrace->empty();
    if (m_DetailTraceTitle)
    {
        if (hasTrace)
            m_DetailTraceTitle->RemoveClass(kHiddenClass);
        else
            m_DetailTraceTitle->AddClass(kHiddenClass);
    }
    if (hasTrace)
    {
        const std::vector<Logger::BacktraceFrame>& frames = *entry.Backtrace;
        for (size_t index = 0; index < frames.size(); ++index)
            m_DetailFrames->AddChild(BuildBacktraceFrameRow(
                index, frames[index], [this](const std::string& file, int line) { OpenSource(file, line); }));
    }

    if (m_DetailPane->HasClass(kHiddenClass))
    {
        m_ListPane->SetFlexWeight(m_VisibleListWeight);
        m_DetailPane->SetFlexWeight(m_VisibleDetailWeight);
        m_DetailPane->RemoveClass(kHiddenClass);
        m_DetailSplitter->RemoveClass(kHiddenClass);
        // The list shrinks as the pane opens, which can cover a row selected near its bottom.
        m_LogView->RevealSelectionAfterLayout();
    }
    m_SplitBody->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void LogPanel::HideDetail()
{
    if (!m_DetailPane || m_DetailPane->HasClass(kHiddenClass))
        return;
    m_VisibleListWeight = m_ListPane->GetFlexWeight();
    m_VisibleDetailWeight = m_DetailPane->GetFlexWeight();
    m_DetailPane->AddClass(kHiddenClass);
    m_DetailSplitter->AddClass(kHiddenClass);
    m_DetailPane->SetFlexWeight(0.0f);
    m_ListPane->SetFlexWeight(1.0f);
    m_DetailFrames->RemoveAllChildren();
    m_SplitBody->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void LogPanel::SyncDetailLayoutMode()
{
    if (!m_SplitBody || !m_DetailSplitter)
        return;
    const float width = GetLayoutWidth();
    const float height = GetLayoutHeight();
    if (width <= 1.0f || height <= 1.0f)
        return;

    const float aspect = width / height;
    bool sideBySide = aspect > 1.0f;
    if (m_DetailLayoutInitialized)
        sideBySide = m_DetailSideBySide ? aspect >= kSideBySideExitAspect : aspect > kSideBySideEnterAspect;
    if (m_DetailLayoutInitialized && sideBySide == m_DetailSideBySide)
        return;

    m_DetailLayoutInitialized = true;
    m_DetailSideBySide = sideBySide;
    if (sideBySide)
    {
        m_SplitBody->AddClass(kSideBySideClass);
        m_DetailSplitter->RemoveClass("col");
        m_DetailSplitter->AddClass("row");
    }
    else
    {
        m_SplitBody->RemoveClass(kSideBySideClass);
        m_DetailSplitter->RemoveClass("row");
        m_DetailSplitter->AddClass("col");
    }
    m_SplitBody->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    m_SplitBody->RequestRelayout();
}

void LogPanel::CopySelected()
{
    if (!m_LogView)
        return;
    const std::vector<const LogModel::Entry*> entries = m_LogView->GetSelectedEntries();
    std::string text;
    for (const LogModel::Entry* entry : entries)
    {
        // A blank line between entries.
        if (!text.empty())
            text += '\n';
        text += LogModel::FormatDetails(*entry);
    }
    CopyText(text, EntriesCopied(entries.size()), /*besideSelection=*/true);
}

void LogPanel::CopyAllVisible()
{
    if (!m_LogView)
        return;
    CopyText(m_LogView->GetModel().FormatVisibleLines(), LinesCopied(m_LogView->GetModel().GetVisibleCount()),
             /*besideSelection=*/false);
}

void LogPanel::CopyText(const std::string& text, const std::string& confirmation, bool besideSelection)
{
    if (text.empty())
        return;
    Platform::SetClipboardText(text.c_str());
    if (m_Notice)
        m_Notice->Show(confirmation, std::chrono::steady_clock::now(),
                       besideSelection ? FindNoticeTopBesideSelection() : std::nullopt);
}

std::optional<float> LogPanel::FindNoticeTopBesideSelection() const
{
    const UIElement* host = m_Notice ? m_Notice->GetParent() : nullptr;
    const std::optional<Mathematics::Rect> row = m_LogView ? m_LogView->GetSelectedRowWindowRect() : std::nullopt;
    if (!host || !row)
        return std::nullopt;
    // Centered on the row's height, kept inside the list; the sheet puts it at the row's right end.
    const Mathematics::Rect bounds = host->GetWindowRect();
    const float noticeHeight = m_Notice->GetLayoutHeight();
    const float centered = row->Y + (row->Height - noticeHeight) * 0.5f - bounds.Y;
    return std::clamp(centered, 0.0f, std::max(0.0f, bounds.Height - noticeHeight));
}

void LogPanel::OpenSelectedSource()
{
    const LogModel::Entry* entry = m_LogView ? m_LogView->GetSelectedEntry() : nullptr;
    if (!entry)
        return;
    if (const std::optional<LogModel::SourceToOpen> source = LogModel::FindSourceToOpen(*entry))
        OpenSource(source->File, source->Line);
}

void LogPanel::OpenSource(const std::string& file, int line)
{
    if (!file.empty() && m_OnOpenSourceFile)
        m_OnOpenSourceFile(file, line);
}

void LogPanel::ShowContextMenu(float x, float y)
{
    const LogModel::Entry* entry = m_LogView ? m_LogView->GetSelectedEntry() : nullptr;
    if (!entry)
        return;
    // The window that hosts this panel, which is not the main window when the panel floats.
    UIManager* manager = GetOwnerManager();
    UI::IPlatformApi* platform = manager ? manager->GetPlatform() : nullptr;
    Platform::Window* window = platform ? platform->GetNativeWindow() : nullptr;
    if (!window)
        return;
    if (!m_ContextMenu)
        m_ContextMenu = CreateContextMenu();
    if (!m_ContextMenu)
        return;

    m_ContextMenu->Clear();
    m_ContextMenu->SetCommandHandler([this](uint32_t commandId) { OnContextMenuCommand(commandId); });
    const bool canOpen = LogModel::FindSourceToOpen(*entry).has_value();
    ContextMenuBuilder builder;
    builder.AddItem("Copy", kCopyCommandId, MenuItemFlag_None, 0, EditorIcons::kCopy, "Ctrl+C");
    if (canOpen)
        builder.AddItem("Open Source", kOpenSourceCommandId, MenuItemFlag_None, 0, EditorIcons::kFolderOpen);
    builder.AddItem("-", 0);
    builder.AddItem("Clear Log", kClearCommandId, MenuItemFlag_None, 0, EditorIcons::kTrash);
    builder.Build(m_ContextMenu.get(), /*SortByPriorityThenPath=*/false);
    m_ContextMenu->Show(window, static_cast<int>(x), static_cast<int>(y));
}

void LogPanel::OnContextMenuCommand(uint32_t commandId)
{
    switch (commandId)
    {
    case kCopyCommandId:
        CopySelected();
        break;
    case kOpenSourceCommandId:
        OpenSelectedSource();
        break;
    case kClearCommandId:
        ClearLog();
        break;
    default:
        break;
    }
}

} // namespace GameEngine
