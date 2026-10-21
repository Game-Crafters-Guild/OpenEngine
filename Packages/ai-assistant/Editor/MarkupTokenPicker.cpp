#include "MarkupTokenPicker.h"

#include "MarkupSearchProvider.h"

#include "Editor/Registries/EditorMarkupRequests.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Controls/TextArea.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <any>
#include <cctype>
#include <utility>

namespace GameEngine
{
namespace
{
// Wide enough for a title of about 40 characters beside its status.
constexpr float kDialogWidthPx = 360.0f;
} // namespace

MarkupTokenPicker::MarkupTokenPicker(TextArea& prompt, StatusHandler onStatus)
    : m_Prompt(prompt)
    , m_OnStatus(std::move(onStatus))
    , m_PromptValue(prompt.GetValue())
{
}

MarkupTokenPicker::~MarkupTokenPicker()
{
    Close();
}

void MarkupTokenPicker::OnPromptChanged(const std::string& value)
{
    const std::string previous = std::exchange(m_PromptValue, value);
    if (m_Dialog || value.size() != previous.size() + 1)
        return;
    const int caret = m_Prompt.GetCaretIndex();
    if (caret <= 0 || static_cast<std::size_t>(caret) > value.size())
        return;
    const std::size_t at = static_cast<std::size_t>(caret) - 1;
    if (value[at] != '@' || (at > 0 && !std::isspace(static_cast<unsigned char>(value[at - 1]))))
        return;
    m_At = at;
    Open();
}

void MarkupTokenPicker::Open()
{
    if (m_Dialog)
        return;
    UIManager* manager = m_Prompt.GetOwnerManager();
    UIElement* root = manager ? manager->GetRootElement() : nullptr;
    if (!root)
        return;
    const Editor::MarkupRequestAnswer list = Editor::EditorMarkupRequests::Get().List(nlohmann::json::object());
    if (list.IsRefusal())
    {
        m_OnStatus(list.Refusal);
        return;
    }
    if (list.Result["markups"].empty())
    {
        m_OnStatus("The scene has no mark-ups to name: place one with the Mark-up tool");
        return;
    }
    m_Search = std::make_unique<MarkupSearchProvider>(list.Result["markups"]);

    auto dialog = std::make_unique<SearchDialog>();
    dialog->SetProvider(m_Search.get());
    // An Enter right after the `@`, meant for the prompt, picks nothing.
    dialog->SetHighlightsFirstBeforeQuery(false);
    dialog->SetOnResult(
        [this](const SearchResultItem& item)
        {
            const auto* found = std::any_cast<MarkupPick>(&item.UserData);
            const MarkupPick pick = found ? *found : MarkupPick{};
            const std::string typed = m_Search ? m_Search->LastQuery() : std::string();
            Close();
            if (pick.Token.empty())
            {
                // A mark-up no token names apart: the prompt keeps what was typed, the status says why.
                PutIntoPrompt(std::min(m_At + 1, m_Prompt.GetValue().size()), 0, typed);
                m_OnStatus(pick.Refusal);
                return;
            }
            const std::string value = m_Prompt.GetValue();
            const bool atSign = m_At < value.size() && value[m_At] == '@';
            PutIntoPrompt(atSign ? m_At : value.size(), atSign ? 1 : 0, pick.Token + " ");
        });
    dialog->SetOnCancel(
        [this]
        {
            const std::string typed = m_Search ? m_Search->LastQuery() : std::string();
            Close();
            PutIntoPrompt(std::min(m_At + 1, m_Prompt.GetValue().size()), 0, typed);
        });
    dialog->SetPanelWidth(kDialogWidthPx);
    m_Dialog = dialog.get();
    m_DialogManager = UIManagerRef(manager);
    root->AddChild(std::move(dialog));
    // Left edges aligned with the prompt field; it opens below, or above when there is no room.
    m_Dialog->SetAnchorPosition(m_Prompt.GetLayoutX(), m_Prompt.GetLayoutY() + m_Prompt.GetLayoutHeight(),
                                SearchDialogHorizontalAnchor::LeadingLeft, m_Prompt.GetLayoutHeight());
    m_Dialog->Show();
}

void MarkupTokenPicker::Close()
{
    SearchDialog* dialog = std::exchange(m_Dialog, nullptr);
    if (dialog && m_DialogManager.Get())
        if (UIElement* parent = dialog->GetParent())
            parent->RemoveChild(dialog);
    m_DialogManager = UIManagerRef();
    m_Search.reset();
}

void MarkupTokenPicker::PutIntoPrompt(std::size_t at, std::size_t replaced, const std::string& text)
{
    std::string value = m_Prompt.GetValue();
    at = std::min(at, value.size());
    value.replace(at, std::min(replaced, value.size() - at), text);
    m_Prompt.SetValue(value);
    const int caret = static_cast<int>(at + text.size());
    m_Prompt.SetSelection(caret, caret);
    if (UIManager* ui = m_Prompt.GetOwnerManager())
        ui->FocusElement(&m_Prompt);
}
} // namespace GameEngine
