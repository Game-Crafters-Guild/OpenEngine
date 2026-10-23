#include "Inspectors/ShadowCastingRow.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"

namespace GameEngine::InspectorDrag
{
namespace
{

constexpr EnumEntry<Components::ShadowCasting> kShadowCastingModes[] = {
    {Components::ShadowCasting::Off, "Off"},
    {Components::ShadowCasting::On, "On"},
    {Components::ShadowCasting::ShadowsOnly, "Shadows Only"},
};

constexpr const char* kShadowCastingTooltip =
    "Off: the mesh is drawn and casts no shadow. On: the mesh is drawn and casts a shadow. Shadows Only: the mesh is "
    "not drawn, but it still casts its shadow. Use Shadows Only for a simple mesh that casts the shadow "
    "of a detailed one. Select a Shadows Only mesh in the Hierarchy.";

} // namespace

EnumField<Components::ShadowCasting>* AddShadowCastingRow(UIElement* parent, ECS::World* w, ECS::EntityHandle e,
                                                          Editor::EditorChangeNotifications* n,
                                                          Editor::UndoRedoService* undo,
                                                          const std::vector<ECS::EntityHandle>& additionalEntities)
{
    const auto* mr = w ? w->GetComponent<Components::MeshRenderer>(e) : nullptr;
    if (!mr)
        return nullptr;
    EnumField<Components::ShadowCasting>* field =
        InspectorUI::AddEnumRow(parent, "Shadow Casting", kShadowCastingModes, mr->shadowCasting, kShadowCastingTooltip);
    field->SetOnValueChanged([w, e, n, undo, additionalEntities](Components::ShadowCasting mode)
    {
        CommitComponentsWithUndo<Components::MeshRenderer>(w, e, additionalEntities, n, undo, "Change Shadow Casting",
                                                           [mode](Components::MeshRenderer& u) { u.shadowCasting = mode; });
    });
    return field;
}

} // namespace GameEngine::InspectorDrag
