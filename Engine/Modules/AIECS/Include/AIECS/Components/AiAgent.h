#pragma once

#include "Types/Types.h"

#include <array>
#include <type_traits>

namespace GameEngine::Components
{

/// An entity that decides with an AI program. Every tick count is in AI ticks (AiClock::Tick).
/// AiDecideSystem writes it; a game sets Program and Cadence, and sets RestartRequested to restart the program.
struct AiAgent
{
    static constexpr uint32 kMaxPathDepth = 8;
    static constexpr uint16 kNoState = 0xFFFF;
    /// The Program of an agent that does not decide. AiRegistry::AddProgram gives it for a refused program.
    static constexpr uint16 kNoProgram = 0xFFFF;
    /// The bit of Program that marks a state-tree program (AiRegistry::CompileStateTree). Without it,
    /// Program names a utility program (AiRegistry::AddProgram).
    static constexpr uint16 kStateTreeProgramBit = 0x8000;

    /// The AI tick of the next decision. The first decision comes at the entity index modulo
    /// Cadence, so a population spreads its decisions over the ticks.
    uint32 NextTick = 0;
    /// The AI tick the active state started; for a state-tree program, the deepest active state.
    uint32 EnteredTick = 0;
    /// The score of the active option at the last decision.
    float32 ActiveScore = 0.0f;
    /// The key of the agent's noise draw; 0 means not set. AiDecideSystem sets it before the agent's
    /// first decision, and a scene save keeps it, so the agent draws the same values after a load.
    /// A copy of an agent keeps its seed: set DrawSeed to 0 on the copy to give it its own.
    uint32 DrawSeed = 0;
    /// The program's id in AIECS::AiRegistry, or kNoProgram. kStateTreeProgramBit is set for a state-tree
    /// program; such an agent also needs the AiStateTree component.
    uint16 Program = kNoProgram;
    /// The AI ticks between two decisions; 0 counts as 1. AiDecideSystem keeps NextTick at most
    /// Cadence ticks ahead, so a lower Cadence applies from the next AI tick.
    uint16 Cadence = 1;
    /// The active states from the root down; a utility program uses one level, its option index, and a
    /// state-tree program one level per state, its index in the program's states.
    std::array<uint16, kMaxPathDepth> ActivePath{kNoState, kNoState, kNoState, kNoState,
                                                 kNoState, kNoState, kNoState, kNoState};
    uint8 PathDepth = 0;
    /// True after the first decision tick is set.
    bool Scheduled : 1 = false;
    /// Set it to make the agent's next decision exit the path and start Program again from its root. That
    /// decision clears it. Call Wake as well to make the decision come on the next AI tick, not at the cadence.
    /// A scene save does not keep it.
    bool RestartRequested : 1 = false;
    /// The program that ActivePath belongs to, or kNoProgram when no path runs. The engine writes it; a game never
    /// does. When it differs from Program, the next decision exits the path with the tasks of this program
    /// and starts Program.
    uint16 PathProgram = kNoProgram;

    /// Makes the agent decide on the next AI tick, whatever its cadence; after that decision it
    /// decides every Cadence ticks again. Call it from the main thread, for example from a command.
    void Wake()
    {
        NextTick = 0;
        Scheduled = true;
    }
};

static_assert(std::is_trivially_copyable_v<AiAgent>);
static_assert(std::is_standard_layout_v<AiAgent>);

} // namespace GameEngine::Components
