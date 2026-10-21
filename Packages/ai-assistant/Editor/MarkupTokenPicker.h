#pragma once

#include "UI/UIManagerRef.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace GameEngine
{
class MarkupSearchProvider;
class SearchDialog;
class TextArea;

/// The prompt's `@` picker: a searchable list of the scene's mark-ups (MarkupSearchProvider)
/// that opens under the prompt field when the user types `@` at the start of a word. No row is
/// highlighted until the user types or presses an arrow key, so an Enter right after the `@`
/// closes the list. Choosing one puts its token (`@markup:<tag>` or `@markup:"<title>"`) where the
/// `@` was, then a space; a row no token names apart (MarkupPick::Refusal) says why on the status
/// line. Escape, Enter with nothing highlighted, or a press outside closes it and puts what was
/// typed in its search field after the `@`, so a token typed by hand reaches the prompt whole.
class MarkupTokenPicker
{
public:
    /// Called with a line for the panel's status line.
    using StatusHandler = std::function<void(const std::string&)>;

    MarkupTokenPicker(TextArea& prompt, StatusHandler onStatus);
    /// Closes a list that is still open.
    ~MarkupTokenPicker();

    MarkupTokenPicker(const MarkupTokenPicker&) = delete;
    MarkupTokenPicker& operator=(const MarkupTokenPicker&) = delete;

    /// Follows an edit of the prompt that left `value`: opens the list when it typed an `@` at
    /// the start of a word.
    void OnPromptChanged(const std::string& value);

    /// Opens the list; says on the status line why not when mark-ups cannot be read (play mode,
    /// no scene) or the scene has none.
    void Open();
    bool IsOpen() const { return m_Dialog != nullptr; }

private:
    void Close();
    /// Puts `text` into the prompt at `at`, replacing `replaced` bytes, the caret after it.
    void PutIntoPrompt(std::size_t at, std::size_t replaced, const std::string& text);

    TextArea& m_Prompt;
    StatusHandler m_OnStatus;
    // The prompt as the last edit left it, so the next one can tell an `@` was typed.
    std::string m_PromptValue;
    // Where the `@` that opened the list sits in the prompt.
    std::size_t m_At = 0;
    std::unique_ptr<MarkupSearchProvider> m_Search;
    /// The open list, a child of the UI root; nullptr when closed.
    SearchDialog* m_Dialog = nullptr;
    /// The manager whose root holds the list, checked before the list is touched after the
    /// panel may have outlived it.
    UIManagerRef m_DialogManager;
};
} // namespace GameEngine
