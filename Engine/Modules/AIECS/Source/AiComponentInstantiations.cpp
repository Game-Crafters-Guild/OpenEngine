// Explicit cross-DLL instantiation for AIECS's public components.
// See ECS/ECSTemplates.h for the rationale (GE_INSTANTIATE_ENGINE_COMPONENT).
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsLarge.h"
#include "AIECS/Components/AiCells/AiCellsMedium.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiClock.h"
#include "AIECS/Components/AiGroup.h"
#include "AIECS/Components/AiPathTrace.h"
#include "AIECS/Components/AiStateTree.h"
#include "AIECS/Components/AiTrace.h"

#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::AiAgent);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::AiCellsLarge);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::AiCellsMedium);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::AiCellsSmall);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::AiClock);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::AiGroup);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::AiPathTrace);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::AiStateTree);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::AiTrace);
} // namespace GameEngine::ECS
