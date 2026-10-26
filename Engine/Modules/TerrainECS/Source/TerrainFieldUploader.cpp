#include "TerrainFieldUploader.h"
#include "Mathematics/Interpolation.h"
#include "TerrainColorMapCompose.h"
#include "TerrainECS/TerrainColorMap.h"
#include "TerrainECS/TerrainGrassField.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "Types/Color.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine::TerrainECS
{
namespace
{
TerrainFieldData& GrassTileField(TerrainTileData& tile) { return tile.GrassField; }
TerrainFieldData& GrassCoarseField(TiledTerrainData& tiled) { return tiled.GrassCoarseField; }
bool GrassActive(const TiledTerrainData& tiled) { return tiled.GrassRegionsActive; }
TerrainFieldData& ColorTileField(TerrainTileData& tile) { return tile.ColorMap; }
TerrainFieldData& ColorCoarseField(TiledTerrainData& tiled) { return tiled.ColorCoarseMap; }
bool ColorActive(const TiledTerrainData& tiled) { return tiled.ColorMapActive; }
}

const TerrainFieldUploadDesc& GrassFieldUploadDesc()
{
    static const TerrainFieldUploadDesc kDesc{TerrainField::GrassControl, kTerrainGrassFieldChannels, 255,
                                              &GrassTileField, &GrassCoarseField, &GrassActive};
    return kDesc;
}

const TerrainFieldUploadDesc& ColorMapUploadDesc()
{
    static const TerrainFieldUploadDesc kDesc{TerrainField::Color, kTerrainColorMapChannels, 0,
                                              &ColorTileField, &ColorCoarseField, &ColorActive};
    return kDesc;
}

// A tile arriving after this tick's modifier pass uses the already-composed
// analytic coarse field until its own samples are ready. Never treat that
// transient as an authored neutral field and lose a suppression for one frame.
uint8 TerrainFieldUploader::SampleCoarse(const TerrainFieldData& field, float32 u, float32 v, uint32 channel) const
{
    if (!field.IsActive()) return m_Desc.Neutral;
    const float32 x = Math::Clamp01(u) * (field.Width - 1);
    const float32 z = Math::Clamp01(v) * (field.Height - 1);
    const uint32 x0 = static_cast<uint32>(x), z0 = static_cast<uint32>(z);
    const uint32 x1 = std::min(x0 + 1, field.Width - 1), z1 = std::min(z0 + 1, field.Height - 1);
    // The colour map's rgb bytes are sRGB-encoded linear values: decoded before the filter and encoded
    // after it, as the GPU's RGBA8_SRGB sampler filters them, so a written and an unwritten texel
    // blend to the half mix.
    const bool srgb = m_Desc.Kind == TerrainField::Color && channel < 3;
    const auto at = [&](uint32 sx, uint32 sz)
    {
        const uint8 byte = field.Pixels[(static_cast<size_t>(sz) * field.Width + sx) * m_Desc.Channels + channel];
        return srgb ? Detail::SRGBChannelToLinear(byte / 255.0f) : static_cast<float32>(byte);
    };
    const float32 a = std::lerp(at(x0, z0), at(x1, z0), x - x0);
    const float32 b = std::lerp(at(x0, z1), at(x1, z1), x - x0);
    const float32 value = std::lerp(a, b, z - z0);
    return srgb ? EncodeLinearToSrgbByte(value) : static_cast<uint8>(std::lround(value));
}

void TerrainFieldUploader::Forget(TerrainHandle handle) { m_Atlases.erase(Key(handle)); }

void TerrainFieldUploader::ForgetRetired(const TerrainService& service)
{
    std::erase_if(m_Atlases, [&](const auto& item) {
        return !service.GetTerrainData(TerrainHandle{static_cast<uint32>(item.first), static_cast<uint32>(item.first >> 32)});
    });
}

bool TerrainFieldUploader::UpdateUnified(TerrainRenderFeature& feature, TerrainHandle handle, TerrainFieldData& field)
{
    const uint32 channels = m_Desc.Channels;
    if (!field.IsActive())
    {
        feature.ReleaseFieldResources(m_Desc.Kind, handle);
        field.Dirty = false;
        Forget(handle);
        return false;
    }
    const bool created = feature.EnsureFieldTexture(m_Desc.Kind, handle, TerrainFieldMap::Unified, field.Width, field.Height);
    if (!feature.GetFieldTexture(m_Desc.Kind, handle, TerrainFieldMap::Unified).IsValid()) return false;
    // Creation invalidates the whole destination, including when the CPU field
    // was previously consumed. Persist this requirement across failed staging.
    if (created)
    {
        field.Dirty = true;
        field.DirtyMinX = field.DirtyMinZ = 0;
        field.DirtyMaxX = field.Width; field.DirtyMaxZ = field.Height;
    }
    if (created || field.Dirty)
    {
        const uint32 x0 = created ? 0 : field.DirtyMinX, z0 = created ? 0 : field.DirtyMinZ;
        const uint32 x1 = created ? field.Width : field.DirtyMaxX, z1 = created ? field.Height : field.DirtyMaxZ;
        const uint32 w = x1 - x0, h = z1 - z0;
        m_Patch.resize(static_cast<size_t>(w) * h * channels);
        for (uint32 z = 0; z < h; ++z)
            std::memcpy(m_Patch.data() + static_cast<size_t>(z) * w * channels,
                        field.Pixels.data() + (static_cast<size_t>(z + z0) * field.Width + x0) * channels,
                        static_cast<size_t>(w) * channels);
        if (feature.UploadField(m_Desc.Kind, handle, TerrainFieldMap::Unified, m_Patch.data(), w, h, x0, z0))
            field.Dirty = false;
    }
    return !field.Dirty;
}

bool TerrainFieldUploader::UpdateAtlas(TerrainRenderFeature& feature, TerrainHandle handle,
                                       TiledTerrainData& tiled, const AtlasGeometry& geo,
                                       std::span<const TileAtlasSlot> rows)
{
    const uint32 channels = m_Desc.Channels;
    if (!m_Desc.Active(tiled) || !geo.IsValid())
    {
        feature.ReleaseFieldResources(m_Desc.Kind, handle);
        Forget(handle);
        return false;
    }
    auto& coarse = m_Desc.CoarseField(tiled);
    const bool coarseCreated = feature.EnsureFieldTexture(m_Desc.Kind, handle, TerrainFieldMap::Coarse, coarse.Width, coarse.Height);
    if (coarseCreated) coarse.Dirty = true;
    if (coarse.Dirty)
    {
        const uint8* source = coarse.Pixels.data();
        if (!coarse.IsActive()) { m_Source.assign(static_cast<size_t>(coarse.Width) * coarse.Height * channels, m_Desc.Neutral); source = m_Source.data(); }
        if (feature.UploadField(m_Desc.Kind, handle, TerrainFieldMap::Coarse, source, coarse.Width, coarse.Height)) coarse.Dirty = false;
    }
    feature.EnsureFieldTexture(m_Desc.Kind, handle, TerrainFieldMap::Atlas, geo.AtlasDim, geo.AtlasDim);
    const auto texture = feature.GetFieldTexture(m_Desc.Kind, handle, TerrainFieldMap::Atlas);
    if (!texture.IsValid()) return false;
    auto& state = m_Atlases[Key(handle)];
    if (state.TextureId != texture.id)
    {
        state.TextureId = texture.id;
        state.Slots.clear();
    }
    state.Slots.resize(geo.SlotCount);
    bool allReady = !coarse.Dirty;
    for (auto& [coord, tile] : tiled.Tiles)
    {
        if (!tile || tile->LodState == TileLodState::Empty || coord.X < 0 || coord.Z < 0
            || coord.X >= static_cast<int32>(geo.TilesPerAxisX) || coord.Z >= static_cast<int32>(geo.TilesPerAxisZ)) continue;
        const uint32 rowIndex = geo.TileIndex(coord.X, coord.Z);
        if (rowIndex >= rows.size()) continue;
        const auto& row = rows[rowIndex];
        if (row.Slot == kAtlasNoSlot || row.Slot >= state.Slots.size()) continue;
        auto& previous = state.Slots[row.Slot];
        auto& field = m_Desc.TileField(*tile);
        const bool sameOwner = previous.Valid && previous.TileX == coord.X && previous.TileZ == coord.Z
                               && previous.Generation == row.Generation;
        const bool sameLattice = previous.Width == field.Width && previous.Height == field.Height
                                 && previous.Initialized == field.Initialized;
        if (sameOwner && sameLattice && !field.Dirty && previous.Version == field.Version
            && (field.Initialized || previous.CoarseVersion == coarse.Version)) continue;
        const bool native = field.Initialized && field.Width == geo.TileRes && field.Height == geo.TileRes;
        const uint8* source = nullptr;
        if (native && field.IsActive()) source = field.Pixels.data();
        else
        {
            m_Source.resize(static_cast<size_t>(geo.TileRes) * geo.TileRes * channels);
            for (uint32 z = 0; z < geo.TileRes; ++z)
                for (uint32 x = 0; x < geo.TileRes; ++x)
                    for (uint32 channel = 0; channel < channels; ++channel)
                    {
                        uint8 value = m_Desc.Neutral;
                        if (!field.Initialized)
                            value = SampleCoarse(coarse,
                                (coord.X + static_cast<float32>(x) / (geo.TileRes - 1)) / geo.TilesPerAxisX,
                                (coord.Z + static_cast<float32>(z) / (geo.TileRes - 1)) / geo.TilesPerAxisZ, channel);
                        else if (field.IsActive())
                            value = SampleCoarse(field, static_cast<float32>(x) / (geo.TileRes - 1),
                                                 static_cast<float32>(z) / (geo.TileRes - 1), channel);
                        m_Source[(static_cast<size_t>(z) * geo.TileRes + x) * channels + channel] = value;
                    }
            source = m_Source.data();
        }
        const bool region = sameOwner && sameLattice && native && field.Dirty;
        const auto patch = PackTileEditRegionIntoSlot(m_Patch, source, channels, geo, row.Slot,
            region ? field.DirtyMinX : 0, region ? field.DirtyMinZ : 0,
            region ? static_cast<int32>(field.DirtyMaxX) - 1 : geo.TileRes - 1,
            region ? static_cast<int32>(field.DirtyMaxZ) - 1 : geo.TileRes - 1);
        if (patch.Width && patch.Height && feature.UploadField(m_Desc.Kind, handle, TerrainFieldMap::Atlas,
                m_Patch.data(), patch.Width, patch.Height, patch.DstTexelX, patch.DstTexelY))
        {
            previous = {coord.X, coord.Z, row.Generation, field.Width, field.Height,
                        field.Version, coarse.Version, field.Initialized, true};
            field.Dirty = false;
        }
        else allReady = false;
    }
    return allReady && feature.GetFieldTexture(m_Desc.Kind, handle, TerrainFieldMap::Coarse).IsValid();
}
}
