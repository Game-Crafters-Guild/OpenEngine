#pragma once

#include "AI/StateTree/StateTreeProgram.h"
#include "AI/StateTree/StateTreeState.h"
#include "AI/StateTree/StateTreeTypes.h"
#include "Types/Types.h"

#include <array>
#include <cassert>
#include <span>
#include <vector>

namespace GameEngine::AI
{

struct StateTreeHost;

/// Runs one agent's state tree. The agent's state (StateTreeState) and its cooldown cells live outside the
/// interpreter; Bind points the interpreter at them. The program is shared and is passed to each call. All
/// times are AI ticks. One interpreter serves one agent at a time and keeps only scratch memory.
class StateTreeInterpreter
{
public:
    using Frame = StateTreeState::Frame;

    /// Points the next calls at one agent. `readyTicks` holds one cell per cooldown slot of the program
    /// (StateTreeProgram::GetCooldownSlotCount). `agent` and `seed` key the Random draws.
    void Bind(StateTreeState& state, std::span<uint32> readyTicks, uint64 agent, uint64 seed);
    /// Enters the root and, by the policies, the states below it.
    void Start(const StateTreeProgram& program, const StateTreeHost& host, uint32 tick);
    /// One step at `tick`. `events` holds the program's event indices that the agent sees in this step.
    /// The states on the path are checked from the root down: the AbortIf modifiers, the transitions in
    /// order, then a Utility state's choice; the first that fires changes the path and ends the step. Then a
    /// state with no active child chooses one, else the tasks tick from the root down.
    void Step(const StateTreeProgram& program, const StateTreeHost& host, std::span<const uint16> events,
              uint32 tick);
    /// Moves the agent from `oldProgram` to `newProgram`. The path keeps each state, from the root down, whose
    /// stable id is in the new program under the same parent; the states below exit with Reloaded. A kept state
    /// whose task kinds changed exits its running tasks with Reloaded and starts its tasks again. The bound cells
    /// hold the cooldown slots of both programs: the exits write the old slots, then all bound cells are reset.
    /// @return false when the path has a state that `oldProgram` does not have: no task exits, the root starts.
    bool Remap(const StateTreeProgram& oldProgram, const StateTreeProgram& newProgram, const StateTreeHost& host,
               uint32 tick);
    /// Exits the running tasks of the path with Preempted, the deepest state first, and empties the path.
    /// `program` is the program the path belongs to. Writes no cooldown cell. A path with a state that
    /// `program` does not have exits no task and is emptied.
    /// @return false when the path had a state that `program` does not have.
    bool Stop(const StateTreeProgram& program, const StateTreeHost& host);
    /// The active path from the root down; empty after the root completes.
    std::span<const Frame> GetActivePath() const
    {
        assert(m_State != nullptr && "StateTreeInterpreter: call Bind first");
        return {m_State->Path.data(), m_State->Depth};
    }
    /// Running while the root is active; else the result the root completed with.
    StateTreeTaskStatus GetResult() const
    {
        assert(m_State != nullptr && "StateTreeInterpreter: call Bind first");
        return m_State->Result;
    }

private:
    void PushState(const StateTreeProgram& program, const StateTreeHost& host, uint16 state, uint32 tick);
    void StartTasks(const StateTreeProgram& program, const StateTreeHost& host, Frame& frame) const;
    void StopTasks(const StateTreeProgram& program, const StateTreeHost& host, Frame& frame,
                   StateTreeExitReason reason) const;
    // Exits the frames at `depth` and below it, the deepest first. A frame whose state `program` does not
    // have is removed with no exit.
    void PopTo(const StateTreeProgram& program, const StateTreeHost& host, uint8 depth, StateTreeExitReason reason,
               uint32 tick);
    // Enters children by the policies until a state has none or none can be entered. True when one entered.
    bool Descend(const StateTreeProgram& program, const StateTreeHost& host, uint32 tick);
    uint16 ChooseChild(const StateTreeProgram& program, const StateTreeHost& host, uint8 depth, uint32 tick);
    uint16 ChooseUtilityChild(const StateTreeProgram& program, const StateTreeHost& host, uint8 depth, uint32 tick);
    bool CanEnter(const StateTreeProgram& program, const StateTreeHost& host, uint16 state, uint32 tick) const;
    // The checks of one frame: modifiers, transitions, a Utility choice. True when the path changed.
    bool CheckFrame(const StateTreeProgram& program, const StateTreeHost& host, std::span<const uint16> events,
                    uint8 depth, uint32 tick);
    void TransitionTo(const StateTreeProgram& program, const StateTreeHost& host, uint16 target, uint32 tick);
    StateTreeTaskStatus TickTasks(const StateTreeProgram& program, const StateTreeHost& host, Frame& frame) const;
    void Complete(const StateTreeProgram& program, const StateTreeHost& host, uint8 depth, StateTreeTaskStatus result,
                  uint32 tick);

    StateTreeState* m_State = nullptr;
    // Per cooldown slot: the first tick its state can be entered again.
    std::span<uint32> m_ReadyTicks;
    uint64 m_Agent = 0;
    uint64 m_Seed = 0;
    // Scratch of the Utility choice, kept to reuse its memory.
    std::vector<float32> m_Columns;
    std::vector<uint32> m_ChildReadyTicks;
};

} // namespace GameEngine::AI
