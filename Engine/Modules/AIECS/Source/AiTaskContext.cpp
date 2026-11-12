#include "AIECS/AiTaskContext.h"

#include "AIECS/Components/AiGroup.h"
#include "AiDecisionRow.h"
#include "AiGroup/AiGroupStore.h"
#include "ECS/Entity.h"

#include <algorithm>
#include <span>

namespace GameEngine::AIECS
{

AiTaskContext::AiTaskContext(const ECS::World& world, ECS::EntityHandle agent, uint32 tick, std::span<uint32> cells,
                             AiDecisionRow& row, size_t commandCount, const AiGroupStore* groups)
    : m_World(world), m_Groups(groups), m_Row(row), m_Cells(cells), m_Agent(agent), m_CommandCount(commandCount),
      m_Tick(tick)
{
}

const AI::RecordStore* AiTaskContext::FindGroupRecords(ECS::EntityHandle group) const
{
    if (m_Groups == nullptr)
        return nullptr;
    const Components::AiGroup* component = m_World.GetComponent<Components::AiGroup>(group);
    const AiGroupStore::Group* found = component != nullptr ? m_Groups->Find(group, component->Knowledge) : nullptr;
    return found != nullptr ? &found->Records : nullptr;
}

bool AiTaskContext::IssueCommand(const AiCommand& command)
{
    if (command.Type >= m_CommandCount)
    {
        const auto refused = std::span(m_Row.RefusedTypes).first(m_Row.RefusedTypeCount);
        if (std::find(refused.begin(), refused.end(), command.Type) == refused.end() &&
            m_Row.RefusedTypeCount < AiDecisionRow::kMaxRefusedTypes)
            m_Row.RefusedTypes[m_Row.RefusedTypeCount++] = command.Type;
        return false;
    }
    if (m_Row.CommandCount >= AiDecisionRow::kMaxCommands)
        return false;
    m_Row.Commands[m_Row.CommandCount++] = command;
    return true;
}

bool AiTaskContext::PostEvent(AiEvent event)
{
    if (m_Row.EventCount >= AiDecisionRow::kMaxEvents)
        return false;
    event.Source = m_Agent;
    event.Tick = m_Tick;
    m_Row.Events[m_Row.EventCount++] = event;
    return true;
}

} // namespace GameEngine::AIECS
