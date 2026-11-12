// The state-tree program file (.scribe): the parser keeps the file as the author wrote it and compiles a
// program with its names, the writer writes the same file back, and the parser refuses what the format or
// the compiler does not hold, with the fix.
#include "AIECS/Assets/AiStateTree/AiStateTreeData.h"

#include "../Source/Assets/AiJsonFields.h"

#include <gtest/gtest.h>

#include <bit>
#include <limits>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr const char* kPatrol = R"({
  "root": { "id": 1, "name": "Guard", "policy": "utility", "utility": { "commitmentMargin": 0.1, "minimumTicks": 20 },
    "children": [
      { "id": 2, "name": "Patrol", "tasks": ["Walk", "LookAround"], "join": "all",
        "option": { "weight": 1, "considerations": [ { "input": "Calm", "curve": { "preset": "linear" } } ] },
        "transitions": [ { "to": 3, "on": "condition", "condition": "SeesEnemy" } ] },
      { "id": 3, "name": "Chase", "tasks": ["Walk"], "enterIf": "SeesEnemy",
        "option": { "rank": 1, "considerations": [ { "input": "Threat", "curve": { "preset": "step" } } ] },
        "transitions": [ { "to": 2, "on": "timeout", "ticks": 100 }, { "to": 2, "on": "event", "event": "AllClear" } ],
        "modifiers": [ { "kind": "cooldown", "ticks": 40 } ] }
    ] }
})";

// The example of docs/ai/ai-state-trees.mdx.
constexpr const char* kDocsExample = R"({
  "root": {
    "id": 1, "name": "Guard", "policy": "utility",
    "utility": { "commitmentMargin": 0.1, "minimumTicks": 20 },
    "children": [
      { "id": 2, "name": "Patrol", "tasks": ["Walk", "LookAround"], "join": "all",
        "option": { "considerations": [ { "input": "Calm", "curve": { "preset": "linear" } } ] },
        "transitions": [ { "to": 3, "on": "condition", "condition": "SeesEnemy" } ] },
      { "id": 3, "name": "Chase", "tasks": ["Walk"], "enterIf": "SeesEnemy",
        "option": { "rank": 1, "considerations": [ { "input": "Threat", "curve": { "preset": "step" } } ] },
        "transitions": [ { "to": 2, "on": "timeout", "ticks": 100 } ],
        "modifiers": [ { "kind": "cooldown", "ticks": 40 } ] }
    ]
  }
})";

// Each field of the format with a value that is not its default.
constexpr const char* kEveryField = R"({ "root": { "id": 7, "name": "Camp", "policy": "random", "join": "any",
  "weight": 0.3, "tasks": ["Rest"], "utility": { "commitmentMargin": 0.25, "minimumTicks": 3, "noiseMargin": 0.05 },
  "modifiers": [ { "kind": "abortIf", "condition": "Hungry" }, { "kind": "repeat", "count": 2 } ],
  "transitions": [ { "to": 8, "on": "completion", "result": "failed" }, { "to": 8, "on": "event", "event": "Horn" } ],
  "children": [
    { "id": 8, "policy": "utility", "join": "primary", "weight": 2.5, "children": [
      { "id": 9, "name": "Eat", "option": { "weight": 0.75, "rank": 2, "considerations": [
        { "input": "Food", "min": -1.5, "max": 3, "curve": { "preset": "logistic", "shape": 4, "midpoint": 0.4 } },
        { "input": "Hunger", "curve": { "preset": "polynomial", "shape": 2 } } ] } },
      { "id": 10, "name": "Wait", "tasks": ["Rest", "Watch"], "join": "all" } ] } ] } })";

// The examples of Tests/Assets/AiStateTreeAssetTypeTests.cpp.
constexpr const char* kOneState = R"({ "root": { "id": 1, "name": "Guard" } })";
constexpr const char* kTwoStates = R"({ "root": { "id": 1, "name": "Guard", "children": [ { "id": 2 } ] } })";

// The text that WriteAiStateTree writes, or its refusal.
std::string WriteText(const AIECS::AiStateTreeSource& source)
{
    std::string text;
    std::string error;
    return AIECS::WriteAiStateTree(source, text, error) ? text : "refused: " + error;
}

} // namespace

TEST(AiStateTreeData, TheFileCompilesIntoAProgramWithItsNames)
{
    AIECS::AiStateTreeData data;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiStateTree(kPatrol, data, error)) << error;

    EXPECT_EQ(data.Source.TaskNames, (std::vector<std::string>{"Walk", "LookAround"}));
    EXPECT_EQ(data.Source.ConditionNames, (std::vector<std::string>{"SeesEnemy"}));
    EXPECT_EQ(data.Source.InputNames, (std::vector<std::string>{"Calm", "Threat"}));
    const auto states = data.Program.GetStates();
    ASSERT_EQ(states.size(), 3u);
    EXPECT_EQ(states[0].Policy, AI::StateTreePolicy::Utility);
    EXPECT_EQ(states[1].Join, AI::StateTreeJoin::All);
    EXPECT_EQ(states[2].EnterCondition, 0);
    EXPECT_EQ(data.Program.GetCooldownSlotCount(), 1);
    const AI::OptionSet& options = data.Program.GetUtilitySets()[0].Options;
    ASSERT_EQ(options.Options.size(), 2u);
    EXPECT_EQ(options.Options[1].Rank, 1);
    EXPECT_EQ(options.MinimumTicks, 20u);
    EXPECT_EQ(data.Program.GetTransitions()[states[2].FirstTransition].TimeoutTicks, 100u);
    EXPECT_EQ(data.Source.EventNames, (std::vector<std::string>{"AllClear"}));
    const AI::StateTreeProgram::Transition& onEvent = data.Program.GetTransitions()[states[2].FirstTransition + 1];
    EXPECT_EQ(onEvent.Trigger, AI::StateTreeTrigger::Event);
    EXPECT_EQ(onEvent.Event, 0);
}

TEST(AiStateTreeData, TheSourceKeepsTheNamesInTheFileOrder)
{
    AIECS::AiStateTreeSource source;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiStateTreeSource(kEveryField, source, error)) << error;

    std::vector<std::string> stateNames;
    for (const AIECS::AiStateTreeSource::StateText& state : source.States)
        stateNames.push_back(state.Name);
    EXPECT_EQ(stateNames, (std::vector<std::string>{"Camp", "", "Eat", "Wait"}));
    EXPECT_EQ(source.TaskNames, (std::vector<std::string>{"Rest", "Watch"}));
    EXPECT_EQ(source.ConditionNames, (std::vector<std::string>{"Hungry"}));
    EXPECT_EQ(source.EventNames, (std::vector<std::string>{"Horn"}));
    EXPECT_EQ(source.InputNames, (std::vector<std::string>{"Food", "Hunger"}));
    const std::vector<AIECS::AiStateTreeSource::Curve>& curves = source.States[1].Curves;
    ASSERT_EQ(curves.size(), 2u);
    EXPECT_EQ(curves[0].Preset, AI::CurvePreset::Logistic);
    EXPECT_EQ(curves[0].Shape, 4.0f);
    EXPECT_EQ(curves[0].Midpoint, 0.4f);
    EXPECT_EQ(curves[1].Preset, AI::CurvePreset::Polynomial);
}

// write(parse(text)) reads back to the same source: a second write gives the same text, and the written
// file compiles to a program with the same states.
TEST(AiStateTreeData, TheWriterWritesWhatTheParserReads)
{
    for (const char* text : {kPatrol, kDocsExample, kEveryField, kOneState, kTwoStates})
    {
        AIECS::AiStateTreeData data;
        std::string error;
        ASSERT_TRUE(AIECS::ParseAiStateTree(text, data, error)) << error;
        const std::string written = WriteText(data.Source);
        AIECS::AiStateTreeData reread;
        ASSERT_TRUE(AIECS::ParseAiStateTree(written, reread, error)) << error << "\n" << written;
        EXPECT_EQ(WriteText(reread.Source), written);
        EXPECT_EQ(reread.Source.TaskNames, data.Source.TaskNames);
        EXPECT_EQ(reread.Source.ConditionNames, data.Source.ConditionNames);
        EXPECT_EQ(reread.Source.EventNames, data.Source.EventNames);
        EXPECT_EQ(reread.Source.InputNames, data.Source.InputNames);
        ASSERT_EQ(reread.Program.GetStates().size(), data.Program.GetStates().size());
        for (size_t state = 0; state < data.Program.GetStates().size(); ++state)
            EXPECT_EQ(reread.Program.GetStates()[state].StableId, data.Program.GetStates()[state].StableId);
    }
    // kEveryField writes no default value, so the written file holds each of its fields with the same value.
    AIECS::AiStateTreeSource source;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiStateTreeSource(kEveryField, source, error)) << error;
    EXPECT_EQ(AIECS::Json::parse(WriteText(source)), AIECS::Json::parse(kEveryField));
    // A finite float reads back with the same bits.
    ASSERT_TRUE(AIECS::ParseAiStateTreeSource(kTwoStates, source, error)) << error;
    for (const float32 weight : {std::numeric_limits<float32>::max(), -0.0f, 1e-7f})
    {
        source.Definition.States[1].Weight = weight;
        AIECS::AiStateTreeSource reread;
        ASSERT_TRUE(AIECS::ParseAiStateTreeSource(WriteText(source), reread, error)) << error;
        EXPECT_EQ(std::bit_cast<uint32>(reread.Definition.States[1].Weight), std::bit_cast<uint32>(weight));
    }
}

// A file holds only finite numbers: the writer refuses NaN and infinity and names the state and the field.
TEST(AiStateTreeData, TheWriterRefusesANumberThatIsNotFinite)
{
    AIECS::AiStateTreeSource source;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiStateTreeSource(kTwoStates, source, error)) << error;
    for (const float32 weight : {std::numeric_limits<float32>::quiet_NaN(), -std::numeric_limits<float32>::infinity()})
    {
        source.Definition.States[1].Weight = weight;
        std::string text;
        EXPECT_FALSE(AIECS::WriteAiStateTree(source, text, error));
        EXPECT_EQ(error, "state 2: 'weight' is not a finite number. Write a finite number such as 1.0.");
    }
}

TEST(AiStateTreeData, TheParserRefusesWithTheFix)
{
    AIECS::AiStateTreeData data;
    std::string error;
    EXPECT_FALSE(AIECS::ParseAiStateTree(R"({ "root": { "id": 1, "children": [ { "id": 2, "option": {} } ] } })", data,
                                         error));
    EXPECT_NE(error.find("only for a child of a utility state"), std::string::npos) << error;
    EXPECT_FALSE(AIECS::ParseAiStateTree(
        R"({ "root": { "id": 1, "transitions": [ { "to": 9, "on": "timeout", "ticks": 1 } ] } })", data, error));
    EXPECT_NE(error.find("not in the program"), std::string::npos) << error;
    EXPECT_FALSE(AIECS::ParseAiStateTree(R"({ "root": { "id": 1, "policy": "best" } })", data, error));
    EXPECT_NE(error.find("first, sequence, utility, random"), std::string::npos) << error;
    EXPECT_FALSE(AIECS::ParseAiStateTree(R"({ "root": { "id": 1, "name": "" } })", data, error));
    EXPECT_EQ(error, "root: 'name' is not a name. Write a non-empty string, or delete the field.");
}

// A compile refusal names its state by its name and its id, or by its id alone when the state has no name.
TEST(AiStateTreeData, ACompileRefusalNamesTheStateByItsNameAndId)
{
    AIECS::AiStateTreeData data;
    std::string error;
    EXPECT_FALSE(AIECS::ParseAiStateTree(
        R"({ "root": { "id": 1, "children": [ { "id": 2, "name": "Patrol", "weight": -1 } ] } })", data, error));
    EXPECT_EQ(error, "the program: Patrol (state 2): the weight is negative. Write a weight of 0 or more.");
    EXPECT_FALSE(AIECS::ParseAiStateTree(R"({ "root": { "id": 1, "children": [ { "id": 2, "weight": -1 } ] } })",
                                         data, error));
    EXPECT_EQ(error, "the program: State 2: the weight is negative. Write a weight of 0 or more.");
}

TEST(AiStateTreeData, TheParserRefusesMoreNamesThanTheSixteenBitIndexHolds)
{
    std::string text = R"({ "root": { "id": 1, "tasks": [)";
    for (uint32 name = 0; name <= AI::kStateTreeNone; ++name)
        text += (name == 0 ? "\"t" : ", \"t") + std::to_string(name) + "\"";
    text += "] } }";
    AIECS::AiStateTreeData data;
    std::string error;
    EXPECT_FALSE(AIECS::ParseAiStateTree(text, data, error));
    EXPECT_NE(error.find("more than 65535 task names; the limit is 65535"), std::string::npos) << error;
}

TEST(AiStateTreeData, TheParserRefusesMoreConsiderationsThanTheSixteenBitIndexHolds)
{
    // The reader is shared by the program file and the option set file; a file of 65535 consideration
    // objects is too slow to parse in a test, so the lists start at the limit.
    AI::OptionSet options;
    options.Considerations.resize(AI::kStateTreeNone - 1);
    options.Curves.resize(AI::kStateTreeNone - 1);
    std::vector<std::string> inputNames;
    std::string error;
    const auto consideration = AIECS::Json::parse(R"({ "input": "Calm", "curve": { "preset": "linear" } })");
    EXPECT_FALSE(
        AIECS::ParseConsideration(consideration, "considerations[65534]", options, inputNames, nullptr, error));
    EXPECT_NE(error.find("more than 65534 considerations; the limit is 65534"), std::string::npos) << error;
}

TEST(AiStateTreeData, TheParserRefusesMoreInputNamesThanTheSixteenBitIndexHolds)
{
    AI::OptionSet options;
    std::vector<std::string> inputNames(AI::kStateTreeNone - 1);
    std::string error;
    const auto consideration = AIECS::Json::parse(R"({ "input": "Calm", "curve": { "preset": "linear" } })");
    EXPECT_FALSE(AIECS::ParseConsideration(consideration, "considerations[0]", options, inputNames, nullptr, error));
    EXPECT_NE(error.find("more than 65534 input names; the limit is 65534"), std::string::npos) << error;
}

TEST(AiStateTreeData, EachTransitionAndModifierKindTakesItsOwnFields)
{
    AIECS::AiStateTreeData data;
    std::string error;
    EXPECT_FALSE(AIECS::ParseAiStateTree(R"({ "root": { "id": 1, "modifiers": [ { "kind": "repeat" } ] } })", data,
                                         error));
    EXPECT_NE(error.find("a 'repeat' needs the field 'count'"), std::string::npos) << error;
    EXPECT_FALSE(AIECS::ParseAiStateTree(
        R"({ "root": { "id": 1, "modifiers": [ { "kind": "cooldown", "count": 3 } ] } })", data, error));
    EXPECT_NE(error.find("a 'cooldown' does not use the field 'count'"), std::string::npos) << error;
    EXPECT_FALSE(AIECS::ParseAiStateTree(R"({ "root": { "id": 1, "transitions": [ { "to": 1, "on": "timeout" } ] } })",
                                         data, error));
    EXPECT_NE(error.find("a 'timeout' needs the field 'ticks'"), std::string::npos) << error;
}
