#include "TerrainFieldUploader.h"
#include "Mathematics/Interpolation.h"
#include "TerrainColorMap/TerrainColorMapCompose.h"
#include "TerrainECS/TerrainColorMap/TerrainColorMap.h"
#include "TerrainECS/TerrainGrass/TerrainGrassField.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainRowBands.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>

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

namespace
{
// The byte offsets of one texel's four bilinear corners in a source field, and its two weights.
struct BilinearTap
{
    size_t X0Z0 = 0, X1Z0 = 0, X0Z1 = 0, X1Z1 = 0;
    float32 WeightX = 0.0f, WeightZ = 0.0f;
};

BilinearTap TapAt(const TerrainFieldData& field, uint32 channels, float32 u, float32 v)
{
    const float32 x = Math::Clamp01(u) * (field.Width - 1);
    const float32 z = Math::Clamp01(v) * (field.Height - 1);
    const uint32 x0 = static_cast<uint32>(x), z0 = static_cast<uint32>(z);
    const uint32 x1 = std::min(x0 + 1, field.Width - 1), z1 = std::min(z0 + 1, field.Height - 1);
    const size_t row0 = static_cast<size_t>(z0) * field.Width, row1 = static_cast<size_t>(z1) * field.Width;
    return {(row0 + x0) * channels, (row0 + x1) * channels, (row1 + x0) * channels, (row1 + x1) * channels,
            x - x0, z - z0};
}

// Rows [z0, z1] of a tile resample. The color map's rgb bytes are sRGB-encoded linear values:
// decoded before the filter and encoded after it, as the GPU's RGBA8_SRGB sampler filters them,
// so a written and an unwritten texel blend to the half mix.
void ResampleRows(const TerrainFieldUploadDesc& desc, const TerrainFieldData& source,
                  const TileResampleWindow& window, uint32 tileRes, uint8* out, int32 z0, int32 z1)
{
    const uint32 channels = desc.Channels;
    const uint32 srgbChannels = desc.Kind == TerrainField::Color ? 3u : 0u;
    const std::array<float32, 256>& toLinear = SrgbByteToLinearTable();
    const uint8* pixels = source.Pixels.data();
    for (int32 z = z0; z <= z1; ++z)
    {
        const float32 v = (window.Origin.y + static_cast<float32>(z) / (tileRes - 1)) / window.Scale.y;
        uint8* texel = out + static_cast<size_t>(z) * tileRes * channels;
        for (uint32 x = 0; x < tileRes; ++x, texel += channels)
        {
            const float32 u = (window.Origin.x + static_cast<float32>(x) / (tileRes - 1)) / window.Scale.x;
            const BilinearTap tap = TapAt(source, channels, u, v);
            for (uint32 channel = 0; channel < channels; ++channel)
            {
                const uint8* channelBytes = pixels + channel;
                if (channel < srgbChannels)
                {
                    const float32 a = std::lerp(toLinear[channelBytes[tap.X0Z0]], toLinear[channelBytes[tap.X1Z0]],
                                                tap.WeightX);
                    const float32 b = std::lerp(toLinear[channelBytes[tap.X0Z1]], toLinear[channelBytes[tap.X1Z1]],
                                                tap.WeightX);
                    texel[channel] = EncodeLinearToSrgbByte(std::lerp(a, b, tap.WeightZ));
                    continue;
                }
                const float32 a = std::lerp(static_cast<float32>(channelBytes[tap.X0Z0]),
                                            static_cast<float32>(channelBytes[tap.X1Z0]), tap.WeightX);
                const float32 b = std::lerp(static_cast<float32>(channelBytes[tap.X0Z1]),
                                            static_cast<float32>(channelBytes[tap.X1Z1]), tap.WeightX);
                texel[channel] = static_cast<uint8>(std::lround(std::lerp(a, b, tap.WeightZ)));
            }
        }
    }
}
} // namespace

void ResampleFieldIntoTile(const TerrainFieldUploadDesc& desc, const TerrainFieldData& source,
                           const TileResampleWindow& window, uint32 tileRes,
                           JobSystem::WorkStealingThreadPool* pool, std::vector<uint8>& out)
{
    out.resize(static_cast<size_t>(tileRes) * tileRes * desc.Channels);
    if (!source.IsActive())
    {
        std::fill(out.begin(), out.end(), desc.Neutral);
        return;
    }
    const BakeBandRegion tile{0, 0, static_cast<int32>(tileRes) - 1, static_cast<int32>(tileRes) - 1};
    RunRowBands(pool, std::span<const BakeBandRegion>(&tile, 1),
                [&](std::size_t, int32, int32, int32 z0, int32 z1)
                { ResampleRows(desc, source, window, tileRes, out.data(), z0, z1); });
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
                                       std::span<const TileAtlasSlot> rows,
                                       JobSystem::WorkStealingThreadPool* pool)
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
        if (geo.RowIndex(coord.X, coord.Z) >= rows.size()) continue;
        const TileAtlasSlot row = AtlasRowAt(rows.data(), geo, coord.X, coord.Z);
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
        else if (!field.Initialized)
        {
            // A tile arriving after this tick's modifier pass uses the already-composed analytic
            // coarse field until its own samples are ready. Never treat that transient as an
            // authored neutral field and lose a suppression for one frame.
            const TileResampleWindow window{
                {static_cast<float32>(coord.X), static_cast<float32>(coord.Z)},
                {static_cast<float32>(geo.TilesPerAxisX), static_cast<float32>(geo.TilesPerAxisZ)}};
            ResampleFieldIntoTile(m_Desc, coarse, window, geo.TileRes, pool, m_Source);
            source = m_Source.data();
        }
        else
        {
            ResampleFieldIntoTile(m_Desc, field, TileResampleWindow{}, geo.TileRes, pool, m_Source);
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
