#include "Assets/TextureCookWorkers.h"

#include "Assets/AssetDecodeGate.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <functional>

namespace GameEngine {

TextureCookWorkers::TextureCookWorkers(JobSystem::WorkStealingThreadPool& pool, std::shared_ptr<AssetDecodeGate> gate)
    : m_Pool(pool), m_Gate(std::move(gate))
{
}

TextureCookWorkers::~TextureCookWorkers() = default;

bool TextureCookWorkers::RunBands(uint32 bandCount, std::function<bool(uint32)> encodeBand,
                                  const std::function<bool()>& stopRequested)
{
    // A helper encodes one band within a gate slot for texture work, then goes
    // back to the end of the Background lane while bands remain; one that finds
    // the slots taken ends, and the calling thread encodes what the helpers leave.
    AssetDecodeGate& gate = *m_Gate;
    JobSystem::ParallelForOptions::UnitAdmission textureSlot;
    textureSlot.TryAcquire = [&gate] { return gate.TryAcquire(AssetDecodeGate::Work::Texture); };
    textureSlot.Release = [&gate] { gate.Release(AssetDecodeGate::Work::Texture); };

    // One helper queued for each free texture slot, read again before each band
    // the calling thread takes, so the level gets the slots other work gives back.
    const std::function<size_t()> freeTextureSlots = [&gate] { return gate.FreeTextureSlots(); };

    JobSystem::ParallelForOptions options;
    options.Helpers = 0;
    options.HelpersToKeepQueued = &freeTextureSlots;
    options.HelperPriority = JobSystem::JobPriority::Background;
    options.Stop = &stopRequested;
    options.Admission = &textureSlot;
    return JobSystem::ParallelFor(
        &m_Pool, bandCount, [&encodeBand](size_t band) { return encodeBand(static_cast<uint32>(band)); }, options);
}

} // namespace GameEngine
