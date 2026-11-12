#include "Panels/Scribe/ScribePanel.h"

#include "AIECS/AiRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetRegistry.h"
#include "Editor/Registries/AssetOpenerRegistry.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "EditorPanelIds.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Panels/ConfirmActionModal.h"
#include "Panels/SaveSceneChangesModal.h"
#include "Panels/Scribe/ScribeWorkspace.h"
#include "Types/StringId.h"
#include "Types/StringUtils.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/InspectorActionButton.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TextField.h"
#include "UI/ElementBuilder.h"
#include "UI/StyleUtil.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
using Row = ScribeOutlineRow;

constexpr const char* kNoneOption = "(none)";

// The input of a new consideration when the game registers no input; its row shows the refusal until it does.
constexpr const char* kNewInputName = "Input";

// Adds `sentence` to `text`, after a space when `text` is not empty.
void AppendSentence(std::string& text, const std::string& sentence)
{
    text += (text.empty() ? "" : " ") + sentence;
}

// A modal goes on the window's root, so its backdrop covers the whole window; the anchor holds it when no
// manager owns the anchor.
void AddToWindowRoot(UIElement& anchor, std::unique_ptr<UIElement> modal)
{
    UIManager* manager = anchor.GetOwnerManager();
    UIElement* root = manager ? manager->GetRootElement() : nullptr;
    (root ? *root : anchor).AddChild(std::move(modal));
}

// ScribePanel.css indents the depths 0 to kDeepestIndent; a deeper row takes the deepest indent.
constexpr uint8 kDeepestIndent = 8;

// Below this panel width in logical pixels, the notice gives the count of refusals alone.
constexpr float kNarrowNoticeWidth = 480.0f;

// The class that ScribePanel.css styles for the row's kind; null for a row with no style of its own.
const char* RowKindClass(Row::Kind kind)
{
    switch (kind)
    {
    case Row::Kind::State: return "scribe-row-state";
    case Row::Kind::Group: return "scribe-row-group";
    case Row::Kind::Program:
    case Row::Kind::EnterCondition:
    case Row::Kind::Consideration:
    case Row::Kind::Task:
    case Row::Kind::Transition:
    case Row::Kind::Name: return nullptr;
    }
    return nullptr;
}

// The row as one line: its kind word, then its name.
std::string RowTitle(const Row& row)
{
    const std::string kindWord = ScribeDocument::KindWord(row);
    return kindWord.empty() ? row.Text : kindWord + " " + row.Text;
}

// The whole text of the row, for its tooltip: a narrow panel cuts the row's text with an ellipsis.
std::string RowLine(const Row& row)
{
    return row.Detail.empty() ? RowTitle(row) : RowTitle(row) + " " + row.Detail;
}


bool IsSameDocument(const std::weak_ptr<const ScribeDocument>& shown, const std::shared_ptr<ScribeDocument>& document)
{
    return !shown.owner_before(document) && !document.owner_before(shown);
}

// The options of a name field: "(none)" when `allowNone`, the registered names, and the current name when no
// one registered it, so that the field shows the name that the file holds.
std::vector<std::string> NameOptions(std::vector<std::string> registered, const std::string& current, bool allowNone)
{
    if (!current.empty() && std::find(registered.begin(), registered.end(), current) == registered.end())
        registered.push_back(current);
    if (allowNone)
        registered.insert(registered.begin(), kNoneOption);
    return registered;
}

// The names of the inputs that a file can name now; empty with no registry.
std::vector<std::string> InputNames(const AIECS::AiRegistry* registry)
{
    return registry ? registry->GetInputNames() : std::vector<std::string>{};
}

int IndexOf(const std::vector<std::string>& options, const std::string& value)
{
    const auto found = std::find(options.begin(), options.end(), value);
    return found == options.end() ? 0 : static_cast<int>(found - options.begin());
}

// An inspector field row: the label cell and the field cell that every inspector row has. `tooltip` goes on the
// label; null for none.
void AddFieldRow(UIElement& details, const char* label, std::unique_ptr<UIElement> field,
                 const char* tooltip = nullptr)
{
    UIElement* row = InspectorUI::AddRow(&details);
    InspectorUI::AddLabel(row, label, tooltip);
    InspectorUI::AddFieldContainer(row)->AddChild(std::move(field));
}

// An inspector action row: the action in the field column under an empty label, as the Inspector's lists add an
// item.
void AddActionRow(UIElement& details, std::unique_ptr<UIElement> action)
{
    InspectorUI::AddActionRow(&details, "scribe-action-row")->AddChild(std::move(action));
}

std::unique_ptr<Label> TextLabel(const char* className, std::string text)
{
    return UI::Element<Label>().Class(className).Set(&Label::SetText, std::move(text)).Build();
}

bool IsStateTreeExtension(std::string_view lowercaseExtension)
{
    return GetAssetTypeFromExtension(std::string(lowercaseExtension)) == AssetType::AiStateTree;
}

// The refused rows of the outline, and the names that no module registers, as a list of the kinds and then the
// names in the same order ("1 condition, 1 event and 1 task are not registered: SeesEnemy, Alarm, Walk").
struct RefusalSummary
{
    // The refused rows, without the rows of the Events and Inputs groups.
    size_t Rows = 0;
    // The refused rows whose cause is a name that no module registers.
    size_t NamedRows = 0;
    std::string Names;
    // The refusal of the first refused row whose cause is not an unregistered name.
    std::string FirstOther;
};

RefusalSummary SummarizeRefusals(const std::vector<ScribeOutlineRow>& rows)
{
    static constexpr const char* kKinds[] = {"input", "condition", "event", "task"};
    std::vector<std::string> names[std::size(kKinds)];
    RefusalSummary summary;
    for (const ScribeOutlineRow& row : rows)
    {
        if (row.Refusal.empty() || row.RowKind == ScribeOutlineRow::Kind::Name)
            continue;
        ++summary.Rows;
        if (!row.UnregisteredKind)
        {
            if (summary.FirstOther.empty())
                summary.FirstOther = row.Refusal;
            continue;
        }
        ++summary.NamedRows;
        for (size_t kind = 0; kind < std::size(kKinds); ++kind)
            if (std::string_view(row.UnregisteredKind) == kKinds[kind] &&
                std::find(names[kind].begin(), names[kind].end(), row.UnregisteredName) == names[kind].end())
                names[kind].push_back(row.UnregisteredName);
    }
    std::vector<std::string> counts;
    std::vector<std::string> ordered;
    for (size_t kind = 0; kind < std::size(kKinds); ++kind)
    {
        if (names[kind].empty())
            continue;
        counts.push_back(FormatCount(names[kind].size(), kKinds[kind], std::string(kKinds[kind]) + "s"));
        ordered.insert(ordered.end(), names[kind].begin(), names[kind].end());
    }
    if (ordered.empty())
        return summary;
    std::string list;
    for (size_t index = 0; index < counts.size(); ++index)
        list += (index == 0 ? "" : index + 1 == counts.size() ? " and " : ", ") + counts[index];
    summary.Names = list + (ordered.size() == 1 ? " is" : " are") + " not registered: " + Join(ordered, ", ");
    return summary;
}

bool OpenInScribe(ScribeWorkspace& workspace, const std::filesystem::path& path)
{
    workspace.Open(path);
    EditorPanelRegistry::Get().OpenPanel(kScribePanelId);
    return true;
}
} // namespace

void ScribePanel::Register(UndoRedoService* undo)
{
    const std::shared_ptr<ScribeWorkspace> workspace =
        std::make_shared<ScribeWorkspace>(undo, &AIECS::AiRegistry::Get());

    EditorPanelDescriptor panel;
    panel.PanelId = kScribePanelId;
    panel.Title = "Scribe";
    // A program's outline is tall, so the panel docks in the Inspector's column.
    panel.DefaultDockAnchorPanelId = EditorPanelIds::Inspector;
    panel.Factory = [workspace]() -> std::unique_ptr<UIElement> { return std::make_unique<ScribePanel>(workspace); };
    EditorPanelRegistry::Get().RegisterPanel(std::move(panel));

    AssetOpenerRegistry::Get().Register(
        {"aiStateTree", &IsStateTreeExtension,
         [workspace](const std::filesystem::path& path) { return OpenInScribe(*workspace, path); }});
}

ScribePanel::ScribePanel(std::shared_ptr<ScribeWorkspace> workspace)
    : AssetBoundDockPanel("Scribe", std::string(kAssetSourceAliasEditor), "UI/panels/Scribe/ScribePanel.uxml", {}),
      m_Workspace(std::move(workspace))
{
    AddClass("scribe-panel");
    RequestSubtreeStyleAssetPath("UI/panels/Scribe/ScribePanel.css");
}

ScribePanel::~ScribePanel()
{
    if (UIManager* manager = m_RefreshManager.Get(); manager && m_RefreshToken != 0)
        manager->UnregisterPeriodicRefresh(m_RefreshToken);
    // The modals live on the window's root, which can outlive the panel: they close and their actions drop
    // this panel. The tree is not changed here, because the root can be in its own teardown.
    if (ConfirmActionModal* saveConfirm = m_SaveConfirm.Get())
    {
        saveConfirm->Hide();
        saveConfirm->SetOnConfirm({});
        saveConfirm->SetOnCancel({});
    }
    if (SaveSceneChangesModal* openConfirm = m_OpenConfirm.Get())
    {
        openConfirm->Hide();
        openConfirm->SetOnSave({});
        openConfirm->SetOnDontSave({});
        openConfirm->SetOnCancel({});
    }
}

const std::shared_ptr<ScribeDocument>& ScribePanel::GetDocument() const
{
    return m_Workspace->GetDocument();
}

void ScribePanel::OnOwnerManagerChanged(UIManager* owner)
{
    AssetBoundDockPanel::OnOwnerManagerChanged(owner);
    if (UIManager* previous = m_RefreshManager.Get(); previous && m_RefreshToken != 0)
        previous->UnregisterPeriodicRefresh(m_RefreshToken);
    m_RefreshToken = 0;
    m_RefreshManager = UIManagerRef(owner);
    if (owner)
        m_RefreshToken = owner->RegisterPeriodicRefresh([this]() { Refresh(); });
}

void ScribePanel::OnPostLayout()
{
    AssetBoundDockPanel::OnPostLayout();
    if (const bool narrow = GetLayoutWidth() < kNarrowNoticeWidth; narrow != m_NarrowNotice)
    {
        m_NarrowNotice = narrow;
        RequestBuild();
    }
    if (m_RevealSelectedRow)
        RevealSelectedRow();
}

// Scrolls the outline the least distance that shows the whole selected row. Runs after a layout, because a
// build replaces the rows and their positions are known only then.
void ScribePanel::RevealSelectedRow()
{
    m_RevealSelectedRow = false;
    ScrollView* scroll = m_OutlineScroll.Get();
    const UIElement* content = m_OutlineContent.Get();
    const UIElement* row = m_SelectedRow.Get();
    if (!scroll || !content || !row)
        return;
    const float rowTop = row->GetLayoutY() - content->GetLayoutY();
    const float rowBottom = rowTop + row->GetLayoutHeight();
    const float viewTop = scroll->GetScrollY();
    const float viewHeight = scroll->GetViewportHeight();
    if (rowTop < viewTop)
        scroll->SetScrollY(rowTop);
    else if (rowBottom > viewTop + viewHeight)
        scroll->SetScrollY(rowBottom - viewHeight);
}

void ScribePanel::OnLayoutBound()
{
    BindElements();
}

void ScribePanel::OnLayoutReconciled()
{
    BindElements();
}

void ScribePanel::BindElements()
{
    m_FileName = FindById<Label>("ScribeFileName"_sid);
    m_DirtyMark = FindById<Label>("ScribeDirtyMark"_sid);
    m_SaveButton = FindById<Button>("ScribeSave"_sid);
    m_Empty = FindById<Label>("ScribeEmpty"_sid);
    m_Notice = FindById<Label>("ScribeNotice"_sid);
    m_OutlineScroll = FindById<ScrollView>("ScribeOutline"_sid);
    m_Details = FindById<UIElement>("ScribeDetails"_sid);
    m_OutlineContent = {};
    if (Button* save = m_SaveButton.Get())
        save->SetOnClick([this](UIEvent&) { SaveDocument(AfterSave::Stay); });
    m_NeedsBuild = true;
    Refresh();
}

void ScribePanel::Refresh()
{
    if (!m_Workspace->GetPendingOpen().empty())
        ConfirmPendingOpen();
    const std::shared_ptr<ScribeDocument>& document = GetDocument();
    // A build removes the field that the user drags, so a change of the names waits for the drag end.
    if (document && document->IsPreviewing())
        return;
    if (const AIECS::AiRegistry* names = m_Workspace->GetNames(); names && names->GetVersion() != m_ShownNamesVersion)
    {
        m_ShownNamesVersion = names->GetVersion();
        if (document)
            document->CheckNames();
    }
    const uint64 version = document ? document->GetVersion() : 0;
    const bool sameDocument = IsSameDocument(m_ShownDocument, document);
    if (!m_NeedsBuild && sameDocument && version == m_ShownVersion)
        return;
    if (!sameDocument)
    {
        m_Selected = {};
        m_SaveError.clear();
    }
    m_ShownDocument = document;
    m_ShownVersion = version;
    m_NeedsBuild = false;
    Build();
}

void ScribePanel::RequestBuild()
{
    m_NeedsBuild = true;
    // A row or a field asks from its own event handler: the rebuild that removes it runs after the handler.
    PostSafeAction([this]() { Refresh(); });
}

std::string ScribePanel::NoticeText(bool narrow) const
{
    const ScribeDocument* document = GetDocument().get();
    std::string text;
    if (const std::string& openError = m_Workspace->GetOpenError(); document && !openError.empty())
        AppendSentence(text, "The file did not open: " + openError);
    if (!m_SaveError.empty())
        AppendSentence(text, "The program was not saved: " + m_SaveError);
    if (!document || document->GetRefusals().empty())
        return text;
    const RefusalSummary summary = SummarizeRefusals(m_Rows);
    if (narrow)
    {
        AppendSentence(text, "Refused: " + FormatCount(summary.Rows, "row needs", "rows need") +
                                 " a fix. Select a red row to see its fix.");
        return text;
    }
    if (summary.Rows == 1 && summary.NamedRows == 0)
    {
        AppendSentence(text, "The program is refused: " + summary.FirstOther);
        return text;
    }
    if (summary.NamedRows > 0)
        AppendSentence(text, summary.Names + " (" + FormatCount(summary.NamedRows, "row", "rows") + ").");
    if (const size_t others = summary.Rows - summary.NamedRows; others > 0)
        AppendSentence(text, FormatCount(others, summary.NamedRows > 0 ? "other row needs" : "row needs",
                                         summary.NamedRows > 0 ? "other rows need" : "rows need") +
                                 " a fix.");
    AppendSentence(text, "Select a red row to see its fix.");
    return text;
}

void ScribePanel::Build()
{
    const ScribeDocument* document = GetDocument().get();
    m_Rows = document ? document->BuildOutline() : std::vector<ScribeOutlineRow>{};

    if (Label* name = m_FileName.Get())
        name->SetText(document ? document->GetPath().filename().string() : std::string("No program open"));
    if (Label* dirty = m_DirtyMark.Get())
        UIUtil::SetShown(*dirty, document && document->IsDirty());
    if (Button* save = m_SaveButton.Get())
        save->SetEnabled(document && document->IsDirty());
    if (Label* empty = m_Empty.Get())
    {
        const std::string& openError = m_Workspace->GetOpenError();
        empty->SetText(openError.empty() ? std::string("Open a state-tree program (.scribe) from the Assets panel to "
                                                       "see its states here.")
                                         : "The program did not open: " + openError);
        UIUtil::SetShown(*empty, !document);
    }
    ShowNotice();
    ScrollView* scroll = m_OutlineScroll.Get();
    if (scroll)
        UIUtil::SetShown(*scroll, document != nullptr);
    if (UIElement* details = m_Details.Get())
        UIUtil::SetShown(*details, document != nullptr);

    if (scroll && !m_OutlineContent.Get())
    {
        std::unique_ptr<UIElement> content = UI::Element<UIElement>().Class("scribe-outline-content").Build();
        m_OutlineContent = UIElement::MakeWeakRef(content.get());
        scroll->AddContent(std::move(content));
    }
    if (UIElement* content = m_OutlineContent.Get())
        BuildOutline(*content);
    if (UIElement* details = m_Details.Get())
        BuildDetails(*details);
}

void ScribePanel::BuildOutline(UIElement& content)
{
    content.RemoveAllChildren();
    m_SelectedRow = {};
    for (const ScribeOutlineRow& row : m_Rows)
    {
        const RowKey key{row.RowKind, row.State, row.Item};
        std::unique_ptr<UIElement> element =
            UI::Element<UIElement>()
                .Class("scribe-row", "scribe-depth-" + std::to_string(std::min(row.Depth, kDeepestIndent)))
                .On(kEventMouseDown, [this, key](UIEvent&) { Select(key); })
                .Build();
        element->SetTooltip(RowLine(row));
        if (const char* kindClass = RowKindClass(row.RowKind))
            element->AddClass(kindClass);
        if (const char* kindWord = ScribeDocument::KindWord(row); kindWord[0] != '\0')
            element->AddChild(TextLabel("scribe-row-kind", kindWord));
        element->AddChild(TextLabel("scribe-row-text", row.Text));
        if (!row.Detail.empty())
            element->AddChild(TextLabel("scribe-row-detail", row.Detail));
        if (IsSelected(row))
        {
            element->AddClass("scribe-row-selected");
            m_SelectedRow = MakeWeakRef(element.get());
            m_RevealSelectedRow = true;
        }
        if (!row.Refusal.empty())
        {
            std::unique_ptr<Label> badge = TextLabel("inspector-section-header-badge", "Refused");
            badge->AddClass("inspector-section-header-badge-error");
            badge->AddClass("scribe-row-badge");
            badge->SetTooltip(row.Refusal);
            element->AddChild(std::move(badge));
        }
        content.AddChild(std::move(element));
    }
}

void ScribePanel::BuildDetails(UIElement& details)
{
    details.RemoveAllChildren();
    const auto selected = std::find_if(m_Rows.begin(), m_Rows.end(),
                                       [this](const ScribeOutlineRow& row) { return IsSelected(row); });
    if (selected == m_Rows.end())
    {
        details.AddChild(TextLabel("scribe-details-hint",
                                   "Select a row to edit it. A state edits its name and its enter condition. An option "
                                   "also edits its weight and rank. A consideration edits its input and its range. An "
                                   "event transition edits its event."));
        return;
    }
    details.AddChild(TextLabel("scribe-details-heading", GetDocument()->GetRowPath(*selected)));
    if (!selected->Refusal.empty())
        details.AddChild(std::make_unique<EditorUI::InspectorNotice>(selected->Refusal,
                                                                     EditorUI::InspectorNotice::Kind::Error));
    switch (selected->RowKind)
    {
    case ScribeOutlineRow::Kind::State:
    case ScribeOutlineRow::Kind::EnterCondition:
        AddStateFields(details, selected->State);
        AddOptionFields(details, selected->State);
        AddUtilityFields(details, selected->State);
        break;
    case ScribeOutlineRow::Kind::Consideration:
        AddConsiderationFields(details, selected->State, selected->Item);
        break;
    case ScribeOutlineRow::Kind::Transition: AddTransitionEventField(details, selected->State, selected->Item); break;
    case ScribeOutlineRow::Kind::Program:
    case ScribeOutlineRow::Kind::Task:
    case ScribeOutlineRow::Kind::Group:
    case ScribeOutlineRow::Kind::Name:
        details.AddChild(TextLabel("scribe-details-hint", RowTitle(*selected)));
        break;
    }
}

void ScribePanel::AddStateFields(UIElement& details, uint16 state)
{
    const std::shared_ptr<ScribeDocument>& document = GetDocument();
    const AIECS::AiStateTreeSource& source = document->GetSource();
    std::weak_ptr<ScribeDocument> weak = document;

    auto name = std::make_unique<TextField>();
    name->SetValue(source.States[state].Name);
    TextField* nameField = name.get();
    nameField->SetOnCommit([weak, nameField, state]() {
        if (const std::shared_ptr<ScribeDocument> edited = weak.lock())
            edited->RenameState(state, nameField->GetValue());
    });
    AddFieldRow(details, "Name", std::move(name));

    const uint16 condition = source.Definition.States[state].EnterCondition;
    const std::string current = condition < source.ConditionNames.size() ? source.ConditionNames[condition] : "";
    const AIECS::AiRegistry* names = m_Workspace->GetNames();
    const std::vector<std::string> options =
        NameOptions(names ? names->GetConditionNames() : std::vector<std::string>{}, current, true);
    auto enterIf = std::make_unique<Dropdown>();
    enterIf->SetOptionsFromLabels(options, current.empty() ? 0 : IndexOf(options, current));
    enterIf->SetTooltip("The parent's policy chooses this state only when the condition is true. The list holds the "
                        "conditions that the game registers.");
    enterIf->SetOnValueChanged([weak, state](const std::string& value) {
        if (const std::shared_ptr<ScribeDocument> edited = weak.lock())
            edited->SetEnterCondition(state, value == kNoneOption ? std::string_view() : std::string_view(value));
    });
    AddFieldRow(details, "Enter if", std::move(enterIf));
}

std::unique_ptr<FloatField> ScribePanel::MakeFloatEdit(float value, float minimum, FloatEdit edit)
{
    auto field = std::make_unique<FloatField>();
    field->EnableDragToChange();
    field->SetValueRange(minimum, std::numeric_limits<float>::max());
    field->SetValue(value);
    std::weak_ptr<ScribeDocument> weak = GetDocument();
    field->SetOnValueChanging([this, weak, edit](const float& changing) {
        if (const std::shared_ptr<ScribeDocument> document = weak.lock())
        {
            edit(*document, changing, ScribeDocument::Phase::Preview);
            ShowPreview();
        }
    });
    field->SetOnValueChanged([weak, edit](const float& changed) {
        if (const std::shared_ptr<ScribeDocument> document = weak.lock())
            edit(*document, changed, ScribeDocument::Phase::Commit);
    });
    return field;
}

void ScribePanel::ShowPreview()
{
    const ScribeDocument* document = GetDocument().get();
    if (!document)
        return;
    m_ShownVersion = document->GetVersion();
    m_Rows = document->BuildOutline();
    if (UIElement* content = m_OutlineContent.Get())
        BuildOutline(*content);
    if (Label* dirty = m_DirtyMark.Get())
        UIUtil::SetShown(*dirty, document->IsDirty());
    if (Button* save = m_SaveButton.Get())
        save->SetEnabled(document->IsDirty());
    ShowNotice();
}

void ScribePanel::ShowNotice()
{
    Label* notice = m_Notice.Get();
    if (!notice)
        return;
    const std::string text = NoticeText(m_NarrowNotice);
    notice->SetText(text);
    UIUtil::SetShown(*notice, !text.empty());
    // A narrow notice gives the count alone; its tooltip gives the causes.
    notice->SetTooltip(m_NarrowNotice ? NoticeText(false) : std::string());
}

void ScribePanel::AddOptionFields(UIElement& details, uint16 state)
{
    const std::shared_ptr<ScribeDocument>& document = GetDocument();
    uint16 utility = Row::kNoState;
    const AI::OptionSet::Option* option = document->FindOption(state, utility);
    if (!option)
        return;
    std::weak_ptr<ScribeDocument> weak = document;

    AddFieldRow(details, "Weight",
                MakeFloatEdit(option->Weight, 0.0f,
                              [state](ScribeDocument& edited, float32 value, ScribeDocument::Phase phase)
                              { edited.SetOptionWeight(state, value, phase); }),
                "The option's score is its weight times the product of the scores of its considerations. "
                "0 turns the option off.");

    auto rank = std::make_unique<IntField>();
    rank->SetRange(0, std::numeric_limits<uint8>::max());
    rank->SetValue(option->Rank);
    rank->SetOnValueChanged([weak, state](const int& value) {
        if (const std::shared_ptr<ScribeDocument> edited = weak.lock())
            edited->SetOptionRank(state, static_cast<uint8>(value));
    });
    AddFieldRow(details, "Rank", std::move(rank),
                "Options compete by rank first. The options of the highest rank that score above 0 compete by score; "
                "an option of a lower rank wins only when no option of a higher rank scores above 0.");

    auto add = std::make_unique<EditorUI::InspectorActionButton>("Add Consideration", "inspector-action-add");
    const std::vector<std::string> inputs = InputNames(m_Workspace->GetNames());
    add->SetTooltip(inputs.empty() ? "Adds a consideration. The game registers no input, so the consideration reads "
                                     "'Input' and is refused until you choose an input that the game registers."
                                   : "Adds a consideration that reads the first input that the game registers.");
    const std::string input = inputs.empty() ? std::string(kNewInputName) : inputs.front();
    add->SetOnClick([weak, state, input](UIEvent&) {
        if (const std::shared_ptr<ScribeDocument> edited = weak.lock())
            edited->AddConsideration(state, input);
    });
    AddActionRow(details, std::move(add));
}

void ScribePanel::AddUtilityFields(UIElement& details, uint16 state)
{
    const AIECS::AiStateTreeSource& source = GetDocument()->GetSource();
    const AI::StateTreeDefinition::State& definition = source.Definition.States[state];
    if (definition.Policy != AI::StateTreePolicy::Utility)
        return;
    const AI::OptionSet& options = definition.Utility;
    AddFieldRow(details, "Commitment margin",
                MakeFloatEdit(options.CommitmentMargin, 0.0f,
                              [state](ScribeDocument& edited, float32 value, ScribeDocument::Phase phase)
                              { edited.SetCommitmentMargin(state, value, phase); }),
                "A score value. A challenger of the active option's rank must score above the active option's "
                "score plus this value before it takes over.");

    auto ticks = std::make_unique<IntField>();
    ticks->SetRange(0, std::numeric_limits<int>::max());
    ticks->SetValue(static_cast<int>(std::min<uint32>(options.MinimumTicks, std::numeric_limits<int>::max())));
    std::weak_ptr<ScribeDocument> weak = GetDocument();
    ticks->SetOnValueChanged([weak, state](const int& value) {
        if (const std::shared_ptr<ScribeDocument> edited = weak.lock())
            edited->SetMinimumTicks(state, static_cast<uint32>(value));
    });
    // AiClock::kDefaultRateHz is 20, and a game can set AiClock::RateHz, so the field counts AI ticks.
    AddFieldRow(details, "Minimum ticks", std::move(ticks),
                "The AI ticks that an active option holds against a challenger of the same rank. At the default AI "
                "rate of 20 ticks per second, 20 ticks are 1 second; a game can set another rate.");

    AddFieldRow(details, "Noise margin",
                MakeFloatEdit(options.NoiseMargin, 0.0f,
                              [state](ScribeDocument& edited, float32 value, ScribeDocument::Phase phase)
                              { edited.SetNoiseMargin(state, value, phase); }),
                "A score value. The options that score within this value of the best score share a random draw. "
                "At 0, the best score wins.");
}

void ScribePanel::AddConsiderationFields(UIElement& details, uint16 state, uint16 consideration)
{
    const std::shared_ptr<ScribeDocument>& document = GetDocument();
    uint16 utility = Row::kNoState;
    const AI::OptionSet::Option* option = document->FindOption(state, utility);
    if (!option || consideration >= option->ConsiderationCount)
        return;
    const AIECS::AiStateTreeSource& source = document->GetSource();
    const AI::OptionSet::Consideration& current =
        source.Definition.States[utility].Utility.Considerations[option->FirstConsideration + consideration];
    std::weak_ptr<ScribeDocument> weak = document;

    const std::string currentInput = current.Column < source.InputNames.size() ? source.InputNames[current.Column] : "";
    const std::vector<std::string> inputs = NameOptions(InputNames(m_Workspace->GetNames()), currentInput, false);
    auto input = std::make_unique<Dropdown>();
    input->SetOptionsFromLabels(inputs, IndexOf(inputs, currentInput));
    input->SetTooltip("The value that the consideration reads. The list holds the inputs that the game registers.");
    input->SetOnValueChanged([weak, state, consideration](const std::string& value) {
        if (const std::shared_ptr<ScribeDocument> edited = weak.lock())
            edited->SetConsiderationInput(state, consideration, value);
    });
    AddFieldRow(details, "Input", std::move(input));

    const float32 max = current.Max;
    const float32 min = current.Min;
    AddFieldRow(details, "Minimum",
                MakeFloatEdit(min, std::numeric_limits<float>::lowest(),
                              [state, consideration, max](ScribeDocument& edited, float32 value,
                                                          ScribeDocument::Phase phase)
                              { edited.SetConsiderationRange(state, consideration, value, max, phase); }),
                "The input value that maps to 0 on the curve. The curve gives the score between Minimum and "
                "Maximum. Minimum can be above Maximum: the curve then starts at the higher input value and ends "
                "at the lower one.");
    AddFieldRow(details, "Maximum",
                MakeFloatEdit(max, std::numeric_limits<float>::lowest(),
                              [state, consideration, min](ScribeDocument& edited, float32 value,
                                                          ScribeDocument::Phase phase)
                              { edited.SetConsiderationRange(state, consideration, min, value, phase); }),
                "The input value that maps to 1 on the curve. The curve gives the score between Minimum and "
                "Maximum. Minimum can be above Maximum: the curve then starts at the higher input value and ends "
                "at the lower one.");

    auto curve = TextLabel("scribe-details-value", ScribeDocument::CurveWord(
                                                      source.States[utility].Curves[option->FirstConsideration +
                                                                                    consideration].Preset));
    AddFieldRow(details, "Curve", std::move(curve),
                "The shape of the score between Minimum and Maximum. Edit the curve in the program file (.scribe).");

    auto remove = std::make_unique<EditorUI::InspectorActionButton>("Remove Consideration", "inspector-action-remove");
    remove->SetOnClick([weak, state, consideration](UIEvent&) {
        if (const std::shared_ptr<ScribeDocument> edited = weak.lock())
            edited->RemoveConsideration(state, consideration);
    });
    AddActionRow(details, std::move(remove));
}

void ScribePanel::AddTransitionEventField(UIElement& details, uint16 state, uint16 transition)
{
    const std::shared_ptr<ScribeDocument>& document = GetDocument();
    const AIECS::AiStateTreeSource& source = document->GetSource();
    const AI::StateTreeDefinition::Transition& definition = source.Definition.States[state].Transitions[transition];
    if (definition.Trigger != AI::StateTreeTrigger::Event)
    {
        details.AddChild(TextLabel("scribe-details-hint", "This transition fires on a condition, a completion or a "
                                                          "timeout. Choose an event on an event transition."));
        return;
    }
    const std::string current = definition.Event < source.EventNames.size() ? source.EventNames[definition.Event] : "";
    const AIECS::AiRegistry* names = m_Workspace->GetNames();
    const std::vector<std::string> options =
        NameOptions(names ? names->GetEventTypeNames() : std::vector<std::string>{}, current, false);
    auto event = std::make_unique<Dropdown>();
    event->SetOptionsFromLabels(options, IndexOf(options, current));
    event->SetTooltip("The event that fires this transition. The list holds the event types that the game registers.");
    std::weak_ptr<ScribeDocument> weak = document;
    event->SetOnValueChanged([weak, state, transition](const std::string& value) {
        if (const std::shared_ptr<ScribeDocument> edited = weak.lock())
            edited->SetTransitionEvent(state, transition, value);
    });
    AddFieldRow(details, "Event", std::move(event));
}

void ScribePanel::Select(RowKey key)
{
    m_Selected = key;
    RequestBuild();
}

bool ScribePanel::IsSelected(const ScribeOutlineRow& row) const
{
    return row.RowKind == m_Selected.RowKind && row.State == m_Selected.State && row.Item == m_Selected.Item &&
           row.State != ScribeOutlineRow::kNoState;
}

void ScribePanel::SaveDocument(AfterSave after)
{
    const std::shared_ptr<ScribeDocument>& document = GetDocument();
    if (document && !document->GetRefusals().empty())
        ConfirmSaveWithRefusal(after);
    else
        WriteDocument(after);
}

void ScribePanel::WriteDocument(AfterSave after)
{
    m_SaveError.clear();
    const std::shared_ptr<ScribeDocument>& document = GetDocument();
    const bool written = !document || document->Save(m_SaveError);
    if (after == AfterSave::OpenPending)
    {
        if (written)
            m_Workspace->ConfirmPendingOpen();
        else
            m_Workspace->CancelPendingOpen();
    }
    RequestBuild();
}

void ScribePanel::CancelPendingOpen()
{
    m_Workspace->CancelPendingOpen();
    RequestBuild();
}

void ScribePanel::ConfirmPendingOpen()
{
    const std::shared_ptr<ScribeDocument>& document = GetDocument();
    if (!document)
    {
        m_Workspace->ConfirmPendingOpen();
        return;
    }
    SaveSceneChangesModal* openConfirm = m_OpenConfirm.Get();
    if (!openConfirm)
    {
        auto modal = std::make_unique<SaveSceneChangesModal>("scribe");
        openConfirm = modal.get();
        m_OpenConfirm = MakeWeakRef(openConfirm);
        openConfirm->SetOnSave([this]() { SaveDocument(AfterSave::OpenPending); });
        openConfirm->SetOnDontSave([this]() {
            m_Workspace->ConfirmPendingOpen();
            RequestBuild();
        });
        openConfirm->SetOnCancel([this]() { CancelPendingOpen(); });
        AddToWindowRoot(*this, std::move(modal));
    }
    // The open waits while its prompt or the refused-program confirm of its Save shows.
    const ConfirmActionModal* saveConfirm = m_SaveConfirm.Get();
    if (openConfirm->IsVisible() || (saveConfirm && saveConfirm->IsVisible()))
        return;
    const std::string shown = document->GetPath().filename().string();
    openConfirm->Show("Save the changes to " + shown + "?",
                      "Opening " + m_Workspace->GetPendingOpen().filename().string() + " loses the unsaved changes to " +
                          shown + " unless you save them first.");
}

void ScribePanel::ConfirmSaveWithRefusal(AfterSave after)
{
    ConfirmActionModal* saveConfirm = m_SaveConfirm.Get();
    if (!saveConfirm)
    {
        auto modal = std::make_unique<ConfirmActionModal>();
        saveConfirm = modal.get();
        m_SaveConfirm = MakeWeakRef(saveConfirm);
        AddToWindowRoot(*this, std::move(modal));
    }
    saveConfirm->SetOnConfirm([this, after]() { WriteDocument(after); });
    // A cancel of the Save that the open prompt asked for cancels the open too.
    saveConfirm->SetOnCancel([this, after]() {
        if (after == AfterSave::OpenPending)
            CancelPendingOpen();
    });
    const auto refused =
        std::find_if(m_Rows.begin(), m_Rows.end(), [](const ScribeOutlineRow& row) { return !row.Refusal.empty(); });
    saveConfirm->Show("Save a program that is refused?",
                      "Save writes the file. Running agents keep the program they run until the file compiles. " +
                          (refused != m_Rows.end() ? refused->Refusal : GetDocument()->GetRefusals().front().Message),
                      "Save Anyway");
}

} // namespace GameEngine::Editor
