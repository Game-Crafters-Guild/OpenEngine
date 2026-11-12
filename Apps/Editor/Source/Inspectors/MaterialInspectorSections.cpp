#include "Inspectors/MaterialInspectorSections.h"

#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"

#include <vector>

namespace GameEngine::Editor
{

namespace
{

constexpr const char* kPatternSymmetryTooltip =
    "The turns and mirrors that leave the pattern the same. Square: a square grid or square stones, the same "
    "after a quarter turn. Rectangular: a grid of oblong tiles, the same after a half turn or a mirror. "
    "None: brick rows, planks, herringbone and photo textures. A Spline Paving's Blend seam bends the tile "
    "lines of a Square or Rectangular pattern and fades a None pattern.";

} // namespace

std::unique_ptr<Foldout> MakeMaterialPropertiesFoldout()
{
    auto foldout = std::make_unique<Foldout>();
    foldout->SetTitle("Material Properties");
    foldout->AddClass("material-properties-foldout");
    foldout->AddClass("rp-foldout");
    foldout->GetHeader()->AddClass("material-properties-header");
    foldout->GetContentContainer()->AddClass("material-properties-content");
    foldout->SetExpanded(true);
    return foldout;
}

std::unique_ptr<Foldout> MakeMaterialSection(const std::string& title, bool expanded)
{
    auto section = std::make_unique<Foldout>();
    section->SetTitle(title);
    section->AddClass("rp-foldout");
    section->GetHeader()->AddClass("material-section-header");
    section->GetContentContainer()->AddClass("material-section-content");
    section->SetExpanded(expanded);
    return section;
}

Label* AddMaterialLine(UIElement* parent, const std::string& text)
{
    auto line = std::make_unique<Label>();
    line->AddClass("inspector-text");
    line->AddClass("material-line");
    line->SetText(text);
    Label* raw = line.get();
    parent->AddChild(std::move(line));
    return raw;
}

Label* AddMaterialWarning(UIElement* parent, const std::string& text)
{
    // Not an inspector-text line: the asset view softens that class's colour with a rule more
    // specific than the warning's own, which would turn the warning grey.
    auto line = std::make_unique<Label>();
    line->AddClass("inspector-warning");
    line->AddClass("material-warning");
    line->SetText(text);
    Label* raw = line.get();
    parent->AddChild(std::move(line));
    return raw;
}

Dropdown* AddMaterialPatternSymmetryRow(UIElement* parent, MaterialPatternSymmetry symmetry)
{
    std::vector<Dropdown::Option> options;
    for (const MaterialPatternSymmetry option :
         {MaterialPatternSymmetry::None, MaterialPatternSymmetry::Rectangular, MaterialPatternSymmetry::Square})
    {
        const std::string name = MaterialPatternSymmetryToString(option);
        options.push_back({name, name});
    }
    return InspectorUI::AddDropdownRow(parent, "Pattern Symmetry", options, static_cast<int>(symmetry),
                                       kPatternSymmetryTooltip);
}

void MakeMaterialRowInactive(const InspectorDrag::SliderWithFloatValueRow& row)
{
    if (row.Row)
        row.Row->SetEnabled(false);
    if (row.Label)
        row.Label->SetEnabled(false);
    if (row.Slider)
        row.Slider->SetEnabled(false);
    if (row.ValueField)
        row.ValueField->SetEnabled(false);
}

} // namespace GameEngine::Editor
