#pragma once

#include "AI/OptionSet.h"
#include "AIECS/AiProgram.h"
#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::AIECS
{

/// One difficulty preset of an option-set file. A game reads it when it adds an agent.
struct AiDifficultyPreset
{
    std::string Name;
    /// The AI ticks between two decisions of the agent (AiAgent::Cadence).
    uint16 CadenceTicks = 1;
};

/// What an option-set file (.aioptions) holds. Options[i].Action is the index i; TaskNames[i] names
/// the task of option i and InputNames[c] the input of column c. AiRegistry::CompileProgram resolves
/// the names to the registered ids.
struct AiOptionSetData
{
    AI::OptionSet Options;
    std::vector<std::string> TaskNames;
    std::vector<std::string> InputNames;
    /// AiProgram::CooldownCell.
    uint16 CooldownCell = AiProgram::kNoCell;
    /// AiProgram::EntityCells.
    std::vector<uint16> EntityCells;
    /// AiProgram::StableId: the GUID of the asset that loaded the file. Null for text parsed with no asset.
    GUID StableId;
    std::vector<AiDifficultyPreset> Presets;

    /// The preset named `name`, or null when the file has none of that name.
    const AiDifficultyPreset* FindPreset(std::string_view name) const;
};

/// Reads an option-set file's text. The format has one version and no version field. Refused, with
/// the field and the fix in `outError`: text that is not a JSON object, a field the format does not
/// have or that one object writes twice, a field of the wrong type or outside its range, an option with
/// no task or with cooldownTicks and no cooldownCell, a consideration with no input or no curve, a curve
/// preset outside its list, and a preset with no name or a repeated name.
bool ParseAiOptionSet(std::string_view text, AiOptionSetData& out, std::string& outError);

} // namespace GameEngine::AIECS
