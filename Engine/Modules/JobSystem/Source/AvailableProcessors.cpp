#include "JobSystem/AvailableProcessors.h"

#include "Platform/Thread.h"

#include <algorithm>
#include <bit>
#include <thread>

namespace JobSystem {

AvailableProcessors AvailableProcessors::Query()
{
    return FromAffinityMask(GameEngine::Platform::GetAllowedProcessorMask(), std::thread::hardware_concurrency());
}

AvailableProcessors AvailableProcessors::FromAffinityMask(std::span<const uint64> affinityMask,
                                                          uint32 machineProcessorCount)
{
    uint32 maskCount = 0;
    for (const uint64 word : affinityMask)
        maskCount += static_cast<uint32>(std::popcount(word));

    // An empty mask means the read failed. A mask with as many bits as the machine, or more, limits nothing.
    const bool maskLimits = maskCount > 0 && (machineProcessorCount == 0 || maskCount < machineProcessorCount);
    if (maskLimits)
        return {maskCount, Limit::AffinityMask};
    return {std::max<uint32>(1, machineProcessorCount), Limit::Machine};
}

} // namespace JobSystem
