#pragma once

#include "Rendering/Core/RenderGraph/RGTypes.h"
#include "Types/FlatMap.h"

#include <compare>
#include <cstdint>

namespace GameEngine::Rendering::RenderGraph
{

/// Where each tracked physical resource was last read and last written, per physical queue, as the
/// timeline value of the submission that touched it. A frame's submission plan reads it to order
/// its accesses after the conflicting accesses earlier frames made on other queues, then records
/// its own: frames overlap in flight, and nothing else orders a queue's work against another
/// queue's work from an earlier frame.
class RGCrossFrameAccesses
{
  public:
    /// A physical resource: its raw device handle and kind (handle bits of different kinds can match).
    struct Key
    {
        uint64_t Physical = 0;
        RGResourceKind Kind = RGResourceKind::Texture;
        auto operator<=>(const Key&) const = default;
    };

    /// The timeline values an access on `physicalQueue` must wait for, per physical queue (0 = none).
    /// An access that only reads waits for the other queues' last writes; one that writes (a layout
    /// transition included) waits for their last reads and writes. Never names `physicalQueue`, and
    /// never a value the queue has already signaled (SetCompleted).
    void CollectWaits(const Key& key, uint8_t physicalQueue, bool writes,
                      uint64_t (&outValues)[kQueueCount]) const;

    /// Records an access of this frame's plan, made by the submission that signals `value` on
    /// `physicalQueue`. Call after every wait of the frame is derived.
    void Record(const Key& key, uint8_t physicalQueue, bool writes, uint64_t value);

    /// The value each physical queue's timeline has signaled, read before a plan is built. A wait on
    /// a signaled value orders nothing: a resource kept as one copy per frame in flight is last
    /// touched by work that has already finished, so its accesses add no wait.
    void SetCompleted(const uint64_t (&completedValues)[kQueueCount]);

    /// Closes a frame's plan and forgets resources no plan has touched for kForgetAfterPlans plans.
    void EndPlan();

    /// Forgets every record and completed value: the timelines they belong to died with the device.
    void Clear();

    /// Plans a resource may go untouched before its record is dropped. A value that old has long
    /// been signaled, so dropping it loses no ordering; it keeps the map from growing with every
    /// physical a long session imports once.
    static constexpr uint64_t kForgetAfterPlans = 1024;

  private:
    struct Entry
    {
        uint64_t LastAccess[kQueueCount] = {0, 0, 0};
        uint64_t LastWrite[kQueueCount] = {0, 0, 0};
        uint64_t LastPlan = 0;
    };

    FlatMap<Key, Entry> m_Entries;
    uint64_t m_Completed[kQueueCount] = {0, 0, 0};
    uint64_t m_Plan = 0;
};

} // namespace GameEngine::Rendering::RenderGraph
