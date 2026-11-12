// Explicit cross-DLL instantiation for SpatialGridECS's components.
// See ECS/ECSTemplates.h for the rationale (GE_INSTANTIATE_ENGINE_COMPONENT).
#include "Components/SpatialGridMover.h"
#include "Components/SpatialGridSettings.h"
#include "SpatialGridState.h"

#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::SpatialGridMover);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::SpatialGridSettings);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::SpatialGridState);
} // namespace GameEngine::ECS
