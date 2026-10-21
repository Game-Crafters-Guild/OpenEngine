#pragma once

#include "UI/Controls/MaskField.h"

#include <string>
#include <vector>

namespace GameEngine
{

// A MaskField's popup and its parts, for tests and tooling. Not part of the control's API: a user opens
// the popup with a click on the header, and the control reports changes through OnBitsChanged.
struct MaskFieldAccess
{
    static void OpenPopup(MaskField& field) { field.OpenPopup(); }
    static void ClosePopup(MaskField& field) { field.ClosePopup(); }
    // The header's text, the summary of the value.
    static const std::string& Summary(const MaskField& field) { return field.m_Summary; }
    static Label* SummaryLabel(const MaskField& field) { return field.m_HeaderLabel; }
    // The option rows in option order, built when the popup opens.
    static const std::vector<Checkbox*>& OptionRows(const MaskField& field) { return field.m_OptionRows; }
    static UIElement* EverythingRow(const MaskField& field) { return field.m_EverythingRow; }
    static UIElement* NothingRow(const MaskField& field) { return field.m_NothingRow; }
    // The popup: Everything, Nothing, the rule and the option list.
    static UIElement* Popup(const MaskField& field) { return field.m_Popup; }
    // The scrolling list that holds the option rows.
    static ScrollView* OptionList(const MaskField& field) { return field.m_OptionList; }
};

} // namespace GameEngine
