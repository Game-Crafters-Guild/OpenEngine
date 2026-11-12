#include "Systems/AiApplySystem.h"

#include "AIECS/AiRegistry.h"
#include "ECS/Entity.h"
#include "Events/AiEventQueue.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <utility>

namespace GameEngine::AIECS
{

AiApplySystem::AiApplySystem(const AiRegistry& registry, std::shared_ptr<std::vector<AiDecisionRow>> rows)
    : m_Registry(registry), m_Rows(std::move(rows))
{
}

void AiApplySystem::Update(ECS::World& world, float32)
{
    std::vector<AiDecisionRow>& rows = *m_Rows;
    // Stable, so the rows of a reload come before the decision rows of the same agent.
    std::stable_sort(rows.begin(), rows.end(),
                     [](const AiDecisionRow& left, const AiDecisionRow& right) { return left.Agent < right.Agent; });
    AiEventQueue* events = AiEventQueue::FindForWorld(world.GetWorldId());
    for (const AiDecisionRow& row : rows)
    {
        for (uint32 index = 0; index < row.EventCount && events != nullptr; ++index)
            events->Post(row.Events[index]);
        for (uint32 index = 0; index < row.RefusedTypeCount; ++index)
            ReportUnregisteredCommand(row.RefusedTypes[index]);
        // A command may destroy an agent, so each order checks that its agent still lives.
        for (uint32 index = 0; index < row.CommandCount && world.IsValid(row.Agent); ++index)
        {
            const AiCommand& command = row.Commands[index];
            if (const AiCommandFunction apply = m_Registry.GetCommand(command.Type))
                apply(world, row.Agent, command);
            else
                ReportUnregisteredCommand(command.Type);
        }
    }
    rows.clear();
}

void AiApplySystem::ReportUnregisteredCommand(uint16 type)
{
    if (std::find(m_ReportedCommands.begin(), m_ReportedCommands.end(), type) != m_ReportedCommands.end())
        return;
    m_ReportedCommands.push_back(type);
    Logger::Log::Error("AiTaskContext::IssueCommand: command type {} is not registered; {} commands are registered. "
                       "Set AiCommand::Type to an id that AiRegistry::RegisterCommand returned.",
                       type, m_Registry.GetCommandCount());
}

} // namespace GameEngine::AIECS
