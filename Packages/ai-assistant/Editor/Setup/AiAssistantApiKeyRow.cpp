#include "Setup/AiAssistantApiKeyRow.h"

#include "AgentSessionState.h"
#include "Providers/ApiKeyStore.h"

#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutAssetPath = "ai-assistant:Editor/UI/controls/AiAssistantSetup/AiAssistantApiKeyRow.uxml";
constexpr const char* kStyleAssetPath = "Editor/UI/controls/AiAssistantSetup/AiAssistantApiKeyRow.css";
constexpr const char* kPackageAlias = "ai-assistant";
constexpr const char* kErrorClass = "ai-setup-key-note-error";
constexpr const char* kEmptyPlaceholder = "Paste your key";
constexpr const char* kSavedPlaceholder = "Paste a new key";

std::string Variable()
{
    return std::string(ApiKeyStore::kVariable);
}

std::string NoStoreNote()
{
    return "This system has no credential store: set " + Variable() +
           " in the environment before starting the editor.";
}

std::string KeptNote(const std::string& storeName)
{
    return "A key you save stays in the system's credential store (" + storeName +
           ") for your user on this computer, never in a file or the project. Get a key at console.anthropic.com.";
}

// A key is printable ASCII with no space: a space or a control character inside it is a copy error.
bool HasHiddenCharacter(std::string_view key)
{
    return std::ranges::any_of(key, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte <= ' ' || byte == 0x7F;
    });
}
} // namespace

AiAssistantApiKeyRow::AiAssistantApiKeyRow(std::function<void(KeyWrite)> runWrite) : m_RunWrite(std::move(runWrite))
{
    RequestSubtreeStyleAssetPath(kStyleAssetPath, kPackageAlias);
}

bool AiAssistantApiKeyRow::Build(UIManager& ui)
{
    if (!ui.InstantiateLayoutChildrenFromAssetPath(this, kLayoutAssetPath))
    {
        Logger::Log::Error("AI Assistant: {} did not instantiate; check that the package's assets are staged.",
                           kLayoutAssetPath);
        return false;
    }
    m_Field = dynamic_cast<TextField*>(FindById("AiSetupApiKeyField"_sid));
    m_Save = dynamic_cast<Button*>(FindById("AiSetupApiKeySave"_sid));
    m_Remove = dynamic_cast<Button*>(FindById("AiSetupApiKeyRemove"_sid));
    m_Note = dynamic_cast<Label*>(FindById("AiSetupApiKeyNote"_sid));
    if (!m_Field || !m_Save || !m_Remove || !m_Note)
    {
        Logger::Log::Error("AI Assistant: {} is missing one of its controls.", kLayoutAssetPath);
        RemoveAllChildren();
        return false;
    }

    m_StoreName = AgentSessionState::Get().ApiKeys().StoreName();
    m_Field->SetTooltip("Paste a key from the Claude Console. The field shows a dot for each character.");
    m_Field->SetOnValueChanging([this](const std::string&) { SyncButtons(); });
    // Escape restores the field's value through a commit, not an edit.
    m_Field->SetOnValueChanged([this](const std::string&) { SyncButtons(); });
    // Enter does what Save does, and only while Save is on.
    m_Field->SetOnEnter([this] {
        if (m_Save->IsEnabled())
            Save();
    });
    m_Save->SetTooltip("Saves the key in the system's credential store and checks it.");
    m_Save->SetOnClick([this](UIEvent&) { Save(); });
    m_Remove->SetTooltip("Deletes the saved key from the system's credential store.");
    m_Remove->SetOnClick([this](UIEvent&) { Remove(); });
    if (m_StoreName.empty())
    {
        m_Field->SetEnabled(false);
        ShowNote(NoStoreNote(), false);
    }
    else
    {
        ShowNote(KeptNote(m_StoreName), false);
    }
    SyncButtons();
    return true;
}

void AiAssistantApiKeyRow::ShowSaved(bool saved)
{
    m_Saved = saved;
    SyncButtons();
}

// The write holds the only copy of the key: the field is cleared when the write is handed over.
// Paste inserts the clipboard as it is, so the spaces and line breaks around the key are removed.
void AiAssistantApiKeyRow::Save()
{
    if (m_Writing != Writing::None)
        return;
    const std::string_view key = TrimWhitespaceView(m_Field->GetValue());
    if (HasHiddenCharacter(key))
    {
        ShowNote("The key has a space, a line break or another hidden character inside it. Copy the key from the "
                 "Claude Console again and paste it.",
                 true);
        return;
    }
    if (key.empty())
    {
        ShowNote("Paste the key from the Claude Console, then click Save.", true);
        return;
    }
    if (key.size() > Platform::kMaxSecretBytes)
    {
        ShowNote("The key is longer than " + std::to_string(Platform::kMaxSecretBytes) +
                     " characters. Copy only the key from the Claude Console and paste it.",
                 true);
        return;
    }
    KeyWrite write = [keys = AgentSessionState::Get().ApiKeys(), value = std::string(key)] {
        return keys.Save(value);
    };
    m_Field->SetValue("");
    m_Writing = Writing::Save;
    SyncButtons();
    m_RunWrite(std::move(write));
}

void AiAssistantApiKeyRow::Remove()
{
    if (m_Writing != Writing::None)
        return;
    m_Writing = Writing::Remove;
    SyncButtons();
    m_RunWrite([keys = AgentSessionState::Get().ApiKeys()] { return keys.Remove(); });
}

void AiAssistantApiKeyRow::ShowWriteResult(Platform::SecretStoreStatus status)
{
    const Writing writing = std::exchange(m_Writing, Writing::None);
    const bool done = status == Platform::SecretStoreStatus::Done ||
                      (writing == Writing::Remove && status == Platform::SecretStoreStatus::NotFound);
    if (done)
    {
        m_Saved = writing == Writing::Save;
        ShowNote(KeptNote(m_StoreName), false);
    }
    else if (status == Platform::SecretStoreStatus::NoStore)
    {
        ShowNote(NoStoreNote(), true);
    }
    else if (writing == Writing::Save)
    {
        ShowNote("The key was not saved: " + m_StoreName + " refused it. Paste the key again and click Save.", true);
    }
    else
    {
        ShowNote("The key was not removed: " + m_StoreName + " refused it. Remove it there, or try again.", true);
    }
    SyncButtons();
}

void AiAssistantApiKeyRow::ShowNote(const std::string& text, bool error)
{
    m_Note->SetText(text);
    if (error)
        m_Note->AddClass(kErrorClass);
    else
        m_Note->RemoveClass(kErrorClass);
}

// The field's placeholder says whether a key is saved. With no store the field is off and shows none.
void AiAssistantApiKeyRow::SyncButtons()
{
    const bool hasStore = !m_StoreName.empty();
    const bool empty = m_Field->GetValue().empty();
    const bool idle = m_Writing == Writing::None;
    m_Save->SetEnabled(hasStore && idle && !empty);
    m_Remove->SetEnabled(hasStore && idle && m_Saved);
    m_Field->SetPlaceholder(!hasStore ? "" : m_Saved ? kSavedPlaceholder : kEmptyPlaceholder);
}
} // namespace GameEngine
