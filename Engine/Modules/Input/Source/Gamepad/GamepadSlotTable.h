#pragma once

#include "Input/GamepadCodes.h"

namespace GameEngine::Input
{

/**
 * @brief The frame thread's map from GamepadFrame slot to device; only that thread writes it.
 * A device keeps its slot while it stays in the snapshot; a new device takes the lowest free slot,
 * and devices past kMaxGamepads wait for a free slot.
 */
template <typename Device>
class GamepadSlotTable
{
  public:
    /// Matches the slots to `snapshot` (`count` distinct devices). Calls `onLeave(device)` for each
    /// device that leaves a slot and `onEnter(device)` for each device that takes one.
    template <typename OnEnter, typename OnLeave>
    void Reconcile(Device* const* snapshot, int count, OnEnter&& onEnter, OnLeave&& onLeave)
    {
        for (Device*& slot : m_Slots)
        {
            if (slot && !Contains(snapshot, count, slot))
            {
                onLeave(slot);
                slot = nullptr;
            }
        }
        for (int i = 0; i < count; ++i)
        {
            Device* device = snapshot[i];
            if (!device || Contains(m_Slots, kMaxGamepads, device))
                continue;
            for (Device*& slot : m_Slots)
            {
                if (!slot)
                {
                    slot = device;
                    onEnter(device);
                    break;
                }
            }
        }
    }

    /// The device in `slot`, or null.
    Device* At(int slot) const { return m_Slots[slot]; }

  private:
    static bool Contains(Device* const* devices, int count, const Device* device)
    {
        for (int i = 0; i < count; ++i)
        {
            if (devices[i] == device)
                return true;
        }
        return false;
    }

    Device* m_Slots[kMaxGamepads] = {};
};

} // namespace GameEngine::Input
