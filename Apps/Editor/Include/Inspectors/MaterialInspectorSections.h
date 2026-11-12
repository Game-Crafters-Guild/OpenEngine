#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace GameEngine
{
enum class MaterialPatternSymmetry : uint8_t;
class Dropdown;
class Foldout;
class Label;
class UIElement;

namespace InspectorDrag
{
struct SliderWithFloatValueRow;
}

namespace Editor
{

// The material inspector's foldouts, the lines between its rows and the rows that are not a single
// property toggle or slider. Each builder sets the classes that inspector.css spaces it by, so the
// material keeps the row rhythm of the component rows around it.

// The foldout that holds a material inline in an entity inspector, under its Material row.
std::unique_ptr<Foldout> MakeMaterialPropertiesFoldout();

// A collapsible section of the material inspector: a built-in section or a declared property group.
std::unique_ptr<Foldout> MakeMaterialSection(const std::string& title, bool expanded);

// Appends a line of the material inspector that is not a row: a note, a sub-heading or a
// diagnostics block.
Label* AddMaterialLine(UIElement* parent, const std::string& text);

// Appends a warning line of the material inspector: a problem with the material that the user can
// fix, in the inspector's warning treatment, keeping the row gap above and below it.
Label* AddMaterialWarning(UIElement* parent, const std::string& text);

// Appends the Pattern Symmetry row: a dropdown of None, Rectangular and Square, in the enum's order,
// with the given value selected. Each option's value is the name the material file stores. The
// caller wires SetOnValueChanged.
Dropdown* AddMaterialPatternSymmetryRow(UIElement* parent, MaterialPatternSymmetry symmetry);

// Makes a slider row inactive: its value is unused in the material's current state (Relief Depth
// while the relief is refused), so the row stays, showing the value, but takes no edit and reads as
// switched off. The row, its label, its slider and its typed field each carry the disabled state,
// because the label drag and the field's scrub read their own, and each takes its inactive look from
// its own rule.
void MakeMaterialRowInactive(const InspectorDrag::SliderWithFloatValueRow& row);

} // namespace Editor
} // namespace GameEngine
