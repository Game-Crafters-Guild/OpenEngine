#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/AiTestWorld.h"
#include "AIECS/Components/AiAgent.h"
#include "ECS/ModuleRegistration.h"
#include "ECS/World.h"
#include "EngineLogCapture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr const char* kModule = "FakeAiModule";

uint32 g_FirstTicks = 0;
uint32 g_SecondTicks = 0;
uint32 g_InputCalls = 0;

AIECS::AiTaskStatus TickFirst(AIECS::AiTaskContext&)
{
    ++g_FirstTicks;
    return AIECS::AiTaskStatus::Running;
}

AIECS::AiTaskStatus TickSecond(AIECS::AiTaskContext&)
{
    ++g_SecondTicks;
    return AIECS::AiTaskStatus::Running;
}

void ReadOne(const ECS::World&, std::span<const ECS::EntityHandle>, std::span<float32> out)
{
    ++g_InputCalls;
    std::fill(out.begin(), out.end(), 1.0f);
}

void ApplyNothing(ECS::World&, ECS::EntityHandle, const AIECS::AiCommand&)
{
}

void SenseNothing(ECS::World&, ECS::EntityHandle, AI::RecordStore&, uint32)
{
}

// Registers the task "Work" as generation `generation` of the fake module.
uint16 RegisterWork(AIECS::AiRegistry& registry, uint64 generation, AIECS::AiTaskStatus (*tick)(AIECS::AiTaskContext&))
{
    ECS::SetActiveRegistrationModule(kModule, generation);
    const uint16 id = registry.RegisterTask({nullptr, tick, nullptr, "Work"});
    ECS::ClearActiveRegistrationModule();
    return id;
}

// A program with one option that runs `task`, scored by `input`.
uint16 MakeProgram(AIECS::AiRegistry& registry, uint16 task, uint16 input)
{
    AIECS::AiProgram program;
    program.ColumnInputs = {input};
    program.Options.Options.push_back({.Action = task});
    return registry.AddProgram(std::move(program));
}

ECS::EntityHandle CreateAgent(ECS::World& world, uint16 program)
{
    Components::AiAgent agent;
    agent.Program = program;
    agent.Cadence = 1;
    return world.Create(agent).GetHandle();
}

void ResetCounters()
{
    g_FirstTicks = 0;
    g_SecondTicks = 0;
    g_InputCalls = 0;
}

} // namespace

// A load that aborts removes the task it added; the agent that runs it is skipped with one error and
// nothing calls into the module's code.
TEST(AiModuleRegistrationTests, AbortedLoadRemovesItsTaskAndTheAgentIsSkippedWithOneError)
{
    ResetCounters();
    AIECS::AiRegistry registry;
    const uint16 input = registry.RegisterInput(&ReadOne, AIECS::AiInputThreading::CallingThread);
    const uint16 program = MakeProgram(registry, RegisterWork(registry, 1, &TickFirst), input);
    AIECS::AiTestWorld test(registry, 1);
    const ECS::EntityHandle agent = CreateAgent(test.GetWorld(), program);

    registry.PurgeModuleRegistrations(kModule, 1);
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
        test.Step(3);
        Logger::Log::Flush();
    }

    EXPECT_EQ(TestLog::CountLinesContaining(lines, "or an input that its module removed"), 1u);
    EXPECT_EQ(g_FirstTicks, 0u);
    EXPECT_EQ(g_InputCalls, 0u);
    EXPECT_FALSE(test.GetWorld().GetComponent<Components::AiAgent>(agent)->Scheduled);
}

// A newer generation that registers the same name takes the entry in place, so a program compiled
// before the reload runs the new code with no new id.
TEST(AiModuleRegistrationTests, NewerGenerationReplacesTheTaskAndKeepsItsId)
{
    ResetCounters();
    AIECS::AiRegistry registry;
    const uint16 input = registry.RegisterInput(&ReadOne, AIECS::AiInputThreading::CallingThread);
    const uint16 first = RegisterWork(registry, 1, &TickFirst);
    const uint16 program = MakeProgram(registry, first, input);
    AIECS::AiTestWorld test(registry, 1);
    CreateAgent(test.GetWorld(), program);

    EXPECT_EQ(RegisterWork(registry, 2, &TickSecond), first);
    registry.RetireSupersededModuleRegistrations(kModule, 2);
    test.Step(2);

    EXPECT_EQ(g_FirstTicks, 0u);
    EXPECT_EQ(g_SecondTicks, 2u);
}

// A load that aborts after it replaced a task gives the task of the older generation back.
TEST(AiModuleRegistrationTests, AbortedLoadRestoresTheTaskItReplaced)
{
    ResetCounters();
    AIECS::AiRegistry registry;
    const uint16 input = registry.RegisterInput(&ReadOne, AIECS::AiInputThreading::CallingThread);
    const uint16 first = RegisterWork(registry, 1, &TickFirst);
    const uint16 program = MakeProgram(registry, first, input);
    AIECS::AiTestWorld test(registry, 1);
    CreateAgent(test.GetWorld(), program);

    RegisterWork(registry, 2, &TickSecond);
    registry.PurgeModuleRegistrations(kModule, 2);
    test.Step(2);

    EXPECT_EQ(registry.GetTask(first).Tick, &TickFirst);
    EXPECT_EQ(g_FirstTicks, 2u);
    EXPECT_EQ(g_SecondTicks, 0u);
}

// A reload removes the input, sensor and command that only the older generation registered, and
// keeps the engine's own entries.
TEST(AiModuleRegistrationTests, ReloadRemovesWhatTheNewerGenerationDidNotRegister)
{
    AIECS::AiRegistry registry;
    const uint16 engineCommand = registry.RegisterCommand(&ApplyNothing);
    ECS::SetActiveRegistrationModule(kModule, 1);
    const uint16 input = registry.RegisterInput(&ReadOne, AIECS::AiInputThreading::CallingThread, "Distance");
    const uint16 command = registry.RegisterCommand(&ApplyNothing);
    registry.RegisterSensor(&SenseNothing);
    ECS::ClearActiveRegistrationModule();

    registry.RetireSupersededModuleRegistrations(kModule, 2);

    EXPECT_EQ(registry.GetInput(input), nullptr);
    EXPECT_EQ(registry.GetCommand(command), nullptr);
    EXPECT_EQ(registry.GetSensors()[0], nullptr);
    EXPECT_EQ(registry.GetCommand(engineCommand), &ApplyNothing);
}
