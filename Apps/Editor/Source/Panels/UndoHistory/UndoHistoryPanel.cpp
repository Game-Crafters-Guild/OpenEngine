#include "Panels/UndoHistory/UndoHistoryPanel.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/UIElement.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Button.h"
#include "UI/UIEvents.h"
#include "UI/ControlLayoutLoader.h"


#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>

namespace GameEngine {

namespace {

std::string FormatTimestamp(std::chrono::system_clock::time_point tp)
{
    if (tp == std::chrono::system_clock::time_point{})
        return {};
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[9]; // "HH:MM:SS\0"
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

} // namespace

UndoHistoryPanel::UndoHistoryPanel()
    : DockPanel("Undo History")
{
    BuildUI();
}

UndoHistoryPanel::~UndoHistoryPanel() = default;

void UndoHistoryPanel::SetUndoRedoService(Editor::UndoRedoService* undo)
{
    if (m_Undo == undo)
        return;
    m_HistoryChanged.Reset();
    m_Undo = undo;
    if (m_Undo)
        m_HistoryChanged = m_Undo->HistoryChanged().Subscribe([this] { Rebuild(); });
    Rebuild();
}

void UndoHistoryPanel::BuildUI()
{
    constexpr std::string_view required[] = {"HistoryScroll", "HistoryList", "HistoryCount", "HistoryUndo", "HistoryRedo", "HistoryClear"};
    auto root = Editor::ControlLayoutLoader::Load("UI/controls/UndoHistory/UndoHistory.uxml", required);
    if (!root)
        return;
    root->RequestSubtreeStyleAssetPath("UI/controls/UndoHistory/UndoHistory.css", "editor");
    m_List = root->FindById(HashStringId("HistoryList"));
    m_ScrollView = dynamic_cast<ScrollView*>(root->FindById(HashStringId("HistoryScroll")));
    if (m_ScrollView && m_List)
        m_ScrollView->AddContent(m_ScrollView->TakeChild(m_List));
    m_CountLabel = dynamic_cast<Label*>(root->FindById(HashStringId("HistoryCount")));
    m_BackBtn = dynamic_cast<Button*>(root->FindById(HashStringId("HistoryUndo")));
    m_ForwardBtn = dynamic_cast<Button*>(root->FindById(HashStringId("HistoryRedo")));
    auto* clear = dynamic_cast<Button*>(root->FindById(HashStringId("HistoryClear")));
    if (m_BackBtn)
        m_BackBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { if (m_Undo) m_Undo->Undo(); });
    if (m_ForwardBtn)
        m_ForwardBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { if (m_Undo) m_Undo->Redo(); });
    if (clear)
        clear->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { if (m_Undo) m_Undo->Clear(); });
    AddChild(std::move(root));
    Rebuild();
}

void UndoHistoryPanel::AddRevision(std::uint64_t id, const std::string& label, std::size_t depth,
                                    const std::string& tooltip)
{
    auto row = std::make_unique<Button>();
    row->SetId("HistoryNode" + std::to_string(id));
    row->AddClass("history-revision");
    row->SetText(std::string(depth * 2, ' ') + label);
    row->SetTooltip(tooltip);
    if (m_Undo && id == m_Undo->GetCurrentNodeId())
        row->AddClass("history-current");
    row->RegisterEventHandler(kEventButtonClick, [this, id](UIEvent& event)
    {
        if (m_Undo)
            m_Undo->Checkout(id);
        event.Stop();
    });
    m_List->AddChild(std::move(row));
}

void UndoHistoryPanel::OnOwnerManagerChanged(UIManager* owner)
{
    DockPanel::OnOwnerManagerChanged(owner);
    m_RebuildQueued = false;
    if (owner)
        Rebuild();
}

void UndoHistoryPanel::Rebuild()
{
    if (!m_List)
        return;
    if (UIElement::IsInEventDispatch())
    {
        if (GetOwnerManager() && !m_RebuildQueued)
            m_RebuildQueued = PostSafeAction([this]
            {
                m_RebuildQueued = false;
                Rebuild();
            });
        return;
    }
    m_List->RemoveAllChildren();

    if (m_Undo)
    {
        AddRevision(m_Undo->GetRootNodeId(), "Starting State", 0, "Restore the earliest retained state");
        std::vector<std::pair<std::uint64_t, std::size_t>> parents{{m_Undo->GetRootNodeId(), 0}};
        const auto& nodes = m_Undo->GetHistoryNodes();
        while (!parents.empty())
        {
            const auto [id, depth] = parents.back();
            parents.pop_back();
            if (id != m_Undo->GetRootNodeId())
            {
                const auto found = std::find_if(nodes.begin(), nodes.end(), [id](const auto& node) { return node.Id == id; });
                const std::string label = "#" + std::to_string(id) + " " + found->GetName();
                const std::string tooltip = "Branch from #" + std::to_string(found->Parent) + " at " + FormatTimestamp(found->Timestamp);
                AddRevision(id, label, depth, tooltip);
            }
            for (auto node = nodes.rbegin(); node != nodes.rend(); ++node)
            {
                if (node->Parent == id)
                    parents.emplace_back(node->Id, depth + 1);
            }
        }
        if (const char* pending = m_Undo->GetPendingName())
        {
            auto row = std::make_unique<Label>();
            row->SetId("HistoryPending");
            row->AddClass("history-pending");
            row->SetText(std::string(pending) + " (pending)");
            row->SetTooltip("Recorded when the next action begins. Undo restores the starting value.");
            m_List->AddChild(std::move(row));
        }
    }
    if (m_Undo && !m_Undo->GetLastError().empty())
    {
        auto error = std::make_unique<Label>();
        error->AddClass("history-pending");
        error->SetText(m_Undo->GetLastError());
        m_List->AddChild(std::move(error));
    }
    if (m_CountLabel)
    {
        const std::size_t count = m_Undo ? m_Undo->GetHistoryNodes().size() : 0;
        m_CountLabel->SetText(std::to_string(count) + (count == 1 ? " Action" : " Actions"));
    }
    if (m_BackBtn)
        m_BackBtn->SetEnabled(m_Undo && m_Undo->CanUndo());
    if (m_ForwardBtn)
        m_ForwardBtn->SetEnabled(m_Undo && m_Undo->CanRedo());
}

} // namespace GameEngine
