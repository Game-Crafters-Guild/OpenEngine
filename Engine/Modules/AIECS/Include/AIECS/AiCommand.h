#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

namespace GameEngine::AIECS
{

/// An order a task issues. AiApplySystem gives it to the command function registered as Type,
/// on the main thread, in entity order. The command function gives Target and Argument their meaning.
struct AiCommand
{
    ECS::EntityHandle Target;
    int32 Argument = 0;
    uint16 Type = 0;
};

} // namespace GameEngine::AIECS
