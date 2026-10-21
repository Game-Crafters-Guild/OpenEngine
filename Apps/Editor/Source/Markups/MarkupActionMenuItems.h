#pragma once

#include "ECS/ECS.h"
#include "UI/Interaction/ContextMenuManipulator.h"

#include <vector>

namespace GameEngine::Editor
{

// The Mark-ups tab row's context menu (MarkupsPanelRows.cpp): one row per action modules register
// on mark-ups (MarkupActionRegistry), in their order, acting on `entity` of `world`; empty, so no
// menu opens, while none is registered.
std::vector<ContextMenuManipulator::Item> BuildMarkupActionMenuItems(const ECS::World& world, ECS::EntityHandle entity);

} // namespace GameEngine::Editor
