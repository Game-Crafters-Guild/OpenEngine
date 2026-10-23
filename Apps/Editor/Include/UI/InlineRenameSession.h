#pragma once

#include <functional>
#include <string>

namespace GameEngine
{
class Label;
class TextField;
class UIElement;

/// One inline rename in a view (an asset cell, a list row, a Hierarchy row): a text field stands
/// in for the item's title label, filled with the current name and selected. Enter commits and
/// Escape cancels. A name the validator refuses is marked as it is typed: the field shows the
/// invalid state and its tooltip shows the reason while the name stays refused, wherever the
/// pointer is, and Enter keeps the session open. When
/// focus leaves the field (a click elsewhere), an accepted name commits and a refused one is
/// dropped, as in every file manager. Committing the unchanged name ends the session without a
/// commit.
class InlineRenameSession
{
public:
    /// Returns why `name` is refused, written for the user; empty when it is accepted.
    using Validator = std::function<std::string(const std::string& name)>;
    /// Runs once with an accepted, changed name, after the session has ended.
    using Committer = std::function<void(const std::string& name)>;

    InlineRenameSession() = default;
    ~InlineRenameSession();
    InlineRenameSession(const InlineRenameSession&) = delete;
    InlineRenameSession& operator=(const InlineRenameSession&) = delete;

    /// Starts editing `name` in place of `title`, which hides while the field shows; its parent
    /// hosts the field. `fieldClass`, when not empty, is a modifier class that fits the field to
    /// the view (a tree row orders its children). When the session ends while the field has
    /// keyboard focus (Enter, Escape), focus returns to `focusOnEnd` (the view the item lives
    /// in), so navigation keys keep working; focus that moved elsewhere stays there. A session
    /// in progress is canceled first.
    void Begin(Label* title, UIElement* focusOnEnd, const std::string& name, const char* fieldClass,
               Validator validate, Committer commit);

    /// Ends the session without committing.
    void Cancel();

    /// Called when a view binds `row` again (a pooled row or cell reused on a scroll or a
    /// refresh). Ends the session without committing when the edited title is inside `row` and
    /// `showsEditedItem` is false; a row bound again to the edited item keeps its field.
    void OnRowBound(const UIElement& row, bool showsEditedItem);

    bool IsActive() const { return m_Field != nullptr; }
    /// The title label the active session edits, or null.
    const Label* GetTitle() const { return m_Title; }

private:
    void Commit();
    void CommitOrCancelOnFocusOut();
    void End();
    bool ApplyValidation(const std::string& name);

    Label* m_Title = nullptr;
    UIElement* m_FocusOnEnd = nullptr;
    TextField* m_Field = nullptr;
    std::string m_Name;
    Validator m_Validate;
    Committer m_Commit;
};

} // namespace GameEngine
