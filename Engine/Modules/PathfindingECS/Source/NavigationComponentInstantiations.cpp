// Explicit cross-DLL instantiation for PathfindingECS's public components.
// See ECS/ECSTemplates.h for the rationale (GE_INSTANTIATE_ENGINE_COMPONENT).
#include "Components/Navigation/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationDebugSettings.h"
#include "PathfindingECS/Components/NavigationObstacle.h"
#include "Components/Navigation/NavigationSource.h"
#include "Components/Navigation/NavigationSurface.h"

#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::NavigationSurface);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::NavigationAgent);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::NavigationObstacle);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::NavigationDebugSettings);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::NavigationSource);
} // namespace GameEngine::ECS
