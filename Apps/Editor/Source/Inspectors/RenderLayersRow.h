#pragma once

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/RenderLayerField.h"
#include "UI/Controls/MaskField.h"

#include <string>
#include <vector>

namespace GameEngine::InspectorDrag
{

/// A render layer row for the TComponent field `member` of the entity `e` and the rest of the selection.
/// The row shows the layers the selection disagrees on as mixed; a click changes that one layer (or, for
/// Everything and Nothing, every layer) on each selected entity and keeps the entity's other layers, as
/// one undo step named `undoName`.
template <typename TComponent>
MaskField* AddComponentRenderLayersRow(UIElement* parent, const std::string& label,
                                       Rendering::RenderLayerMask TComponent::*member, ECS::World* w,
                                       ECS::EntityHandle e, Editor::EditorChangeNotifications* n,
                                       Editor::UndoRedoService* undo, const std::string& undoName,
                                       const char* tooltip, const char* nothingTooltip,
                                       const std::vector<ECS::EntityHandle>& additionalEntities = {})
{
    const auto* primary = w ? w->GetComponent<TComponent>(e) : nullptr;
    if (!primary)
        return nullptr;
    const Rendering::RenderLayerMask value = primary->*member;
    uint32 mixedBits = 0u;
    for (const ECS::EntityHandle other : additionalEntities)
    {
        if (const auto* component = w->GetComponent<TComponent>(other))
            mixedBits |= (component->*member).Bits ^ value.Bits;
    }

    MaskField* field = InspectorUI::AddRenderLayersRow(parent, label, value, mixedBits, tooltip, nothingTooltip);
    field->SetOnBitsChanged([w, e, n, undo, undoName, member, additionalEntities](uint32 bits, bool on)
    {
        CommitComponentsWithUndo<TComponent>(w, e, additionalEntities, n, undo, undoName,
                                             [member, bits, on](TComponent& component)
                                             {
                                                 uint32& layers = (component.*member).Bits;
                                                 layers = on ? (layers | bits) : (layers & ~bits);
                                             });
    });
    return field;
}

} // namespace GameEngine::InspectorDrag
