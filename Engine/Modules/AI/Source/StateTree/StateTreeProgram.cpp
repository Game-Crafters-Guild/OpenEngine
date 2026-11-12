#include "AI/StateTree/StateTreeProgram.h"

#include "AI/StateTree/StateTreeCompileError.h"
#include "AI/StateTree/StateTreeDefinition.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace GameEngine::AI
{

namespace
{

// (stable id, definition index) or (parent id, definition index).
using IdIndex = std::pair<uint32, uint32>;

constexpr uint32 kNone = std::numeric_limits<uint32>::max();

// Writes the refusal to `out` and returns false.
bool Refuse(StateTreeCompileError& out, std::string message, uint32 stateId, StateTreeCompileItem item,
            uint16 itemIndex = 0)
{
    out = {std::move(message), stateId, item, itemIndex};
    return false;
}

bool LessId(const IdIndex& a, const IdIndex& b)
{
    return a.first < b.first;
}

// The definition index of the state with `id` in `byId` (sorted by id), or kNone.
uint32 FindDefinitionIndex(const std::vector<IdIndex>& byId, uint32 id)
{
    const auto it = std::lower_bound(byId.begin(), byId.end(), IdIndex{id, 0}, &LessId);
    return it != byId.end() && it->first == id ? it->second : kNone;
}

std::string StateName(uint32 id)
{
    return "state " + std::to_string(id);
}

uint16 ColumnCount(const OptionSet& options)
{
    uint16 count = 0;
    for (const OptionSet::Consideration& consideration : options.Considerations)
        count = std::max<uint16>(count, static_cast<uint16>(consideration.Column + 1));
    return count;
}

// Sorts the states by id and refuses a repeated id.
bool IndexById(const std::vector<StateTreeDefinition::State>& states, std::vector<IdIndex>& byId,
               StateTreeCompileError& outError)
{
    byId.reserve(states.size());
    for (uint32 index = 0; index < states.size(); ++index)
        byId.emplace_back(states[index].Id, index);
    std::sort(byId.begin(), byId.end());
    for (size_t index = 1; index < byId.size(); ++index)
    {
        if (byId[index].first == byId[index - 1].first)
        {
            const uint32 id = byId[index].first;
            return Refuse(outError, StateName(id) + ": the id is used twice. Give each state its own id.", id,
                          StateTreeCompileItem::State);
        }
    }
    return true;
}

// Finds the one root and sorts the other states by parent id, in list order within one parent.
bool IndexByParent(const std::vector<StateTreeDefinition::State>& states, const std::vector<IdIndex>& byId,
                   std::vector<IdIndex>& byParent, uint32& root, StateTreeCompileError& outError)
{
    root = kNone;
    for (uint32 index = 0; index < states.size(); ++index)
    {
        const StateTreeDefinition::State& state = states[index];
        if (state.ParentId == StateTreeDefinition::kNoParent)
        {
            if (root != kNone)
                return Refuse(outError,
                              StateName(state.Id) + ": a second state with no parent. Give it a parent; a program has "
                                                    "one root.",
                              state.Id, StateTreeCompileItem::State);
            root = index;
            continue;
        }
        if (FindDefinitionIndex(byId, state.ParentId) == kNone)
            return Refuse(outError,
                          StateName(state.Id) + ": the parent " + std::to_string(state.ParentId) +
                              " is not in the program. Use the id of a state in the program.",
                          state.Id, StateTreeCompileItem::State);
        byParent.emplace_back(state.ParentId, index);
    }
    if (root == kNone)
        return Refuse(outError, "the program has no root. Give one state no parent.", StateTreeCompileError::kNoState,
                      StateTreeCompileItem::Program);
    std::sort(byParent.begin(), byParent.end());
    return true;
}

// Refuses a program whose tasks, transitions or modifiers do not fit the 16-bit spans of the program.
// The limit is one below kStateTreeNone, the same as the state count.
bool CheckSpanTotals(const std::vector<StateTreeDefinition::State>& states, StateTreeCompileError& outError)
{
    size_t tasks = 0;
    size_t transitions = 0;
    size_t modifiers = 0;
    for (const StateTreeDefinition::State& state : states)
    {
        tasks += state.Tasks.size();
        transitions += state.Transitions.size();
        modifiers += state.Modifiers.size();
    }
    for (const auto& [count, noun] : {std::pair{tasks, "tasks"}, std::pair{transitions, "transitions"},
                                      std::pair{modifiers, "modifiers"}})
    {
        if (count >= kStateTreeNone)
            return Refuse(outError,
                          "the program has " + std::to_string(count) + " " + noun + "; the limit is " +
                              std::to_string(kStateTreeNone - 1) + ". Split the program into smaller programs.",
                          StateTreeCompileError::kNoState, StateTreeCompileItem::Program);
    }
    return true;
}

} // namespace

bool StateTreeProgram::Compile(const StateTreeDefinition& definition, StateTreeProgram& out,
                               StateTreeCompileError& outError)
{
    out = {};
    outError = {};
    const std::vector<StateTreeDefinition::State>& states = definition.States;
    if (states.empty() || states.size() >= kStateTreeNone)
        return Refuse(outError, "the program has no states or more than 65534. Write one root state and its children.",
                      StateTreeCompileError::kNoState, StateTreeCompileItem::Program);
    if (!CheckSpanTotals(states, outError))
        return false;
    std::vector<IdIndex> byId;
    std::vector<IdIndex> byParent;
    uint32 root = kNone;
    if (!IndexById(states, byId, outError) || !IndexByParent(states, byId, byParent, root, outError))
        return false;

    // Breadth first from the root, so the children of each state are contiguous.
    std::vector<uint32> definitionOf{root};
    out.m_States.resize(1);
    for (uint32 index = 0; index < definitionOf.size(); ++index)
    {
        const uint32 id = states[definitionOf[index]].Id;
        const auto [first, last] = std::equal_range(byParent.begin(), byParent.end(), IdIndex{id, 0}, &LessId);
        if (first != last && out.m_States[index].Depth + 1 >= kMaxDepth)
            return Refuse(outError,
                          StateName(id) + ": its children are deeper than " + std::to_string(kMaxDepth) +
                              " levels. Move them up.",
                          id, StateTreeCompileItem::State);
        out.m_States[index].FirstChild = static_cast<uint16>(definitionOf.size());
        out.m_States[index].ChildCount = static_cast<uint16>(last - first);
        const uint8 childDepth = static_cast<uint8>(out.m_States[index].Depth + 1);
        for (auto child = first; child != last; ++child)
        {
            definitionOf.push_back(child->second);
            State& entry = out.m_States.emplace_back();
            entry.Parent = static_cast<uint16>(index);
            entry.Depth = childDepth;
        }
    }
    if (definitionOf.size() != states.size())
        return Refuse(outError,
                      "the program has states that are not under the root (a parent loop). Give each state a parent "
                      "on a path to the root.",
                      StateTreeCompileError::kNoState, StateTreeCompileItem::Program);
    std::vector<uint16> programIndexOf(states.size());
    for (uint32 index = 0; index < definitionOf.size(); ++index)
        programIndexOf[definitionOf[index]] = static_cast<uint16>(index);

    for (uint32 index = 0; index < out.m_States.size(); ++index)
    {
        const StateTreeDefinition::State& source = states[definitionOf[index]];
        State& state = out.m_States[index];
        const std::string where = StateName(source.Id);
        if (source.Tasks.size() > kMaxTasksPerState)
            return Refuse(outError,
                          where + ": more than " + std::to_string(kMaxTasksPerState) +
                              " tasks. Move some tasks to a child state.",
                          source.Id, StateTreeCompileItem::Task, kMaxTasksPerState);
        if (!(source.Weight >= 0.0f))
            return Refuse(outError, where + ": the weight is negative. Write a weight of 0 or more.", source.Id,
                          StateTreeCompileItem::State);
        state.StableId = source.Id;
        state.Policy = source.Policy;
        state.Join = source.Join;
        state.EnterCondition = source.EnterCondition;
        state.Weight = source.Weight;

        state.FirstTask = static_cast<uint16>(out.m_Tasks.size());
        state.TaskCount = static_cast<uint16>(source.Tasks.size());
        for (uint16 kind : source.Tasks)
            out.m_Tasks.push_back({kind, static_cast<uint16>(index)});

        state.FirstTransition = static_cast<uint16>(out.m_Transitions.size());
        state.TransitionCount = static_cast<uint16>(source.Transitions.size());
        for (uint16 item = 0; item < source.Transitions.size(); ++item)
        {
            const StateTreeDefinition::Transition& transition = source.Transitions[item];
            const uint32 target = FindDefinitionIndex(byId, transition.TargetId);
            if (target == kNone)
                return Refuse(outError,
                              where + ": a transition goes to " + std::to_string(transition.TargetId) +
                                  ", which is not in the program. Use the id of a state in the program.",
                              source.Id, StateTreeCompileItem::Transition, item);
            if (transition.Trigger == StateTreeTrigger::Condition && transition.Condition == kStateTreeNone)
                return Refuse(outError, where + ": a condition transition has no condition. Give it a condition.",
                              source.Id, StateTreeCompileItem::Transition, item);
            if (transition.Trigger == StateTreeTrigger::Event && transition.Event == kStateTreeNone)
                return Refuse(outError, where + ": an event transition has no event. Give it an event.", source.Id,
                              StateTreeCompileItem::Transition, item);
            out.m_Transitions.push_back({transition.Trigger, transition.On, transition.Condition,
                                         programIndexOf[target], transition.Event, transition.TimeoutTicks});
        }

        state.FirstModifier = static_cast<uint16>(out.m_Modifiers.size());
        state.ModifierCount = static_cast<uint16>(source.Modifiers.size());
        for (uint16 item = 0; item < source.Modifiers.size(); ++item)
        {
            const StateTreeModifier& modifier = source.Modifiers[item];
            if (modifier.Kind == StateTreeModifierKind::AbortIf && modifier.Condition == kStateTreeNone)
                return Refuse(outError, where + ": an 'abort if' modifier has no condition. Give it a condition.",
                              source.Id, StateTreeCompileItem::Modifier, item);
            if (modifier.Kind == StateTreeModifierKind::Cooldown && state.CooldownSlot == kStateTreeNone)
                state.CooldownSlot = out.m_CooldownSlotCount++;
            out.m_Modifiers.push_back(modifier);
        }

        if (state.Policy == StateTreePolicy::Utility)
        {
            if (source.Utility.Options.size() != state.ChildCount)
                return Refuse(outError,
                              where + ": the utility state has " + std::to_string(source.Utility.Options.size()) +
                                  " options and " + std::to_string(state.ChildCount) +
                                  " children. Give each child one option, in the children's order.",
                              source.Id, StateTreeCompileItem::Option,
                              static_cast<uint16>(std::min<size_t>(source.Utility.Options.size(), state.ChildCount)));
            state.UtilitySet = static_cast<uint16>(out.m_UtilitySets.size());
            out.m_UtilitySets.push_back({source.Utility, ColumnCount(source.Utility)});
        }
    }

    out.m_StatesById.reserve(states.size());
    for (uint32 index = 0; index < out.m_States.size(); ++index)
        out.m_StatesById.emplace_back(out.m_States[index].StableId, static_cast<uint16>(index));
    std::sort(out.m_StatesById.begin(), out.m_StatesById.end());
    return true;
}

uint16 StateTreeProgram::FindState(uint32 stableId) const
{
    const auto it = std::lower_bound(m_StatesById.begin(), m_StatesById.end(),
                                     std::pair<uint32, uint16>{stableId, 0});
    return it != m_StatesById.end() && it->first == stableId ? it->second : kStateTreeNone;
}

} // namespace GameEngine::AI
