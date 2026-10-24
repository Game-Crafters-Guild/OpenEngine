#include "UI/Controls/Dropdown.h"

#include "../Layout/DropdownPopupPlacement.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace GameEngine {

namespace {

// Returned for an option that declares no art class and for an out-of-range
// index, so callers can hold a reference either way.
const std::string kNoArtClass;

// A closed popup has no layout height, so its first placement estimates one from
// the row metrics in widgets.css (a text or icon row) and controls/Dropdown.css
// (a picture row: the 48 px picture with its padding and margins).
constexpr float kEstimatedRowHeight = 24.0f;
constexpr float kEstimatedPictureRowHeight = 58.0f;
constexpr float kEstimatedPopupPadding = 4.0f;

// The picture row's layout, pinned to the editor mount like the control's stylesheet so
// a project file of the same name cannot replace it.
constexpr const char* kPictureRowLayoutPath = "editor:UI/controls/DropdownPictureRow.uxml";

static std::string TrimCopy(const std::string& s)
{
    size_t start = 0;
    size_t end   = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

// Parse #rrggbb or #rgb to ARGB (0xAARRGGBB). Returns 0 and false on failure.
static bool TryParseHexColor(const char* s, uint32_t& outArgb)
{
    if (!s || s[0] != '#' || std::strlen(s) < 4)
        return false;
    auto hexNibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
        if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
        if (c >= 'A' && c <= 'F') return (uint8_t)(10 + (c - 'A'));
        return 0xFFu;
    };
    const size_t len = std::strlen(s);
    uint8_t r, g, b;
    if (len >= 7)
    {
        r = (uint8_t)((hexNibble(s[1]) << 4) | hexNibble(s[2]));
        g = (uint8_t)((hexNibble(s[3]) << 4) | hexNibble(s[4]));
        b = (uint8_t)((hexNibble(s[5]) << 4) | hexNibble(s[6]));
    }
    else if (len >= 4)
    {
        r = hexNibble(s[1]) * 17u;
        g = hexNibble(s[2]) * 17u;
        b = hexNibble(s[3]) * 17u;
    }
    else
        return false;
    outArgb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return true;
}

// A row with an icon or a picture keeps its text in a child Label, so the row
// itself stays a flex container instead of a text-measure leaf; a picture row
// nests it one level down, in the text column. Either way this is the Label the
// option's color applies to.
static Label* ItemTextLabel(Label* row)
{
    for (const auto& child : row->GetChildren())
    {
        if (auto* text = dynamic_cast<Label*>(child.get()))
            return text;
        for (const auto& grandchild : child->GetChildren())
        {
            if (auto* text = dynamic_cast<Label*>(grandchild.get()))
                return text;
        }
    }
    return row;
}

static UIElement* ChildWithClass(const UIElement& parent, const char* className)
{
    for (const auto& child : parent.GetChildren())
    {
        if (child->HasClass(className))
            return child.get();
    }
    return nullptr;
}

static bool AnyOptionDeclares(const std::vector<Dropdown::Option>& options, std::string Dropdown::Option::*field)
{
    return std::any_of(options.begin(), options.end(),
                       [field](const Dropdown::Option& option) { return !(option.*field).empty(); });
}

static void ApplyOptionColorToItemLabel(Label* raw, const Dropdown::Option& opt, bool isSelected)
{
    if (isSelected)
    {
        raw->Overrides().Reset(Style::Color);
        return;
    }
    if (!opt.color.empty())
    {
        uint32_t argb = 0;
        if (TryParseHexColor(opt.color.c_str(), argb))
            raw->Overrides().Set(Style::Color, argb);
        else
            raw->Overrides().Reset(Style::Color);
    }
    else
        raw->Overrides().Reset(Style::Color);
}

} // namespace

Dropdown::~Dropdown()
{
}

Dropdown::Dropdown()
    : DismissablePopup(this)
{
    AddClass("dropdown");
    // Control styling is an Editor-shipped UIStyle asset.
    RequestSubtreeStyleAssetPath("UI/controls/Dropdown.css", "editor");
    SetFocusable(true);
    EnsureHeader();
    EnsureItemsContainer();
}

void Dropdown::SetMode(Mode mode)
{
    if (m_Mode == mode)
        return;
    m_Mode = mode;
}

void Dropdown::SetOptions(const std::vector<Option>& options, int selectedIndex)
{
    m_Options = options;
    const OptionArt art = AnyOptionDeclares(m_Options, &Option::pictureClass) ? OptionArt::Picture
                          : AnyOptionDeclares(m_Options, &Option::iconClass)  ? OptionArt::Icon
                                                                              : OptionArt::None;
    EnsureHeader();
    if (art != m_OptionArt)
        RemoveHeaderArt();
    m_OptionArt = art;
    EnsureItemsContainer();
    UpdatePopupWidthClass();
    RebuildUiItems();

    if (!m_Options.empty())
    {
        if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_Options.size()))
            selectedIndex = 0;
        SetSelectedIndex(selectedIndex);
        // SetSelectedIndex may early-return (same index) without updating the
        // header — force a refresh so the label reflects the new option set.
        UpdateHeaderLabel();
    }
    else
    {
        m_SelectedIndex = -1;
        Field<std::string>::SetValue(std::string());
        NotifyValueChanging();
        NotifyValueChanged();
        UpdateHeaderLabel();
    }
}

void Dropdown::SetOptionsFromLabels(const std::vector<std::string>& labels, int selectedIndex)
{
    std::vector<Option> options;
    options.reserve(labels.size());
    for (const auto& label : labels)
    {
        Option opt;
        opt.value = label;
        opt.label = label;
        options.push_back(std::move(opt));
    }
    SetOptions(options, selectedIndex);
}

void Dropdown::SetOptionsFromString(const std::string& csv)
{
    std::vector<std::string> labels;
    std::stringstream        ss(csv);
    std::string              item;
    while (std::getline(ss, item, ','))
    {
        labels.push_back(TrimCopy(item));
    }
    SetOptionsFromLabels(labels);
}

void Dropdown::SetNoSelectionText(std::string text)
{
    if (m_NoSelectionText == text)
        return;
    m_NoSelectionText = std::move(text);
    UpdateHeaderLabel();
}

const Dropdown::Option* Dropdown::SelectedOption() const
{
    if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Options.size()))
    {
        return nullptr;
    }
    return &m_Options[static_cast<size_t>(m_SelectedIndex)];
}

const std::string& Dropdown::GetSelectedValue() const
{
    static const std::string kEmpty;
    const Option* selected = SelectedOption();
    return selected ? selected->value : kEmpty;
}

const std::string& Dropdown::GetSelectedLabel() const
{
    static const std::string kEmpty;
    const Option* selected = SelectedOption();
    return selected ? selected->label : kEmpty;
}

float Dropdown::WidthForLabel(std::string_view label) const
{
    UIManager* ui = GetOwnerManager();
    if (!ui || !m_HeaderContainer || !m_HeaderLabel)
        return 0.0f;
    const float text = ui->MeasureTextWidth(*m_HeaderLabel, label);
    if (text <= 0.0f)
        return 0.0f;
    const auto& labelLayout = m_HeaderLabel->GetResolvedStyle().Layout;
    float width = text + UI::Layout::HorizontalInsetPx(*this) +
                  UI::Layout::HorizontalInsetPx(*m_HeaderContainer) + UI::Layout::HorizontalInsetPx(*m_HeaderLabel) +
                  labelLayout.Margin.Left + labelLayout.Margin.Right;
    const float gap = m_HeaderContainer->GetResolvedStyle().Layout.ColumnGap;
    for (const UIElement* part : {static_cast<const UIElement*>(m_HeaderArt), static_cast<const UIElement*>(m_Chevron)})
    {
        if (!part || part->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
            continue;
        const auto& layout = part->GetResolvedStyle().Layout;
        width += part->GetLayoutWidth() + layout.Margin.Left + layout.Margin.Right + gap;
    }
    return width;
}

void Dropdown::SetSelectedValue(const std::string& value)
{
    for (size_t i = 0; i < m_Options.size(); ++i)
    {
        if (m_Options[i].value == value)
        {
            SetSelectedIndex(static_cast<int>(i));
            return;
        }
    }
}

void Dropdown::SetSelectedIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Options.size()))
        index = -1;

    if (m_SelectedIndex == index)
        return;

    m_SelectedIndex = index;

    if (m_SelectedIndex >= 0)
    {
        const auto& opt = m_Options[static_cast<size_t>(m_SelectedIndex)];
        if (GetValue() != opt.value)
        {
            Field<std::string>::SetValue(opt.value);
            NotifyValueChanging();
            NotifyValueChanged();
        }
    }
    else
    {
        if (!GetValue().empty())
        {
            Field<std::string>::SetValue(std::string());
            NotifyValueChanging();
            NotifyValueChanged();
        }
    }

    UpdateHeaderLabel();
    SyncItemSelectionClasses();
}

void Dropdown::SetSelectedIndexWithoutNotify(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Options.size()))
        index = -1;

    if (m_SelectedIndex == index)
        return;

    m_SelectedIndex = index;

    // Mirror SetSelectedIndex's backing-value update, but via the field's
    // without-notify path so OnValueChanged (which commits an edit) does not
    // fire. SetValueWithoutNotify already short-circuits an unchanged value.
    if (m_SelectedIndex >= 0)
        Field<std::string>::SetValueWithoutNotify(m_Options[static_cast<size_t>(m_SelectedIndex)].value);
    else
        Field<std::string>::SetValueWithoutNotify(std::string());

    UpdateHeaderLabel();
    SyncItemSelectionClasses();
}

void Dropdown::SyncItemSelectionClasses()
{
    if (!m_ItemsContainer)
        return;
    const auto& children = m_ItemsContainer->GetChildren();
    for (size_t i = 0; i < children.size() && i < m_Options.size(); ++i)
    {
        auto* raw = dynamic_cast<Label*>(children[i].get());
        if (!raw)
            continue;
        const bool isSelected = (static_cast<int>(i) == m_SelectedIndex);
        if (isSelected)
            raw->AddClass("selected");
        else
            raw->RemoveClass("selected");
        ApplyOptionColorToItemLabel(ItemTextLabel(raw), m_Options[i], isSelected);
    }
}

void Dropdown::SetNativeMenuInvoker(NativeMenuInvoker invoker)
{
    m_NativeMenuInvoker = std::move(invoker);
}

void Dropdown::SetAutoWidthPopup(bool enabled)
{
    if (m_AutoWidthPopup == enabled)
        return;
    m_AutoWidthPopup = enabled;
    EnsureItemsContainer();
    UpdatePopupWidthClass();
}

void Dropdown::UpdatePopupWidthClass()
{
    // The header sizes the popup, except when the popup needs more: a caller
    // that asked for auto width, or the icon or picture column the control adds,
    // which would otherwise be taken out of the labels and wrap them.
    if (m_AutoWidthPopup || m_OptionArt != OptionArt::None)
        m_ItemsContainer->AddClass("fit-content");
    else
        m_ItemsContainer->RemoveClass("fit-content");
}

// An open popup re-places itself after every layout pass: its own width is not
// known until it has been measured, and both the flip and the clamp below need it.
void Dropdown::OnPostLayout()
{
    if (IsMenuOpen())
        UpdatePopupPlacement();
}

// Where the popup sits relative to its header. Vertically it opens above when
// there is not enough room below and the upper side offers more; horizontally it
// is pulled back inside its panel, and capped, when it would otherwise run past
// the edge. Both decisions are re-made on every converge pass, so each one is
// applied only when it changes: a class flipped off and on again costs the whole
// tree another post-layout dispatch and solve.
void Dropdown::UpdatePopupPlacement()
{
    if (!m_ItemsContainer)
    {
        RemoveClass("open-upward");
        return;
    }
    const float rowHeight = m_OptionArt == OptionArt::Picture ? kEstimatedPictureRowHeight : kEstimatedRowHeight;
    UI::Layout::PlaceDropdownPopup(*this, *m_ItemsContainer,
                                   kEstimatedPopupPadding + static_cast<float>(m_Options.size()) * rowHeight);
}

void Dropdown::EnsureHeader()
{
    if (m_HeaderLabel)
        return;

    // Header container (the clickable row)
    auto container = std::make_unique<UIElement>();
    m_HeaderContainer = container.get();
    m_HeaderContainer->AddClass("dropdown-header");
    /* Same shared class TextFieldBase puts on its inner editor: a dropdown
       header is a field editor for styling purposes, so one selector can reach
       every field's editable surface instead of enumerating control types. */
    m_HeaderContainer->AddClass("field-editor");
    // Delegate focus for mouse/tap interactions on the header to the
    // Dropdown itself so keyboard focus and UI.FocusIn/Out are expressed in
    // terms of the control, not its internal label.
    m_HeaderContainer->SetFocusProxy(this);

    m_HeaderContainer->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        // Treat primary mouse button as the activator for opening the menu.
        if (e.Button != 0 && e.Button != 1)
            return;
        if (!IsEnabled())
            return;

        if (m_Mode == Mode::Native)
            OpenMenuNative();
        else
            OpenMenuUi();

        e.Stop();
    });

    // Text label (fills available space) — proxy focus to the Dropdown so
    // clicking the label (or chevron) registers focus on the outer control
    // and FocusOut fires correctly when the user clicks elsewhere.
    auto lbl = std::make_unique<Label>();
    m_HeaderLabel = lbl.get();
    m_HeaderLabel->AddClass("dropdown-header-label");
    m_HeaderLabel->SetFocusProxy(this);
    m_HeaderContainer->AddChild(std::move(lbl));

    // Chevron indicator (right-aligned)
    auto chevron = std::make_unique<UIElement>();
    m_Chevron = chevron.get();
    m_Chevron->AddClass("dropdown-chevron");
    m_Chevron->SetFocusProxy(this);
    m_HeaderContainer->AddChild(std::move(chevron));

    AddChild(std::move(container));
}

const std::string& Dropdown::ArtClass(const Option& option) const
{
    switch (m_OptionArt)
    {
    case OptionArt::Icon:
        return option.iconClass;
    case OptionArt::Picture:
        return option.pictureClass;
    case OptionArt::None:
        break;
    }
    return kNoArtClass;
}

void Dropdown::EnsureHeaderArt()
{
    if (m_HeaderArt || !m_HeaderContainer)
        return;

    auto art = std::make_unique<UIElement>();
    m_HeaderArt = art.get();
    m_HeaderArt->AddClass(m_OptionArt == OptionArt::Picture ? "dropdown-header-picture" : "dropdown-header-icon");
    m_HeaderArt->SetFocusProxy(this);
    // Leading slot: the header reads art, label, chevron.
    m_HeaderContainer->InsertChild(0, std::move(art));
}

void Dropdown::RemoveHeaderArt()
{
    if (!m_HeaderArt)
        return;
    m_HeaderContainer->RemoveChild(m_HeaderArt);
    m_HeaderArt = nullptr;
    m_HeaderArtClass.clear();
}

void Dropdown::BuildIconRow(Label& row, const Option& option)
{
    // Every row of an annotated control gets the column, so a row whose
    // option declares no icon still lines its text up with the rest.
    auto icon = std::make_unique<UIElement>();
    icon->AddClass("dropdown-item-icon");
    if (!option.iconClass.empty())
        icon->AddClass(option.iconClass);
    icon->SetFocusProxy(this);
    row.AddChild(std::move(icon));
    // A Label with children is no longer a Yoga text-measure leaf. Keep
    // the option text in its own flow child so both row height and
    // fit-content popup width still include the glyph metrics.
    auto text = std::make_unique<Label>();
    text->AddClass("dropdown-item-label");
    text->SetText(option.label);
    text->SetFocusProxy(this);
    row.AddChild(std::move(text));
}

void Dropdown::BuildPictureRow(Label& row, const Option& option)
{
    // The popup rebuilds its rows each time it opens, and an open dropdown has an
    // owner, so a row built before then stays empty until it is shown.
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    if (!ui->InstantiateLayoutChildrenFromAssetPath(&row, kPictureRowLayoutPath))
    {
        Logger::Log::Error("Dropdown: {} did not instantiate, so the picture rows are empty. Check that it is "
                           "staged under the editor asset mount.",
                           kPictureRowLayoutPath);
        return;
    }
    UIElement* picture = ChildWithClass(row, "dropdown-item-picture-image");
    UIElement* column = ChildWithClass(row, "dropdown-item-text");
    auto* name = column ? dynamic_cast<Label*>(ChildWithClass(*column, "dropdown-item-label")) : nullptr;
    auto* description = column ? dynamic_cast<Label*>(ChildWithClass(*column, "dropdown-item-description")) : nullptr;
    if (!picture || !name || !description)
    {
        Logger::Log::Error("Dropdown: {} is missing a part of the picture row.", kPictureRowLayoutPath);
        return;
    }
    if (!option.pictureClass.empty())
        picture->AddClass(option.pictureClass);
    name->SetText(option.label);
    // Focus stays on the control, and the row stays the one press and hover surface.
    for (UIElement* part : {picture, column, static_cast<UIElement*>(name), static_cast<UIElement*>(description)})
        part->SetFocusProxy(this);
    if (option.description.empty())
        column->RemoveChild(description);
    else
        description->SetText(option.description);
}

void Dropdown::EnsureItemsContainer()
{
    if (m_ItemsContainer)
    {
        // Give the popup container a stable, discoverable id for automation/debugging.
        // This is intentionally derived from the dropdown id so UI replay scenarios can
        // target/probe the popup without needing extra theme/layout authoring.
        if (!GetId().empty())
            m_ItemsContainer->SetId(GetId() + ":items");
        return;
    }

    auto container = std::make_unique<UIElement>();
    m_ItemsContainer = container.get();
    m_ItemsContainer->AddClass("dropdown-items");
    if (!GetId().empty())
        m_ItemsContainer->SetId(GetId() + ":items");
    // Keep focus ownership on the Dropdown when interacting with the popup
    // list so that losing focus (clicking elsewhere) can reliably close it.
    m_ItemsContainer->SetFocusProxy(this);
    m_ItemsContainer->SetOverlayLayer(OverlayLayer::Dropdown);
    m_ItemsContainer->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        // The popup is an input shield: clicks on its background must never
        // fall through to controls visually underneath it.
        e.Capture(this);
        e.Stop();
    });
    AddChild(std::move(container));
}

void Dropdown::RebuildUiItems()
{
    // If this is called from within an event dispatch (e.g. the header's
    // mouse-down handler), UIElement::RemoveChild will defer actual child
    // removal via PostAction(). A synchronous while-loop that expects
    // GetChildren() to shrink would then spin forever. Mirror the TreeView
    // pattern and defer the entire rebuild to a safe point after dispatch.
    if (UIElement::IsInEventDispatch())
    {
        this->PostSafeAction([this]() { this->RebuildUiItems(); });
        return;
    }

    if (!m_ItemsContainer)
        return;

    // Clear any existing items.
    m_ItemsContainer->RemoveAllChildren();

    for (size_t i = 0; i < m_Options.size(); ++i)
    {
        auto  lbl = std::make_unique<Label>();
        Label* raw = lbl.get();
        raw->AddClass("dropdown-item");
        raw->SetFocusProxy(this);
        const int index = static_cast<int>(i);
        const bool isSelected = (m_SelectedIndex == index);
        if (isSelected)
            raw->AddClass("selected");
        switch (m_OptionArt)
        {
        case OptionArt::Icon:
            BuildIconRow(*raw, m_Options[i]);
            break;
        case OptionArt::Picture:
            BuildPictureRow(*raw, m_Options[i]);
            break;
        case OptionArt::None:
            raw->SetText(m_Options[i].label);
            break;
        }
        ApplyOptionColorToItemLabel(ItemTextLabel(raw), m_Options[i], isSelected);

        raw->RegisterEventHandler(kEventMouseDown, [this, index](UIEvent& e) {
            if (e.Button != 0 && e.Button != 1)
                return;
            e.Capture(this);
            SetSelectedIndex(index);
            // Close the popup list; mirror whatever OpenMenuUi() does so CSS
            // rules can rely either on the root .dropdown or the items
            // container carrying an "open" class.
            RemoveClass("open");
            if (m_ItemsContainer)
                m_ItemsContainer->RemoveClass("open");
            e.Stop();
        });

        m_ItemsContainer->AddChild(std::move(lbl));
    }
}

void Dropdown::UpdateHeaderLabel()
{
    EnsureHeader();
    if (!m_HeaderLabel)
        return;

    UpdateHeaderArt();

    if (m_SelectedIndex >= 0 && m_SelectedIndex < static_cast<int>(m_Options.size()))
    {
        const Option& opt = m_Options[static_cast<size_t>(m_SelectedIndex)];
        m_HeaderLabel->SetText(opt.label);
        if (!opt.color.empty())
        {
            uint32_t argb = 0;
            if (TryParseHexColor(opt.color.c_str(), argb))
                m_HeaderLabel->Overrides().Set(Style::Color, argb);
            else
                m_HeaderLabel->Overrides().Reset(Style::Color);
        }
        else
            m_HeaderLabel->Overrides().Reset(Style::Color);
    }
    else
    {
        m_HeaderLabel->SetText(m_NoSelectionText);
        m_HeaderLabel->Overrides().Reset(Style::Color);
    }
}

void Dropdown::UpdateHeaderArt()
{
    const Option* selected = SelectedOption();
    if (m_OptionArt == OptionArt::None || !selected)
    {
        RemoveHeaderArt();
        return;
    }
    EnsureHeaderArt();

    const std::string& artClass = ArtClass(*selected);
    if (artClass == m_HeaderArtClass)
        return;

    if (!m_HeaderArtClass.empty())
        m_HeaderArt->RemoveClass(m_HeaderArtClass);
    m_HeaderArtClass = artClass;
    if (!m_HeaderArtClass.empty())
        m_HeaderArt->AddClass(m_HeaderArtClass);
}

void Dropdown::OpenMenuUi()
{
    EnsureItemsContainer();
    RebuildUiItems();

    // Toggle an "open" class on both the dropdown root and the items
    // container. The Editor theme styles dropdowns primarily via the items
    // container, so mirroring the state there makes it easy to use simple
    // selectors like ".dropdown-items.open" in addition to descendant-based
    // rules.
    const bool isOpen = m_ItemsContainer && m_ItemsContainer->HasClass("open");
    if (isOpen)
    {
        RemoveClass("open");
        if (m_ItemsContainer)
            m_ItemsContainer->RemoveClass("open");
        UpdateHeaderLabel();
    }
    else
    {
        UpdatePopupPlacement();
        AddClass("open");
        if (m_ItemsContainer)
            m_ItemsContainer->AddClass("open");
        if (m_HeaderLabel)
            m_HeaderLabel->Overrides().Reset(Style::Color);
    }
    // Opening/closing the popup changes style/layout (display:none <-> flex). We rely on the
    // dirty marks from AddClass/RemoveClass and UIManager's "late dirty" rebuild to reflect
    // the change in the same frame (for both mouse and keyboard activation) without forcing
    // a redundant YogaSolve-only pass.
}

void Dropdown::CloseMenuUi()
{
    RemoveClass("open");
    if (m_ItemsContainer)
        m_ItemsContainer->RemoveClass("open");
    UpdateHeaderLabel();
}

bool Dropdown::IsMenuOpen() const
{
    return HasClass("open") || (m_ItemsContainer && m_ItemsContainer->HasClass("open"));
}

void Dropdown::OpenMenuNative()
{
    if (!m_NativeMenuInvoker)
    {
        // Fallback to UI-mode behavior if no native invoker is installed.
        OpenMenuUi();
        return;
    }

    std::vector<std::string> labels;
    labels.reserve(m_Options.size());
    for (const auto& opt : m_Options)
        labels.push_back(opt.label);

    int current = m_SelectedIndex;
    Dropdown* self = this;
    auto       onSelected = [self](int index) {
        if (!self)
            return;
        self->SetSelectedIndex(index);
    };

    m_NativeMenuInvoker(*this, labels, current, std::move(onSelected));
}

void Dropdown::OnEvent(UIEvent& e)
{
    if (e.Id == kEventFocusOut)
    {
        // Auto-close the UI dropdown when focus is lost.
        CloseMenuUi();
    }

    // A disabled control never activates. Focus and the pointer already skip a
    // disabled element and everything inside it; this gate is the control's own
    // guarantee, so a key that arrives anyway cannot open the menu and show a
    // picked value its owner refuses to store.
    if (e.Id == kEventKeyDown && IsEnabled())
    {
        if (e.Key == Input::kKeyCode_Space || e.Key == Input::kKeyCode_Enter)
        {
            if (m_Mode == Mode::Native)
                OpenMenuNative();
            else
                OpenMenuUi();
            e.Stop();
            return;
        }
    }

    // Defer all other events to the base Field bridge so focus/text behavior
    // remains consistent with other field-like controls.
    Field<std::string>::OnEvent(e);
}

} // namespace GameEngine
