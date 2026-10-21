#include "Editor/Hierarchy/HierarchyRenderLayerEdit.h"

#include "Components/Rendering/MeshRenderer.h"
#include "ECS/World.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <string>

namespace GameEngine::Editor
{
namespace
{
const std::string kEditName = "Change Render Layer";

}  // namespace

void ApplyRenderLayerBits(ECS::World& world,
                          const std::vector<ECS::EntityHandle>& entities,
                          uint32 bits,
                          bool on,
                          UndoRedoService* undo,
                          EditorChangeNotifications* notifications)
{
    if (entities.empty())
        return;

    // One compound for the whole list. UndoRedoService::CommitInternal routes every
    // nested commit into the open compound, so N rows still cost the author one undo.
    if (undo)
        undo->BeginCompound(kEditName);

    for (const ECS::EntityHandle entity : entities)
    {
        if (!entity.IsValid() || !world.IsValid(entity))
            continue;

        // An entity whose layers this edit leaves as they are commits nothing, so a
        // no-op row never enlarges the undo step.
        const auto* renderer = world.GetComponent<Components::MeshRenderer>(entity);
        if (!renderer)
            continue;
        const uint32 current = renderer->RenderLayers.Bits;
        const uint32 updated = on ? (current | bits) : (current & ~bits);
        if (updated == current)
            continue;
        InspectorDrag::CommitComponentWithUndo<Components::MeshRenderer>(
            &world, entity, notifications, undo, kEditName,
            [updated](Components::MeshRenderer& target) { target.RenderLayers.Bits = updated; });
    }

    if (undo)
        undo->EndCompound();
}

}  // namespace GameEngine::Editor
