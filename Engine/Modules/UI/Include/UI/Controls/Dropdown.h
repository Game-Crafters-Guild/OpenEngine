#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "UI/Controls/BaseField.h"
#include "UI/Interaction/DismissablePopup.h"
#include "UI/UIEvents.h"

namespace GameEngine {

class UIElement;
class Label;

// Simple string-valued dropdown control with two presentation modes:
//  - UI mode: pure UI popup list built from child elements.
//  - Native mode: delegates to a host-provided OS/context-menu invoker.
class Dropdown : public Field<std::string>, public DismissablePopup
{
public:
    using ValueType = std::string;

    enum class Mode
    {
        Ui,
        Native
    };

    struct Option
    {
        std::string value;
        std::string label;
        std::string color;  // optional; if non-empty, applied as inline color to the dropdown item (e.g. "#3498db")
        // Optional CSS class carrying this option's icon, declared by whoever
        // declares the option. The stylesheet owns which image that is; nothing
        // here names an image file. Empty leaves the option text-only.
        std::string iconClass;
        // Optional CSS class carrying a picture of what this option does (a look, a
        // preset), declared like iconClass: the stylesheet owns which image it is. The
        // picture draws untinted. When any option declares one, every popup row is a
        // picture row (the picture, the label and the description beside it, at the row
        // height the control's stylesheet sets) and the header shows the selected
        // option's picture small; the icon slot is then not drawn. Native mode shows
        // labels only.
        std::string pictureClass;
        // Optional line under the label in a picture row: what the option does, in a few
        // words. A row without pictures does not show it.
        std::string description;
    };

    using NativeMenuInvoker = std::function<void(
        Dropdown& dropdown,
        const std::vector<std::string>& labels,
        int selectedIndex,
        std::function<void(int)> onSelected)>;

    Dropdown();
    ~Dropdown() override;

    // Maintains UIManager's dismissable-popup registry, which drives the
    // pointer gate and the outside-press / Escape dismissal.
    void OnOwnerManagerChanged(UIManager* owner) override { UpdatePopupRegistration(owner); }

    // DismissablePopup. The control is its own visible surface, so the
    // default popup root (this) is correct.
    bool IsPopupOpen() const override { return IsMenuOpen(); }
    void DismissPopup() override { CloseMenuUi(); }

    void SetMode(Mode mode);
    Mode GetMode() const { return m_Mode; }

    void SetOptions(const std::vector<Option>& options, int selectedIndex = 0);
    void SetOptionsFromLabels(const std::vector<std::string>& labels, int selectedIndex = 0);
    void SetOptionsFromString(const std::string& csv);

    int  GetSelectedIndex() const { return m_SelectedIndex; }
    void SetSelectedIndex(int index);
    // Like SetSelectedIndex but without firing the value-changed callback,
    // mirroring Field<T>::SetValueWithoutNotify. Programmatic sync paths (e.g.
    // the inspector's play-mode live refresh) use this so pushing a fresh value
    // back into the widget does not re-commit an edit. It updates the selection,
    // header label, and item-selection classes unconditionally; a caller that
    // must not disturb an in-progress interaction (an open popup) guards on
    // IsMenuOpen() itself rather than baking that policy into this primitive.
    void SetSelectedIndexWithoutNotify(int index);

    // The header text while no option is selected (index -1). A preset list whose
    // values match no preset sets "Custom" here and selects -1, so the state reads in
    // the field without being an entry the user can pick. Empty by default.
    void SetNoSelectionText(std::string text);

    const std::string& GetSelectedValue() const;
    void SetSelectedValue(const std::string& value);
    // The selected option's LABEL — what the header shows. The option's VALUE is the
    // Field<std::string> value (GetSelectedValue), and it is what value events carry.
    // Empty when nothing is selected.
    const std::string& GetSelectedLabel() const;
    // The width, in logical px, the control needs to show `label` whole in its header: the
    // label measured in the header's face plus the header's padding, border, icon and
    // chevron. 0 before the header has been laid out as text.
    float WidthForLabel(std::string_view label) const;

    void SetNativeMenuInvoker(NativeMenuInvoker invoker);

    // When true, the popup list grows wider than the header to fit its
    // content. When false (default), the popup matches the header width
    // and clips overflow — unless the options declare icons or pictures, whose column
    // would otherwise come out of the option labels.
    void SetAutoWidthPopup(bool enabled);
    bool GetAutoWidthPopup() const { return m_AutoWidthPopup; }

	    // Test helpers / advanced customization hooks.
	    // The clickable header surface is m_HeaderContainer (which holds the
	    // label and chevron); tests simulating a header click should dispatch
	    // on the container, since the runtime path bubbles via UIManager.
	    UIElement* GetHeaderContainer() const { return m_HeaderContainer; }
	    Label*     GetHeaderLabel() const { return m_HeaderLabel; }
	    UIElement* GetItemsContainer() const { return m_ItemsContainer; }
	    // Explicitly open the UI / native menus. These are primarily intended for
	    // tests and tooling, but remain public to keep those call sites simple.
	    void OpenMenuUi();
        void CloseMenuUi();
        bool IsMenuOpen() const;
	    void OpenMenuNative();

    void OnEvent(UIEvent& e) override;

    // The popup's own width is only known after it has been laid out, and the
    // placement clamp needs it, so an open popup re-places itself once the
    // measurement exists.
    void OnPostLayout() override;

private:
    // The option the current index names, or null when nothing is selected.
    const Option* SelectedOption() const;

    void EnsureHeader();
    // What the options draw beside their labels. Pictures take the place of icons
    // when both are declared: a row has one leading slot.
    enum class OptionArt : uint8_t
    {
        None,
        Icon,
        Picture
    };

    // The class this option puts on the leading slot for the control's art kind.
    const std::string& ArtClass(const Option& option) const;
    // The header's leading slot, present only while the options declare art and an
    // option is selected, so every other header keeps its label and chevron alone. Its
    // class names the slot kind (an icon or a picture), so a change of kind replaces it.
    void EnsureHeaderArt();
    void RemoveHeaderArt();
    void BuildIconRow(Label& row, const Option& option);
    void BuildPictureRow(Label& row, const Option& option);
    void EnsureItemsContainer();
    void RebuildUiItems();
    void UpdateHeaderLabel();
    void UpdateHeaderArt();
    void UpdatePopupWidthClass();
    void SyncItemSelectionClasses();
    void UpdatePopupPlacement();

    Mode                    m_Mode{Mode::Ui};
    std::vector<Option>     m_Options;
    int                     m_SelectedIndex{-1};
    UIElement*              m_HeaderContainer{nullptr};
    Label*                  m_HeaderLabel{nullptr};
    UIElement*              m_HeaderArt{nullptr};
    // The art class currently on m_HeaderArt, removed before the next one is
    // applied: the classes come from option data, so there is no prefix to scan for.
    std::string             m_HeaderArtClass;
    UIElement*              m_Chevron{nullptr};
    std::string             m_NoSelectionText;
    UIElement*              m_ItemsContainer{nullptr};
    NativeMenuInvoker       m_NativeMenuInvoker;
    bool                    m_AutoWidthPopup{false};
    // The art any option declares: the whole control then reserves the leading
    // column so annotated and unannotated rows align.
    OptionArt               m_OptionArt{OptionArt::None};
};

} // namespace GameEngine
