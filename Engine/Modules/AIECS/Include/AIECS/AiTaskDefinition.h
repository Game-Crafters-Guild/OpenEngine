#pragma once

#include "Types/Types.h"

#include <string_view>

namespace GameEngine::AIECS
{

class AiTaskContext;

/// The result of one tick of a task.
enum class AiTaskStatus : uint8
{
    Running,
    Succeeded,
    Failed,
};

/// Why a task stops.
enum class AiTaskExitReason : uint8
{
    Completed, ///< Its tick returned Succeeded.
    Failed,    ///< Its tick returned Failed.
    Preempted, ///< The program chose another option, a state-tree transition left the task's state, the
               ///< agent's AiAgent::Program changed to another program, or the path restarted.
    Reloaded,  ///< The state-tree program reloaded, and the task's state is gone or its task list changed.
};

/// What an option does. Enter runs when the option becomes active, Tick on each decision while it
/// is active, Exit when it stops. Enter and Exit may be null. A task that returns Succeeded or
/// Failed stops, and the program chooses again at the next decision.
/// The functions run on job threads. Of the AI components (AiAgent, AiStateTree, the cells, AiPathTrace),
/// they read only their own agent's: other jobs write those of other agents at the same time.
struct AiTaskDefinition
{
    void (*Enter)(AiTaskContext& context) = nullptr;
    AiTaskStatus (*Tick)(AiTaskContext& context) = nullptr;
    void (*Exit)(AiTaskContext& context, AiTaskExitReason reason) = nullptr;
    /// The name an option-set file gives in an option's "task". Empty: no file can name the task.
    std::string_view Name;
};

} // namespace GameEngine::AIECS
