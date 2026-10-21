#include "Diagnostics/FramePhaseHistory.h"

#include <algorithm>

namespace GameEngine::Editor
{

FramePhaseHistory::FramePhaseHistory()
    : m_Samples(std::make_unique<Sample[]>(kCapacity))
{
}

FramePhaseHistory::~FramePhaseHistory() = default;

void FramePhaseHistory::Record(const Sample& sample)
{
    m_Samples[m_FramesRecorded % kCapacity] = sample;
    ++m_FramesRecorded;
}

void FramePhaseHistory::CopyNewest(std::vector<Sample>& out, std::size_t maxFrames) const
{
    const std::uint64_t held = std::min<std::uint64_t>(m_FramesRecorded, kCapacity);
    const std::uint64_t count = std::min<std::uint64_t>(held, maxFrames);
    out.resize(static_cast<std::size_t>(count));
    const std::uint64_t first = m_FramesRecorded - count;
    for (std::uint64_t i = 0; i < count; ++i)
        out[static_cast<std::size_t>(i)] = m_Samples[(first + i) % kCapacity];
}

} // namespace GameEngine::Editor
