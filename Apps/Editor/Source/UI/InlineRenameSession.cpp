#include "UI/InlineRenameSession.h"

#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <utility>

namespace GameEngine
{

namespace
{

constexpr const char* kFieldClass = "inline-rename-field";
constexpr const char* kInvalidClass = "invalid";

// Enter commits and Escape cancels the whole session whether or not the text changed: the base
// field only reports edits, and an unchanged Enter must still close the editor.
class RenameTextField final : public TextField
{
public:
    std::function<void()> OnEnter;
    std::function<void()> OnEscape;

    bool OnKey(int key, int mods, UI::IPlatformApi* platform) override
    {
        if (key == Input::kKeyCode_Enter || key == Input::kKeyCode_NumPadEnter)
        {
            if (OnEnter)
                OnEnter();
            return true;
        }
        if (key == Input::kKeyCode_Escape)
        {
            if (OnEscape)
                OnEscape();
            return true;
        }
        return TextField::OnKey(key, mods, platform);
    }
};

// The field sits inside the item's cell or row, whose press, drag and rename handlers must not
// see clicks meant for the caret.
void IsolatePointerEventsFromHost(UIElement& field)
{
    for (const EventId id : {kEventMouseDown, kEventMouseUp, kEventMouseMove})
        field.RegisterEventHandler(id, [](UIEvent& e) { e.Stop(); });
}

} // namespace

InlineRenameSession::~InlineRenameSession()
{
    End();
}

void InlineRenameSession::Begin(Label* title, UIElement* focusOnEnd, const std::string& name, const char* fieldClass,
                                Validator validate, Committer commit)
{
    End();
    if (!title || !title->GetParent())
        return;

    m_Title = title;
    m_FocusOnEnd = focusOnEnd;
    m_Name = name;
    m_Validate = std::move(validate);
    m_Commit = std::move(commit);

    auto field = std::make_unique<RenameTextField>();
    field->AddClass(kFieldClass);
    if (fieldClass && *fieldClass)
        field->AddClass(fieldClass);
    field->SetValue(name);
    field->SetFocusable(true);
    field->OnEnter = [this]() { Commit(); };
    field->OnEscape = [this]() { End(); };
    field->SetOnValueChanging([this](const std::string& text) { ApplyValidation(text); });
    field->RegisterEventHandler(kEventFocusOut, [this](UIEvent&) { CommitOrCancelOnFocusOut(); });
    IsolatePointerEventsFromHost(*field);
    m_Field = field.get();

    UI::Layout::SetElementHidden(*m_Title, true);

    UIElement* host = m_Title->GetParent();
    const auto& siblings = host->GetChildren();
    std::size_t insertAt = siblings.size();
    for (std::size_t i = 0; i < siblings.size(); ++i)
    {
        if (siblings[i].get() == m_Title)
        {
            insertAt = i + 1;
            break;
        }
    }
    host->InsertChild(insertAt, std::move(field));

    if (UIManager* ui = host->GetOwnerManager())
    {
        ui->FocusElement(m_Field);
        m_Field->SelectAll();
    }
}

void InlineRenameSession::Cancel()
{
    End();
}

void InlineRenameSession::OnRowBound(const UIElement& row, bool showsEditedItem)
{
    if (!m_Title || showsEditedItem)
        return;
    for (const UIElement* e = m_Title->GetParent(); e; e = e->GetParent())
    {
        if (e == &row)
        {
            Cancel();
            return;
        }
    }
}

bool InlineRenameSession::ApplyValidation(const std::string& name)
{
    if (!m_Field)
        return false;
    const std::string reason = m_Validate ? m_Validate(name) : std::string{};
    UIManager* ui = m_Field->GetOwnerManager();
    if (reason.empty())
    {
        m_Field->RemoveClass(kInvalidClass);
        m_Field->SetTooltip({});
        if (ui)
            ui->UnpinTooltip(*m_Field);
        return true;
    }
    m_Field->AddClass(kInvalidClass);
    m_Field->SetTooltip(reason);
    // The user is typing, so the pointer is usually elsewhere: the reason shows without a hover.
    if (ui)
        ui->PinTooltip(*m_Field);
    return false;
}

void InlineRenameSession::Commit()
{
    if (!m_Field)
        return;

    const std::string name = m_Field->GetValue();
    if (name == m_Name)
    {
        End();
        return;
    }
    // A refused name keeps the session open: the field stays marked and its tooltip says why.
    if (!ApplyValidation(name))
    {
        Logger::Log::Warning("Rename of '{}' refused: {}", m_Name, m_Field->GetTooltip());
        return;
    }

    Committer commit = std::move(m_Commit);
    End();
    if (commit)
        commit(name);
}

void InlineRenameSession::CommitOrCancelOnFocusOut()
{
    if (!m_Field)
        return;

    // The keyboard went elsewhere, so the field cannot stay open waiting for a fix: a refused
    // name is dropped and the old one stays.
    const std::string name = m_Field->GetValue();
    if (name != m_Name && !ApplyValidation(name))
    {
        Logger::Log::Warning("Rename of '{}' canceled: {}", m_Name, m_Field->GetTooltip());
        End();
        return;
    }
    Commit();
}

void InlineRenameSession::End()
{
    if (!m_Field)
        return;

    TextField* field = m_Field;
    Label* title = m_Title;
    UIElement* focusOnEnd = m_FocusOnEnd;
    m_Field = nullptr;
    m_Title = nullptr;
    m_FocusOnEnd = nullptr;
    m_Name.clear();
    m_Validate = nullptr;
    m_Commit = nullptr;

    UIManager* ui = field->GetOwnerManager();
    // Focus that moved elsewhere (a click, Tab, a panel that takes it) stays there.
    const bool fieldHadFocus = ui && !field->GetId().empty() && ui->GetFocusedElementId() == field->GetId();
    if (ui)
        ui->UnpinTooltip(*field);

    UI::Layout::SetElementHidden(*title, false);
    // Detach after the current event finishes: End runs from the field's own key and focus
    // handlers, and destroying the element mid-dispatch would pull the handler out from under
    // itself. Safe-posted, so a view torn down before the drain takes the field with it.
    field->PostSafeAction([field]()
    {
        if (UIElement* host = field->GetParent())
            host->RemoveChild(field);
    });
    if (focusOnEnd && fieldHadFocus)
        ui->FocusElement(focusOnEnd);
}

} // namespace GameEngine
