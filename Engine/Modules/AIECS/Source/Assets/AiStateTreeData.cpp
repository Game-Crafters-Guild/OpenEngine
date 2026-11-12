#include "AIECS/Assets/AiStateTree/AiStateTreeData.h"

#include "AI/StateTree/StateTreeCompileError.h"
#include "AI/StateTree/StateTreeDefinition.h"
#include "AiJsonFields.h"
#include "Serialization/JsonRead.h"
#include "Types/StringHash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <utility>

namespace GameEngine::AIECS
{

namespace
{

constexpr Keywords<AI::StateTreePolicy, 4> kPolicies{{{"first", AI::StateTreePolicy::First},
                                                      {"sequence", AI::StateTreePolicy::Sequence},
                                                      {"utility", AI::StateTreePolicy::Utility},
                                                      {"random", AI::StateTreePolicy::Random}}};
constexpr Keywords<AI::StateTreeJoin, 4> kJoins{{{"sequence", AI::StateTreeJoin::Sequence},
                                                 {"all", AI::StateTreeJoin::All},
                                                 {"any", AI::StateTreeJoin::Any},
                                                 {"primary", AI::StateTreeJoin::Primary}}};
constexpr Keywords<AI::StateTreeTrigger, 4> kTriggers{{{"condition", AI::StateTreeTrigger::Condition},
                                                       {"completion", AI::StateTreeTrigger::Completion},
                                                       {"timeout", AI::StateTreeTrigger::Timeout},
                                                       {"event", AI::StateTreeTrigger::Event}}};
constexpr Keywords<AI::StateTreeCompletion, 3> kResults{{{"any", AI::StateTreeCompletion::Any},
                                                         {"succeeded", AI::StateTreeCompletion::Succeeded},
                                                         {"failed", AI::StateTreeCompletion::Failed}}};
constexpr Keywords<AI::StateTreeModifierKind, 3> kModifiers{{{"abortIf", AI::StateTreeModifierKind::AbortIf},
                                                             {"cooldown", AI::StateTreeModifierKind::Cooldown},
                                                             {"repeat", AI::StateTreeModifierKind::Repeat}}};

constexpr uint32 kMaxTicks = std::numeric_limits<uint32>::max();

using NameIndex = FastHashMap<std::string, uint16, StringHash, std::equal_to<>>;

// The parse of one file: the source it builds, with an index of each name list.
struct ParseState
{
    AiStateTreeSource& Out;
    NameIndex ConditionIndex;
    NameIndex EventIndex;
    NameIndex TaskIndex;
};

// Writes the index of `name` in `names` to `out` and adds the name at the end when it is new. Refuses a
// name whose index does not fit below kStateTreeNone.
bool Intern(std::vector<std::string>& names, NameIndex& index, std::string name, const char* noun,
            const std::string& where, uint16& out, std::string& outError)
{
    if (const auto it = index.find(name); it != index.end())
    {
        out = it->second;
        return true;
    }
    if (names.size() >= AI::kStateTreeNone)
    {
        outError = where + ": the file has more than " + std::to_string(AI::kStateTreeNone) + " " + noun +
                   " names; the limit is " + std::to_string(AI::kStateTreeNone) + ". Reuse names or split the program.";
        return false;
    }
    out = static_cast<uint16>(names.size());
    index.emplace(name, out);
    names.push_back(std::move(name));
    return true;
}

// Reads the optional "name" of a state: a non-empty string when the field is present.
bool ReadStateName(const Json& object, const std::string& where, std::string& out, std::string& outError)
{
    const auto it = object.find("name");
    if (it == object.end())
        return true;
    if (!it->is_string() || it->get_ref<const std::string&>().empty())
    {
        outError = where + ": 'name' is not a name. Write a non-empty string, or delete the field.";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

// Reads the optional condition name `key` as an index in ConditionNames; kStateTreeNone when absent.
bool ReadCondition(const Json& object, const char* key, const std::string& where, ParseState& state, uint16& out,
                   std::string& outError)
{
    if (!object.contains(key))
        return true;
    std::string name;
    if (!ReadName(object, key, where, name, outError))
        return false;
    return Intern(state.Out.ConditionNames, state.ConditionIndex, std::move(name), "condition", where, out, outError);
}

// Reads the optional event name `key` as an index in EventNames; kStateTreeNone when absent.
bool ReadEvent(const Json& object, const char* key, const std::string& where, ParseState& state, uint16& out,
               std::string& outError)
{
    if (!object.contains(key))
        return true;
    std::string name;
    if (!ReadName(object, key, where, name, outError))
        return false;
    return Intern(state.Out.EventNames, state.EventIndex, std::move(name), "event", where, out, outError);
}

// Refuses a field of `object` that the kind written in `kindKey` does not use, and a missing `required` field.
bool CheckKindFields(const Json& object, const char* kindKey, const std::string& where,
                     std::initializer_list<std::string_view> fields, const char* required, std::string& outError)
{
    const std::string& kind = object.at(kindKey).get_ref<const std::string&>();
    for (const auto& [key, field] : object.items())
    {
        if (std::find(fields.begin(), fields.end(), key) == fields.end())
        {
            outError = where + ": a '" + kind + "' does not use the field '" + key + "'. Remove it.";
            return false;
        }
    }
    if (required && !object.contains(required))
    {
        outError = where + ": a '" + kind + "' needs the field '" + required + "'. Add it.";
        return false;
    }
    return true;
}

// Refuses a transition field that its trigger does not use, and a missing field that it needs.
bool CheckTransitionFields(const Json& value, AI::StateTreeTrigger trigger, const std::string& where,
                           std::string& outError)
{
    switch (trigger)
    {
    case AI::StateTreeTrigger::Condition:
        return CheckKindFields(value, "on", where, {"to", "on", "condition"}, "condition", outError);
    case AI::StateTreeTrigger::Completion:
        return CheckKindFields(value, "on", where, {"to", "on", "result"}, nullptr, outError);
    case AI::StateTreeTrigger::Timeout:
        return CheckKindFields(value, "on", where, {"to", "on", "ticks"}, "ticks", outError);
    case AI::StateTreeTrigger::Event:
        return CheckKindFields(value, "on", where, {"to", "on", "event"}, "event", outError);
    }
    return false;
}

// Refuses a modifier field that its kind does not use, and a missing field that it needs.
bool CheckModifierFields(const Json& value, AI::StateTreeModifierKind kind, const std::string& where,
                         std::string& outError)
{
    switch (kind)
    {
    case AI::StateTreeModifierKind::AbortIf:
        return CheckKindFields(value, "kind", where, {"kind", "condition"}, "condition", outError);
    case AI::StateTreeModifierKind::Cooldown:
        return CheckKindFields(value, "kind", where, {"kind", "ticks"}, "ticks", outError);
    case AI::StateTreeModifierKind::Repeat:
        return CheckKindFields(value, "kind", where, {"kind", "count"}, "count", outError);
    }
    return false;
}

bool ParseTransition(const Json& value, const std::string& where, ParseState& state,
                     AI::StateTreeDefinition::Transition& out, std::string& outError)
{
    if (!CheckObject(value, where, {"to", "on", "condition", "event", "result", "ticks"}, outError))
        return false;
    if (!value.contains("to"))
    {
        outError = where + ": the transition has no 'to'. Write the id of the state it goes to.";
        return false;
    }
    return ReadInteger(value, "to", where, 0, kMaxTicks - 1, out.TargetId, outError) &&
           ReadKeyword(value, "on", where, kTriggers, true, out.Trigger, outError) &&
           CheckTransitionFields(value, out.Trigger, where, outError) &&
           ReadCondition(value, "condition", where, state, out.Condition, outError) &&
           ReadEvent(value, "event", where, state, out.Event, outError) &&
           ReadKeyword(value, "result", where, kResults, false, out.On, outError) &&
           ReadInteger(value, "ticks", where, 0, kMaxTicks, out.TimeoutTicks, outError);
}

bool ParseModifier(const Json& value, const std::string& where, ParseState& state, AI::StateTreeModifier& out,
                   std::string& outError)
{
    if (!CheckObject(value, where, {"kind", "condition", "ticks", "count"}, outError))
        return false;
    return ReadKeyword(value, "kind", where, kModifiers, true, out.Kind, outError) &&
           CheckModifierFields(value, out.Kind, where, outError) &&
           ReadCondition(value, "condition", where, state, out.Condition, outError) &&
           ReadInteger(value, "ticks", where, 0, kMaxTicks, out.Value, outError) &&
           ReadInteger(value, "count", where, 0, std::numeric_limits<uint16>::max(), out.Value, outError);
}

// Reads the "utility" object of a Utility state into its option set.
bool ParseUtility(const Json& value, const std::string& where, AI::OptionSet& out, std::string& outError)
{
    if (!CheckObject(value, where, {"commitmentMargin", "minimumTicks", "noiseMargin"}, outError))
        return false;
    return ReadFloat(value, "commitmentMargin", where, out.CommitmentMargin, outError) &&
           ReadFloat(value, "noiseMargin", where, out.NoiseMargin, outError) &&
           ReadInteger(value, "minimumTicks", where, 0, kMaxTicks, out.MinimumTicks, outError);
}

// Reads the "option" object of a Utility state's child into the parent's options and curves; a child with no
// "option" scores its weight 1.
bool ParseOption(const Json* value, const std::string& where, ParseState& state, AI::OptionSet& options,
                 std::vector<AiStateTreeSource::Curve>& curves, std::string& outError)
{
    AI::OptionSet::Option option;
    option.FirstConsideration = static_cast<uint16>(options.Considerations.size());
    if (value)
    {
        if (!CheckObject(*value, where, {"weight", "rank", "considerations"}, outError))
            return false;
        uint32 rank = 0;
        if (!ReadFloat(*value, "weight", where, option.Weight, outError) ||
            !ReadInteger(*value, "rank", where, 0, std::numeric_limits<uint8>::max(), rank, outError))
            return false;
        option.Rank = static_cast<uint8>(rank);
        const Json considerations = value->value("considerations", Json::array());
        if (!considerations.is_array())
        {
            outError = where + ": 'considerations' is not a list. Write it as [ { \"input\": ... } ].";
            return false;
        }
        for (size_t index = 0; index < considerations.size(); ++index)
        {
            if (!ParseConsideration(considerations[index], where + ".considerations[" + std::to_string(index) + "]",
                                    options, state.Out.InputNames, &curves, outError))
                return false;
        }
    }
    option.ConsiderationCount = static_cast<uint16>(options.Considerations.size() - option.FirstConsideration);
    options.Options.push_back(option);
    return true;
}

// Reads the list `key` of `object` with `parse` into `out`.
template <typename T, typename Parse>
bool ParseList(const Json& object, const char* key, const std::string& where, ParseState& state, std::vector<T>& out,
               Parse parse, std::string& outError)
{
    const Json list = object.value(key, Json::array());
    if (!list.is_array())
    {
        outError = where + ": '" + key + "' is not a list. Write it as [ ... ].";
        return false;
    }
    for (size_t index = 0; index < list.size(); ++index)
    {
        if (!parse(list[index], where + "." + key + "[" + std::to_string(index) + "]", state, out.emplace_back(),
                   outError))
            return false;
    }
    return true;
}

bool ParseTaskName(const Json& value, const std::string& where, ParseState& state, uint16& out, std::string& outError)
{
    if (!value.is_string() || value.get_ref<const std::string&>().empty())
    {
        outError = where + ": the task is not a name. Write the task name as a string.";
        return false;
    }
    return Intern(state.Out.TaskNames, state.TaskIndex, value.get<std::string>(), "task", where, out, outError);
}

// Reads one state and, depth first, its children. `parent` is the definition index of its parent, or
// kStateTreeNone for the root.
bool ParseStateObject(const Json& value, const std::string& where, uint16 parent, uint8 depth, ParseState& state,
                      std::string& outError)
{
    if (depth >= AI::StateTreeProgram::kMaxDepth)
    {
        outError = where + ": the state is deeper than " + std::to_string(AI::StateTreeProgram::kMaxDepth) +
                   " levels. Move it up.";
        return false;
    }
    if (!CheckObject(value, where,
                     {"id", "name", "policy", "join", "enterIf", "weight", "tasks", "transitions", "modifiers",
                      "utility", "option", "children"},
                     outError))
        return false;
    if (!value.contains("id"))
    {
        outError = where + ": the state has no 'id'. Give it a number that no other state in the file uses.";
        return false;
    }
    std::vector<AI::StateTreeDefinition::State>& states = state.Out.Definition.States;
    if (states.size() >= AI::kStateTreeNone - 1)
    {
        outError = where + ": the program has more than " + std::to_string(AI::kStateTreeNone - 1) +
                   " states; the limit is " + std::to_string(AI::kStateTreeNone - 1) +
                   ". Split the program into smaller programs.";
        return false;
    }
    const uint16 index = static_cast<uint16>(states.size());
    AI::StateTreeDefinition::State entry;
    AiStateTreeSource::StateText text;
    if (parent != AI::kStateTreeNone)
        entry.ParentId = states[parent].Id;
    if (!ReadInteger(value, "id", where, 0, AI::StateTreeDefinition::kNoParent - 1, entry.Id, outError) ||
        !ReadStateName(value, where, text.Name, outError) ||
        !ReadKeyword(value, "policy", where, kPolicies, false, entry.Policy, outError) ||
        !ReadKeyword(value, "join", where, kJoins, false, entry.Join, outError) ||
        !ReadCondition(value, "enterIf", where, state, entry.EnterCondition, outError) ||
        !ReadFloat(value, "weight", where, entry.Weight, outError) ||
        !ParseList(value, "tasks", where, state, entry.Tasks, &ParseTaskName, outError) ||
        !ParseList(value, "transitions", where, state, entry.Transitions, &ParseTransition, outError) ||
        !ParseList(value, "modifiers", where, state, entry.Modifiers, &ParseModifier, outError))
        return false;
    if (const auto utility = value.find("utility"); utility != value.end() &&
                                                    !ParseUtility(*utility, where + ".utility", entry.Utility, outError))
        return false;
    const auto option = value.find("option");
    const bool parentIsUtility = parent != AI::kStateTreeNone && states[parent].Policy == AI::StateTreePolicy::Utility;
    if (option != value.end() && !parentIsUtility)
    {
        outError = where + ": 'option' is only for a child of a utility state. Remove it or set the parent's "
                           "policy to \"utility\".";
        return false;
    }
    if (parentIsUtility && !ParseOption(option != value.end() ? &*option : nullptr, where + ".option", state,
                                        states[parent].Utility, state.Out.States[parent].Curves, outError))
        return false;
    states.push_back(std::move(entry));
    state.Out.States.push_back(std::move(text));

    const Json children = value.value("children", Json::array());
    if (!children.is_array())
    {
        outError = where + ": 'children' is not a list. Write it as [ { \"id\": ... } ].";
        return false;
    }
    for (size_t child = 0; child < children.size(); ++child)
    {
        if (!ParseStateObject(children[child], where + ".children[" + std::to_string(child) + "]", index,
                              static_cast<uint8>(depth + 1), state, outError))
            return false;
    }
    return true;
}

// Writes the name at `index` of `names` as the field `key`; writes nothing for an index outside the list.
void WriteName(OrderedJson& object, const char* key, const std::vector<std::string>& names, uint16 index)
{
    if (index < names.size())
        object[key] = names[index];
}

OrderedJson WriteTransition(const AI::StateTreeDefinition::Transition& transition, const AiStateTreeSource& source)
{
    OrderedJson written = OrderedJson::object();
    written["to"] = transition.TargetId;
    written["on"] = KeywordOf(kTriggers, transition.Trigger);
    switch (transition.Trigger)
    {
    case AI::StateTreeTrigger::Condition:
        WriteName(written, "condition", source.ConditionNames, transition.Condition);
        break;
    case AI::StateTreeTrigger::Completion:
        if (transition.On != AI::StateTreeCompletion::Any)
            written["result"] = KeywordOf(kResults, transition.On);
        break;
    case AI::StateTreeTrigger::Timeout:
        written["ticks"] = transition.TimeoutTicks;
        break;
    case AI::StateTreeTrigger::Event:
        WriteName(written, "event", source.EventNames, transition.Event);
        break;
    }
    return written;
}

OrderedJson WriteModifier(const AI::StateTreeModifier& modifier, const AiStateTreeSource& source)
{
    OrderedJson written = OrderedJson::object();
    written["kind"] = KeywordOf(kModifiers, modifier.Kind);
    switch (modifier.Kind)
    {
    case AI::StateTreeModifierKind::AbortIf:
        WriteName(written, "condition", source.ConditionNames, modifier.Condition);
        break;
    case AI::StateTreeModifierKind::Cooldown:
        written["ticks"] = modifier.Value;
        break;
    case AI::StateTreeModifierKind::Repeat:
        written["count"] = modifier.Value;
        break;
    }
    return written;
}

// Writes option `optionIndex` of the Utility state `parent`; null when the option has the default fields.
OrderedJson WriteOption(const AiStateTreeSource& source, uint16 parent, size_t optionIndex)
{
    const AI::OptionSet& options = source.Definition.States[parent].Utility;
    if (optionIndex >= options.Options.size())
        return nullptr;
    const AI::OptionSet::Option& option = options.Options[optionIndex];
    const AI::OptionSet::Option defaults;
    if (option.Weight == defaults.Weight && option.Rank == defaults.Rank && option.ConsiderationCount == 0)
        return nullptr;
    OrderedJson written = OrderedJson::object();
    if (option.Weight != defaults.Weight)
        written["weight"] = WriteFloat(option.Weight);
    if (option.Rank != defaults.Rank)
        written["rank"] = static_cast<uint32>(option.Rank);
    const std::vector<AiStateTreeSource::Curve>& curves = source.States[parent].Curves;
    const uint32 end = option.FirstConsideration + option.ConsiderationCount;
    for (uint32 index = option.FirstConsideration; index < end; ++index)
        written["considerations"].push_back(
            WriteConsideration(options.Considerations[index], curves[index], source.InputNames));
    return written;
}

OrderedJson WriteUtility(const AI::OptionSet& options)
{
    const AI::OptionSet defaults;
    OrderedJson written = OrderedJson::object();
    if (options.CommitmentMargin != defaults.CommitmentMargin)
        written["commitmentMargin"] = WriteFloat(options.CommitmentMargin);
    if (options.MinimumTicks != defaults.MinimumTicks)
        written["minimumTicks"] = options.MinimumTicks;
    if (options.NoiseMargin != defaults.NoiseMargin)
        written["noiseMargin"] = WriteFloat(options.NoiseMargin);
    return written;
}

// Writes state `index` and, depth first, its children; `children[s]` lists the children of state s in order.
// `option` is the state's option in its Utility parent, or null.
OrderedJson WriteState(const AiStateTreeSource& source, const std::vector<std::vector<uint16>>& children, uint16 index,
                       OrderedJson option)
{
    const AI::StateTreeDefinition::State& state = source.Definition.States[index];
    const AI::StateTreeDefinition::State defaults;
    OrderedJson written = OrderedJson::object();
    written["id"] = state.Id;
    if (index < source.States.size() && !source.States[index].Name.empty())
        written["name"] = source.States[index].Name;
    if (state.Policy != defaults.Policy)
        written["policy"] = KeywordOf(kPolicies, state.Policy);
    if (state.Join != defaults.Join)
        written["join"] = KeywordOf(kJoins, state.Join);
    WriteName(written, "enterIf", source.ConditionNames, state.EnterCondition);
    for (uint16 task : state.Tasks)
    {
        if (task < source.TaskNames.size())
            written["tasks"].push_back(source.TaskNames[task]);
    }
    for (const AI::StateTreeDefinition::Transition& transition : state.Transitions)
        written["transitions"].push_back(WriteTransition(transition, source));
    for (const AI::StateTreeModifier& modifier : state.Modifiers)
        written["modifiers"].push_back(WriteModifier(modifier, source));
    if (state.Weight != defaults.Weight)
        written["weight"] = WriteFloat(state.Weight);
    if (OrderedJson utility = WriteUtility(state.Utility); !utility.empty())
        written["utility"] = std::move(utility);
    if (!option.is_null())
        written["option"] = std::move(option);
    const bool utilityParent = state.Policy == AI::StateTreePolicy::Utility;
    for (size_t child = 0; child < children[index].size(); ++child)
        written["children"].push_back(WriteState(source, children, children[index][child],
                                                 utilityParent ? WriteOption(source, index, child) : nullptr));
    return written;
}

// Refuses `value` when it is not finite: the file format holds only finite numbers.
bool CheckFinite(float32 value, const std::string& where, const char* key, std::string& outError)
{
    if (std::isfinite(value))
        return true;
    outError = where + ": '" + key + "' is not a finite number. Write a finite number such as 1.0.";
    return false;
}

// Refuses the first float of `source` that the file writes and that is not finite.
bool CheckFloats(const AiStateTreeSource& source, std::string& outError)
{
    for (size_t index = 0; index < source.Definition.States.size(); ++index)
    {
        const AI::StateTreeDefinition::State& state = source.Definition.States[index];
        const AI::OptionSet& options = state.Utility;
        const std::string where = "state " + std::to_string(state.Id);
        if (!CheckFinite(state.Weight, where, "weight", outError) ||
            !CheckFinite(options.CommitmentMargin, where, "commitmentMargin", outError) ||
            !CheckFinite(options.NoiseMargin, where, "noiseMargin", outError))
            return false;
        for (const AI::OptionSet::Option& option : options.Options)
        {
            if (!CheckFinite(option.Weight, where + ", option", "weight", outError))
                return false;
        }
        for (const AI::OptionSet::Consideration& consideration : options.Considerations)
        {
            if (!CheckFinite(consideration.Min, where + ", consideration", "min", outError) ||
                !CheckFinite(consideration.Max, where + ", consideration", "max", outError))
                return false;
        }
        if (index >= source.States.size())
            continue;
        for (const AiStateTreeSource::Curve& curve : source.States[index].Curves)
        {
            if (!CheckFinite(curve.Shape, where + ", curve", "shape", outError) ||
                !CheckFinite(curve.Midpoint, where + ", curve", "midpoint", outError))
                return false;
        }
    }
    return true;
}

} // namespace

bool ParseAiStateTreeSource(std::string_view text, AiStateTreeSource& out, std::string& outError)
{
    out = {};
    Json root;
    if (!Serialization::ParseJson(text, root, outError))
    {
        outError = "the file is not JSON: " + outError;
        return false;
    }
    if (!CheckObject(root, "the file", {"root"}, outError))
        return false;
    if (const std::string repeated = Serialization::FirstRepeatedKey(text); !repeated.empty())
    {
        outError = "the file: the field '" + repeated + "' is written twice in one object. Keep one.";
        return false;
    }
    if (!root.contains("root"))
    {
        outError = "the file has no 'root'. Write the root state as \"root\": { \"id\": 1, ... }.";
        return false;
    }
    ParseState state{out, {}, {}, {}};
    return ParseStateObject(root["root"], "root", AI::kStateTreeNone, 0, state, outError);
}

bool WriteAiStateTree(const AiStateTreeSource& source, std::string& outText, std::string& outError)
{
    if (!CheckFloats(source, outError))
        return false;
    const std::vector<AI::StateTreeDefinition::State>& states = source.Definition.States;
    OrderedJson file = OrderedJson::object();
    if (!states.empty())
    {
        FastHashMap<uint32, uint16> indexOfId;
        for (uint16 index = 0; index < states.size(); ++index)
            indexOfId.emplace(states[index].Id, index);
        std::vector<std::vector<uint16>> children(states.size());
        for (uint16 index = 1; index < states.size(); ++index)
        {
            if (const auto parent = indexOfId.find(states[index].ParentId); parent != indexOfId.end())
                children[parent->second].push_back(index);
        }
        file["root"] = WriteState(source, children, 0, nullptr);
    }
    outText = file.dump(2) + "\n";
    return true;
}

bool ParseAiStateTree(std::string_view text, AiStateTreeData& out, std::string& outError)
{
    out = {};
    if (!ParseAiStateTreeSource(text, out.Source, outError))
        return false;
    AI::StateTreeCompileError error;
    if (!CompileAiStateTree(out.Source, out.Program, error))
    {
        outError = "the program: " + error.Message;
        return false;
    }
    return true;
}

bool CompileAiStateTree(const AiStateTreeSource& source, AI::StateTreeProgram& out, AI::StateTreeCompileError& outError)
{
    if (AI::StateTreeProgram::Compile(source.Definition, out, outError))
        return true;
    // The compiler leads the refusal of a state with "state <id>:"; only the source knows the state's name.
    const std::string lead = "state " + std::to_string(outError.StateId);
    const std::vector<AI::StateTreeDefinition::State>& states = source.Definition.States;
    for (size_t index = 0; index < states.size(); ++index)
    {
        if (states[index].Id != outError.StateId)
            continue;
        if (outError.Message.starts_with(lead + ":"))
            outError.Message = StateRefusalName(source, index) + outError.Message.substr(lead.size());
        break;
    }
    return false;
}

std::string StateTitle(const AiStateTreeSource& source, size_t index)
{
    if (index < source.States.size() && !source.States[index].Name.empty())
        return source.States[index].Name;
    return "State " + std::to_string(source.Definition.States[index].Id);
}

std::string StateRefusalName(const AiStateTreeSource& source, size_t index)
{
    const std::string id = std::to_string(source.Definition.States[index].Id);
    if (index < source.States.size() && !source.States[index].Name.empty())
        return source.States[index].Name + " (state " + id + ")";
    return "State " + id;
}

} // namespace GameEngine::AIECS
