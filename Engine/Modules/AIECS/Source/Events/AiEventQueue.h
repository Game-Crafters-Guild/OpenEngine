#pragma once

#include "AIECS/AiEvent.h"
#include "Types/Types.h"

#include <span>
#include <utility>
#include <vector>

namespace GameEngine::AIECS
{

/// The AI events of one world. Only the main thread changes it: game code and AiApplySystem post, AiTickSystem
/// delivers, and AiDecideSystem sets the keep ticks. AiDecideSystem reads the deliveries while no other system
/// runs. The world owns it: it stays until the world is gone and a queue is made for another world.
class AiEventQueue
{
public:
    AiEventQueue() = default;
    AiEventQueue(const AiEventQueue&) = delete;
    AiEventQueue& operator=(const AiEventQueue&) = delete;

    /// The queue of the world, or null when no AI tick ran and no event was posted in the world.
    static AiEventQueue* FindForWorld(uint64 worldId);
    /// The queue of the world. When it is missing, frees the queues of the worlds that are gone and makes it.
    static AiEventQueue& FindOrMakeForWorld(uint64 worldId);

    /// The events of one delivery, taken from the posted events, with an index by target.
    struct Delivery
    {
        /// Ordered by Tick, then by Source, then in post order.
        std::vector<AiEvent> Events;
        /// The index of each event in Events, ordered by target, then by index.
        std::vector<std::pair<ECS::EntityHandle, uint32>> ByTarget;
    };

    /// Adds an event for the next delivery after its tick.
    void Post(const AiEvent& event) { m_Pending.push_back(event); }
    /// Sets the AI ticks that a delivered event stays after its tick: the largest cadence of the world's agents,
    /// so each addressed agent steps once while the event stays.
    void SetKeepTicks(uint32 ticks) { m_KeepTicks = ticks; }
    /// Drops the deliveries whose events are all more than the keep ticks older than `tick`, then adds a delivery
    /// of the posted events whose Tick is before `tick`. The other posted events wait.
    void Deliver(uint32 tick);
    /// The deliveries that stay, oldest first.
    std::span<const Delivery> GetDeliveries() const { return m_Deliveries; }

private:
    std::vector<AiEvent> m_Pending;
    std::vector<Delivery> m_Deliveries;
    // Dropped deliveries, kept for their memory.
    std::vector<Delivery> m_Spare;
    uint32 m_KeepTicks = 1;
};

} // namespace GameEngine::AIECS
