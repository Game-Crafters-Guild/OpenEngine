#pragma once

#include "TerrainECS/PageTable.h"
#include "Rendering/Core/Handle.h"
#include "Types/Types.h"

#include <array>
#include <vector>

namespace GameEngine::Rendering
{
class CommandList;
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::TerrainECS
{

class PackedPageTable;

/// One host-visible buffer with a slot per frame in flight. A slot holds the page tables of one terrain:
/// the height table, then the surface table. The terrain draw and the grass placement bind the same slot.
class TerrainPageTableRing
{
public:
    static constexpr uint32 kSlots = 4; // Rendering::IDevice::kMaxSupportedFramesInFlight

    /// Whose tables a slot holds: the generation catches a destroy-and-recreate that reuses the terrain
    /// index and the table versions.
    struct Key
    {
        uint32 Terrain = 0;
        uint32 Generation = 0;
        bool operator==(const Key&) const = default;
    };

    /// Where a slot sits in the ring buffer.
    struct Slot
    {
        Rendering::BufferHandle Buffer;
        uint64 Offset = 0;    // bytes
        uint64 Bytes = 0;     // the slot's size
        uint32 FirstWord = 0; // Offset in words
        uint32 SurfaceBase = 0; // the first word of the surface table, relative to FirstWord
    };

    /// The height table that a written slot holds, read from a GPU copy of the slot.
    struct SlotReport
    {
        uint32 Slot = 0;
        Key Owner;
        uint64 HeightVersion = 0;
        uint32 HeightWords = 0;
        uint64 HeightHash = 0; // PackedPageTable::HashWords of the slot's height words
    };

    /// Brings slot `frameSlot` to `key`'s tables (`surface` null: none): the words changed since the slot's
    /// version, else every word. A larger table grows the ring and puts the old buffer in `retired`; the caller
    /// destroys it when no frame in flight reads it. Returns an invalid Buffer when the device refuses the allocation.
    Slot Bind(Rendering::IDevice& device, const Key& key, const PackedPageTable& height,
              const PackedPageTable* surface, uint32 frameSlot, Rendering::BufferHandle& retired);
    /// Records a copy of slot `frameSlot` into the ring's readback buffer, which has host-cached memory. Call it
    /// on request, in the frame that binds the slot, and only after the last copy was read or dropped.
    /// False when the ring has no buffer or the device refuses the readback buffer.
    bool RecordSlotCopy(Rendering::IDevice& device, Rendering::CommandList& cl, uint32 frameSlot);
    /// After the submit of the copy's frame: takes what the copied slot holds. False (the copy is dropped)
    /// when no copy is recorded, the slot is not written, or the ring changed its buffer after the copy.
    bool SealSlotCopy();
    /// After the GPU completes the sealed copy: the report of the copied slot, hashed from the readback buffer.
    /// False when no copy is sealed. Each sealed copy is read once.
    bool ReadSlotCopy(Rendering::IDevice& device, SlotReport& out);
    /// Destroys the buffers (`device` non-null) or forgets them (a rebuilt device).
    void Release(Rendering::IDevice* device);

private:
    /// What a slot holds.
    struct SlotState
    {
        Key Owner;
        uint64 HeightVersion = 0;
        uint64 SurfaceVersion = 0;
        uint32 HeightWords = 0;
        uint32 SurfaceWords = 0;
        bool Written = false;
    };
    /// A copy of one slot into m_Readback: recorded, then sealed with the slot's state at the submit.
    struct SlotCopy
    {
        enum class Stage : uint8
        {
            None,
            Recorded,
            Sealed
        };
        Stage Step = Stage::None;
        uint32 Slot = 0;
        Rendering::BufferHandle Source; // the ring buffer that the copy reads
        SlotState State;
    };

    /// Writes `table` at byte `offset`: the runs changed since version `since` when `holdsOlderVersion` and
    /// the table's history reaches it, else every word.
    void WriteTable(Rendering::IDevice& device, uint64 offset, const PackedPageTable& table, bool holdsOlderVersion,
                    uint64 since);
    /// Destroys the readback buffer (`device` non-null) or forgets it (a rebuilt device).
    void ReleaseReadback(Rendering::IDevice* device);

    Rendering::BufferHandle m_Buffer;
    uint32 m_SlotWords = 0;
    std::array<SlotState, kSlots> m_Slots{};
    std::vector<PageWordRange> m_Runs; // scratch of WriteTable
    Rendering::BufferHandle m_Readback; // one slot of m_ReadbackWords words
    uint32 m_ReadbackWords = 0;
    SlotCopy m_Copy;
};

} // namespace GameEngine::TerrainECS
