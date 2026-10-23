#pragma once

#include "Types/Types.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace GameEngine::TerrainECS
{

// Upper bound on a decoded payload's dimensions. Guards DecodeZonePayload
// against a crafted header whose width*height would overflow or force an
// enormous allocation; well above any brush-authored zone (auto-grow caps at
// ~1k texels/axis).
constexpr uint32 kMaxZoneDimension = 8192;

// Storage layout of a terrain zone's brush-authored payload (edit-pipeline
// design §9.3). Sculpt zones store R32F height offsets in world units; paint
// zones store an R8 weight mask; colour zones store four floats per texel: the
// straight linear colour and its weight, 0 to 1. A zero texel is identity in
// every format.
enum class ZonePayloadFormat : uint8
{
    SculptOffsetR32F = 0,
    PaintMaskR8 = 1,
    ColorRGBA32F = 2,
};

// The last format a payload can carry: the decoder's bound.
inline constexpr ZonePayloadFormat kLastZonePayloadFormat = ZonePayloadFormat::ColorRGBA32F;

// Floats per texel of a ColorRGBA32F payload: red, green, blue, weight.
inline constexpr uint32 kZonePayloadColorChannels = 4u;

// Bytes one texel of `format` takes in memory and in the .tzone blob.
constexpr std::size_t ZonePayloadBytesPerTexel(ZonePayloadFormat format)
{
    switch (format)
    {
    case ZonePayloadFormat::SculptOffsetR32F: return sizeof(float32);
    case ZonePayloadFormat::PaintMaskR8: return sizeof(uint8);
    case ZonePayloadFormat::ColorRGBA32F: return kZonePayloadColorChannels * sizeof(float32);
    }
    return 0;
}

// One zone's payload, owned by TerrainService and keyed by the zone component's
// payload GUID. The DataVersion and the accumulated per-stroke dirty texel rect
// live here (not on the ECS component): a brush stroke mutates this store and
// never touches any component field, so the modifier system's payload-dirty-rect
// extension (§3.2) can re-bake only the stroke's footprint.
struct TerrainZonePayload
{
    ZonePayloadFormat Format = ZonePayloadFormat::SculptOffsetR32F;
    uint32 Width = 0;
    uint32 Height = 0;

    // Exactly one of these is populated per Format.
    std::vector<float32> Offsets; // SculptOffsetR32F, row-major, world units
    std::vector<uint8> Mask;      // PaintMaskR8, row-major, 0-255
    std::vector<float32> Color;   // ColorRGBA32F, row-major, kZonePayloadColorChannels per texel

    // Bumped on every edit; the modifier bake diffs it against its snapshot to
    // scope a re-bake to the payload dirty rect. 0 is reserved for "unloaded".
    uint64 DataVersion = 1;

    // Accumulated dirty texel rect since the last bake consumption. Max bounds
    // are exclusive. Empty when DirtyAny is false.
    int32 DirtyMinX = 0, DirtyMinZ = 0, DirtyMaxX = 0, DirtyMaxZ = 0;
    bool DirtyAny = false;

    // Set when the payload changed and the editor should coalesce a save.
    bool NeedsSave = false;

    bool IsSculpt() const { return Format == ZonePayloadFormat::SculptOffsetR32F; }
    std::size_t TexelCount() const { return static_cast<std::size_t>(Width) * Height; }

    // (Re)allocate storage for the given format/dimensions, zero-filled.
    void Allocate(ZonePayloadFormat format, uint32 width, uint32 height)
    {
        Format = format;
        Width = width;
        Height = height;
        const std::size_t count = TexelCount();
        Offsets.clear();
        Mask.clear();
        Color.clear();
        switch (format)
        {
        case ZonePayloadFormat::SculptOffsetR32F: Offsets.assign(count, 0.0f); break;
        case ZonePayloadFormat::PaintMaskR8: Mask.assign(count, 0); break;
        case ZonePayloadFormat::ColorRGBA32F: Color.assign(count * kZonePayloadColorChannels, 0.0f); break;
        }
    }

    // The texel bytes of the populated store, TexelCount() * ZonePayloadBytesPerTexel(Format) long.
    const uint8* TexelBytes() const
    {
        switch (Format)
        {
        case ZonePayloadFormat::SculptOffsetR32F: return reinterpret_cast<const uint8*>(Offsets.data());
        case ZonePayloadFormat::PaintMaskR8: return Mask.data();
        case ZonePayloadFormat::ColorRGBA32F: return reinterpret_cast<const uint8*>(Color.data());
        }
        return nullptr;
    }
    uint8* MutableTexelBytes() { return const_cast<uint8*>(TexelBytes()); }

    // Expand the accumulated dirty rect to cover [minX, maxX) x [minZ, maxZ)
    // (texel coords, max exclusive), clamped to the payload dimensions.
    void MarkDirtyTexels(int32 minX, int32 minZ, int32 maxX, int32 maxZ)
    {
        minX = std::clamp(minX, 0, static_cast<int32>(Width));
        minZ = std::clamp(minZ, 0, static_cast<int32>(Height));
        maxX = std::clamp(maxX, 0, static_cast<int32>(Width));
        maxZ = std::clamp(maxZ, 0, static_cast<int32>(Height));
        if (maxX <= minX || maxZ <= minZ)
            return;
        if (!DirtyAny)
        {
            DirtyMinX = minX;
            DirtyMinZ = minZ;
            DirtyMaxX = maxX;
            DirtyMaxZ = maxZ;
            DirtyAny = true;
        }
        else
        {
            DirtyMinX = std::min(DirtyMinX, minX);
            DirtyMinZ = std::min(DirtyMinZ, minZ);
            DirtyMaxX = std::max(DirtyMaxX, maxX);
            DirtyMaxZ = std::max(DirtyMaxZ, maxZ);
        }
    }

    void ClearDirty()
    {
        DirtyAny = false;
        DirtyMinX = DirtyMinZ = DirtyMaxX = DirtyMaxZ = 0;
    }
};

// Serialize a payload to the .tzone blob format (magic + version + format +
// dims + row-major data). Round-trips exactly through DecodeZonePayload.
std::vector<uint8> EncodeZonePayload(const TerrainZonePayload& payload);

// Parse a .tzone blob into `out`. Returns false on a bad magic/version, a
// truncated buffer, a dimension/byte-count mismatch, or a colour payload holding
// a non-finite value (out is left unchanged).
bool DecodeZonePayload(const uint8* data, std::size_t size, TerrainZonePayload& out);

} // namespace GameEngine::TerrainECS
