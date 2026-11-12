#include "AIECS/AiRegistry.h"

#include "AI/StateTree/StateTreeCompileError.h"
#include "AIECS/Assets/AiOptionSetData.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeData.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsLarge.h"
#include "ECS/SystemScheduling.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Systems/AiApplySystem.h"
#include "Systems/AiDecideSystem.h"
#include "Systems/AiKnowledgeSystem.h"
#include "Systems/AiTickSystem.h"
#include "Types/FlatMap.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace GameEngine::AIECS
{

namespace
{

template <typename T>
uint16 Append(std::vector<T>& entries, T entry)
{
    assert(entries.size() < std::numeric_limits<uint16>::max() && "AiRegistry: a table holds at most 65535 entries");
    entries.push_back(std::move(entry));
    return static_cast<uint16>(entries.size() - 1);
}

// The index of `name` in `names`, or AiRegistry::kNoId. An empty name is never found.
uint16 FindName(const std::vector<std::string>& names, std::string_view name)
{
    if (name.empty())
        return AiRegistry::kNoId;
    const auto it = std::find(names.begin(), names.end(), name);
    return it != names.end() ? static_cast<uint16>(it - names.begin()) : AiRegistry::kNoId;
}

// True when the registration `module` may take the entry of a registered name: the entry was
// removed, or a newer generation of its module registers it again.
bool MayReplace(bool removed, const ECS::ModuleRegistrationStamp& owner, const ECS::ModuleRegistrationStamp& module)
{
    return removed || module.Supersedes(owner);
}

// True when `stamp` is an older generation of `moduleId`.
bool IsOlderGeneration(const ECS::ModuleRegistrationStamp& stamp, std::string_view moduleId, uint64 generation)
{
    return stamp.ModuleId == moduleId && stamp.Generation < generation;
}

// The non-empty names, comma separated, or "none".
std::string JoinNames(const std::vector<std::string>& names)
{
    std::vector<std::string_view> named;
    for (const std::string& name : names)
    {
        if (!name.empty())
            named.push_back(name);
    }
    return named.empty() ? std::string("none") : Join(named, ", ");
}

// Appends "<kind> '<name>'" to `out` for each id in `ids`.
void AppendKindNames(const char* kind, const std::vector<std::string>& names, const std::vector<uint16>& ids,
                     std::vector<std::string>& out)
{
    for (const uint16 id : ids)
        out.push_back(std::string(kind) + " '" + names[id] + "'");
}

// Resolves each name to its id in `registered`. Logs the first unknown name with the registered ones;
// `source` names the caller and the file kind.
bool ResolveNames(const char* source, const std::vector<std::string>& names, const std::vector<std::string>& registered,
                  const char* kind, std::vector<uint16>& outIds)
{
    outIds.clear();
    for (const std::string& name : names)
    {
        const uint16 id = FindName(registered, name);
        if (id == AiRegistry::kNoId)
        {
            Logger::Log::Error("{} names the {} '{}', but no module registered it. Register it with that name, or "
                               "use one of the registered {}s: {}.",
                               source, kind, name, kind, JoinNames(registered));
            return false;
        }
        outIds.push_back(id);
    }
    return true;
}

// The registered id of each name in `names`. Each name must be registered.
std::vector<uint16> ResolveCheckedNames(const std::vector<std::string>& names,
                                        const std::vector<std::string>& registered)
{
    std::vector<uint16> ids;
    ids.reserve(names.size());
    for (const std::string& name : names)
        ids.push_back(FindName(registered, name));
    return ids;
}

// One name list of a program file, with a flag per name that is true when a module registered the name.
struct NameCheck
{
    NameCheck(const std::vector<std::string>& names, const std::vector<std::string>& registered, const char* kind)
        : Names(names), Registered(registered), Kind(kind)
    {
        Known.reserve(names.size());
        for (const std::string& name : names)
            Known.push_back(FindName(registered, name) != AiRegistry::kNoId);
        Named.assign(names.size(), false);
    }

    const std::vector<std::string>& Names;
    const std::vector<std::string>& Registered;
    const char* Kind;
    std::vector<bool> Known;
    // True for each name that a state uses: the program-level pass refuses only the names that no state uses.
    std::vector<bool> Named;
};

// Adds a refusal to `out` when no module registered the name at `index` of `check`; `where` names its user.
void CheckName(NameCheck& check, uint16 index, const std::string& where, uint32 stateId, AI::StateTreeCompileItem item,
               uint16 itemIndex, std::vector<AI::StateTreeCompileError>& out)
{
    if (index >= check.Names.size())
        return;
    check.Named[index] = true;
    if (check.Known[index])
        return;
    AI::StateTreeCompileError& refusal = out.emplace_back();
    refusal.Message = where + " names the " + check.Kind + " '" + check.Names[index] + "', but no module registered it. ";
    refusal.Message += check.Registered.empty()
                           ? "Register '" + check.Names[index] + "' in your game's AI module."
                           : std::string("Register it with that name, or use one of the registered ") + check.Kind +
                                 "s: " + JoinNames(check.Registered) + ".";
    refusal.StateId = stateId;
    refusal.Item = item;
    refusal.ItemIndex = itemIndex;
}

// How many states of a program use one name, and how many of them the check passed.
struct NameUse
{
    uint16 Count = 0;
    uint16 Seen = 0;
};

// The use of each state name of `source`. A state with no name has no entry.
FlatMap<std::string, NameUse> CountStateNames(const AiStateTreeSource& source)
{
    FlatMap<std::string, NameUse> uses;
    uses.Reserve(source.States.size());
    for (const AiStateTreeSource::StateText& state : source.States)
    {
        if (state.Name.empty())
            continue;
        if (NameUse* use = uses.Find(state.Name))
            ++use->Count;
        else
            uses.InsertOrAssign(state.Name, NameUse{1, 0});
    }
    return uses;
}

// Refuses the name of state `index` when it is the second state with a name that more than one state uses.
void CheckRepeatedName(const AiStateTreeSource& source, size_t index, FlatMap<std::string, NameUse>& uses,
                       std::vector<AI::StateTreeCompileError>& out)
{
    if (index >= source.States.size())
        return;
    const std::string& name = source.States[index].Name;
    NameUse* use = name.empty() ? nullptr : uses.Find(name);
    if (!use || use->Count < 2 || ++use->Seen != 2)
        return;
    AI::StateTreeCompileError& refusal = out.emplace_back();
    refusal.Message = std::to_string(use->Count) + " states are named " + name + ". Give each state its own name.";
    refusal.StateId = source.Definition.States[index].Id;
    refusal.Item = AI::StateTreeCompileItem::State;
}

// Refuses each unregistered name that state `index` of `source` uses: its entry condition, its tasks, its
// transitions, its modifiers, then the inputs of its options.
void CheckStateNames(const AiStateTreeSource& source, size_t index, NameCheck& tasks, NameCheck& conditions,
                     NameCheck& events, NameCheck& inputs, std::vector<AI::StateTreeCompileError>& out)
{
    using Item = AI::StateTreeCompileItem;
    const AI::StateTreeDefinition::State& state = source.Definition.States[index];
    const std::string where = StateRefusalName(source, index);
    CheckName(conditions, state.EnterCondition, where, state.Id, Item::EnterCondition, 0, out);
    for (uint16 item = 0; item < state.Tasks.size(); ++item)
        CheckName(tasks, state.Tasks[item], where, state.Id, Item::Task, item, out);
    for (uint16 item = 0; item < state.Transitions.size(); ++item)
    {
        const AI::StateTreeDefinition::Transition& transition = state.Transitions[item];
        if (transition.Trigger == AI::StateTreeTrigger::Condition)
            CheckName(conditions, transition.Condition, where, state.Id, Item::Transition, item, out);
        if (transition.Trigger == AI::StateTreeTrigger::Event)
            CheckName(events, transition.Event, where, state.Id, Item::Transition, item, out);
    }
    for (uint16 item = 0; item < state.Modifiers.size(); ++item)
    {
        const AI::StateTreeModifier& modifier = state.Modifiers[item];
        if (modifier.Kind == AI::StateTreeModifierKind::AbortIf)
            CheckName(conditions, modifier.Condition, where, state.Id, Item::Modifier, item, out);
    }
    const AI::OptionSet& options = state.Utility;
    for (uint16 item = 0; item < options.Options.size(); ++item)
    {
        const AI::OptionSet::Option& option = options.Options[item];
        const uint32 end = option.FirstConsideration + option.ConsiderationCount;
        for (uint32 consideration = option.FirstConsideration; consideration < end; ++consideration)
            CheckName(inputs, options.Considerations[consideration].Column, where, state.Id, Item::Option, item, out);
    }
}

// The option, consideration and column indices of the program. Logs the first bad index with its fix.
bool CheckProgramIndices(const AiProgram& program, size_t taskCount, size_t inputCount)
{
    const AI::OptionSet& options = program.Options;
    for (size_t option = 0; option < options.Options.size(); ++option)
    {
        const AI::OptionSet::Option& entry = options.Options[option];
        if (entry.Action >= taskCount)
        {
            Logger::Log::Error("AiRegistry::AddProgram: option {} names task {}, but {} tasks are registered. "
                               "Set Action to an id that RegisterTask returned.",
                               option, entry.Action, taskCount);
            return false;
        }
        if (static_cast<size_t>(entry.FirstConsideration) + entry.ConsiderationCount > options.Considerations.size())
        {
            Logger::Log::Error("AiRegistry::AddProgram: option {} uses considerations {} to {}, but the program has {}. "
                               "Add them to Options.Considerations or lower ConsiderationCount.",
                               option, entry.FirstConsideration, entry.FirstConsideration + entry.ConsiderationCount - 1,
                               options.Considerations.size());
            return false;
        }
    }
    for (size_t index = 0; index < options.Considerations.size(); ++index)
    {
        const AI::OptionSet::Consideration& consideration = options.Considerations[index];
        if (consideration.Column >= program.ColumnInputs.size())
        {
            Logger::Log::Error("AiRegistry::AddProgram: consideration {} reads column {}, but the program has {} columns. "
                               "Add the input to ColumnInputs.",
                               index, consideration.Column, program.ColumnInputs.size());
            return false;
        }
        if (consideration.Curve >= options.Curves.size())
        {
            Logger::Log::Error("AiRegistry::AddProgram: consideration {} uses curve {}, but the program has {} curves. "
                               "Add the curve to Options.Curves.",
                               index, consideration.Curve, options.Curves.size());
            return false;
        }
    }
    for (size_t column = 0; column < program.ColumnInputs.size(); ++column)
    {
        if (program.ColumnInputs[column] >= inputCount)
        {
            Logger::Log::Error("AiRegistry::AddProgram: column {} names input {}, but {} inputs are registered. "
                               "Use an id that RegisterInput returned.",
                               column, program.ColumnInputs[column], inputCount);
            return false;
        }
    }
    return true;
}

// A program with a cooldown has cooldown cells, and they fit in the largest cell class.
bool CheckCooldownCell(const AiProgram& program)
{
    if (program.CooldownCell == AiProgram::kNoCell)
    {
        const std::vector<AI::OptionSet::Option>& options = program.Options.Options;
        for (size_t option = 0; option < options.size(); ++option)
        {
            if (options[option].CooldownTicks == 0)
                continue;
            Logger::Log::Error("AiRegistry::AddProgram: option {} has CooldownTicks {}, but the program has no "
                               "CooldownCell. Set CooldownCell to the first of {} free cells.",
                               option, options[option].CooldownTicks, options.size());
            return false;
        }
        return true;
    }
    if (program.CooldownCell + program.Options.Options.size() <= Components::AiCellsLarge::kCount)
        return true;
    Logger::Log::Error("AiRegistry::AddProgram: CooldownCell {} with {} options needs cells up to {}, but the largest "
                       "cell class (AiCellsLarge) holds {}. Lower CooldownCell.",
                       program.CooldownCell, program.Options.Options.size(),
                       program.CooldownCell + program.Options.Options.size() - 1, Components::AiCellsLarge::kCount);
    return false;
}

// Every entity cell is a cell of the largest cell class.
bool CheckEntityCells(const AiProgram& program)
{
    for (const uint16 cell : program.EntityCells)
    {
        if (cell < Components::AiCellsLarge::kCount)
            continue;
        Logger::Log::Error("AiRegistry::AddProgram: program {}: entity cell {} is outside the cells of the largest "
                           "cell class (AiCellsLarge), which holds {}. Use a whole number below {}.",
                           program.StableId.IsNull() ? std::string("with no stable id") : program.StableId.ToString(),
                           cell, Components::AiCellsLarge::kCount, Components::AiCellsLarge::kCount);
        return false;
    }
    return true;
}

} // namespace

AiRegistry& AiRegistry::Get()
{
    static AiRegistry registry;
    return registry;
}

void AiRegistry::AddSystemsToSchedule(ECS::SystemScheduleBuilder& schedule)
{
    // AiDecideSystem fills the rows and AiApplySystem drains them in the same step.
    auto rows = std::make_shared<std::vector<AiDecisionRow>>();
    schedule.Add<AiTickSystem>("AiTickSystem", ECS::SystemPhase::Early, 10, {}, std::ref(*this), rows);
    schedule.Add<AiKnowledgeSystem>("AiKnowledgeSystem", ECS::SystemPhase::Early, 12, {"AiTickSystem"}, std::cref(*this));
    schedule.Add<AiDecideSystem>("AiDecideSystem", ECS::SystemPhase::Early, 14, {"AiKnowledgeSystem"}, std::cref(*this), rows);
    schedule.Add<AiApplySystem>("AiApplySystem", ECS::SystemPhase::Early, 16, {"AiDecideSystem"}, std::cref(*this), rows);
}

uint16 AiRegistry::RegisterInput(AiInputFunction input, AiInputThreading threading, std::string_view name,
                                 std::string_view caption, std::string_view unit)
{
    ++m_Version;
    if (input == nullptr)
    {
        Logger::Log::Error("AiRegistry::RegisterInput: the input function is null, so no input is added. Pass a "
                           "function that fills out[i] for agents[i].");
        return kNoId;
    }
    const ECS::ModuleRegistrationStamp& module = ECS::GetActiveRegistrationModule();
    const uint16 existing = FindName(m_InputNames, name);
    if (existing != kNoId)
    {
        if (!MayReplace(m_Inputs[existing].Function == nullptr, m_InputOwners[existing].Current, module))
        {
            Logger::Log::Error("AiRegistry::RegisterInput: the input '{}' is registered, so no input is added. Give "
                               "the input a name of its own.",
                               name);
            return kNoId;
        }
        m_ReplacedInputs[existing] = m_Inputs[existing];
        m_InputOwners[existing] = {module, m_InputOwners[existing].Current};
        m_Inputs[existing] = {input, threading, std::string(caption), std::string(unit)};
        RefreshRunnablePrograms();
        return existing;
    }
    m_InputNames.emplace_back(name);
    m_InputOwners.push_back({module, {}});
    m_ReplacedInputs.emplace_back();
    return Append(m_Inputs, InputEntry{input, threading, std::string(caption), std::string(unit)});
}

uint16 AiRegistry::RegisterCondition(AiConditionFunction condition, std::string_view name)
{
    ++m_Version;
    if (condition == nullptr || name.empty())
    {
        Logger::Log::Error("AiRegistry::RegisterCondition: the condition '{}' has no function or no name, so no "
                           "condition is added. Pass a function that sets out[i] for agents[i] and the name a "
                           "program file gives it.",
                           name);
        return kNoId;
    }
    const ECS::ModuleRegistrationStamp& module = ECS::GetActiveRegistrationModule();
    const uint16 existing = FindName(m_ConditionNames, name);
    if (existing != kNoId)
    {
        if (!MayReplace(m_Conditions[existing] == nullptr, m_ConditionOwners[existing].Current, module))
        {
            Logger::Log::Error("AiRegistry::RegisterCondition: the condition '{}' is registered, so no condition is "
                               "added. Give the condition a name of its own.",
                               name);
            return kNoId;
        }
        m_ReplacedConditions[existing] = m_Conditions[existing];
        m_ConditionOwners[existing] = {module, m_ConditionOwners[existing].Current};
        m_Conditions[existing] = condition;
        RefreshRunnablePrograms();
        return existing;
    }
    m_ConditionNames.emplace_back(name);
    m_ConditionOwners.push_back({module, {}});
    m_ReplacedConditions.emplace_back();
    return Append(m_Conditions, condition);
}

uint16 AiRegistry::RegisterEventType(std::string_view name)
{
    ++m_Version;
    if (name.empty())
    {
        Logger::Log::Error("AiRegistry::RegisterEventType: the name is empty, so no event type is added. Pass the "
                           "name a program file gives in an \"event\" field.");
        return kNoId;
    }
    const ECS::ModuleRegistrationStamp& module = ECS::GetActiveRegistrationModule();
    const uint16 existing = FindName(m_EventTypeNames, name);
    if (existing != kNoId)
    {
        const bool removed = m_EventTypes[existing] == 0;
        // Only a module load takes a removed name, so that a reload can remove it again.
        if (removed && !module.IsSet())
        {
            Logger::Log::Error("AiRegistry::RegisterEventType: the event '{}' was removed by a reload of its module, "
                               "so a call outside a module load does not register it again. Register it with "
                               "GE_REGISTER_AI_EVENT in a game module.",
                               name);
            return kNoId;
        }
        if (!MayReplace(removed, m_EventTypeOwners[existing].Current, module))
        {
            Logger::Log::Error("AiRegistry::RegisterEventType: the event '{}' is registered, so no event type is "
                               "added. A program names it with no second registration, and code gets its id from "
                               "AiRegistry::FindEventType.",
                               name);
            return kNoId;
        }
        // The name keeps its id. A module takes a removed name; only a newer generation of the owner's module takes
        // a registered name.
        m_ReplacedEventTypes[existing] = m_EventTypes[existing];
        m_EventTypeOwners[existing] = {module, m_EventTypeOwners[existing].Current};
        m_EventTypes[existing] = 1;
        RefreshRunnablePrograms();
        return existing;
    }
    m_EventTypeNames.emplace_back(name);
    m_EventTypeOwners.push_back({module, {}});
    m_ReplacedEventTypes.emplace_back();
    return Append(m_EventTypes, uint8{1});
}

uint16 AiRegistry::FindEventType(std::string_view name) const
{
    const uint16 id = FindName(m_EventTypeNames, name);
    return id != kNoId && m_EventTypes[id] != 0 ? id : kNoId;
}

std::vector<std::string> AiRegistry::GetEventTypeNames() const
{
    std::vector<std::string> names;
    names.reserve(m_EventTypeNames.size());
    for (size_t id = 0; id < m_EventTypeNames.size(); ++id)
        if (m_EventTypes[id] != 0)
            names.push_back(m_EventTypeNames[id]);
    return names;
}

std::vector<AiRegistry::NamedInput> AiRegistry::GetNamedInputs() const
{
    std::vector<NamedInput> inputs;
    inputs.reserve(m_InputNames.size());
    for (size_t id = 0; id < m_InputNames.size(); ++id)
        if (m_Inputs[id].Function && !m_InputNames[id].empty())
            inputs.push_back({m_InputNames[id], m_Inputs[id].Caption, m_Inputs[id].Unit});
    return inputs;
}

std::vector<std::string> AiRegistry::GetInputNames() const
{
    std::vector<std::string> names;
    for (NamedInput& input : GetNamedInputs())
        names.push_back(std::move(input.Name));
    return names;
}

std::vector<std::string> AiRegistry::GetConditionNames() const
{
    std::vector<std::string> names;
    names.reserve(m_ConditionNames.size());
    for (size_t id = 0; id < m_ConditionNames.size(); ++id)
        if (m_Conditions[id])
            names.push_back(m_ConditionNames[id]);
    return names;
}

uint16 AiRegistry::RegisterTask(const AiTaskDefinition& task)
{
    ++m_Version;
    if (task.Tick == nullptr)
    {
        Logger::Log::Error("AiRegistry::RegisterTask: the task has no Tick function, so no task is added. Set "
                           "AiTaskDefinition::Tick to a function that returns the task's status.");
        return kNoId;
    }
    // The registry keeps its own copy of the name; the view can point into a module that unloads.
    AiTaskDefinition stored = task;
    stored.Name = {};
    const ECS::ModuleRegistrationStamp& module = ECS::GetActiveRegistrationModule();
    const uint16 existing = FindName(m_TaskNames, task.Name);
    if (existing != kNoId)
    {
        if (!MayReplace(m_Tasks[existing].Tick == nullptr, m_TaskOwners[existing].Current, module))
        {
            Logger::Log::Error("AiRegistry::RegisterTask: the task '{}' is registered, so no task is added. Give the "
                               "task a name of its own.",
                               task.Name);
            return kNoId;
        }
        m_ReplacedTasks[existing] = m_Tasks[existing];
        m_TaskOwners[existing] = {module, m_TaskOwners[existing].Current};
        m_Tasks[existing] = stored;
        RefreshRunnablePrograms();
        return existing;
    }
    m_TaskNames.emplace_back(task.Name);
    m_TaskOwners.push_back({module, {}});
    m_ReplacedTasks.emplace_back();
    return Append(m_Tasks, stored);
}

uint16 AiRegistry::RegisterSensor(AiSensorFunction sensor)
{
    ++m_Version;
    if (sensor == nullptr)
    {
        Logger::Log::Error("AiRegistry::RegisterSensor: the sensor function is null, so no sensor is added. Pass a "
                           "function that writes what the group sees into its records.");
        return kNoId;
    }
    m_SensorOwners.push_back({ECS::GetActiveRegistrationModule(), {}});
    return Append(m_Sensors, sensor);
}

uint16 AiRegistry::RegisterCommand(AiCommandFunction command)
{
    ++m_Version;
    if (command == nullptr)
    {
        Logger::Log::Error("AiRegistry::RegisterCommand: the command function is null, so no command is added. Pass "
                           "a function that carries out the order.");
        return kNoId;
    }
    m_CommandOwners.push_back({ECS::GetActiveRegistrationModule(), {}});
    return Append(m_Commands, command);
}

uint16 AiRegistry::AddProgram(AiProgram program)
{
    if (!CheckProgramIndices(program, m_Tasks.size(), m_Inputs.size()) || !CheckCooldownCell(program) ||
        !CheckEntityCells(program))
        return Components::AiAgent::kNoProgram;
    if (m_Programs.size() >= Components::AiAgent::kStateTreeProgramBit)
    {
        Logger::Log::Error("AiRegistry::AddProgram: the registry holds {} utility programs, the most it can hold, so "
                           "the program is not added. Compile each option set once and share its id.",
                           m_Programs.size());
        return Components::AiAgent::kNoProgram;
    }
    m_ProgramRunnable.push_back(IsRunnable(program) ? 1 : 0);
    return Append(m_Programs, std::move(program));
}

uint16 AiRegistry::CompileProgram(const AiOptionSetData& data)
{
    std::vector<uint16> tasks;
    AiProgram program;
    constexpr const char* kSource = "AiRegistry::CompileProgram: the option set";
    if (!ResolveNames(kSource, data.TaskNames, m_TaskNames, "task", tasks) ||
        !ResolveNames(kSource, data.InputNames, m_InputNames, "input", program.ColumnInputs))
        return Components::AiAgent::kNoProgram;
    program.Options = data.Options;
    for (AI::OptionSet::Option& option : program.Options.Options)
        option.Action = tasks[option.Action];
    program.CooldownCell = data.CooldownCell;
    program.EntityCells = data.EntityCells;
    program.StableId = data.StableId;
    return AddProgram(std::move(program));
}

std::vector<AI::StateTreeCompileError> AiRegistry::CheckStateTreeNames(const AiStateTreeSource& source) const
{
    // A condition, an event or an input that its module removed is not registered: the check uses the lists of
    // the getters.
    const std::vector<std::string> conditionNames = GetConditionNames();
    const std::vector<std::string> eventNames = GetEventTypeNames();
    const std::vector<std::string> inputNames = GetInputNames();
    NameCheck tasks(source.TaskNames, m_TaskNames, "task");
    NameCheck conditions(source.ConditionNames, conditionNames, "condition");
    NameCheck events(source.EventNames, eventNames, "event");
    NameCheck inputs(source.InputNames, inputNames, "input");
    FlatMap<std::string, NameUse> stateNames = CountStateNames(source);
    std::vector<AI::StateTreeCompileError> refusals;
    for (size_t index = 0; index < source.Definition.States.size(); ++index)
    {
        CheckRepeatedName(source, index, stateNames, refusals);
        CheckStateNames(source, index, tasks, conditions, events, inputs, refusals);
    }
    for (NameCheck* check : {&tasks, &conditions, &events, &inputs})
    {
        for (uint16 index = 0; index < check->Names.size(); ++index)
        {
            if (!check->Named[index])
                CheckName(*check, index, "the program", AI::StateTreeCompileError::kNoState,
                          AI::StateTreeCompileItem::Program, 0, refusals);
        }
    }
    return refusals;
}

bool AiRegistry::ResolveStateTree(const char* source, const AiStateTreeData& data, AiStateTreeProgram& out) const
{
    if (const std::vector<AI::StateTreeCompileError> refusals = CheckStateTreeNames(data.Source); !refusals.empty())
    {
        Logger::Log::Error("{}: {}", source, refusals.front().Message);
        return false;
    }
    out.Tasks = ResolveCheckedNames(data.Source.TaskNames, m_TaskNames);
    out.Conditions = ResolveCheckedNames(data.Source.ConditionNames, m_ConditionNames);
    out.Events = ResolveCheckedNames(data.Source.EventNames, m_EventTypeNames);
    out.Inputs = ResolveCheckedNames(data.Source.InputNames, m_InputNames);
    out.Program = data.Program;
    out.StableId = data.StableId;
    return true;
}

uint16 AiRegistry::CompileStateTree(const AiStateTreeData& data)
{
    AiStateTreeProgram program;
    if (!ResolveStateTree("AiRegistry::CompileStateTree: the program file", data, program))
        return Components::AiAgent::kNoProgram;
    if (m_StateTrees.size() >= Components::AiAgent::kStateTreeProgramBit - 1)
    {
        Logger::Log::Error("AiRegistry::CompileStateTree: the registry holds {} state-tree programs, the most it can "
                           "hold, so the program is not added. Compile each program file once and share its id.",
                           m_StateTrees.size());
        return Components::AiAgent::kNoProgram;
    }
    m_StateTreeRunnable.push_back(IsRunnable(program) ? 1 : 0);
    m_StateTrees.push_back(std::move(program));
    m_StateTreeSources.emplace_back();
    return static_cast<uint16>((m_StateTrees.size() - 1) | Components::AiAgent::kStateTreeProgramBit);
}

uint16 AiRegistry::LoadStateTree(SharedPtr<const AiStateTreeAsset> asset)
{
    SharedPtr<const AiStateTreeData> data = asset != nullptr ? asset->GetData() : nullptr;
    if (data == nullptr)
    {
        Logger::Log::Error("AiRegistry::LoadStateTree: the asset has no data, so no program is added. Load the asset "
                           "first, and fix the errors its load logged.");
        return Components::AiAgent::kNoProgram;
    }
    const uint16 id = CompileStateTree(*data);
    if (id != Components::AiAgent::kNoProgram)
        m_StateTreeSources.back() = {std::move(asset), std::move(data)};
    return id;
}

void AiRegistry::ReloadStateTrees(uint64 worldId,
                                  FunctionRef<void(uint16 id, const AiStateTreeProgram& oldProgram)> remap)
{
    // A world at its first AI tick has no path of a program that an earlier reload replaced.
    const uint32* found = m_RemappedGenerations.Find(worldId);
    const uint32 remapped = found != nullptr ? *found : m_ReloadGeneration;
    CompileReloadedStateTrees();
    // The agents of the world run the oldest program of each id that the world did not remap.
    for (size_t index = 0; index < m_RetiredStateTrees.size(); ++index)
    {
        const RetiredStateTree& retired = m_RetiredStateTrees[index];
        const auto sameId = [&retired, remapped](const RetiredStateTree& other)
        { return other.Id == retired.Id && other.Generation > remapped; };
        if (retired.Generation > remapped &&
            std::none_of(m_RetiredStateTrees.begin(), m_RetiredStateTrees.begin() + index, sameId))
            remap(retired.Id, retired.Program);
    }
    m_RemappedGenerations.InsertOrAssign(worldId, m_ReloadGeneration);
    FreeRetiredStateTrees();
}

void AiRegistry::CompileReloadedStateTrees()
{
    for (size_t index = 0; index < m_StateTrees.size(); ++index)
    {
        StateTreeSource& source = m_StateTreeSources[index];
        if (source.Asset == nullptr)
            continue;
        SharedPtr<const AiStateTreeData> data = source.Asset->GetData();
        if (data == nullptr || data == source.Data)
            continue;
        // The source keeps the data, so a refused file is reported once and its address is not used again.
        source.Data = data;
        AiStateTreeProgram program;
        if (!ResolveStateTree("AiRegistry::ReloadStateTrees: the reloaded program file", *data, program))
            continue;
        std::swap(m_StateTrees[index], program);
        m_StateTreeRunnable[index] = IsRunnable(m_StateTrees[index]) ? 1 : 0;
        m_RetiredStateTrees.push_back({static_cast<uint16>(index | Components::AiAgent::kStateTreeProgramBit),
                                       ++m_ReloadGeneration, std::move(program)});
    }
}

void AiRegistry::FreeRetiredStateTrees()
{
    m_RemappedGenerations.EraseIf([](uint64 worldId, uint32)
                                  { return !ECS::World::FindLiveEntityHandleEpoch(worldId).has_value(); });
    if (m_RetiredStateTrees.empty())
        return;
    uint32 oldest = m_ReloadGeneration;
    m_RemappedGenerations.ForEach([&oldest](uint64, uint32 generation) { oldest = std::min(oldest, generation); });
    std::erase_if(m_RetiredStateTrees, [oldest](const RetiredStateTree& retired)
                  { return retired.Generation <= oldest; });
}

const AiStateTreeProgram* AiRegistry::FindStateTree(uint16 id) const
{
    if (id == Components::AiAgent::kNoProgram || (id & Components::AiAgent::kStateTreeProgramBit) == 0)
        return nullptr;
    const size_t index = id & ~Components::AiAgent::kStateTreeProgramBit;
    return index < m_StateTrees.size() ? &m_StateTrees[index] : nullptr;
}

bool AiRegistry::IsProgramRunnable(uint16 id) const
{
    if ((id & Components::AiAgent::kStateTreeProgramBit) == 0)
        return m_ProgramRunnable[id] != 0;
    return m_StateTreeRunnable[id & ~Components::AiAgent::kStateTreeProgramBit] != 0;
}

template <typename T>
void AiRegistry::PurgeTable(std::vector<T>& values, std::vector<EntryOwner>& owners,
                            std::type_identity_t<std::vector<T>>* replaced, std::string_view moduleId,
                            uint64 generation)
{
    for (size_t id = 0; id < values.size(); ++id)
    {
        EntryOwner& owner = owners[id];
        if (!owner.Current.Matches(moduleId, generation))
            continue;
        // An entry that replaced none restores the empty value, which removes it.
        values[id] = replaced != nullptr ? std::exchange((*replaced)[id], T{}) : T{};
        owner = {owner.Replaced, {}};
    }
}

template <typename T>
std::vector<uint16> AiRegistry::RetireTable(std::vector<T>& values, std::vector<EntryOwner>& owners,
                                            std::type_identity_t<std::vector<T>>* replaced,
                                            std::string_view moduleId, uint64 currentGeneration)
{
    std::vector<uint16> retired;
    for (size_t id = 0; id < values.size(); ++id)
    {
        EntryOwner& owner = owners[id];
        if (IsOlderGeneration(owner.Current, moduleId, currentGeneration))
        {
            values[id] = T{};
            owner.Current = {};
            retired.push_back(static_cast<uint16>(id));
        }
        if (IsOlderGeneration(owner.Replaced, moduleId, currentGeneration))
        {
            if (replaced != nullptr)
                (*replaced)[id] = T{};
            owner.Replaced = {};
        }
    }
    return retired;
}

void AiRegistry::PurgeModuleRegistrations(std::string_view moduleId, uint64 generation)
{
    if (moduleId.empty())
        return;
    PurgeTable(m_Inputs, m_InputOwners, &m_ReplacedInputs, moduleId, generation);
    PurgeTable(m_Conditions, m_ConditionOwners, &m_ReplacedConditions, moduleId, generation);
    PurgeTable(m_EventTypes, m_EventTypeOwners, &m_ReplacedEventTypes, moduleId, generation);
    PurgeTable(m_Tasks, m_TaskOwners, &m_ReplacedTasks, moduleId, generation);
    PurgeTable(m_Sensors, m_SensorOwners, nullptr, moduleId, generation);
    PurgeTable(m_Commands, m_CommandOwners, nullptr, moduleId, generation);
    ++m_Version;
    RefreshRunnablePrograms();
}

void AiRegistry::RetireSupersededModuleRegistrations(std::string_view moduleId, uint64 currentGeneration)
{
    if (moduleId.empty())
        return;
    std::vector<std::string> removed;
    AppendKindNames("input", m_InputNames,
                    RetireTable(m_Inputs, m_InputOwners, &m_ReplacedInputs, moduleId, currentGeneration), removed);
    AppendKindNames("condition", m_ConditionNames,
                    RetireTable(m_Conditions, m_ConditionOwners, &m_ReplacedConditions, moduleId, currentGeneration),
                    removed);
    AppendKindNames("event", m_EventTypeNames,
                    RetireTable(m_EventTypes, m_EventTypeOwners, &m_ReplacedEventTypes, moduleId, currentGeneration),
                    removed);
    AppendKindNames("task", m_TaskNames,
                    RetireTable(m_Tasks, m_TaskOwners, &m_ReplacedTasks, moduleId, currentGeneration), removed);
    RetireTable(m_Sensors, m_SensorOwners, nullptr, moduleId, currentGeneration);
    RetireTable(m_Commands, m_CommandOwners, nullptr, moduleId, currentGeneration);
    if (!removed.empty())
        Logger::Log::Warning("AiRegistry: the reload of module '{}' removed the names that its new code does not "
                             "register: {}. A program that uses a removed name does not run until the code "
                             "registers the name again.",
                             moduleId, JoinNames(removed));
    ++m_Version;
    RefreshRunnablePrograms();
}

bool AiRegistry::IsRunnable(const AiProgram& program) const
{
    for (const AI::OptionSet::Option& option : program.Options.Options)
    {
        if (m_Tasks[option.Action].Tick == nullptr)
            return false;
    }
    for (const uint16 input : program.ColumnInputs)
    {
        if (m_Inputs[input].Function == nullptr)
            return false;
    }
    return true;
}

bool AiRegistry::IsRunnable(const AiStateTreeProgram& program) const
{
    const auto removedTask = [this](uint16 task) { return m_Tasks[task].Tick == nullptr; };
    const auto removedCondition = [this](uint16 condition) { return m_Conditions[condition] == nullptr; };
    const auto removedEvent = [this](uint16 event) { return m_EventTypes[event] == 0; };
    const auto removedInput = [this](uint16 input) { return m_Inputs[input].Function == nullptr; };
    return std::none_of(program.Tasks.begin(), program.Tasks.end(), removedTask) &&
           std::none_of(program.Conditions.begin(), program.Conditions.end(), removedCondition) &&
           std::none_of(program.Events.begin(), program.Events.end(), removedEvent) &&
           std::none_of(program.Inputs.begin(), program.Inputs.end(), removedInput);
}

void AiRegistry::RefreshRunnablePrograms()
{
    for (size_t id = 0; id < m_Programs.size(); ++id)
        m_ProgramRunnable[id] = IsRunnable(m_Programs[id]) ? 1 : 0;
    for (size_t index = 0; index < m_StateTrees.size(); ++index)
        m_StateTreeRunnable[index] = IsRunnable(m_StateTrees[index]) ? 1 : 0;
}

} // namespace GameEngine::AIECS
