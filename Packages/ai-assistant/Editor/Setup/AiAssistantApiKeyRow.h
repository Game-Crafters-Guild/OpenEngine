#pragma once

#include "UI/UIElement.h"

#include <cstdint>
#include <functional>
#include <string>

namespace GameEngine
{
namespace Platform
{
enum class SecretStoreStatus : uint8_t;
}
class Button;
class Label;
class TextField;
class UIManager;

/// The API key row of Settings > AI Assistant, shown for the Claude (API key) connection: the
/// layout comes from the package's `Editor/UI/controls/AiAssistantSetup/AiAssistantApiKeyRow.uxml`
/// (a masked field with a placeholder that says whether a key is saved, Save, Remove and the line
/// that names where the key is kept and where to get one). Save and Remove hand a write of the
/// credential store to the page, which runs it on its check thread; the field is cleared when
/// Save hands the key over, so the row holds no copy of it.
class AiAssistantApiKeyRow final : public UIElement
{
public:
    /// A write of the credential store, run off the UI thread; returns the store's status.
    using KeyWrite = std::function<Platform::SecretStoreStatus()>;

    /// `runWrite` runs a write off the UI thread, then calls ShowWriteResult on the UI thread.
    explicit AiAssistantApiKeyRow(std::function<void(KeyWrite)> runWrite);

    /// Instantiates the row layout through `ui`. False (and logged) when the layout is not
    /// staged or is missing an element.
    bool Build(UIManager& ui);

    /// Shows whether a key is saved (read on the setup check's thread): Remove is on only then.
    void ShowSaved(bool saved);

    /// Shows the end of the write that Save or Remove started; the buttons are off until then.
    void ShowWriteResult(Platform::SecretStoreStatus status);

private:
    enum class Writing : uint8_t
    {
        None,
        Save,
        Remove,
    };

    void Save();
    void Remove();
    void ShowNote(const std::string& text, bool error);
    void SyncButtons();

    std::function<void(KeyWrite)> m_RunWrite;
    Writing m_Writing = Writing::None;
    TextField* m_Field = nullptr;
    Button* m_Save = nullptr;
    Button* m_Remove = nullptr;
    Label* m_Note = nullptr;
    std::string m_StoreName;
    bool m_Saved = false;
};
} // namespace GameEngine
