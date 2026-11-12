#include "AIECS/Assets/AiOptionSetData.h"

#include "AiJsonFields.h"
#include "AIECS/Components/AiCells/AiCellsLarge.h"
#include "Serialization/JsonRead.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>

namespace GameEngine::AIECS
{

namespace
{

bool ParseOption(const Json& value, const std::string& where, AiOptionSetData& out, std::string& outError)
{
    if (!CheckObject(value, where, {"task", "weight", "rank", "cooldownTicks", "considerations"}, outError))
        return false;
    std::string task;
    if (!ReadName(value, "task", where, task, outError))
        return false;
    AI::OptionSet::Option option;
    uint32 rank = 0;
    uint32 cooldown = 0;
    if (!ReadFloat(value, "weight", where, option.Weight, outError) ||
        !ReadInteger(value, "rank", where, 0, std::numeric_limits<uint8>::max(), rank, outError) ||
        !ReadInteger(value, "cooldownTicks", where, 0, std::numeric_limits<uint16>::max(), cooldown, outError))
        return false;
    if (cooldown != 0 && out.CooldownCell == AiProgram::kNoCell)
    {
        outError = where + ": 'cooldownTicks' needs 'cooldownCell' in the file. Set 'cooldownCell' to the first of "
                           "the agent cells that hold the option cooldowns.";
        return false;
    }
    const auto considerations = value.find("considerations");
    if (considerations == value.end() || !considerations->is_array())
    {
        outError = where + ": 'considerations' is missing or is not a list. Write it as [ { \"input\": ... } ].";
        return false;
    }
    option.Rank = static_cast<uint8>(rank);
    option.CooldownTicks = static_cast<uint16>(cooldown);
    option.FirstConsideration = static_cast<uint16>(out.Options.Considerations.size());
    for (size_t index = 0; index < considerations->size(); ++index)
    {
        if (!ParseConsideration((*considerations)[index], where + ".considerations[" + std::to_string(index) + "]",
                                out.Options, out.InputNames, nullptr, outError))
            return false;
    }
    option.ConsiderationCount = static_cast<uint16>(out.Options.Considerations.size() - option.FirstConsideration);
    option.Action = static_cast<uint16>(out.Options.Options.size());
    out.Options.Options.push_back(option);
    out.TaskNames.push_back(std::move(task));
    return true;
}

bool ParsePreset(const Json& value, const std::string& where, AiOptionSetData& out, std::string& outError)
{
    if (!CheckObject(value, where, {"name", "cadenceTicks"}, outError))
        return false;
    AiDifficultyPreset preset;
    uint32 cadence = preset.CadenceTicks;
    if (!ReadName(value, "name", where, preset.Name, outError) ||
        !ReadInteger(value, "cadenceTicks", where, 1, std::numeric_limits<uint16>::max(), cadence, outError))
        return false;
    if (out.FindPreset(preset.Name) != nullptr)
    {
        outError = where + ": the preset name '" + preset.Name + "' is used twice. Give each preset its own name.";
        return false;
    }
    preset.CadenceTicks = static_cast<uint16>(cadence);
    out.Presets.push_back(std::move(preset));
    return true;
}

using ParseElement = bool (*)(const Json& value, const std::string& where, AiOptionSetData& out, std::string& outError);

// Parses each element of the optional list `key` with `parse`.
bool ParseList(const Json& root, const char* key, AiOptionSetData& out, std::string& outError, ParseElement parse)
{
    const auto list = root.find(key);
    if (list == root.end())
        return true;
    if (!list->is_array())
    {
        outError = std::string(key) + ": the value is not a list. Write it as [ { ... } ].";
        return false;
    }
    for (size_t index = 0; index < list->size(); ++index)
    {
        if (!parse((*list)[index], std::string(key) + "[" + std::to_string(index) + "]", out, outError))
            return false;
    }
    return true;
}

// Reads the optional list of agent cells that hold entities.
bool ReadEntityCells(const Json& root, AiOptionSetData& out, std::string& outError)
{
    const auto list = root.find("entityCells");
    if (list == root.end())
        return true;
    if (!list->is_array())
    {
        outError = "entityCells: the value is not a list. Write it as [ 2, 3 ].";
        return false;
    }
    const std::string count = std::to_string(Components::AiCellsLarge::kCount);
    for (const Json& cell : *list)
    {
        if (!cell.is_number_unsigned() || cell.get<uint64>() >= Components::AiCellsLarge::kCount)
        {
            outError = "entityCells: cell " + cell.dump() + " is outside the cells of the largest cell class " +
                       "(AiCellsLarge), which holds " + count + ". Use a whole number below " + count + ".";
            return false;
        }
        out.EntityCells.push_back(static_cast<uint16>(cell.get<uint64>()));
    }
    return true;
}

} // namespace

const AiDifficultyPreset* AiOptionSetData::FindPreset(std::string_view name) const
{
    const auto it = std::find_if(Presets.begin(), Presets.end(),
                                 [name](const AiDifficultyPreset& preset) { return preset.Name == name; });
    return it != Presets.end() ? &*it : nullptr;
}

bool ParseAiOptionSet(std::string_view text, AiOptionSetData& out, std::string& outError)
{
    out = {};
    Json root;
    if (!Serialization::ParseJson(text, root, outError))
    {
        outError = "the file is not JSON: " + outError;
        return false;
    }
    if (!CheckObject(root, "the file",
                     {"commitmentMargin", "minimumTicks", "noiseMargin", "cooldownCell", "entityCells", "options", "presets"}, outError))
        return false;
    if (const std::string repeated = Serialization::FirstRepeatedKey(text); !repeated.empty())
    {
        outError = "the file: the field '" + repeated + "' is written twice in one object. Keep one.";
        return false;
    }
    uint32 minimumTicks = 0;
    uint32 cooldownCell = out.CooldownCell;
    if (!ReadFloat(root, "commitmentMargin", "the file", out.Options.CommitmentMargin, outError) ||
        !ReadFloat(root, "noiseMargin", "the file", out.Options.NoiseMargin, outError) ||
        !ReadInteger(root, "minimumTicks", "the file", 0, std::numeric_limits<uint32>::max(), minimumTicks, outError) ||
        !ReadInteger(root, "cooldownCell", "the file", 0, std::numeric_limits<uint16>::max() - 1, cooldownCell,
                     outError))
        return false;
    out.Options.MinimumTicks = minimumTicks;
    out.CooldownCell = static_cast<uint16>(cooldownCell);
    return ReadEntityCells(root, out, outError) && ParseList(root, "options", out, outError, &ParseOption) &&
           ParseList(root, "presets", out, outError, &ParsePreset);
}

} // namespace GameEngine::AIECS
