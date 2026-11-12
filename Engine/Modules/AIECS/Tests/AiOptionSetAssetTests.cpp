// The option-set file (.aioptions): the parser reads an option set and its difficulty presets with the
// pinned values, refuses what the format does not hold with the fix, and the registry compiles the
// task and input names into a program.
#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/Assets/AiOptionSetData.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsLarge.h"
#include "EngineLogCapture.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr const char* kCommander = R"({
  "commitmentMargin": 0.1,
  "minimumTicks": 20,
  "noiseMargin": 0.05,
  "cooldownCell": 2,
  "options": [
    { "task": "Attack", "weight": 2.0, "rank": 1, "cooldownTicks": 40,
      "considerations": [ { "input": "Threat", "min": 0, "max": 10,
                            "curve": { "preset": "logistic", "shape": 8, "midpoint": 0.25 } } ] },
    { "task": "Retreat",
      "considerations": [ { "input": "Health", "curve": { "preset": "linear" } },
                          { "input": "Threat", "max": 5, "curve": { "preset": "step", "midpoint": 0.5 } } ] }
  ],
  "presets": [ { "name": "Easy", "cadenceTicks": 50 }, { "name": "Normal", "cadenceTicks": 24 } ]
})";

void ReadZero(const ECS::World&, std::span<const ECS::EntityHandle>, std::span<float32> out)
{
    for (float32& value : out)
        value = 0.0f;
}

AIECS::AiTaskStatus TickIdle(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Running;
}

std::string RefusalOf(const std::string& text)
{
    AIECS::AiOptionSetData data;
    std::string error;
    EXPECT_FALSE(AIECS::ParseAiOptionSet(text, data, error)) << text;
    return error;
}

} // namespace

TEST(AiOptionSetAsset, ParsesTheOptionSetAndPresetsWithThePinnedValues)
{
    AIECS::AiOptionSetData data;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiOptionSet(kCommander, data, error)) << error;

    const AI::OptionSet& set = data.Options;
    EXPECT_FLOAT_EQ(set.CommitmentMargin, 0.1f);
    EXPECT_EQ(set.MinimumTicks, 20u);
    EXPECT_FLOAT_EQ(set.NoiseMargin, 0.05f);
    EXPECT_EQ(data.CooldownCell, 2u);
    ASSERT_EQ(set.Options.size(), 2u);
    EXPECT_FLOAT_EQ(set.Options[0].Weight, 2.0f);
    EXPECT_EQ(set.Options[0].Rank, 1u);
    EXPECT_EQ(set.Options[0].CooldownTicks, 40u);
    EXPECT_EQ(set.Options[1].FirstConsideration, 1u);
    EXPECT_EQ(set.Options[1].ConsiderationCount, 2u);
    EXPECT_FLOAT_EQ(set.Options[1].Weight, 1.0f);
    EXPECT_EQ(data.TaskNames, (std::vector<std::string>{"Attack", "Retreat"}));
    EXPECT_EQ(data.InputNames, (std::vector<std::string>{"Threat", "Health"}));

    ASSERT_EQ(set.Considerations.size(), 3u);
    EXPECT_FLOAT_EQ(set.Considerations[0].Max, 10.0f);
    EXPECT_EQ(set.Considerations[1].Column, 1u);
    EXPECT_EQ(set.Considerations[2].Column, 0u);
    EXPECT_FLOAT_EQ(set.Considerations[2].Max, 5.0f);
    EXPECT_EQ(set.Considerations[2].Curve, 2u);
    ASSERT_EQ(set.Curves.size(), 3u);
    const AI::ResponseCurve logistic = AI::ResponseCurve::FromPreset(AI::CurvePreset::Logistic, 8.0f, 0.25f);
    EXPECT_FLOAT_EQ(set.Curves[0].Evaluate(0.4f), logistic.Evaluate(0.4f));
    EXPECT_FLOAT_EQ(set.Curves[2].Evaluate(0.6f), 1.0f);

    ASSERT_EQ(data.Presets.size(), 2u);
    const AIECS::AiDifficultyPreset* normal = data.FindPreset("Normal");
    ASSERT_NE(normal, nullptr);
    EXPECT_EQ(normal->CadenceTicks, 24u);
    EXPECT_EQ(data.FindPreset("Hard"), nullptr);
}

TEST(AiOptionSetAsset, RefusesAFieldTheFormatDoesNotHaveAndNamesTheFix)
{
    const std::string error =
        RefusalOf(R"({ "options": [ { "task": "Attack", "wieght": 2, "considerations": [] } ] })");
    EXPECT_NE(error.find("options[0]"), std::string::npos) << error;
    EXPECT_NE(error.find("'wieght'"), std::string::npos) << error;
    EXPECT_NE(error.find("weight"), std::string::npos) << error;
}

TEST(AiOptionSetAsset, RefusesACurvePresetOutsideItsList)
{
    const std::string error = RefusalOf(R"({ "options": [ { "task": "Attack", "considerations": [
        { "input": "Threat", "curve": { "preset": "cubic" } } ] } ] })");
    EXPECT_NE(error.find("'cubic'"), std::string::npos) << error;
    EXPECT_NE(error.find("logistic"), std::string::npos) << error;
}

TEST(AiOptionSetAsset, RefusesAFieldWrittenTwiceInOneObject)
{
    const std::string error =
        RefusalOf(R"({ "options": [ { "task": "Attack", "weight": 1, "weight": 2, "considerations": [] } ] })");
    EXPECT_NE(error.find("'weight'"), std::string::npos) << error;
}

TEST(AiOptionSetAsset, RefusesTextThatIsNotJsonAndNamesTheLine)
{
    const std::string error = RefusalOf("{\n  \"options\": [ }\n}");
    EXPECT_NE(error.find("line 2"), std::string::npos) << error;
}

// Each refusal the format lists names the field that the author must fix.
TEST(AiOptionSetAsset, RefusesEachInvalidValueAndNamesItsField)
{
    struct Case
    {
        const char* Text;
        const char* Field;
    };
    const Case cases[] = {
        {R"({ "commitmentMargin": 1e300 })", "'commitmentMargin'"},
        {R"({ "options": [ { "weight": 1, "considerations": [] } ] })", "'task'"},
        {R"({ "options": [ { "task": "Attack", "cooldownTicks": 5, "considerations": [] } ] })", "'cooldownCell'"},
        {R"({ "options": [ { "task": "Attack", "considerations": [ { "input": "Threat" } ] } ] })", "'curve'"},
        {R"({ "presets": [ { "name": "Easy" }, { "name": "Easy" } ] })", "'Easy'"},
        {R"({ "options": [ { "task": "Attack", "rank": 256, "considerations": [] } ] })", "'rank'"},
    };
    for (const Case& refused : cases)
    {
        SCOPED_TRACE(refused.Text);
        const std::string error = RefusalOf(refused.Text);
        EXPECT_NE(error.find(refused.Field), std::string::npos) << error;
    }
}

TEST(AiOptionSetAsset, RefusesAValueItCannotReadInWordsADesignerReads)
{
    for (const char* text : {R"({ "commitmentMargin": 1e300 })", R"({ "commitmentMargin": -1e300 })",
                             R"({ "commitmentMargin": "high" })"})
    {
        SCOPED_TRACE(text);
        const std::string error = RefusalOf(text);
        EXPECT_NE(error.find("'commitmentMargin' is out of range or is not a number. Write a number such as 1.0."),
                  std::string::npos)
            << error;
    }
}

TEST(AiOptionSetAsset, CompileProgramResolvesTheTaskAndInputNames)
{
    AIECS::AiRegistry registry;
    const uint16 health = registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::JobThreads, "Health");
    const uint16 threat = registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::JobThreads, "Threat");
    const uint16 retreat = registry.RegisterTask({.Tick = &TickIdle, .Name = "Retreat"});
    const uint16 attack = registry.RegisterTask({.Tick = &TickIdle, .Name = "Attack"});
    AIECS::AiOptionSetData data;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiOptionSet(kCommander, data, error)) << error;

    const uint16 id = registry.CompileProgram(data);
    const AIECS::AiProgram* program = registry.FindProgram(id);
    ASSERT_NE(program, nullptr);
    EXPECT_EQ(program->ColumnInputs, (std::vector<uint16>{threat, health}));
    EXPECT_EQ(program->Options.Options[0].Action, attack);
    EXPECT_EQ(program->Options.Options[1].Action, retreat);
    EXPECT_EQ(program->CooldownCell, 2u);
}

TEST(AiOptionSetAsset, CompileProgramRefusesATaskNoModuleRegisteredAndNamesTheRegisteredOnes)
{
    AIECS::AiRegistry registry;
    registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::JobThreads, "Health");
    registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::JobThreads, "Threat");
    registry.RegisterTask({.Tick = &TickIdle, .Name = "Attack"});
    AIECS::AiOptionSetData data;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiOptionSet(kCommander, data, error)) << error;

    std::vector<std::string> lines;
    uint16 id = 0;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        id = registry.CompileProgram(data);
    }
    EXPECT_EQ(id, Components::AiAgent::kNoProgram);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("'Retreat'"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find("Attack"), std::string::npos) << lines[0];
}

TEST(AiOptionSetAsset, CompileProgramRefusesAnEntityCellOutsideTheLargestCellClass)
{
    AIECS::AiRegistry registry;
    registry.RegisterTask({.Tick = &TickIdle, .Name = "Attack"});
    AIECS::AiOptionSetData data;
    data.TaskNames = {"Attack"};
    data.Options.Options.push_back({.Action = 0});
    data.EntityCells = {Components::AiCellsLarge::kCount};
    data.StableId = GUID::Generate();

    std::vector<std::string> lines;
    uint16 id = 0;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        id = registry.CompileProgram(data);
    }
    EXPECT_EQ(id, Components::AiAgent::kNoProgram);
    ASSERT_EQ(lines.size(), 1u);
    const std::string cell = std::to_string(Components::AiCellsLarge::kCount);
    EXPECT_NE(lines[0].find("entity cell " + cell), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find("holds " + cell), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find(data.StableId.ToString()), std::string::npos) << lines[0];
}

// The asset reader refuses the entity cells CompileProgram refuses, so the error names the file.
TEST(AiOptionSetAsset, RefusesAnEntityCellOutsideTheLargestCellClass)
{
    const std::string cell = std::to_string(Components::AiCellsLarge::kCount);
    const std::string error = RefusalOf(R"({ "entityCells": [ )" + cell + " ] }");
    EXPECT_NE(error.find("entityCells: cell " + cell), std::string::npos) << error;
    EXPECT_NE(error.find("holds " + cell), std::string::npos) << error;
}
