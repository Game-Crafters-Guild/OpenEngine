#pragma once

#include "Types/Types.h"
#include "UI/Controls/BaseField.h"
#include "UI/Interaction/DismissablePopup.h"
#include "UI/UIEvents.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

class Checkbox;
class Label;
class ScrollView;
class UIElement;

/// One option of a MaskField: its own bits in the value (one bit, usually), and the label its row shows.
struct MaskOption
{
    uint32 Bits = 0u;
    std::string Label;
};

/// A field that picks any set of options, each with its own bit in a 32-bit value.
///
/// The header shows a summary of the value: "Everything" when every option is set, "Nothing" when none
/// is, one or two option labels ("Props, Foliage"), "All but X" or "All but X, Y" when one or two options
/// are off, otherwise "N <noun>". It shows "Mixed" while a multi-selection disagrees on any bit.
/// The popup lists "Everything" and "Nothing", a rule, then one row per option; a row the selection
/// disagrees on is drawn mixed.
///
/// A row click raises one OnBitsChanged with that option's bits; "Everything" and "Nothing" raise one with
/// every option's bits. The control updates its own value and raises no value-changed event: the caller
/// applies the bits to whatever it edits, so a multi-selection keeps every other bit of each entity.
class MaskField : public Field<uint32>, public DismissablePopup
{
  public:
    using BitsChangedCallback = std::function<void(uint32 bits, bool on)>;

    MaskField();
    ~MaskField() override;

    void OnOwnerManagerChanged(UIManager* owner) override { UpdatePopupRegistration(owner); }
    bool IsPopupOpen() const override;
    void DismissPopup() override { ClosePopup(); }

    /// The options, at most 32. Their union is "Everything".
    void SetOptions(std::span<const MaskOption> options);
    /// The plural noun of the "N <noun>" summary ("layers"). N is at least 3 there, so it is always plural.
    void SetSummaryNoun(std::string_view plural);
    /// The tooltip the summary shows while the value is Nothing ("Sees no layer.").
    void SetNothingTooltip(std::string_view tooltip);
    /// The bits a multi-selection disagrees on. A value write keeps them; a row click clears its bits.
    void SetMixedBits(uint32 bits);
    void SetOnBitsChanged(BitsChangedCallback callback) { m_OnBitsChanged = std::move(callback); }

    void SetValue(const uint32& value) override;
    void SetValueWithoutNotify(const uint32& value) override;

    void OnEvent(UIEvent& event) override;
    void OnPostLayout() override;

  private:
    // Tests and tooling reach the popup and its parts through UI/Internal/MaskFieldAccess.h.
    friend struct MaskFieldAccess;

    void OpenPopup();
    void ClosePopup();
    uint32 OptionBits() const;
    void BuildHeader();
    void BuildPopup();
    UIElement* AddActionRow(const char* text, bool on);
    void UpdatePopupPlacement();
    void RebuildOptionRows();
    void Refresh();
    void ApplyBits(uint32 bits, bool on);
    void OnOptionRowChanged(size_t index, bool on);
    std::string Summarize() const;

    std::vector<MaskOption> m_Options;
    std::string m_Plural = "options";
    std::string m_NothingTooltip;
    std::string m_Summary;
    uint32 m_MixedBits = 0u;
    BitsChangedCallback m_OnBitsChanged;
    UIElement* m_Header = nullptr;
    Label* m_HeaderLabel = nullptr;
    UIElement* m_Popup = nullptr;
    ScrollView* m_OptionList = nullptr;
    UIElement* m_EverythingRow = nullptr;
    UIElement* m_NothingRow = nullptr;
    std::vector<Checkbox*> m_OptionRows;
    // Set while Refresh writes the rows, so their change callbacks do not read the write as a click.
    bool m_Refreshing = false;
    // The options changed since the rows were built.
    bool m_RowsStale = true;
};

} // namespace GameEngine
