#include "UI/Controls/MaskField.h"

#include "Input/KeyCodes.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "../Layout/DropdownPopupPlacement.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <cassert>
#include <optional>

namespace GameEngine
{

namespace
{
constexpr size_t kMaxOptions = 32;

// The labels of the options whose bits are all in `value` (`wanted` true), or not all in it (false).
std::vector<const std::string*> LabelsWhere(const std::vector<MaskOption>& options, uint32 value, bool wanted)
{
    std::vector<const std::string*> labels;
    for (const MaskOption& option : options)
    {
        const bool set = (value & option.Bits) == option.Bits;
        if (set == wanted)
            labels.push_back(&option.Label);
    }
    return labels;
}
} // namespace

MaskField::MaskField()
    : DismissablePopup(this)
{
    // A MaskField looks like a dropdown: the .dropdown family in theme/widgets.css draws its header and
    // popup. MaskField.css carries what only this control has.
    AddClass("dropdown");
    AddClass("mask-field");
    RequestSubtreeStyleAssetPath("UI/controls/Dropdown.css", "editor");
    RequestSubtreeStyleAssetPath("UI/controls/MaskField.css", "editor");
    SetFocusable(true);
    m_Value = 0u;
    BuildHeader();
    BuildPopup();
    Refresh();
}

MaskField::~MaskField() = default;

bool MaskField::IsPopupOpen() const
{
    return HasClass("open");
}

void MaskField::SetOptions(std::span<const MaskOption> options)
{
    assert(options.size() <= kMaxOptions && "MaskField: at most 32 options");
    m_Options.assign(options.begin(), options.begin() + static_cast<std::ptrdiff_t>(std::min(options.size(), kMaxOptions)));
    // The rows are built when the popup opens: a list of fields (one per hierarchy row) holds no popup rows.
    m_RowsStale = true;
    if (IsPopupOpen())
        RebuildOptionRows();
    Refresh();
}

void MaskField::SetSummaryNoun(std::string_view plural)
{
    m_Plural = plural;
    Refresh();
}

void MaskField::SetNothingTooltip(std::string_view tooltip)
{
    m_NothingTooltip = tooltip;
    Refresh();
}

void MaskField::SetMixedBits(uint32 bits)
{
    m_MixedBits = bits;
    Refresh();
}

void MaskField::SetValue(const uint32& value)
{
    Field<uint32>::SetValue(value);
    Refresh();
}

void MaskField::SetValueWithoutNotify(const uint32& value)
{
    Field<uint32>::SetValueWithoutNotify(value);
    Refresh();
}

void MaskField::OpenPopup()
{
    if (m_RowsStale)
    {
        RebuildOptionRows();
        Refresh();
    }
    UpdatePopupPlacement();
    AddClass("open");
    m_Popup->AddClass("open");
}

void MaskField::ClosePopup()
{
    RemoveClass("open");
    m_Popup->RemoveClass("open");
}

uint32 MaskField::OptionBits() const
{
    uint32 bits = 0u;
    for (const MaskOption& option : m_Options)
        bits |= option.Bits;
    return bits;
}

void MaskField::BuildHeader()
{
    auto header = std::make_unique<UIElement>();
    m_Header = header.get();
    m_Header->AddClass("dropdown-header");
    m_Header->AddClass("field-editor");
    m_Header->SetFocusProxy(this);
    m_Header->RegisterEventHandler(kEventMouseDown, [this](UIEvent& event)
    {
        if (event.Button != 0 || !IsEnabled())
            return;
        if (IsPopupOpen())
            ClosePopup();
        else
            OpenPopup();
        event.Stop();
    });

    auto label = std::make_unique<Label>();
    m_HeaderLabel = label.get();
    m_HeaderLabel->AddClass("dropdown-header-label");
    m_HeaderLabel->SetFocusProxy(this);
    m_Header->AddChild(std::move(label));

    auto chevron = std::make_unique<UIElement>();
    chevron->AddClass("dropdown-chevron");
    chevron->SetFocusProxy(this);
    m_Header->AddChild(std::move(chevron));

    AddChild(std::move(header));
}

UIElement* MaskField::AddActionRow(const char* text, bool on)
{
    auto row = std::make_unique<Label>();
    row->AddClass("dropdown-item");
    row->AddClass("mask-field-action");
    row->SetText(text);
    row->SetFocusProxy(this);
    row->RegisterEventHandler(kEventMouseDown, [this, on](UIEvent& event)
    {
        if (event.Button != 0)
            return;
        event.Capture(this);
        ApplyBits(OptionBits(), on);
        event.Stop();
    });
    UIElement* raw = row.get();
    m_Popup->AddChild(std::move(row));
    return raw;
}

void MaskField::BuildPopup()
{
    auto popup = std::make_unique<UIElement>();
    m_Popup = popup.get();
    m_Popup->AddClass("dropdown-items");
    m_Popup->AddClass("fit-content");
    m_Popup->SetFocusProxy(this);
    m_Popup->SetOverlayLayer(OverlayLayer::Dropdown);
    m_Popup->RegisterEventHandler(kEventMouseDown, [this](UIEvent& event)
    {
        // The popup is an input shield: a press on its background never reaches what is under it.
        event.Capture(this);
        event.Stop();
    });
    AddChild(std::move(popup));

    m_EverythingRow = AddActionRow("Everything", true);
    m_NothingRow = AddActionRow("Nothing", false);

    auto rule = std::make_unique<UIElement>();
    rule->AddClass("dropdown-separator");
    m_Popup->AddChild(std::move(rule));

    // The option rows scroll when the popup is bounded (UpdatePopupPlacement); Everything and Nothing stay.
    auto list = std::make_unique<ScrollView>();
    list->AddClass("mask-field-options");
    list->SetFocusProxy(this);
    m_OptionList = list.get();
    m_Popup->AddChild(std::move(list));
}

void MaskField::RebuildOptionRows()
{
    for (Checkbox* row : m_OptionRows)
        m_OptionList->GetViewport()->RemoveChild(row);
    m_OptionRows.clear();
    m_RowsStale = false;
    for (size_t index = 0; index < m_Options.size(); ++index)
    {
        auto row = std::make_unique<Checkbox>();
        row->AddClass("dropdown-item");
        row->AddClass("mask-field-option");
        row->SetText(m_Options[index].Label);
        row->SetFocusProxy(this);
        row->SetOnValueChanged([this, index](const bool& on) { OnOptionRowChanged(index, on); });
        m_OptionRows.push_back(row.get());
        m_OptionList->AddContent(std::move(row));
    }
}

void MaskField::Refresh()
{
    const uint32 value = GetValue();
    m_Refreshing = true;
    for (size_t index = 0; index < m_OptionRows.size(); ++index)
    {
        const uint32 bit = m_Options[index].Bits;
        Checkbox* row = m_OptionRows[index];
        row->SetValueWithoutNotify((value & bit) == bit);
        if ((m_MixedBits & bit) != 0u)
            row->SetMixed();
    }
    m_Refreshing = false;

    m_Summary = Summarize();
    m_HeaderLabel->SetText(m_Summary);
    const bool nothing = m_MixedBits == 0u && !m_Options.empty() && (value & OptionBits()) == 0u;
    // The tooltip goes on the summary, so the field's own tooltip, which its owner sets, stays.
    if (nothing)
    {
        m_HeaderLabel->AddClass("mask-field-nothing");
        m_HeaderLabel->SetTooltip(m_NothingTooltip);
    }
    else
    {
        m_HeaderLabel->RemoveClass("mask-field-nothing");
        m_HeaderLabel->SetTooltip(std::string());
    }
}

void MaskField::ApplyBits(uint32 bits, bool on)
{
    m_MixedBits &= ~bits;
    Field<uint32>::SetValueWithoutNotify(on ? (GetValue() | bits) : (GetValue() & ~bits));
    Refresh();
    if (m_OnBitsChanged)
        m_OnBitsChanged(bits, on);
}

void MaskField::OnOptionRowChanged(size_t index, bool on)
{
    if (m_Refreshing || index >= m_Options.size())
        return;
    // A mixed row resolves to on: the click sets the bit on every entity of the selection.
    const uint32 bit = m_Options[index].Bits;
    ApplyBits(bit, (m_MixedBits & bit) != 0u ? true : on);
}

std::string MaskField::Summarize() const
{
    if (m_MixedBits != 0u)
        return "Mixed";
    if (m_Options.empty())
        return std::string();
    const uint32 value = GetValue();
    const std::vector<const std::string*> on = LabelsWhere(m_Options, value, true);
    const std::vector<const std::string*> off = LabelsWhere(m_Options, value, false);
    if (off.empty())
        return "Everything";
    if (on.empty())
        return "Nothing";
    if (on.size() == 1)
        return *on[0];
    if (on.size() == 2)
        return *on[0] + ", " + *on[1];
    if (off.size() == 1)
        return "All but " + *off[0];
    if (off.size() == 2)
        return "All but " + *off[0] + ", " + *off[1];
    return std::to_string(on.size()) + " " + m_Plural;
}

void MaskField::OnPostLayout()
{
    // The popup's own height is only known once it has been laid out, so an open popup places itself again.
    if (IsPopupOpen())
        UpdatePopupPlacement();
}

// Opens the popup by the dropdown's rule (UI::Layout::PlaceDropdownPopup). When the popup is still taller
// than the room on its side, the option list gets that room and scrolls, and Everything and Nothing stay.
// The bound is set only when it changes, so a converged layout is not dirtied again.
void MaskField::UpdatePopupPlacement()
{
    constexpr float kEstimatedRowHeight = 24.0f;
    constexpr float kEstimatedPopupPadding = 4.0f;
    // Everything, Nothing and the rule above the option list.
    constexpr float kFixedRows = 3.0f;

    // The flip is decided on the popup's whole height, list unbounded.
    const float fixedHeight = kEstimatedPopupPadding + kFixedRows * kEstimatedRowHeight;
    const float requiredHeight = fixedHeight + static_cast<float>(m_Options.size()) * kEstimatedRowHeight;
    const UI::Layout::DropdownPopupRoom room = UI::Layout::PlaceDropdownPopup(*this, *m_Popup, requiredHeight);
    const float roomLayout = room.Height / room.WindowPerLayoutY;

    const std::optional<StyleLength> bound = m_OptionList->Overrides().Get(Style::MaxHeight);
    if (requiredHeight > roomLayout && GetOwnerManager())
    {
        const StyleLength wanted = StyleLength::Px(std::max(kEstimatedRowHeight, roomLayout - fixedHeight));
        if (!bound || *bound != wanted)
            m_OptionList->Overrides().Set(Style::MaxHeight, wanted);
    }
    else if (bound)
    {
        m_OptionList->Overrides().Reset(Style::MaxHeight);
    }
}

void MaskField::OnEvent(UIEvent& event)
{
    if (event.Id == kEventFocusOut)
        ClosePopup();

    if (event.Id == kEventKeyDown && IsEnabled() &&
        (event.Key == Input::kKeyCode_Space || event.Key == Input::kKeyCode_Enter))
    {
        if (IsPopupOpen())
            ClosePopup();
        else
            OpenPopup();
        event.Stop();
        return;
    }

    Field<uint32>::OnEvent(event);
}

} // namespace GameEngine
