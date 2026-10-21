#pragma once

#include "ECS/Entity.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

/// Turns the layers `bits` on (or off) in each entity's MeshRenderer::RenderLayers and keeps every other
/// layer of that entity, recorded as ONE undo step, so a multi-row edit undoes in one press. An entity whose
/// layers do not change, and an entity with no MeshRenderer, contributes nothing to the undo step.
void ApplyRenderLayerBits(
    ECS::World& world,
    const std::vector<ECS::EntityHandle>& entities,
    uint32 bits,
    bool on,
    UndoRedoService* undo,
    EditorChangeNotifications* notifications);

}  // namespace GameEngine::Editor
