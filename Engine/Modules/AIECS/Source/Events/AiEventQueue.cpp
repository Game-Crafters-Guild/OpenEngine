#include "Events/AiEventQueue.h"

#include "AIECS/Components/AiClock.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Types/FlatMap.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>

namespace GameEngine::AIECS
{

namespace
{

// The queues of the worlds by world id. A world id is never used again, so a key names one world. The lock
// covers only the map. The main thread takes it once per AI system update and once per PostAiEvent; no job takes it.
struct WorldQueues
{
    std::mutex Mutex;
    FlatMap<uint64, std::unique_ptr<AiEventQueue>> Queues;
};

WorldQueues& GetWorldQueues()
{
    static WorldQueues queues;
    return queues;
}

bool DeliversFirst(const AiEvent& left, const AiEvent& right)
{
    return left.Tick != right.Tick ? left.Tick < right.Tick : left.Source < right.Source;
}

} // namespace

AiEventQueue* AiEventQueue::FindForWorld(uint64 worldId)
{
    WorldQueues& worlds = GetWorldQueues();
    std::lock_guard lock(worlds.Mutex);
    std::unique_ptr<AiEventQueue>* found = worlds.Queues.Find(worldId);
    return found != nullptr ? found->get() : nullptr;
}

AiEventQueue& AiEventQueue::FindOrMakeForWorld(uint64 worldId)
{
    WorldQueues& worlds = GetWorldQueues();
    std::lock_guard lock(worlds.Mutex);
    if (std::unique_ptr<AiEventQueue>* found = worlds.Queues.Find(worldId))
        return **found;
    worlds.Queues.EraseIf([](uint64 id, const std::unique_ptr<AiEventQueue>&)
                          { return !ECS::World::FindLiveEntityHandleEpoch(id).has_value(); });
    return *worlds.Queues.InsertOrAssign(worldId, std::make_unique<AiEventQueue>());
}

void AiEventQueue::Deliver(uint32 tick)
{
    size_t dropped = 0;
    while (dropped < m_Deliveries.size() && m_Deliveries[dropped].Events.back().Tick + m_KeepTicks < tick)
        ++dropped;
    for (size_t index = 0; index < dropped; ++index)
        m_Spare.push_back(std::move(m_Deliveries[index]));
    m_Deliveries.erase(m_Deliveries.begin(), m_Deliveries.begin() + static_cast<std::ptrdiff_t>(dropped));

    const auto waiting = std::stable_partition(m_Pending.begin(), m_Pending.end(),
                                               [tick](const AiEvent& event) { return event.Tick < tick; });
    if (waiting == m_Pending.begin())
        return;
    if (m_Spare.empty())
        m_Spare.emplace_back();
    Delivery& delivery = m_Deliveries.emplace_back(std::move(m_Spare.back()));
    m_Spare.pop_back();
    delivery.Events.assign(m_Pending.begin(), waiting);
    m_Pending.erase(m_Pending.begin(), waiting);
    std::stable_sort(delivery.Events.begin(), delivery.Events.end(), &DeliversFirst);
    delivery.ByTarget.clear();
    for (size_t index = 0; index < delivery.Events.size(); ++index)
        delivery.ByTarget.emplace_back(delivery.Events[index].Target, static_cast<uint32>(index));
    std::sort(delivery.ByTarget.begin(), delivery.ByTarget.end());
}

bool PostAiEvent(ECS::World& world, AiEvent event)
{
    const Components::AiClock* clock = world.GetSingleton<Components::AiClock>();
    if (clock == nullptr)
    {
        Logger::Log::Error("PostAiEvent: the world runs no AI systems, so the event of type {} is dropped. Add them "
                           "with AiRegistry::AddSystemsToSchedule and post after the first AI step.",
                           event.Type);
        return false;
    }
    event.Tick = clock->Tick;
    AiEventQueue::FindOrMakeForWorld(world.GetWorldId()).Post(event);
    return true;
}

} // namespace GameEngine::AIECS
