#include "Panels/Scribe/ScribeDocument.h"

#include "AI/StateTree/StateTreeDefinition.h"
#include "AI/StateTree/StateTreeProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeData.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "Panels/Scribe/ScribeEditCommand.h"
#include "Types/FlatMap.h"
#include "Types/FormatNumber.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
using Row = ScribeOutlineRow;

std::string NameAt(const std::vector<std::string>& names, uint16 index)
{
    return index < names.size() ? names[index] : std::string("(none)");
}

// How a state chooses its active child, as a verb phrase after the state's name.
const char* PolicyText(AI::StateTreePolicy policy)
{
    switch (policy)
    {
    case AI::StateTreePolicy::First: return "picks the first child";
    case AI::StateTreePolicy::Sequence: return "runs children in order";
    case AI::StateTreePolicy::Random: return "picks a random child";
    case AI::StateTreePolicy::Utility: return "picks the child by utility";
    }
    return "picks the first child";
}

bool HasChildren(const AI::StateTreeDefinition& definition, uint32 id)
{
    for (const AI::StateTreeDefinition::State& state : definition.States)
        if (state.ParentId == id)
            return true;
    return false;
}

const char* CompletionText(AI::StateTreeCompletion completion)
{
    switch (completion)
    {
    case AI::StateTreeCompletion::Any: return "on completion";
    case AI::StateTreeCompletion::Succeeded: return "on success";
    case AI::StateTreeCompletion::Failed: return "on failure";
    }
    return "on completion";
}

std::string TriggerText(const AIECS::AiStateTreeSource& source, const AI::StateTreeDefinition::Transition& transition)
{
    switch (transition.Trigger)
    {
    case AI::StateTreeTrigger::Condition: return "when " + NameAt(source.ConditionNames, transition.Condition);
    case AI::StateTreeTrigger::Completion: return CompletionText(transition.On);
    case AI::StateTreeTrigger::Timeout: return "after " + std::to_string(transition.TimeoutTicks) + " ticks";
    case AI::StateTreeTrigger::Event: return "on event " + NameAt(source.EventNames, transition.Event);
    }
    return {};
}

// The Utility parent of `state` and the index of the option that scores the state. False when the parent is not
// a Utility state or has no option for the state. A parent comes before its children in the definition.
bool FindOptionIndex(const AI::StateTreeDefinition& definition, uint16 state, uint16& outParent, uint16& outOption)
{
    if (state >= definition.States.size())
        return false;
    const uint32 parentId = definition.States[state].ParentId;
    outParent = Row::kNoState;
    uint16 option = 0;
    for (uint16 index = 0; index < state; ++index)
    {
        if (definition.States[index].Id == parentId)
            outParent = index;
        else if (definition.States[index].ParentId == parentId)
            ++option;
    }
    if (outParent == Row::kNoState)
        return false;
    const AI::StateTreeDefinition::State& parent = definition.States[outParent];
    if (parent.Policy != AI::StateTreePolicy::Utility || option >= parent.Utility.Options.size())
        return false;
    outOption = option;
    return true;
}

// The index of child `child` of state `parent`, in the children's order; kNoState when there is no such child.
uint16 ChildAt(const AI::StateTreeDefinition& definition, uint16 parent, uint16 child)
{
    uint16 seen = 0;
    for (uint16 index = static_cast<uint16>(parent + 1); index < definition.States.size(); ++index)
        if (definition.States[index].ParentId == definition.States[parent].Id && seen++ == child)
            return index;
    return Row::kNoState;
}

// The detail of an option's state row: its rank, which decides first, then its weight.
std::string OptionText(const AI::OptionSet::Option& option)
{
    return "rank " + std::to_string(option.Rank) + ", weight " + FormatFloat(option.Weight);
}

void AddConsiderationRows(const AIECS::AiStateTreeSource& source, uint16 state, uint8 depth, std::vector<Row>& rows)
{
    uint16 parent = Row::kNoState;
    uint16 option = 0;
    if (!FindOptionIndex(source.Definition, state, parent, option))
        return;
    const AI::OptionSet& options = source.Definition.States[parent].Utility;
    const AI::OptionSet::Option& scored = options.Options[option];
    for (uint16 index = 0; index < scored.ConsiderationCount; ++index)
    {
        const uint32 at = scored.FirstConsideration + index;
        const AI::OptionSet::Consideration& consideration = options.Considerations[at];
        const AIECS::AiStateTreeSource::Curve& curve = source.States[parent].Curves[at];
        // A Minimum above the Maximum is a valid range: the curve starts at the higher input value.
        const char* to = consideration.Min > consideration.Max ? " down to " : " to ";
        rows.push_back({Row::Kind::Consideration, state, index, depth, NameAt(source.InputNames, consideration.Column),
                        FormatFloat(consideration.Min) + to + FormatFloat(consideration.Max) + ", " +
                            ScribeDocument::CurveWord(curve.Preset),
                        {}});
    }
}

void AddStateRows(const AIECS::AiStateTreeSource& source, const FlatMap<uint32, uint16>& indexOfId,
                  std::vector<uint8>& depths, uint16 state, std::vector<Row>& rows)
{
    const AI::StateTreeDefinition::State& definition = source.Definition.States[state];
    const uint16* parent = indexOfId.Find(definition.ParentId);
    depths[state] = parent ? static_cast<uint8>(depths[*parent] + 1) : 0;
    const uint8 depth = depths[state];

    std::string detail = HasChildren(source.Definition, definition.Id) ? PolicyText(definition.Policy) : "";
    uint16 utility = Row::kNoState;
    uint16 option = 0;
    const bool isOption = FindOptionIndex(source.Definition, state, utility, option);
    if (isOption)
        detail += (detail.empty() ? "" : "; ") + OptionText(source.Definition.States[utility].Utility.Options[option]);
    rows.push_back({Row::Kind::State, state, 0, depth, AIECS::StateTitle(source, state), std::move(detail), {}});
    rows.back().IsOption = isOption;
    if (definition.EnterCondition != AI::kStateTreeNone)
        rows.push_back({Row::Kind::EnterCondition, state, 0, static_cast<uint8>(depth + 1),
                        NameAt(source.ConditionNames, definition.EnterCondition), {}, {}});
    AddConsiderationRows(source, state, static_cast<uint8>(depth + 1), rows);
    for (uint16 task = 0; task < definition.Tasks.size(); ++task)
        rows.push_back({Row::Kind::Task, state, task, static_cast<uint8>(depth + 1),
                        NameAt(source.TaskNames, definition.Tasks[task]), {}, {}});
    for (uint16 index = 0; index < definition.Transitions.size(); ++index)
    {
        const AI::StateTreeDefinition::Transition& transition = definition.Transitions[index];
        const uint16* target = indexOfId.Find(transition.TargetId);
        rows.push_back({Row::Kind::Transition, state, index, static_cast<uint8>(depth + 1),
                        target ? AIECS::StateTitle(source, *target) : "state " + std::to_string(transition.TargetId),
                        TriggerText(source, transition), {}});
    }
}

void AddNameGroup(const char* heading, const std::vector<std::string>& names, std::vector<Row>& rows)
{
    rows.push_back({Row::Kind::Group, Row::kNoState, 0, 0, heading, std::to_string(names.size()), {}});
    for (uint16 index = 0; index < names.size(); ++index)
        rows.push_back({Row::Kind::Name, Row::kNoState, index, 1, names[index], {}, {}});
}

// True when `row`, a row of the refusal's state, is the row that the refusal names.
bool HoldsRefusal(const Row& row, const AI::StateTreeCompileError& refusal)
{
    switch (refusal.Item)
    {
    case AI::StateTreeCompileItem::EnterCondition: return row.RowKind == Row::Kind::EnterCondition;
    case AI::StateTreeCompileItem::Task: return row.RowKind == Row::Kind::Task && row.Item == refusal.ItemIndex;
    case AI::StateTreeCompileItem::Transition:
        return row.RowKind == Row::Kind::Transition && row.Item == refusal.ItemIndex;
    case AI::StateTreeCompileItem::Program: return false;
    case AI::StateTreeCompileItem::State:
    case AI::StateTreeCompileItem::Modifier:
    case AI::StateTreeCompileItem::Option: return row.RowKind == Row::Kind::State;
    }
    return false;
}

// The names that the registry gives, for the fix of a name refusal.
struct RegisteredNames
{
    std::vector<std::string> Inputs;
    std::vector<std::string> Conditions;
    std::vector<std::string> Events;
};

// A name that no module registers. Field names the panel's field that chooses the name; null when no field does.
// Kind is null when the refusal names no single name of its state.
struct UnregisteredName
{
    const char* Kind = nullptr;
    std::string Name;
    const char* Field = nullptr;
    // True when the registry gives at least one name of this kind, so that the field has a name to choose.
    bool KindRegistered = false;
};

// The name that a name refusal of `state` names.
UnregisteredName NamedName(const AIECS::AiStateTreeSource& source, uint16 state,
                           const AI::StateTreeCompileError& refusal, const RegisteredNames& registered)
{
    const AI::StateTreeDefinition::State& definition = source.Definition.States[state];
    const uint16 item = refusal.ItemIndex;
    switch (refusal.Item)
    {
    case AI::StateTreeCompileItem::EnterCondition:
        return {"condition", NameAt(source.ConditionNames, definition.EnterCondition), "Enter if",
                !registered.Conditions.empty()};
    case AI::StateTreeCompileItem::Task:
        if (item < definition.Tasks.size())
            return {"task", NameAt(source.TaskNames, definition.Tasks[item]), nullptr, false};
        break;
    case AI::StateTreeCompileItem::Transition:
        if (item < definition.Transitions.size() &&
            definition.Transitions[item].Trigger == AI::StateTreeTrigger::Event)
            return {"event", NameAt(source.EventNames, definition.Transitions[item].Event), "Event",
                    !registered.Events.empty()};
        if (item < definition.Transitions.size())
            return {"condition", NameAt(source.ConditionNames, definition.Transitions[item].Condition), nullptr, false};
        break;
    case AI::StateTreeCompileItem::Modifier:
        if (item < definition.Modifiers.size())
            return {"condition", NameAt(source.ConditionNames, definition.Modifiers[item].Condition), nullptr, false};
        break;
    case AI::StateTreeCompileItem::Program:
    case AI::StateTreeCompileItem::State:
    case AI::StateTreeCompileItem::Option: break;
    }
    return {};
}

// The row as a path: the title of the state that holds it, then the row's kind word and its name
// ("Attack > Consider Morale"). The state that holds an option's row is its Utility parent.
std::string RowPath(const AIECS::AiStateTreeSource& source, const Row& row)
{
    const std::string title = std::string(ScribeDocument::KindWord(row)) + " " + row.Text;
    uint16 holder = row.State;
    uint16 option = 0;
    if (row.RowKind == Row::Kind::State && !FindOptionIndex(source.Definition, row.State, holder, option))
        return title;
    return AIECS::StateTitle(source, holder) + " > " + title;
}

// The end of the programmer's fix for a name of `kind` ("input", "condition", "event" or "task"), after "register
// it": the GameSDK macro and when the row clears. It is true when no code ever registered the name and when a
// reload removed it.
std::string RegistrationFix(std::string_view kind)
{
    const char* macro = kind == "input"       ? "GE_REGISTER_AI_INPUT"
                        : kind == "condition" ? "GE_REGISTER_AI_CONDITION"
                        : kind == "event"     ? "GE_REGISTER_AI_EVENT"
                                              : "GE_REGISTER_AI_TASK";
    return std::string(" in the game's code with ") + macro + ". A registration clears this row when the code reloads.";
}

// Puts the refusal of `name` on `row`: the row, the cause, the designer's fix in the field that chooses the name,
// then the programmer's fix.
void HoldUnregisteredName(const AIECS::AiStateTreeSource& source, const UnregisteredName& name, Row& row)
{
    const std::string kind = name.Kind;
    row.Refusal = RowPath(source, row) + ": no loaded code registers the " + kind + " '" + name.Name + "'. ";
    if (!name.Field)
        row.Refusal += "Remove the " + kind + " from the program file, or register it" + RegistrationFix(kind);
    else if (!name.KindRegistered)
        row.Refusal += "Register it" + RegistrationFix(kind);
    else
        row.Refusal +=
            "Choose a registered " + kind + " in " + name.Field + ", or register it" + RegistrationFix(kind);
    row.UnregisteredKind = name.Kind;
    row.UnregisteredName = name.Name;
}

// The rows of the outline and where the rows of each state are: the rows of state s are
// [StateRows[s], StateRows[s + 1]), and the first of them is the state's row.
struct RefusalPlacement
{
    const AIECS::AiStateTreeSource& Source;
    const FlatMap<uint32, uint16>& IndexOfId;
    const std::vector<size_t>& StateRows;
    std::vector<Row>& Rows;
    // The rows of the refusals that name no state; they go at the top of the outline.
    std::vector<Row> ProgramRows;

    std::span<Row> RowsOf(uint16 state) const
    {
        return std::span<Row>(Rows).subspan(StateRows[state], StateRows[state + 1] - StateRows[state]);
    }
};

// Puts a refusal of option `refusal.ItemIndex` of the Utility state `utility` on the row of the option's state.
// A name refusal goes on the first consideration of the option whose input is not registered and that holds no
// refusal. False when the Utility state has no child for the option.
bool LocateOptionRefusal(RefusalPlacement& placement, uint16 utility, const AI::StateTreeCompileError& refusal,
                         const RegisteredNames* registered)
{
    const uint16 child = ChildAt(placement.Source.Definition, utility, refusal.ItemIndex);
    if (child == Row::kNoState)
        return false;
    const std::span<Row> rows = placement.RowsOf(child);
    if (registered)
        for (Row& row : rows)
            if (row.RowKind == Row::Kind::Consideration && row.Refusal.empty() &&
                std::find(registered->Inputs.begin(), registered->Inputs.end(), row.Text) == registered->Inputs.end())
            {
                HoldUnregisteredName(placement.Source, {"input", row.Text, "Input", !registered->Inputs.empty()}, row);
                return true;
            }
    if (rows.front().Refusal.empty())
        rows.front().Refusal = refusal.Message;
    return true;
}

// Puts the refusal on the row it names: the state row when the state has no row for the item, and a program row
// when it names no state. A row that holds a refusal keeps it. `registered` is null for the compile refusal, and
// gives the registered names for a name refusal.
void LocateRefusal(RefusalPlacement& placement, const AI::StateTreeCompileError& refusal,
                   const RegisteredNames* registered)
{
    const uint16* found = placement.IndexOfId.Find(refusal.StateId);
    if (!found)
    {
        placement.ProgramRows.push_back(Row{Row::Kind::Program, Row::kNoState, 0, 0, "Program", {}, refusal.Message});
        return;
    }
    const uint16 state = *found;
    if (refusal.Item == AI::StateTreeCompileItem::Option && LocateOptionRefusal(placement, state, refusal, registered))
        return;
    const std::span<Row> rows = placement.RowsOf(state);
    const auto named = std::find_if(rows.begin(), rows.end(), [&refusal](const Row& row)
                                    { return row.Refusal.empty() && HoldsRefusal(row, refusal); });
    Row& holder = named != rows.end() ? *named : rows.front();
    if (!holder.Refusal.empty())
        return;
    const UnregisteredName name =
        registered ? NamedName(placement.Source, state, refusal, *registered) : UnregisteredName{};
    if (name.Kind)
        HoldUnregisteredName(placement.Source, name, holder);
    else
        holder.Refusal = refusal.Message;
}

// Puts a refusal of a repeated state name on the row of each state with that name. The registry gives the
// refusal once, on the second state. A row that holds a refusal keeps it.
void LocateNameRefusal(RefusalPlacement& placement, const AI::StateTreeCompileError& refusal)
{
    const uint16* named = placement.IndexOfId.Find(refusal.StateId);
    if (!named || *named >= placement.Source.States.size())
    {
        LocateRefusal(placement, refusal, nullptr);
        return;
    }
    const std::string& name = placement.Source.States[*named].Name;
    for (uint16 state = 0; state < placement.Source.States.size(); ++state)
    {
        Row& row = placement.Rows[placement.StateRows[state]];
        if (row.Refusal.empty() && placement.Source.States[state].Name == name)
            row.Refusal = refusal.Message;
    }
}

// A group of the program's names at the end of the outline: the rows [First, End) and the rows that use a name.
struct NameGroup
{
    const char* Kind;
    // The rows that use a name of the group, after "on each" ("consideration that reads it").
    const char* Users;
    size_t First;
    size_t End;
};

// Marks each row of `group` whose name no loaded code registers.
void MarkUnregisteredNames(const NameGroup& group, const std::vector<std::string>& registered, std::vector<Row>& rows)
{
    const std::string kind = group.Kind;
    for (size_t index = group.First; index < group.End; ++index)
    {
        Row& row = rows[index];
        if (std::find(registered.begin(), registered.end(), row.Text) != registered.end())
            continue;
        row.Refusal = "No loaded code registers the " + kind + " '" + row.Text + "'. " +
                      (registered.empty() ? "Register it" : "Choose a registered " + kind + " on each " +
                                                                group.Users + ", or register it") +
                      RegistrationFix(kind);
        row.UnregisteredKind = group.Kind;
        row.UnregisteredName = row.Text;
    }
}

// True when the two sources write the same file text.
bool WriteSameText(const AIECS::AiStateTreeSource& first, const AIECS::AiStateTreeSource& second)
{
    std::string firstText;
    std::string secondText;
    std::string error;
    return AIECS::WriteAiStateTree(first, firstText, error) && AIECS::WriteAiStateTree(second, secondText, error) &&
           firstText == secondText;
}

// The source as the file reads it back: each name list holds the names in use, in first-use order.
bool Normalize(const AIECS::AiStateTreeSource& source, AIECS::AiStateTreeSource& out, std::string& outError)
{
    std::string text;
    return AIECS::WriteAiStateTree(source, text, outError) && AIECS::ParseAiStateTreeSource(text, out, outError);
}

uint16 InternName(std::vector<std::string>& names, std::string_view name)
{
    for (uint16 index = 0; index < names.size(); ++index)
        if (names[index] == name)
            return index;
    names.emplace_back(name);
    return static_cast<uint16>(names.size() - 1);
}

bool WriteFile(const std::filesystem::path& path, const std::string& text, std::string& outError)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (out.good())
        return true;
    outError = "Could not write " + path.string() + "; check that the file is not read-only or open elsewhere.";
    return false;
}
} // namespace

ScribeDocument::ScribeDocument(ConstructionKey, std::filesystem::path path, UndoRedoService* undo,
                               const AIECS::AiRegistry* names)
    : m_Path(std::move(path)), m_Undo(undo), m_Names(names)
{
}

std::shared_ptr<ScribeDocument> ScribeDocument::Open(const std::filesystem::path& path, UndoRedoService* undo,
                                                     const AIECS::AiRegistry* names, std::string& outError)
{
    String text;
    if (!ReadFileTextShared(path, text))
    {
        outError = "Could not read " + path.string() + "; check that the file exists and is not open elsewhere.";
        return nullptr;
    }
    AIECS::AiStateTreeSource source;
    if (!AIECS::ParseAiStateTreeSource(text, source, outError))
        return nullptr;
    const std::shared_ptr<ScribeDocument> document =
        std::make_shared<ScribeDocument>(ConstructionKey{}, path, undo, names);
    if (!AIECS::WriteAiStateTree(source, document->m_SavedText, outError))
        return nullptr;
    document->ReplaceSource(std::move(source));
    return document;
}

const AI::OptionSet::Option* ScribeDocument::FindOption(uint16 state, uint16& outUtility) const
{
    uint16 option = 0;
    if (!FindOptionIndex(m_Source.Definition, state, outUtility, option))
        return nullptr;
    return &m_Source.Definition.States[outUtility].Utility.Options[option];
}

const char* ScribeDocument::CurveWord(AI::CurvePreset preset)
{
    switch (preset)
    {
    case AI::CurvePreset::Linear: return "linear";
    case AI::CurvePreset::Polynomial: return "polynomial";
    case AI::CurvePreset::Logistic: return "logistic";
    case AI::CurvePreset::Logit: return "logit";
    case AI::CurvePreset::Step: return "step";
    }
    return "linear";
}

const char* ScribeDocument::KindWord(const ScribeOutlineRow& row)
{
    switch (row.RowKind)
    {
    case Row::Kind::State: return row.IsOption ? "Option" : "State";
    case Row::Kind::EnterCondition: return "Enter if";
    case Row::Kind::Consideration: return "Consider";
    case Row::Kind::Task: return "Task";
    case Row::Kind::Transition: return "To";
    case Row::Kind::Program:
    case Row::Kind::Group:
    case Row::Kind::Name: return "";
    }
    return "";
}

std::string ScribeDocument::GetRowPath(const ScribeOutlineRow& row) const
{
    return RowPath(m_Source, row);
}

std::vector<ScribeOutlineRow> ScribeDocument::BuildOutline() const
{
    const std::vector<AI::StateTreeDefinition::State>& states = m_Source.Definition.States;
    FlatMap<uint32, uint16> indexOfId;
    indexOfId.Reserve(states.size());
    for (uint16 state = 0; state < states.size(); ++state)
        indexOfId.InsertOrAssign(states[state].Id, state);

    std::vector<Row> rows;
    std::vector<size_t> stateRows;
    stateRows.reserve(states.size() + 1);
    std::vector<uint8> depths(states.size(), 0);
    for (uint16 state = 0; state < states.size(); ++state)
    {
        stateRows.push_back(rows.size());
        AddStateRows(m_Source, indexOfId, depths, state, rows);
    }
    stateRows.push_back(rows.size());
    // The first row of each group is after its heading.
    const NameGroup events{"event", "transition that fires on it", rows.size() + 1,
                           rows.size() + 1 + m_Source.EventNames.size()};
    AddNameGroup("Events", m_Source.EventNames, rows);
    const NameGroup inputs{"input", "consideration that reads it", rows.size() + 1,
                           rows.size() + 1 + m_Source.InputNames.size()};
    AddNameGroup("Inputs", m_Source.InputNames, rows);

    RegisteredNames registered;
    if (m_Names)
    {
        registered.Inputs = m_Names->GetInputNames();
        registered.Conditions = m_Names->GetConditionNames();
        registered.Events = m_Names->GetEventTypeNames();
    }
    RefusalPlacement placement{m_Source, indexOfId, stateRows, rows, {}};
    // A name refusal of the registry with the item State is a repeated state name.
    for (size_t index = 0; index < m_Refusals.size(); ++index)
    {
        const AI::StateTreeCompileError& refusal = m_Refusals[index];
        const bool nameRefusal = !(m_CompileRefused && index == 0);
        if (nameRefusal && refusal.Item == AI::StateTreeCompileItem::State)
            LocateNameRefusal(placement, refusal);
        else
            LocateRefusal(placement, refusal, nameRefusal ? &registered : nullptr);
    }
    if (m_Names)
    {
        MarkUnregisteredNames(events, registered.Events, rows);
        MarkUnregisteredNames(inputs, registered.Inputs, rows);
    }
    rows.insert(rows.begin(), std::make_move_iterator(placement.ProgramRows.begin()),
                std::make_move_iterator(placement.ProgramRows.end()));
    return rows;
}

void ScribeDocument::RenameState(uint16 state, std::string name)
{
    if (state >= m_Source.States.size() || m_Source.States[state].Name == name)
        return;
    AIECS::AiStateTreeSource edited = m_Source;
    edited.States[state].Name = std::move(name);
    Commit("Rename State", std::move(edited));
}

void ScribeDocument::SetEnterCondition(uint16 state, std::string_view conditionName)
{
    if (state >= m_Source.Definition.States.size())
        return;
    AIECS::AiStateTreeSource edited = m_Source;
    edited.Definition.States[state].EnterCondition =
        conditionName.empty() ? AI::kStateTreeNone : InternName(edited.ConditionNames, conditionName);
    Commit("Set Enter Condition", std::move(edited));
}

void ScribeDocument::SetTransitionEvent(uint16 state, uint16 transition, std::string_view eventName)
{
    if (state >= m_Source.Definition.States.size() || eventName.empty())
        return;
    AIECS::AiStateTreeSource edited = m_Source;
    std::vector<AI::StateTreeDefinition::Transition>& transitions = edited.Definition.States[state].Transitions;
    if (transition >= transitions.size() || transitions[transition].Trigger != AI::StateTreeTrigger::Event)
        return;
    transitions[transition].Event = InternName(edited.EventNames, eventName);
    Commit("Set Transition Event", std::move(edited));
}

void ScribeDocument::SetOptionWeight(uint16 state, float32 weight, Phase phase)
{
    uint16 utility = Row::kNoState;
    uint16 option = 0;
    if (!FindOptionIndex(EditBase().Definition, state, utility, option))
        return;
    AIECS::AiStateTreeSource edited = EditBase();
    edited.Definition.States[utility].Utility.Options[option].Weight = weight;
    Apply("Set Option Weight", std::move(edited), phase);
}

void ScribeDocument::SetOptionRank(uint16 state, uint8 rank)
{
    uint16 utility = Row::kNoState;
    uint16 option = 0;
    if (!FindOptionIndex(EditBase().Definition, state, utility, option))
        return;
    AIECS::AiStateTreeSource edited = EditBase();
    edited.Definition.States[utility].Utility.Options[option].Rank = rank;
    Apply("Set Option Rank", std::move(edited), Phase::Commit);
}

void ScribeDocument::AddConsideration(uint16 state, std::string_view input)
{
    uint16 utility = Row::kNoState;
    uint16 option = 0;
    if (input.empty() || !FindOptionIndex(EditBase().Definition, state, utility, option))
        return;
    AIECS::AiStateTreeSource edited = EditBase();
    AI::OptionSet& options = edited.Definition.States[utility].Utility;
    AI::OptionSet::Option& scored = options.Options[option];
    const uint32 at = scored.FirstConsideration + scored.ConsiderationCount;
    AI::OptionSet::Consideration consideration;
    consideration.Column = InternName(edited.InputNames, input);
    options.Considerations.insert(options.Considerations.begin() + at, consideration);
    std::vector<AIECS::AiStateTreeSource::Curve>& curves = edited.States[utility].Curves;
    curves.insert(curves.begin() + at, AIECS::AiStateTreeSource::Curve{});
    ++scored.ConsiderationCount;
    for (size_t later = option + 1; later < options.Options.size(); ++later)
        ++options.Options[later].FirstConsideration;
    Apply("Add Consideration", std::move(edited), Phase::Commit);
}

void ScribeDocument::RemoveConsideration(uint16 state, uint16 consideration)
{
    uint16 utility = Row::kNoState;
    uint16 option = 0;
    if (!FindOptionIndex(EditBase().Definition, state, utility, option))
        return;
    AIECS::AiStateTreeSource edited = EditBase();
    AI::OptionSet& options = edited.Definition.States[utility].Utility;
    AI::OptionSet::Option& scored = options.Options[option];
    if (consideration >= scored.ConsiderationCount)
        return;
    const uint32 at = scored.FirstConsideration + consideration;
    options.Considerations.erase(options.Considerations.begin() + at);
    std::vector<AIECS::AiStateTreeSource::Curve>& curves = edited.States[utility].Curves;
    curves.erase(curves.begin() + at);
    --scored.ConsiderationCount;
    for (size_t later = option + 1; later < options.Options.size(); ++later)
        --options.Options[later].FirstConsideration;
    Apply("Remove Consideration", std::move(edited), Phase::Commit);
}

void ScribeDocument::SetConsiderationInput(uint16 state, uint16 consideration, std::string_view input)
{
    uint16 utility = Row::kNoState;
    uint16 option = 0;
    if (input.empty() || !FindOptionIndex(EditBase().Definition, state, utility, option))
        return;
    AIECS::AiStateTreeSource edited = EditBase();
    AI::OptionSet& options = edited.Definition.States[utility].Utility;
    const AI::OptionSet::Option& scored = options.Options[option];
    if (consideration >= scored.ConsiderationCount)
        return;
    options.Considerations[scored.FirstConsideration + consideration].Column = InternName(edited.InputNames, input);
    Apply("Set Consideration Input", std::move(edited), Phase::Commit);
}

void ScribeDocument::SetConsiderationRange(uint16 state, uint16 consideration, float32 min, float32 max, Phase phase)
{
    uint16 utility = Row::kNoState;
    uint16 option = 0;
    if (!FindOptionIndex(EditBase().Definition, state, utility, option))
        return;
    AIECS::AiStateTreeSource edited = EditBase();
    AI::OptionSet& options = edited.Definition.States[utility].Utility;
    const AI::OptionSet::Option& scored = options.Options[option];
    if (consideration >= scored.ConsiderationCount)
        return;
    AI::OptionSet::Consideration& edit = options.Considerations[scored.FirstConsideration + consideration];
    edit.Min = min;
    edit.Max = max;
    Apply("Set Consideration Range", std::move(edited), phase);
}

void ScribeDocument::SetCommitmentMargin(uint16 state, float32 margin, Phase phase)
{
    const std::vector<AI::StateTreeDefinition::State>& states = EditBase().Definition.States;
    if (state >= states.size() || states[state].Policy != AI::StateTreePolicy::Utility)
        return;
    AIECS::AiStateTreeSource edited = EditBase();
    edited.Definition.States[state].Utility.CommitmentMargin = margin;
    Apply("Set Commitment Margin", std::move(edited), phase);
}

void ScribeDocument::SetMinimumTicks(uint16 state, uint32 ticks)
{
    const std::vector<AI::StateTreeDefinition::State>& states = EditBase().Definition.States;
    if (state >= states.size() || states[state].Policy != AI::StateTreePolicy::Utility)
        return;
    AIECS::AiStateTreeSource edited = EditBase();
    edited.Definition.States[state].Utility.MinimumTicks = ticks;
    Apply("Set Minimum Ticks", std::move(edited), Phase::Commit);
}

void ScribeDocument::SetNoiseMargin(uint16 state, float32 margin, Phase phase)
{
    const std::vector<AI::StateTreeDefinition::State>& states = EditBase().Definition.States;
    if (state >= states.size() || states[state].Policy != AI::StateTreePolicy::Utility)
        return;
    AIECS::AiStateTreeSource edited = EditBase();
    edited.Definition.States[state].Utility.NoiseMargin = margin;
    Apply("Set Noise Margin", std::move(edited), phase);
}

bool ScribeDocument::Save(std::string& outError)
{
    std::string text;
    if (!AIECS::WriteAiStateTree(m_Source, text, outError))
        return false;
    AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!assets)
    {
        if (!WriteFile(m_Path, text, outError))
            return false;
    }
    else
    {
        // The report reloads the asset; the reload moves the running agents to the new program.
        ExpectedAssetWrite write = assets->ExpectWrite(m_Path);
        if (!WriteFile(m_Path, text, outError))
            return false;
        write.Report(m_Path);
    }
    m_SavedText = std::move(text);
    m_Dirty = false;
    ++m_Version;
    return true;
}

void ScribeDocument::ReplaceSource(AIECS::AiStateTreeSource source)
{
    m_PreviewStart.reset();
    Show(std::move(source));
}

const AIECS::AiStateTreeSource& ScribeDocument::EditBase() const
{
    return m_PreviewStart ? *m_PreviewStart : m_Source;
}

void ScribeDocument::Show(AIECS::AiStateTreeSource source)
{
    m_Source = std::move(source);
    std::string text;
    std::string error;
    m_Dirty = !AIECS::WriteAiStateTree(m_Source, text, error) || text != m_SavedText;
    CheckNames();
}

void ScribeDocument::Commit(const char* name, AIECS::AiStateTreeSource edited)
{
    AIECS::AiStateTreeSource normalized;
    std::string error;
    if (!Normalize(edited, normalized, error))
    {
        Logger::Log::Error("Scribe: '{}' was not applied to {}: {}", name, m_Path.string(), error);
        return;
    }
    std::optional<AIECS::AiStateTreeSource> start = std::exchange(m_PreviewStart, std::nullopt);
    if (!m_Undo)
    {
        ReplaceSource(std::move(normalized));
        return;
    }
    AIECS::AiStateTreeSource before = start ? std::move(*start) : m_Source;
    // An edit that ends at the value it started from takes no undo step; a drag shows its start again.
    if (WriteSameText(before, normalized))
    {
        if (start)
            ReplaceSource(std::move(before));
        return;
    }
    m_Undo->Execute(std::make_unique<ScribeEditCommand>(weak_from_this(), name, std::move(before), std::move(normalized)));
}

void ScribeDocument::Apply(const char* name, AIECS::AiStateTreeSource edited, Phase phase)
{
    if (phase == Phase::Commit)
    {
        Commit(name, std::move(edited));
        return;
    }
    AIECS::AiStateTreeSource normalized;
    std::string error;
    if (!Normalize(edited, normalized, error))
    {
        Logger::Log::Error("Scribe: the preview of '{}' was not shown for {}: {}", name, m_Path.string(), error);
        return;
    }
    if (!m_PreviewStart)
        m_PreviewStart = m_Source;
    Show(std::move(normalized));
}

void ScribeDocument::CheckNames()
{
    m_Refusals.clear();
    AI::StateTreeProgram program;
    AI::StateTreeCompileError refusal;
    m_CompileRefused = !AIECS::CompileAiStateTree(m_Source, program, refusal);
    if (m_CompileRefused)
    {
        m_Refusals.push_back(std::move(refusal));
    }
    // The name check runs also when the compile refuses, so that each row that needs a fix shows at once.
    if (m_Names)
    {
        std::vector<AI::StateTreeCompileError> names = m_Names->CheckStateTreeNames(m_Source);
        m_Refusals.insert(m_Refusals.end(), std::make_move_iterator(names.begin()),
                          std::make_move_iterator(names.end()));
    }
    ++m_Version;
}

} // namespace GameEngine::Editor
