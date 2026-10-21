#include "ECSModules/Rendering/Systems/KeyedWorldSubmissions.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{

void KeyedWorldSubmissions::Clear()
{
    m_Records.clear();
    std::fill(m_RecordCount.begin(), m_RecordCount.end(), 0u);
}

void KeyedWorldSubmissions::Append(const WorldSubmissionRecord& record)
{
    const uint32 instance = record.instanceIndex;
    if (instance >= m_RecordCount.size())
    {
        m_RecordCount.resize(static_cast<std::size_t>(instance) + 1u, 0u);
        m_Erasing.resize(m_RecordCount.size(), 0u);
    }
    m_Records.push_back(record);
    ++m_RecordCount[instance];
}

void KeyedWorldSubmissions::Erase(std::span<const uint32> instanceIndices)
{
    bool anyHeld = false;
    for (const uint32 instance : instanceIndices)
    {
        if (instance < m_RecordCount.size() && m_RecordCount[instance] != 0u)
        {
            m_Erasing[instance] = 1u;
            anyHeld = true;
        }
    }
    if (!anyHeld)
        return;

    // std::remove_if is stable: the survivors keep their order.
    const auto kept = std::remove_if(m_Records.begin(), m_Records.end(),
                                     [this](const WorldSubmissionRecord& record)
                                     { return m_Erasing[record.instanceIndex] != 0u; });
    m_Records.erase(kept, m_Records.end());
    for (const uint32 instance : instanceIndices)
    {
        if (instance < m_Erasing.size() && m_Erasing[instance] != 0u)
        {
            m_Erasing[instance] = 0u;
            m_RecordCount[instance] = 0u;
        }
    }
}

} // namespace GameEngine::Engine::Renderer
