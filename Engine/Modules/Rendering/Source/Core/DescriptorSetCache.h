#pragma once

#include "Rendering/Core/Device.h"
#include "Types/FlatMap.h"

#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// Descriptor sets keyed by their content: a layout and every write, in order. IDevice owns one,
// builds each miss through the backend and destroys the sets that Age and TakeAll hand back.
// No backend call runs under the mutex, so it is a leaf lock below every backend lock.
class DescriptorSetCache
{
  public:
    // Frames a set stays alive after its last use: the longest per-frame ring a caller binds
    // (5 slots) plus the most frames the GPU can still read the set.
    static constexpr uint64_t kFramesKeptAfterLastUse = 5u + IDevice::kMaxSupportedFramesInFlight;

    static uint64_t HashContent(const DescriptorSetLayoutDesc& layout, std::span<const DescriptorWrite> writes);

    // The cached set that holds this content, marked as used in this frame. Invalid on a miss.
    DescriptorSetHandle Find(uint64_t key, const DescriptorSetLayoutDesc& layout,
                             std::span<const DescriptorWrite> writes);
    // Keep `set` for this content and return it, or return the set that another thread kept first.
    DescriptorSetHandle Insert(uint64_t key, const DescriptorSetLayoutDesc& layout,
                               std::span<const DescriptorWrite> writes, DescriptorSetHandle set);
    // Advance one frame. Move out the sets not used for more than kFramesKeptAfterLastUse frames.
    void Age(std::vector<DescriptorSetHandle>& expired);
    // Stop returning the sets that bind this resource. Age destroys them on the same rule.
    void EvictUsing(BufferHandle buffer);
    void EvictUsing(TextureHandle texture);
    void EvictUsing(TextureViewHandle view);
    void EvictUsing(SamplerHandle sampler);
    // Move out every set, cached or evicted, for the device teardown.
    void TakeAll(std::vector<DescriptorSetHandle>& sets);

  private:
    struct Entry
    {
        DescriptorSetLayoutDesc Layout;
        std::vector<DescriptorWrite> Writes;
        DescriptorSetHandle Set{};
        uint64_t LastUsedFrame = 0;
    };
    struct EvictedSet
    {
        DescriptorSetHandle Set{};
        uint64_t LastUsedFrame = 0;
    };

    template <typename BindsResource>
    void EvictWhere(BindsResource bindsResource);

    std::mutex m_Mutex;
    FlatMap<uint64_t, Entry> m_Cached;
    std::vector<EvictedSet> m_Evicted;
    uint64_t m_Frame = 0;
};

} // namespace Rendering
} // namespace GameEngine
