#include "TerrainECS/TerrainZoneAuthoring.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace GameEngine::TerrainECS
{

namespace
{
// Widens [lo, hi] outward to the world texel lattice (multiples of 1 / texelsPerMeter) and caps the
// span at maxSpanTexels. Every footprint of a zone then samples the same world points.
void SnapAxisToTexelLattice(float32 lo, float32 hi, float32 texelsPerMeter, float32 maxSpanTexels,
                            float32& outCenter, float32& outExtent)
{
    if (texelsPerMeter <= 0.0f)
    {
        outCenter = (lo + hi) * 0.5f;
        outExtent = (hi - lo) * 0.5f;
        return;
    }
    const float32 loTexel = std::floor(lo * texelsPerMeter);
    const float32 hiTexel = std::min(std::ceil(hi * texelsPerMeter), loTexel + maxSpanTexels);
    outCenter = (loTexel + hiTexel) * 0.5f / texelsPerMeter;
    outExtent = (hiTexel - loTexel) * 0.5f / texelsPerMeter;
}

// World coordinate of texel `index` of an axis that spans [center - extent, center + extent] in `dim` texels.
float32 TexelCenterToWorld(float32 center, float32 extent, uint32 index, uint32 dim)
{
    const float32 u = dim > 1 ? static_cast<float32>(index) / static_cast<float32>(dim - 1) : 0.5f;
    return center + (u - 0.5f) * 2.0f * extent;
}
} // namespace

ZoneFootprint FitZoneToDab(float32 hitX, float32 hitZ, float32 brushRadius,
                           const ZoneAuthoringSettings& settings)
{
    const float32 half = std::min(brushRadius + settings.Padding, settings.MaxExtent);
    const float32 maxSpanTexels = std::floor(2.0f * settings.MaxExtent * settings.TexelsPerMeter);
    ZoneFootprint fp;
    SnapAxisToTexelLattice(hitX - half, hitX + half, settings.TexelsPerMeter, maxSpanTexels, fp.CenterX,
                           fp.ExtentX);
    SnapAxisToTexelLattice(hitZ - half, hitZ + half, settings.TexelsPerMeter, maxSpanTexels, fp.CenterZ,
                           fp.ExtentZ);
    return fp;
}

bool GrowZoneToDab(ZoneFootprint& footprint, float32 hitX, float32 hitZ,
                   float32 brushRadius, const ZoneAuthoringSettings& settings)
{
    // Union the existing footprint with the dab's padded AABB, then re-center on the texel lattice.
    const float32 dabHalf = brushRadius + settings.Padding;
    const float32 minX = std::min(footprint.CenterX - footprint.ExtentX, hitX - dabHalf);
    const float32 maxX = std::max(footprint.CenterX + footprint.ExtentX, hitX + dabHalf);
    const float32 minZ = std::min(footprint.CenterZ - footprint.ExtentZ, hitZ - dabHalf);
    const float32 maxZ = std::max(footprint.CenterZ + footprint.ExtentZ, hitZ + dabHalf);

    constexpr float32 kNoSpanCap = std::numeric_limits<float32>::max();
    ZoneFootprint grown;
    SnapAxisToTexelLattice(minX, maxX, settings.TexelsPerMeter, kNoSpanCap, grown.CenterX, grown.ExtentX);
    SnapAxisToTexelLattice(minZ, maxZ, settings.TexelsPerMeter, kNoSpanCap, grown.CenterZ, grown.ExtentZ);
    if (grown.ExtentX > settings.MaxExtent || grown.ExtentZ > settings.MaxExtent)
        return false;

    footprint = grown;
    return true;
}

uint32 PayloadDimForExtent(float32 extent, const ZoneAuthoringSettings& settings)
{
    // Full width = 2*extent meters; +1 so both edges land on a texel center.
    const float32 span = 2.0f * std::max(extent, 0.0f) * settings.TexelsPerMeter;
    const auto dim = static_cast<uint32>(std::lround(span)) + 1u;
    return std::clamp(dim, settings.MinDim, settings.MaxDim);
}

void WorldToPayloadTexel(const ZoneFootprint& footprint, uint32 width, uint32 height,
                         float32 worldX, float32 worldZ, float32& outTx, float32& outTz)
{
    // Local in [-Extent, +Extent] -> UV [0,1] -> texel [0, dim-1].
    const float32 u = footprint.ExtentX > 0.0f
        ? (worldX - footprint.CenterX) / (2.0f * footprint.ExtentX) + 0.5f : 0.5f;
    const float32 v = footprint.ExtentZ > 0.0f
        ? (worldZ - footprint.CenterZ) / (2.0f * footprint.ExtentZ) + 0.5f : 0.5f;
    outTx = u * static_cast<float32>(width - 1);
    outTz = v * static_cast<float32>(height - 1);
}

TerrainZonePayload RegrowPayload(const TerrainZonePayload& old,
                                 const ZoneFootprint& oldFootprint,
                                 const ZoneFootprint& newFootprint, uint32 newWidth,
                                 uint32 newHeight)
{
    TerrainZonePayload grown;
    grown.Allocate(old.Format, newWidth, newHeight);

    if (old.Width < 1 || old.Height < 1 || newWidth < 1 || newHeight < 1)
        return grown;

    // Each new texel reads the old texel nearest its world center, so no new texel inside the old
    // footprint is left unwritten. On the shared texel lattice the copy is exact.
    const std::size_t texelBytes = ZonePayloadBytesPerTexel(old.Format);
    for (uint32 nz = 0; nz < newHeight; ++nz)
    {
        const float32 worldZ = TexelCenterToWorld(newFootprint.CenterZ, newFootprint.ExtentZ, nz, newHeight);
        for (uint32 nx = 0; nx < newWidth; ++nx)
        {
            const float32 worldX = TexelCenterToWorld(newFootprint.CenterX, newFootprint.ExtentX, nx, newWidth);

            float32 tx = 0.0f, tz = 0.0f;
            WorldToPayloadTexel(oldFootprint, old.Width, old.Height, worldX, worldZ, tx, tz);
            const int32 ox = static_cast<int32>(std::lround(tx));
            const int32 oz = static_cast<int32>(std::lround(tz));
            if (ox < 0 || oz < 0 || ox >= static_cast<int32>(old.Width) || oz >= static_cast<int32>(old.Height))
                continue;

            const std::size_t src = (static_cast<std::size_t>(oz) * old.Width + ox) * texelBytes;
            const std::size_t dst = (static_cast<std::size_t>(nz) * newWidth + nx) * texelBytes;
            std::memcpy(grown.MutableTexelBytes() + dst, old.TexelBytes() + src, texelBytes);
        }
    }
    return grown;
}

} // namespace GameEngine::TerrainECS
