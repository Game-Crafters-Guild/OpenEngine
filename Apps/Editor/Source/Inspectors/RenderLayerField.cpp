#include "Inspectors/RenderLayerField.h"

#include "Rendering/RenderLayersScriptApi.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/MaskField.h"

#include <algorithm>
#include <array>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
// Every field FeedRenderLayerNames fed that may still be alive. Dead entries are dropped on the next feed
// or refresh, so the list stays as long as the fields on screen.
std::vector<UIElement::WeakRef<MaskField>>& LiveFields()
{
    static std::vector<UIElement::WeakRef<MaskField>> s_Fields;
    return s_Fields;
}

void DropDeadFields()
{
    auto& fields = LiveFields();
    fields.erase(std::remove_if(fields.begin(), fields.end(),
                                [](const UIElement::WeakRef<MaskField>& ref) { return ref.Get() == nullptr; }),
                 fields.end());
}

void GiveNames(MaskField& field)
{
    std::array<MaskOption, Rendering::RenderLayerMask::kCount> options;
    for (uint32 layer = 0; layer < Rendering::RenderLayerMask::kCount; ++layer)
        options[layer] = {Rendering::RenderLayerMask::FromLayer(layer).Bits, Rendering::Script::NameOf(Rendering::RenderLayerMask::FromLayer(layer))};
    field.SetOptions(options);
}
} // namespace

void FeedRenderLayerNames(MaskField& field)
{
    DropDeadFields();
    field.SetSummaryNoun("layers");
    GiveNames(field);
    LiveFields().push_back(UIElement::MakeWeakRef(&field));
}

void RefreshRenderLayerFields()
{
    DropDeadFields();
    for (const auto& ref : LiveFields())
    {
        if (MaskField* field = ref.Get())
            GiveNames(*field);
    }
    RenderLayerFieldsRefreshed().Invoke();
}

Event<>& RenderLayerFieldsRefreshed()
{
    static Event<> s_Refreshed;
    return s_Refreshed;
}

} // namespace GameEngine::Editor

namespace GameEngine::InspectorUI
{

MaskField* AddRenderLayersRow(UIElement* parent, const std::string& label, Rendering::RenderLayerMask value,
                              uint32 mixedBits, const char* tooltip, const char* nothingTooltip)
{
    MaskField* field = AddMaskRow(parent, label, {}, value.Bits, tooltip);
    Editor::FeedRenderLayerNames(*field);
    field->SetNothingTooltip(nothingTooltip ? nothingTooltip : "");
    field->SetMixedBits(mixedBits);
    return field;
}

} // namespace GameEngine::InspectorUI
