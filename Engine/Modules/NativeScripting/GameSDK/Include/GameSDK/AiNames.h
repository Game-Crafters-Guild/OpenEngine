#pragma once

// GameSDK/AiNames.h: give the AI programs of a project the names of the game's inputs, tasks, conditions and
// events. Each macro registers one name in AIECS::AiRegistry::Get() when its module loads. The user page
// docs/ai/ai-state-trees.mdx states the rules.

#include "AIECS/AiRegistry.h"
#include "ECS/Reflection.h"

/// Registers the input `Function` (AIECS::AiInputFunction) under `Name` when its module loads. `Threading` is
/// an AIECS::AiInputThreading value: CallingThread or JobThreads. The editor shows `Caption` and `Unit` for
/// the input; either can be "".
#define GE_REGISTER_AI_INPUT(Function, Threading, Name, Caption, Unit)                                    \
    static_assert(sizeof(Name) > 1, "GE_REGISTER_AI_INPUT: the name is empty. Give the name that a "       \
                                    "program file uses in a consideration's \"input\".");                  \
    namespace                                                                                              \
    {                                                                                                      \
    [[maybe_unused]] const ::GameEngine::uint16 GE_PP_CONCAT(ge_ai_input_, __LINE__) =                     \
        ::GameEngine::AIECS::AiRegistry::Get().RegisterInput(                                              \
            Function, ::GameEngine::AIECS::AiInputThreading::Threading, Name, Caption, Unit);              \
    }

/// Registers a task under `Name` when its module loads. `Enter` and `Exit` can be nullptr; `Tick` cannot
/// (AIECS::AiTaskDefinition).
#define GE_REGISTER_AI_TASK(Enter, Tick, Exit, Name)                                                       \
    static_assert(sizeof(Name) > 1, "GE_REGISTER_AI_TASK: the name is empty. Give the name that a "        \
                                    "program file uses in a \"tasks\" list.");                             \
    namespace                                                                                              \
    {                                                                                                      \
    [[maybe_unused]] const ::GameEngine::uint16 GE_PP_CONCAT(ge_ai_task_, __LINE__) =                      \
        ::GameEngine::AIECS::AiRegistry::Get().RegisterTask({Enter, Tick, Exit, Name});                    \
    }

/// Registers the condition `Function` (AIECS::AiConditionFunction) under `Name` when its module loads.
#define GE_REGISTER_AI_CONDITION(Function, Name)                                                           \
    static_assert(sizeof(Name) > 1, "GE_REGISTER_AI_CONDITION: the name is empty. Give the name that a "   \
                                    "program file uses in a \"condition\" field.");                        \
    namespace                                                                                              \
    {                                                                                                      \
    [[maybe_unused]] const ::GameEngine::uint16 GE_PP_CONCAT(ge_ai_condition_, __LINE__) =                 \
        ::GameEngine::AIECS::AiRegistry::Get().RegisterCondition(Function, Name);                          \
    }

/// Registers the event type `Name` when its module loads, and defines `Id`: a const uint16 in the enclosing
/// namespace that holds the id for AIECS::AiEvent::Type. A program names the event in an "event" field. Another
/// file gets the id at run time from AIECS::AiRegistry::FindEventType.
#define GE_REGISTER_AI_EVENT(Id, Name)                                                                     \
    static_assert(sizeof(Name) > 1, "GE_REGISTER_AI_EVENT: the name is empty. Give the name that a "       \
                                    "program file uses in an \"event\" field.");                           \
    [[maybe_unused]] const ::GameEngine::uint16 Id = ::GameEngine::AIECS::AiRegistry::Get().RegisterEventType(Name);
