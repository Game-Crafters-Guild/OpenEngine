#include "DescriptorSetCache.h"

#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Types/Fnv1a.h"

#include <algorithm>

namespace GameEngine
{
namespace Rendering
{

namespace
{

// The same fields as HashDescriptorSetLayoutDesc: every binding field except the debug name.
bool SameBinding(const DescriptorBinding& a, const DescriptorBinding& b)
{
    return a.binding == b.binding && a.type == b.type && a.count == b.count && a.shaderStages == b.shaderStages &&
           a.flags == b.flags && a.imageDim == b.imageDim && a.imageArrayed == b.imageArrayed &&
           a.imageIsDepth == b.imageIsDepth && a.imageIsUnsignedInteger == b.imageIsUnsignedInteger &&
           a.storageTexelFormat == b.storageTexelFormat && a.storageReadOnly == b.storageReadOnly &&
           a.imageFilterableFloat == b.imageFilterableFloat && a.imageUsageReflected == b.imageUsageReflected &&
           a.imageMultisample == b.imageMultisample;
}

bool SameLayout(const DescriptorSetLayoutDesc& a, const DescriptorSetLayoutDesc& b)
{
    return std::equal(a.bindings.begin(), a.bindings.end(), b.bindings.begin(), b.bindings.end(), SameBinding);
}

bool SameWrites(std::span<const DescriptorWrite> a, std::span<const DescriptorWrite> b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

} // namespace

uint64_t DescriptorSetCache::HashContent(const DescriptorSetLayoutDesc& layout, std::span<const DescriptorWrite> writes)
{
    using Hashing::Fnv1a64Value;
    uint64_t hash = HashDescriptorSetLayoutDesc(layout);
    hash = Fnv1a64Value(hash, writes.size());
    for (const DescriptorWrite& write : writes)
    {
        hash = Fnv1a64Value(hash, write.Binding);
        hash = Fnv1a64Value(hash, write.ArrayElement);
        hash = Fnv1a64Value(hash, write.Type);
        hash = Fnv1a64Value(hash, write.Buffer.id);
        hash = Fnv1a64Value(hash, write.Offset);
        hash = Fnv1a64Value(hash, write.Range);
        hash = Fnv1a64Value(hash, write.Texture.id);
        hash = Fnv1a64Value(hash, write.View.id);
        hash = Fnv1a64Value(hash, write.Sampler.id);
    }
    return hash;
}

DescriptorSetHandle DescriptorSetCache::Find(uint64_t key, const DescriptorSetLayoutDesc& layout,
                                             std::span<const DescriptorWrite> writes)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    Entry* entry = m_Cached.Find(key);
    if (!entry || !SameLayout(entry->Layout, layout) || !SameWrites(entry->Writes, writes))
        return {};
    entry->LastUsedFrame = m_Frame;
    return entry->Set;
}

DescriptorSetHandle DescriptorSetCache::Insert(uint64_t key, const DescriptorSetLayoutDesc& layout,
                                               std::span<const DescriptorWrite> writes, DescriptorSetHandle set)
{
    Entry fresh{layout, std::vector<DescriptorWrite>(writes.begin(), writes.end()), set, 0};
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (Entry* entry = m_Cached.Find(key))
    {
        if (SameLayout(entry->Layout, layout) && SameWrites(entry->Writes, writes))
        {
            entry->LastUsedFrame = m_Frame;
            return entry->Set;
        }
        // Two contents with one hash: the newer one takes the key.
        m_Evicted.push_back({entry->Set, entry->LastUsedFrame});
    }
    fresh.LastUsedFrame = m_Frame;
    m_Cached.InsertOrAssign(key, std::move(fresh));
    return set;
}

void DescriptorSetCache::Age(std::vector<DescriptorSetHandle>& expired)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    ++m_Frame;
    const uint64_t frame = m_Frame;
    m_Cached.EraseIf(
        [&expired, frame](uint64_t, Entry& entry)
        {
            if (frame - entry.LastUsedFrame <= kFramesKeptAfterLastUse)
                return false;
            expired.push_back(entry.Set);
            return true;
        });
    std::erase_if(m_Evicted,
                  [&expired, frame](const EvictedSet& evicted)
                  {
                      if (frame - evicted.LastUsedFrame <= kFramesKeptAfterLastUse)
                          return false;
                      expired.push_back(evicted.Set);
                      return true;
                  });
}

template <typename BindsResource>
void DescriptorSetCache::EvictWhere(BindsResource bindsResource)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Cached.EraseIf(
        [this, &bindsResource](uint64_t, Entry& entry)
        {
            if (std::none_of(entry.Writes.begin(), entry.Writes.end(), bindsResource))
                return false;
            m_Evicted.push_back({entry.Set, entry.LastUsedFrame});
            return true;
        });
}

void DescriptorSetCache::EvictUsing(BufferHandle buffer)
{
    EvictWhere([buffer](const DescriptorWrite& write) { return write.Buffer == buffer; });
}

void DescriptorSetCache::EvictUsing(TextureHandle texture)
{
    EvictWhere([texture](const DescriptorWrite& write) { return write.Texture == texture; });
}

void DescriptorSetCache::EvictUsing(TextureViewHandle view)
{
    EvictWhere([view](const DescriptorWrite& write) { return write.View == view; });
}

void DescriptorSetCache::EvictUsing(SamplerHandle sampler)
{
    EvictWhere([sampler](const DescriptorWrite& write) { return write.Sampler == sampler; });
}

void DescriptorSetCache::TakeAll(std::vector<DescriptorSetHandle>& sets)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const auto& [key, entry] : m_Cached)
        sets.push_back(entry.Set);
    for (const EvictedSet& evicted : m_Evicted)
        sets.push_back(evicted.Set);
    m_Cached.Clear();
    m_Evicted.clear();
}

} // namespace Rendering
} // namespace GameEngine
