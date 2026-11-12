#include "TerrainECS/TerrainPageTableRing.h"
#include "TerrainECS/PackedPageTable.h"
#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

namespace GameEngine::TerrainECS
{
namespace
{
// A slot grows in whole steps, so a table that grows a little does not reallocate the ring each time.
constexpr uint32 kSlotWordsStep = 4096u;
static_assert(TerrainPageTableRing::kSlots == Rendering::IDevice::kMaxSupportedFramesInFlight);
} // namespace

TerrainPageTableRing::Slot TerrainPageTableRing::Bind(Rendering::IDevice& device, const Key& key,
                                                      const PackedPageTable& height, const PackedPageTable* surface,
                                                      uint32 frameSlot, Rendering::BufferHandle& retired)
{
    retired = {};
    const uint32 slot = frameSlot % kSlots;
    const uint32 heightWords = static_cast<uint32>(height.Words().size());
    const uint32 surfaceWords = surface != nullptr ? static_cast<uint32>(surface->Words().size()) : 0u;
    const uint32 words = heightWords + surfaceWords;
    if (!m_Buffer.IsValid() || words > m_SlotWords)
    {
        const uint32 slotWords = (words + kSlotWordsStep - 1u) / kSlotWordsStep * kSlotWordsStep;
        Rendering::BufferDesc desc{};
        desc.size = static_cast<size_t>(kSlots) * slotWords * sizeof(uint32);
        // TransferSrc: RecordSlotCopy copies a slot out.
        desc.usage = static_cast<uint32>(Rendering::BufferUsage::Storage) |
                     static_cast<uint32>(Rendering::BufferUsage::TransferSrc);
        desc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        desc.persistent = true;
        desc.debugName = "Terrain.PageTableRing";
        const Rendering::BufferHandle next = device.CreateBuffer(desc);
        if (!next.IsValid())
        {
            Logger::Log::Error("Terrain pages: no page table ring of {} KiB; the terrain draws from its textures",
                               desc.size / 1024u);
            return {};
        }
        retired = m_Buffer;
        m_Buffer = next;
        m_SlotWords = slotWords;
        m_Slots.fill(SlotState{});
    }
    SlotState& state = m_Slots[slot];
    // The same tables at the same sizes: each table catches up from the version the slot holds.
    const bool same = state.Written && state.Owner == key && state.HeightWords == heightWords &&
                      state.SurfaceWords == surfaceWords;
    const bool heightChanged = !same || state.HeightVersion != height.Version();
    const bool surfaceChanged = surface != nullptr && (!same || state.SurfaceVersion != surface->Version());
    if (heightChanged || surfaceChanged)
    {
        GE_CPU_PROFILE_SCOPE("Terrain.PageTable.RingWrite");
        const uint64 offset = static_cast<uint64>(slot) * m_SlotWords * sizeof(uint32);
        if (heightChanged)
            WriteTable(device, offset, height, same, state.HeightVersion);
        if (surfaceChanged)
            WriteTable(device, offset + static_cast<uint64>(heightWords) * sizeof(uint32), *surface, same,
                       state.SurfaceVersion);
        state = SlotState{key, height.Version(), surface != nullptr ? surface->Version() : 0u, heightWords,
                          surfaceWords, true};
    }
    Slot out{};
    out.Buffer = m_Buffer;
    out.FirstWord = slot * m_SlotWords;
    out.Offset = static_cast<uint64>(out.FirstWord) * sizeof(uint32);
    out.Bytes = static_cast<uint64>(m_SlotWords) * sizeof(uint32);
    out.SurfaceBase = heightWords;
    return out;
}

void TerrainPageTableRing::WriteTable(Rendering::IDevice& device, uint64 offset, const PackedPageTable& table,
                                      bool holdsOlderVersion, uint64 since)
{
    const std::span<const uint32> words = table.Words();
    if (holdsOlderVersion && table.ChangedSince(since, m_Runs))
    {
        for (const PageWordRange& run : m_Runs)
            device.UpdateBuffer(m_Buffer, offset + static_cast<uint64>(run.First) * sizeof(uint32),
                                run.Count * sizeof(uint32), words.data() + run.First);
        return;
    }
    device.UpdateBuffer(m_Buffer, offset, words.size_bytes(), words.data());
}

void TerrainPageTableRing::Release(Rendering::IDevice* device)
{
    if (device != nullptr && m_Buffer.IsValid())
        device->DestroyBuffer(m_Buffer);
    ReleaseReadback(device);
    m_Buffer = {};
    m_SlotWords = 0;
    m_Slots.fill(SlotState{});
    m_Copy = {};
}

void TerrainPageTableRing::ReleaseReadback(Rendering::IDevice* device)
{
    if (device != nullptr && m_Readback.IsValid())
        device->DestroyBuffer(m_Readback);
    m_Readback = {};
    m_ReadbackWords = 0;
}

bool TerrainPageTableRing::RecordSlotCopy(Rendering::IDevice& device, Rendering::CommandList& cl, uint32 frameSlot)
{
    m_Copy = {};
    if (!m_Buffer.IsValid())
        return false;
    const std::size_t bytes = static_cast<std::size_t>(m_SlotWords) * sizeof(uint32);
    // The readback buffer is sized once for each ring size. No copy reads it here: the last one was read or dropped.
    if (!m_Readback.IsValid() || m_ReadbackWords != m_SlotWords)
    {
        ReleaseReadback(&device);
        m_Readback = device.CreateReadbackBuffer(bytes, "Terrain.PageTableRing.SlotReadback");
        m_ReadbackWords = m_Readback.IsValid() ? m_SlotWords : 0u;
        if (!m_Readback.IsValid())
        {
            Logger::Log::Warning("Terrain pages: no page table readback of {} KiB; the slot report is not taken",
                                 bytes / 1024u);
            return false;
        }
    }
    const uint32 slot = frameSlot % kSlots;
    // The CPU writes the slot before the submit, and no GPU work writes the ring: the source needs no barrier.
    // The destination gets no transfer-to-host barrier: ResourceState has no host-read state.
    cl.CopyBuffer(m_Buffer, m_Readback, bytes, slot * bytes, 0);
    m_Copy.Step = SlotCopy::Stage::Recorded;
    m_Copy.Slot = slot;
    m_Copy.Source = m_Buffer;
    return true;
}

bool TerrainPageTableRing::SealSlotCopy()
{
    const SlotState& state = m_Slots[m_Copy.Slot];
    if (m_Copy.Step != SlotCopy::Stage::Recorded || m_Copy.Source != m_Buffer || !state.Written)
    {
        m_Copy = {};
        return false;
    }
    m_Copy.State = state;
    m_Copy.Step = SlotCopy::Stage::Sealed;
    return true;
}

bool TerrainPageTableRing::ReadSlotCopy(Rendering::IDevice& device, SlotReport& out)
{
    const SlotCopy copy = m_Copy;
    m_Copy = {};
    if (copy.Step != SlotCopy::Stage::Sealed || !m_Readback.IsValid())
        return false;
    const auto* mapped = static_cast<const uint32*>(device.MapBuffer(m_Readback));
    if (mapped == nullptr)
        return false;
    const std::span<const uint32> words(mapped, copy.State.HeightWords);
    out = SlotReport{copy.Slot, copy.State.Owner, copy.State.HeightVersion, copy.State.HeightWords,
                     PackedPageTable::HashWords(words)};
    device.UnmapBuffer(m_Readback);
    return true;
}

} // namespace GameEngine::TerrainECS
