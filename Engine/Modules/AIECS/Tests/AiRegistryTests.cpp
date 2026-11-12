#include "AI/StateTree/StateTreeCompileError.h"
#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeData.h"
#include "AIECS/Components/AiAgent.h"
#include "ECS/ModuleRegistration.h"
#include "EngineLogCapture.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

void ReadZero(const ECS::World&, std::span<const ECS::EntityHandle>, std::span<float32> out)
{
    for (float32& value : out)
        value = 0.0f;
}

AIECS::AiTaskStatus TickIdle(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Running;
}

// One option with one consideration that reads column 0 through curve 0; every index is valid.
AIECS::AiProgram MakeValidProgram(uint16 input, uint16 task)
{
    AIECS::AiProgram program;
    program.ColumnInputs = {input};
    program.Options.Curves.push_back(AI::ResponseCurve::FromPreset(AI::CurvePreset::Linear, 1.0f, 0.5f));
    program.Options.Considerations.push_back({.Column = 0, .Curve = 0});
    program.Options.Options.push_back({.FirstConsideration = 0, .ConsiderationCount = 1, .Action = task});
    return program;
}

struct BadProgram
{
    const char* Name;
    void (*Break)(AIECS::AiProgram& program);
    const char* Message;
};

const BadProgram kBadPrograms[] = {
    {"task", [](AIECS::AiProgram& program) { program.Options.Options[0].Action = 5; }, "option 0 names task 5"},
    {"consideration", [](AIECS::AiProgram& program) { program.Options.Options[0].ConsiderationCount = 2; },
     "option 0 uses considerations 0 to 1"},
    {"column", [](AIECS::AiProgram& program) { program.Options.Considerations[0].Column = 3; },
     "consideration 0 reads column 3"},
    {"curve", [](AIECS::AiProgram& program) { program.Options.Considerations[0].Curve = 2; },
     "consideration 0 uses curve 2"},
    {"input", [](AIECS::AiProgram& program) { program.ColumnInputs[0] = 9; }, "column 0 names input 9"},
    {"cooldown cell", [](AIECS::AiProgram& program) { program.CooldownCell = 0xFFF0; }, "CooldownCell 65520"},
    {"cooldown without cell", [](AIECS::AiProgram& program) { program.Options.Options[0].CooldownTicks = 4; },
     "option 0 has CooldownTicks 4"},
};

void TestNothing(const ECS::World&, std::span<const ECS::EntityHandle>, std::span<uint8> out)
{
    for (uint8& value : out)
        value = 0;
}

struct BadCondition
{
    const char* Name;
    AIECS::AiConditionFunction Function;
    const char* ConditionName;
    // Words of the fix that the error states.
    const char* Fix;
};

struct BadEventType
{
    const char* Name;
    // The module load that registers; empty: a call outside a module load.
    const char* ModuleId;
    const char* EventName;
    // Words of the fix that the error states.
    const char* Fix;
};

} // namespace

// AddProgram refuses each bad index with an error that names it, and accepts the valid program.
TEST(AiRegistryTests, AddProgramRefusesEachBadIndex)
{
    AIECS::AiRegistry registry;
    const uint16 input = registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::CallingThread);
    const uint16 task = registry.RegisterTask({nullptr, &TickIdle, nullptr});
    for (const BadProgram& bad : kBadPrograms)
    {
        SCOPED_TRACE(bad.Name);
        AIECS::AiProgram program = MakeValidProgram(input, task);
        bad.Break(program);
        std::vector<std::string> lines;
        uint16 id = 0;
        {
            TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
            id = registry.AddProgram(std::move(program));
            Logger::Log::Flush();
        }
        EXPECT_EQ(id, Components::AiAgent::kNoProgram);
        EXPECT_EQ(TestLog::CountLinesContaining(lines, bad.Message), 1u) << TestLog::FirstLineContaining(lines, "AddProgram");
    }
    EXPECT_EQ(registry.AddProgram(MakeValidProgram(input, task)), 0u);
}

// A null input would run on a job thread, so RegisterInput refuses it with an error, and a
// program that names the refused id is refused too.
TEST(AiRegistryTests, RegisterInputRefusesANullFunction)
{
    AIECS::AiRegistry registry;
    const uint16 task = registry.RegisterTask({nullptr, &TickIdle, nullptr});
    std::vector<std::string> lines;
    uint16 input = 0;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        input = registry.RegisterInput(nullptr, AIECS::AiInputThreading::JobThreads);
        Logger::Log::Flush();
    }
    EXPECT_EQ(input, AIECS::AiRegistry::kNoId);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "RegisterInput"), 1u);
    EXPECT_EQ(registry.AddProgram(MakeValidProgram(input, task)), Components::AiAgent::kNoProgram);
}

// A task with no Tick function would be called on a job thread, so RegisterTask refuses it with
// an error, and a program that names the refused id is refused too.
TEST(AiRegistryTests, RegisterTaskRefusesANullTick)
{
    AIECS::AiRegistry registry;
    const uint16 input = registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::CallingThread);
    std::vector<std::string> lines;
    uint16 task = 0;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        task = registry.RegisterTask({nullptr, nullptr, nullptr});
        Logger::Log::Flush();
    }
    EXPECT_EQ(task, AIECS::AiRegistry::kNoId);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "RegisterTask"), 1u);
    EXPECT_EQ(registry.AddProgram(MakeValidProgram(input, task)), Components::AiAgent::kNoProgram);
}

// A null sensor would be called for every group, so RegisterSensor refuses it with an error.
TEST(AiRegistryTests, RegisterSensorRefusesANullFunction)
{
    AIECS::AiRegistry registry;
    std::vector<std::string> lines;
    uint16 sensor = 0;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        sensor = registry.RegisterSensor(nullptr);
        Logger::Log::Flush();
    }
    EXPECT_EQ(sensor, AIECS::AiRegistry::kNoId);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "RegisterSensor"), 1u);
    EXPECT_TRUE(registry.GetSensors().empty());
}

// A null command would be called for each order of its type, so RegisterCommand refuses it with
// an error, and the refused id is not a command id.
TEST(AiRegistryTests, RegisterCommandRefusesANullFunction)
{
    AIECS::AiRegistry registry;
    std::vector<std::string> lines;
    uint16 command = 0;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        command = registry.RegisterCommand(nullptr);
        Logger::Log::Flush();
    }
    EXPECT_EQ(command, AIECS::AiRegistry::kNoId);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "RegisterCommand"), 1u);
    EXPECT_EQ(registry.GetCommandCount(), 0u);
}

// RegisterCondition refuses a null function, an empty name and a taken name with an error that names the fix.
TEST(AiRegistryTests, RegisterConditionRefusesEachBadEntry)
{
    const BadCondition kBadConditions[] = {
        {"no function", nullptr, "Free", "Pass a function"},
        {"no name", &TestNothing, "", "Pass a function"},
        {"taken name", &TestNothing, "Taken", "a name of its own"},
    };
    AIECS::AiRegistry registry;
    ASSERT_EQ(registry.RegisterCondition(&TestNothing, "Taken"), 0u);
    for (const BadCondition& bad : kBadConditions)
    {
        SCOPED_TRACE(bad.Name);
        std::vector<std::string> lines;
        uint16 id = 0;
        {
            TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
            id = registry.RegisterCondition(bad.Function, bad.ConditionName);
            Logger::Log::Flush();
        }
        EXPECT_EQ(id, AIECS::AiRegistry::kNoId);
        EXPECT_EQ(TestLog::CountLinesContaining(lines, "RegisterCondition"), 1u);
        EXPECT_EQ(TestLog::CountLinesContaining(lines, bad.Fix), 1u);
    }
}

// A program file that names an unregistered condition is refused with an error that names the condition.
TEST(AiRegistryTests, CompileStateTreeRefusesAnUnregisteredCondition)
{
    AIECS::AiRegistry registry;
    registry.RegisterTask({nullptr, &TickIdle, nullptr, "Idle"});
    AIECS::AiStateTreeData data;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiStateTree(R"({ "root": { "id": 1, "children": [
      { "id": 2, "tasks": ["Idle"], "enterIf": "SeesEnemy" } ] } })",
                                        data, error))
        << error;
    std::vector<std::string> lines;
    uint16 id = 0;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        id = registry.CompileStateTree(data);
        Logger::Log::Flush();
    }
    EXPECT_EQ(id, Components::AiAgent::kNoProgram);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "State 2 names the condition 'SeesEnemy'"), 1u);
    const std::vector<AI::StateTreeCompileError> refusals = registry.CheckStateTreeNames(data.Source);
    ASSERT_EQ(refusals.size(), 1u);
    EXPECT_EQ(refusals[0].StateId, 2u);
    EXPECT_EQ(refusals[0].Item, AI::StateTreeCompileItem::EnterCondition);
}

// The name check refuses each use of an unregistered name, state by state, with its state, its item and the
// state's name, and adds nothing to the registry.
TEST(AiRegistryTests, CheckStateTreeNamesRefusesEachUnknownNameWithItsState)
{
    AIECS::AiRegistry registry;
    registry.RegisterTask({nullptr, &TickIdle, nullptr, "Idle"});
    registry.RegisterCondition(&TestNothing, "Rested");
    AIECS::AiStateTreeSource source;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiStateTreeSource(R"({ "root": { "id": 1, "children": [
      { "id": 2, "tasks": ["Idle"], "transitions": [ { "to": 3, "on": "condition", "condition": "Rested" } ] },
      { "id": 3, "name": "Chase", "tasks": ["Walk"], "transitions": [ { "to": 2, "on": "completion" },
        { "to": 2, "on": "condition", "condition": "SeesEnemy" } ] } ] } })",
                                              source, error))
        << error;

    const std::vector<AI::StateTreeCompileError> refusals = registry.CheckStateTreeNames(source);
    ASSERT_EQ(refusals.size(), 2u);
    EXPECT_EQ(refusals[0].StateId, 3u);
    EXPECT_EQ(refusals[0].Item, AI::StateTreeCompileItem::Task);
    EXPECT_NE(refusals[0].Message.find("Chase (state 3) names the task 'Walk'"), std::string::npos)
        << refusals[0].Message;
    EXPECT_EQ(refusals[1].Item, AI::StateTreeCompileItem::Transition);
    EXPECT_EQ(refusals[1].ItemIndex, 1);
    EXPECT_NE(refusals[1].Message.find("registered conditions: Rested."), std::string::npos) << refusals[1].Message;
    EXPECT_EQ(registry.FindStateTree(Components::AiAgent::kStateTreeProgramBit), nullptr);

    registry.RegisterCondition(&TestNothing, "SeesEnemy");
    registry.RegisterTask({nullptr, &TickIdle, nullptr, "Walk"});
    EXPECT_TRUE(registry.CheckStateTreeNames(source).empty());
}

// With no name of a kind registered, the refusal asks to register the name in the game's AI module.
TEST(AiRegistryTests, ARefusalWithNoRegisteredTasksAsksToRegisterTheName)
{
    AIECS::AiRegistry registry;
    AIECS::AiStateTreeSource source;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiStateTreeSource(R"({ "root": { "id": 1, "tasks": ["Walk"] } })", source, error)) << error;

    const std::vector<AI::StateTreeCompileError> refusals = registry.CheckStateTreeNames(source);
    ASSERT_EQ(refusals.size(), 1u);
    EXPECT_NE(refusals[0].Message.find("Register 'Walk' in your game's AI module."), std::string::npos)
        << refusals[0].Message;
}

// A state name that more than one state uses is refused once, on the second state with the name, and the
// program does not load.
TEST(AiRegistryTests, CheckStateTreeNamesRefusesAStateNameThatMoreThanOneStateUses)
{
    AIECS::AiRegistry registry;
    registry.RegisterTask({nullptr, &TickIdle, nullptr, "Idle"});
    AIECS::AiStateTreeData data;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiStateTree(R"({ "root": { "id": 1, "children": [
      { "id": 2, "name": "Patrol", "tasks": ["Idle"] }, { "id": 3, "name": "Patrol", "tasks": ["Idle"] },
      { "id": 4, "name": "Patrol", "tasks": ["Idle"] } ] } })",
                                        data, error))
        << error;

    const std::vector<AI::StateTreeCompileError> refusals = registry.CheckStateTreeNames(data.Source);
    ASSERT_EQ(refusals.size(), 1u);
    EXPECT_EQ(refusals[0].StateId, 3u);
    EXPECT_EQ(refusals[0].Item, AI::StateTreeCompileItem::State);
    EXPECT_EQ(refusals[0].Message, "3 states are named Patrol. Give each state its own name.");
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        EXPECT_EQ(registry.CompileStateTree(data), Components::AiAgent::kNoProgram);
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "3 states are named Patrol"), 1u);
}

// Each register, purge and retire call changes the version, so a name check can run again.
TEST(AiRegistryTests, TheVersionChangesAtEachRegisterPurgeAndRetire)
{
    AIECS::AiRegistry registry;
    uint64 version = registry.GetVersion();
    registry.RegisterTask({nullptr, &TickIdle, nullptr, "Idle"});
    EXPECT_NE(registry.GetVersion(), version);
    version = registry.GetVersion();
    registry.RegisterEventType("Alarm");
    EXPECT_NE(registry.GetVersion(), version);
    version = registry.GetVersion();
    registry.PurgeModuleRegistrations("SomeModule", 1);
    EXPECT_NE(registry.GetVersion(), version);
    version = registry.GetVersion();
    registry.RetireSupersededModuleRegistrations("SomeModule", 2);
    EXPECT_NE(registry.GetVersion(), version);
}

// A condition that its module removed is left out of the names that a file can use.
TEST(AiRegistryTests, GetConditionNamesLeavesOutAConditionThatItsModuleRemoved)
{
    AIECS::AiRegistry registry;
    registry.RegisterCondition(&TestNothing, "Rested");
    ECS::SetActiveRegistrationModule("ConditionModule", 1);
    registry.RegisterCondition(&TestNothing, "SeesEnemy");
    ECS::ClearActiveRegistrationModule();
    ASSERT_EQ(registry.GetConditionNames(), (std::vector<std::string>{"Rested", "SeesEnemy"}));

    registry.PurgeModuleRegistrations("ConditionModule", 1);
    EXPECT_EQ(registry.GetConditionNames(), std::vector<std::string>{"Rested"});
}

// A reload retires an event type that the newer code dropped, and a program that names it stops running; the
// newer code keeps the id of a name it keeps.
TEST(AiRegistryTests, AReloadRetiresAnEventTypeThatTheNewerCodeDropped)
{
    AIECS::AiRegistry registry;
    registry.RegisterTask({nullptr, &TickIdle, nullptr, "Idle"});
    ECS::SetActiveRegistrationModule("EventModule", 1);
    const uint16 alarm = registry.RegisterEventType("Alarm");
    registry.RegisterEventType("AllClear");
    ECS::SetActiveRegistrationModule("EventModule", 2);
    const uint16 reloadedAlarm = registry.RegisterEventType("Alarm");
    ECS::ClearActiveRegistrationModule();
    AIECS::AiStateTreeData data;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiStateTree(R"({ "root": { "id": 1, "children": [
      { "id": 2, "tasks": ["Idle"], "transitions": [ { "to": 2, "on": "event", "event": "AllClear" } ] } ] } })",
                                        data, error))
        << error;
    const uint16 program = registry.CompileStateTree(data);
    ASSERT_NE(program, Components::AiAgent::kNoProgram);

    registry.RetireSupersededModuleRegistrations("EventModule", 2);

    EXPECT_EQ(reloadedAlarm, alarm);
    EXPECT_EQ(registry.GetEventTypeNames(), std::vector<std::string>{"Alarm"});
    EXPECT_FALSE(registry.IsProgramRunnable(program));
}

// A reload logs one warning that names each input, condition, event and task that it removed, and a reload that
// removes no name logs none.
TEST(AiRegistryTests, AReloadLogsTheNamesThatItRemoved)
{
    AIECS::AiRegistry registry;
    ECS::SetActiveRegistrationModule("GuardModule", 1);
    registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::CallingThread, "Threat");
    registry.RegisterCondition(&TestNothing, "SeesEnemy");
    registry.RegisterEventType("Alarm");
    registry.RegisterEventType("AllClear");
    registry.RegisterTask({nullptr, &TickIdle, nullptr, "Walk"});
    ECS::SetActiveRegistrationModule("GuardModule", 2);
    registry.RegisterEventType("AllClear");
    ECS::ClearActiveRegistrationModule();

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        registry.RetireSupersededModuleRegistrations("GuardModule", 2);
        registry.RetireSupersededModuleRegistrations("GuardModule", 2);
        Logger::Log::Flush();
    }
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("the reload of module 'GuardModule' removed the names that its new code does not "
                            "register: input 'Threat', condition 'SeesEnemy', event 'Alarm', task 'Walk'."),
              std::string::npos)
        << lines[0];
}

// RegisterEventType refuses an empty name, a name that another module owns, and a removed name outside a module
// load, each with an error that states the fix; the removed name stays removed.
TEST(AiRegistryTests, RegisterEventTypeRefusesEachBadEntry)
{
    const BadEventType kBadEventTypes[] = {
        {"no name", "", "", "Pass the name"},
        {"name of another module", "OtherModule", "Alarm", "FindEventType"},
        {"removed name outside a module load", "", "AllClear", "GE_REGISTER_AI_EVENT"},
    };
    AIECS::AiRegistry registry;
    ECS::SetActiveRegistrationModule("EventModule", 1);
    registry.RegisterEventType("Alarm");
    registry.RegisterEventType("AllClear");
    ECS::SetActiveRegistrationModule("EventModule", 2);
    registry.RegisterEventType("Alarm");
    ECS::ClearActiveRegistrationModule();
    registry.RetireSupersededModuleRegistrations("EventModule", 2);
    for (const BadEventType& bad : kBadEventTypes)
    {
        SCOPED_TRACE(bad.Name);
        std::vector<std::string> lines;
        uint16 id = 0;
        {
            TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
            ECS::SetActiveRegistrationModule(bad.ModuleId, 1);
            id = registry.RegisterEventType(bad.EventName);
            ECS::ClearActiveRegistrationModule();
            Logger::Log::Flush();
        }
        EXPECT_EQ(id, AIECS::AiRegistry::kNoId);
        EXPECT_EQ(TestLog::CountLinesContaining(lines, "RegisterEventType"), 1u);
        EXPECT_EQ(TestLog::CountLinesContaining(lines, bad.Fix), 1u);
    }
    EXPECT_EQ(registry.GetEventTypeNames(), std::vector<std::string>{"Alarm"});
}

// FindEventType returns the id that the registration returned, and kNoId for a name that no loaded code registers.
TEST(AiRegistryTests, FindEventTypeReturnsTheIdOfAnEventThatLoadedCodeRegisters)
{
    AIECS::AiRegistry registry;
    const uint16 alarm = registry.RegisterEventType("Alarm");
    ECS::SetActiveRegistrationModule("EventModule", 1);
    const uint16 allClear = registry.RegisterEventType("AllClear");
    ECS::ClearActiveRegistrationModule();
    ASSERT_EQ(registry.FindEventType("AllClear"), allClear);

    registry.RetireSupersededModuleRegistrations("EventModule", 2);

    EXPECT_EQ(registry.FindEventType("AllClear"), AIECS::AiRegistry::kNoId);
    EXPECT_EQ(registry.FindEventType("Retreat"), AIECS::AiRegistry::kNoId);
    EXPECT_EQ(registry.FindEventType("Alarm"), alarm);
}

// A purge of an aborted load removes the event types that the load added, and an event type that it replaced
// stays registered with its id.
TEST(AiRegistryTests, APurgeUndoesTheEventTypesOfTheAbortedLoad)
{
    AIECS::AiRegistry registry;
    ECS::SetActiveRegistrationModule("EventModule", 1);
    const uint16 alarm = registry.RegisterEventType("Alarm");
    ECS::SetActiveRegistrationModule("EventModule", 2);
    registry.RegisterEventType("Alarm");
    registry.RegisterEventType("Retreat");
    ECS::ClearActiveRegistrationModule();

    registry.PurgeModuleRegistrations("EventModule", 2);

    EXPECT_EQ(registry.GetEventTypeNames(), std::vector<std::string>{"Alarm"});
    EXPECT_EQ(registry.FindEventType("Alarm"), alarm);
}

// An input with no name, or that its module removed, is left out of the inputs that a file can use.
TEST(AiRegistryTests, GetNamedInputsLeavesOutAnUnnamedInputAndAnInputThatItsModuleRemoved)
{
    AIECS::AiRegistry registry;
    registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::CallingThread, "Soldiers");
    registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::CallingThread);
    ECS::SetActiveRegistrationModule("InputModule", 1);
    registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::CallingThread, "Distance");
    ECS::ClearActiveRegistrationModule();
    ASSERT_EQ(registry.GetInputNames(), (std::vector<std::string>{"Soldiers", "Distance"}));

    registry.PurgeModuleRegistrations("InputModule", 1);
    EXPECT_EQ(registry.GetInputNames(), std::vector<std::string>{"Soldiers"});
}

// Each input comes back with the caption and the unit of its registration.
TEST(AiRegistryTests, GetNamedInputsReturnsTheCaptionAndTheUnitOfEachInput)
{
    AIECS::AiRegistry registry;
    registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::CallingThread, "Distance", "Distance to target", "m");
    registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::JobThreads, "Morale", "Morale", "%");

    const std::vector<AIECS::AiRegistry::NamedInput> inputs = registry.GetNamedInputs();

    ASSERT_EQ(inputs.size(), 2u);
    EXPECT_EQ(inputs[0].Caption, "Distance to target");
    EXPECT_EQ(inputs[0].Unit, "m");
    EXPECT_EQ(inputs[1].Caption, "Morale");
    EXPECT_EQ(inputs[1].Unit, "%");
}

// An input with an empty caption and an empty unit registers and is listed under its name.
TEST(AiRegistryTests, AnInputWithNoCaptionAndNoUnitIsListedUnderItsName)
{
    AIECS::AiRegistry registry;

    const uint16 id = registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::CallingThread, "Threat", "", "");

    EXPECT_NE(id, AIECS::AiRegistry::kNoId);
    const std::vector<AIECS::AiRegistry::NamedInput> inputs = registry.GetNamedInputs();
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].Name, "Threat");
    EXPECT_TRUE(inputs[0].Caption.empty());
    EXPECT_TRUE(inputs[0].Unit.empty());
}

// A program that names an input or a condition that its module removed is refused, as the name getters leave
// the name out.
TEST(AiRegistryTests, CheckStateTreeNamesRefusesAnInputAndAConditionThatTheirModuleRemoved)
{
    AIECS::AiRegistry registry;
    registry.RegisterTask({nullptr, &TickIdle, nullptr, "Idle"});
    ECS::SetActiveRegistrationModule("NameModule", 1);
    registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::CallingThread, "Distance");
    registry.RegisterCondition(&TestNothing, "Rested");
    ECS::ClearActiveRegistrationModule();
    AIECS::AiStateTreeSource source;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiStateTreeSource(R"({ "root": { "id": 1, "policy": "utility", "children": [
      { "id": 2, "tasks": ["Idle"],
        "option": { "considerations": [ { "input": "Distance", "curve": { "preset": "linear" } } ] },
        "transitions": [ { "to": 2, "on": "condition", "condition": "Rested" } ] } ] } })",
                                              source, error))
        << error;
    ASSERT_TRUE(registry.CheckStateTreeNames(source).empty());

    registry.PurgeModuleRegistrations("NameModule", 1);
    std::string messages;
    for (const AI::StateTreeCompileError& refusal : registry.CheckStateTreeNames(source))
        messages += refusal.Message + "\n";
    EXPECT_NE(messages.find("the input 'Distance'"), std::string::npos) << messages;
    EXPECT_NE(messages.find("the condition 'Rested'"), std::string::npos) << messages;
}

// The utility ids stop below kStateTreeProgramBit, and each table finds only its own ids.
TEST(AiRegistryTests, TheKindBitSeparatesUtilityAndStateTreePrograms)
{
    AIECS::AiRegistry registry;
    const uint16 input = registry.RegisterInput(&ReadZero, AIECS::AiInputThreading::CallingThread);
    const uint16 task = registry.RegisterTask({nullptr, &TickIdle, nullptr, "Idle"});
    AIECS::AiStateTreeData data;
    std::string error;
    ASSERT_TRUE(AIECS::ParseAiStateTree(R"({ "root": { "id": 1, "tasks": ["Idle"] } })", data, error)) << error;
    const uint16 stateTree = registry.CompileStateTree(data);
    for (uint32 index = 0; index < Components::AiAgent::kStateTreeProgramBit; ++index)
        ASSERT_EQ(registry.AddProgram(MakeValidProgram(input, task)), index);

    std::vector<std::string> lines;
    uint16 refused = 0;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        refused = registry.AddProgram(MakeValidProgram(input, task));
        Logger::Log::Flush();
    }
    EXPECT_EQ(refused, Components::AiAgent::kNoProgram);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "the most it can hold"), 1u);
    EXPECT_EQ(stateTree, Components::AiAgent::kStateTreeProgramBit);
    EXPECT_EQ(registry.FindProgram(stateTree), nullptr);
    EXPECT_NE(registry.FindStateTree(stateTree), nullptr);
    EXPECT_EQ(registry.FindStateTree(0), nullptr);
}
