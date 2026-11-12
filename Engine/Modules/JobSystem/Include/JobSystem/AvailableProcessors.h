#pragma once

#include "JobSystem/Types.h"

#include <span>

namespace JobSystem {

/**
 * @brief The number of processors this process may run on, and what sets that number.
 *        A pool that is not given a worker count starts one worker per available processor.
 */
struct AvailableProcessors
{
    /** @brief What sets Count. */
    enum class Limit : uint8
    {
        AffinityMask, ///< The process affinity mask allows fewer processors than the machine has.
        Machine,      ///< The machine: no mask, or a mask that allows every processor.
    };

    uint32 Count = 1;
    Limit LimitedBy = Limit::Machine;

    /**
     * @brief The counting rule over the platform's mask (Platform::GetAllowedProcessorMask) and the
     *        machine (hardware_concurrency). An empty mask gives the machine.
     */
    static AvailableProcessors Query();

    /**
     * @brief The counting rule: the bits set in affinityMask (processor 0 is bit 0 of word 0), never more
     *        than machineProcessorCount (hardware_concurrency, 0 when not known). An empty mask, or a mask
     *        that allows every processor, gives the machine.
     */
    static AvailableProcessors FromAffinityMask(std::span<const uint64> affinityMask, uint32 machineProcessorCount);
};

} // namespace JobSystem
