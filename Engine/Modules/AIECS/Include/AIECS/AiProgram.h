#pragma once

#include "AI/OptionSet.h"
#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::AIECS
{

/// A utility program: one option set whose options each run one registered task.
/// Each OptionSet::Option::Action is a task id; column c of the option set reads the input
/// registered as ColumnInputs[c].
struct AiProgram
{
    static constexpr uint16 kNoCell = 0xFFFF;

    AI::OptionSet Options;
    std::vector<uint16> ColumnInputs;
    /// The first of Options.Options.size() agent cells that hold the tick each option is ready
    /// again after its cooldown. kNoCell when no option has CooldownTicks.
    uint16 CooldownCell = kNoCell;
    /// The agent cells that hold an entity handle's id. A scene save writes these cells as entity
    /// references, and a load points them at the loaded entities.
    std::vector<uint16> EntityCells;
    /// The GUID of the option-set asset the program came from, or a fixed GUID a game gives a program it
    /// builds in code. A scene save names the agent's program by it, and a load finds the newest program
    /// with it. Null: a save does not keep the agent's program.
    GUID StableId;
};

} // namespace GameEngine::AIECS
