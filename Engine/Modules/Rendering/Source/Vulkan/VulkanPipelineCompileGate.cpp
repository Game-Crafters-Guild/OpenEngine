#include "VulkanPipelineCompileGate.h"

#include "Types/Fnv1a.h"

#include <algorithm>

namespace GameEngine::Rendering
{

VulkanPipelineCompileGate::Hold::Hold(VulkanPipelineCompileGate* gate, uint64_t key) noexcept
    : m_Gate(gate), m_Key(key)
{
}

VulkanPipelineCompileGate::Hold::~Hold()
{
    m_Gate->Leave(m_Key);
}

VulkanPipelineCompileGate::Hold VulkanPipelineCompileGate::Enter(uint64_t stageKey)
{
    std::unique_lock lock(m_Mutex);
    m_Left.wait(lock, [this, stageKey]
                { return std::find(m_Running.begin(), m_Running.end(), stageKey) == m_Running.end(); });
    m_Running.push_back(stageKey);
    return Hold(this, stageKey);
}

void VulkanPipelineCompileGate::Leave(uint64_t stageKey)
{
    {
        std::lock_guard lock(m_Mutex);
        m_Running.erase(std::find(m_Running.begin(), m_Running.end(), stageKey));
    }
    m_Left.notify_all();
}

uint64_t VulkanPipelineCompileGate::StageKey(std::span<const std::span<const uint8_t>> stages,
                                             const VkSpecializationInfo* specialization)
{
    uint64_t hash = Hashing::kFnv1a64OffsetBasis;
    for (const std::span<const uint8_t> stage : stages)
    {
        const uint64_t size = stage.size();
        hash = Hashing::Fnv1a64(&size, sizeof(size), hash);
        hash = Hashing::Fnv1a64(stage.data(), stage.size(), hash);
    }
    if (specialization)
    {
        for (uint32_t i = 0; i < specialization->mapEntryCount; ++i)
            hash = Hashing::Fnv1a64(&specialization->pMapEntries[i], sizeof(VkSpecializationMapEntry), hash);
        hash = Hashing::Fnv1a64(specialization->pData, specialization->dataSize, hash);
    }
    return hash;
}

} // namespace GameEngine::Rendering
