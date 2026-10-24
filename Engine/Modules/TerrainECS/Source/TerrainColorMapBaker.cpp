#include "TerrainECS/TerrainColorMapBaker.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainSizingPlan.h"
#include "Types/Color.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cstring>

namespace GameEngine::TerrainECS
{
namespace
{

uint64 TiledKey(uint32 index, uint32 generation)
{
    return (static_cast<uint64>(generation) << 32) | index;
}

uint64 HashFloat(uint64 hash, float32 value)
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return Hashing::Fnv1a64Value(hash, bits);
}

// Copies the inclusive tile-local rect of `tile` into the unified map at (dstX, dstZ), resampling
// a tile whose map is on a coarser lattice than its unified region. Returns whether a byte changed.
bool CopyTileIntoUnified(const TerrainColorMap& tile, uint32 regionDim, TerrainColorMap& unified, uint32 dstX,
                         uint32 dstZ)
{
    bool changed = false;
    const bool native = tile.Width == regionDim && tile.Height == regionDim;
    for (uint32 z = 0; z < regionDim && dstZ + z < unified.Height; ++z)
        for (uint32 x = 0; x < regionDim && dstX + x < unified.Width; ++x)
        {
            uint8 texel[kTerrainColorMapChannels] = {0, 0, 0, 0};
            if (tile.IsActive() && native)
                std::memcpy(texel, tile.Pixels.data() + (static_cast<size_t>(z) * tile.Width + x) * kTerrainColorMapChannels,
                            kTerrainColorMapChannels);
            else if (tile.IsActive() && tile.Width >= 2 && tile.Height >= 2)
            {
                // Nearest texel of the coarser lattice: a streaming transient, replaced by the
                // tile's own map once it streams in at full detail.
                const uint32 sx = std::min(tile.Width - 1, static_cast<uint32>(
                    static_cast<float32>(x) / static_cast<float32>(regionDim - 1) * static_cast<float32>(tile.Width - 1) + 0.5f));
                const uint32 sz = std::min(tile.Height - 1, static_cast<uint32>(
                    static_cast<float32>(z) / static_cast<float32>(regionDim - 1) * static_cast<float32>(tile.Height - 1) + 0.5f));
                std::memcpy(texel, tile.Pixels.data() + (static_cast<size_t>(sz) * tile.Width + sx) * kTerrainColorMapChannels,
                            kTerrainColorMapChannels);
            }
            uint8* stored = unified.Pixels.data()
                + (static_cast<size_t>(dstZ + z) * unified.Width + dstX + x) * kTerrainColorMapChannels;
            if (std::memcmp(stored, texel, kTerrainColorMapChannels) == 0)
                continue;
            std::memcpy(stored, texel, kTerrainColorMapChannels);
            changed = true;
        }
    return changed;
}

void MarkDirty(TerrainColorMap& map, uint32 x0, uint32 z0, uint32 x1, uint32 z1)
{
    if (!map.Dirty)
    {
        map.DirtyMinX = x0;
        map.DirtyMinZ = z0;
        map.DirtyMaxX = x1;
        map.DirtyMaxZ = z1;
    }
    else
    {
        map.DirtyMinX = std::min(map.DirtyMinX, x0);
        map.DirtyMinZ = std::min(map.DirtyMinZ, z0);
        map.DirtyMaxX = std::max(map.DirtyMaxX, x1);
        map.DirtyMaxZ = std::max(map.DirtyMaxZ, z1);
    }
    map.Dirty = true;
}

} // namespace

TerrainColorGroundWrite TerrainColorMapBaker::ResolveGroundWrite(const ECS::World& world,
                                                                 ECS::EntityHandle terrainEntity)
{
    TerrainColorGroundWrite write;
    const auto* terrain = world.GetComponent<Components::Terrain>(terrainEntity);
    const auto* grass = world.GetComponent<Components::TerrainGrass>(terrainEntity);
    if (!terrain || !grass || terrain->Domain != Components::TerrainDomain::Planar)
        return write;
    if (!ECS::Entity(const_cast<ECS::World*>(&world), terrainEntity).IsEnabled<Components::TerrainGrass>())
        return write;
    // The placement's own predicate (Terrain::TerrainGrassRowPlaces): a row that places no blade
    // has no field to write the ground of.
    const float32 nearDensity = grass->TextureGrass ? grass->TextureCardsPerSquareMeter : grass->BladesPerSquareMeter;
    const float32 amount = std::isfinite(grass->GroundDarkening) ? std::clamp(grass->GroundDarkening, 0.0f, 1.0f) : 0.0f;
    if (!(nearDensity > 0.0f) || !(grass->BladeHeight > 0.0f) || !(grass->BladeWidth > 0.0f) || amount <= 0.0f)
        return write;
    const ColorLinear soil = ColorSRGB::FromSRGB255(static_cast<uint8>((grass->SoilColor >> 16) & 0xFFu),
                                                    static_cast<uint8>((grass->SoilColor >> 8) & 0xFFu),
                                                    static_cast<uint8>(grass->SoilColor & 0xFFu))
                                 .ToLinear();
    write.Enabled = true;
    write.Amount = amount;
    write.SoilLinear[0] = soil.r;
    write.SoilLinear[1] = soil.g;
    write.SoilLinear[2] = soil.b;
    write.GrassLayer = std::min(grass->LayerIndex, Terrain::kMaxTerrainMaterialLayers - 1u);
    write.MaskThreshold = std::clamp(grass->MaskThreshold, 0.0f, 1.0f);
    return write;
}

TerrainColorComposeInputs TerrainColorMapBaker::SingleTerrainInputs(const TerrainData& data, float32 originX,
                                                                    float32 originZ,
                                                                    const TerrainColorGroundWrite& write,
                                                                    std::span<const ResolvedModifier> zones)
{
    TerrainColorComposeInputs in;
    in.Width = data.Heightfield.GetWidth();
    in.Height = data.Heightfield.GetHeight();
    in.OriginX = originX;
    in.OriginZ = originZ;
    in.SizeX = data.Config.WorldSizeX;
    in.SizeZ = data.Config.WorldSizeZ;
    in.Splat.HasSplatmap = !data.Splatmap.empty();
    if (data.SplatmapWidth >= 2 && data.SplatmapHeight >= 2
        && data.Splatmap.size() == static_cast<size_t>(data.SplatmapWidth) * data.SplatmapHeight * 4u)
    {
        in.Splat.Texels = data.Splatmap.data();
        in.Splat.Width = data.SplatmapWidth;
        in.Splat.Height = data.SplatmapHeight;
    }
    in.Grass = {&data.GrassField, originX, originZ, data.Config.WorldSizeX, data.Config.WorldSizeZ};
    in.Write = write;
    in.Zones = zones;
    return in;
}

uint64 TerrainColorMapBaker::InputsStamp(const TerrainColorComposeInputs& in, uint64 grassFieldVersion,
                                         uint64 zonesStamp)
{
    uint64 hash = Hashing::kFnv1a64OffsetBasis;
    hash = Hashing::Fnv1a64Value(hash, in.Width);
    hash = Hashing::Fnv1a64Value(hash, in.Height);
    hash = HashFloat(hash, in.OriginX);
    hash = HashFloat(hash, in.OriginZ);
    hash = HashFloat(hash, in.SizeX);
    hash = HashFloat(hash, in.SizeZ);
    hash = Hashing::Fnv1a64Value(hash, static_cast<uint32>(in.Write.Enabled ? 1u : 0u));
    if (in.Write.Enabled)
    {
        hash = HashFloat(hash, in.Write.Amount);
        for (const float32 channel : in.Write.SoilLinear)
            hash = HashFloat(hash, channel);
        hash = Hashing::Fnv1a64Value(hash, in.Write.GrassLayer);
        hash = HashFloat(hash, in.Write.MaskThreshold);
    }
    hash = Hashing::Fnv1a64Value(hash, static_cast<uint32>(in.Splat.HasSplatmap ? 1u : 0u));
    hash = Hashing::Fnv1a64Value(hash, grassFieldVersion);
    hash = Hashing::Fnv1a64Value(hash, zonesStamp);
    // 0 means never composed.
    return hash == 0 ? 1 : hash;
}

void TerrainColorMapBaker::UpdateSingle(TerrainData& data, const TerrainColorComposeInputs& inputs, uint64 stamp)
{
    TerrainColorMap& map = data.ColorMap;
    const bool whole = map.InputsStamp != stamp || data.SplatColorDirtyFull || !map.Initialized
        || map.Width != inputs.Width || map.Height != inputs.Height;
    if (whole)
        ComposeTerrainColorMap(map, inputs);
    else if (!data.SplatColorDirty.IsEmpty())
        ComposeTerrainColorMap(map, inputs, data.SplatColorDirty.MinX, data.SplatColorDirty.MinZ,
                               data.SplatColorDirty.MaxX - 1, data.SplatColorDirty.MaxZ - 1);
    map.InputsStamp = stamp;
    data.SplatColorDirtyFull = false;
    data.SplatColorDirty = {};
}

void TerrainColorMapBaker::Update(ECS::World& world, TerrainService& service, std::span<const ResolvedModifier> zones,
                                  uint64 zonesStamp)
{
    if (service.IsInteractiveModifierEdit())
        return;

    // The tiled terrains this update reaches; the state of any other is dropped below, so a
    // destroyed or re-provisioned terrain leaves nothing behind.
    std::vector<uint64> live;

    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain, const Components::WorldTransform& xf)
        {
            if (terrain.Domain != Components::TerrainDomain::Planar)
                return;
            const TerrainColorGroundWrite write = ResolveGroundWrite(world, entity);
            const float32 ox = xf.matrix[12] - terrain.SizeX * 0.5f;
            const float32 oz = xf.matrix[14] - terrain.SizeZ * 0.5f;

            if (terrain.TiledTerrainHandle == 0 && terrain.TiledTerrainGeneration == 0)
            {
                TerrainData* data = service.GetTerrainData({terrain.TerrainDataHandle, terrain.TerrainDataGeneration});
                if (!data || data->Heightfield.GetWidth() < 2 || data->Heightfield.GetHeight() < 2)
                    return;
                const TerrainColorComposeInputs inputs = SingleTerrainInputs(*data, ox, oz, write, zones);
                UpdateSingle(*data, inputs, InputsStamp(inputs, data->GrassField.Version, zonesStamp));
                return;
            }

            TiledTerrainData* tiled =
                service.GetTiledTerrainData({terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
            if (!tiled)
                return;
            const TiledRenderExtent extent = ComputeTiledRenderExtent(tiled->Config);
            const bool unified = extent.UnifiedWidth <= kMaxUnifiedTiledResolution
                && extent.UnifiedHeight <= kMaxUnifiedTiledResolution;
            const float32 tileSize = tiled->Config.TileWorldSize;
            const TerrainGrassField& terrainGrass = unified ? tiled->GrassUnifiedField : tiled->GrassCoarseField;
            const TerrainColorGrassSource terrainGrassSource{&terrainGrass, ox, oz, extent.WorldSizeX, extent.WorldSizeZ};

            // Everything a tile's map depends on besides its own splat and grass field.
            TerrainColorComposeInputs terrainInputs;
            terrainInputs.Width = extent.TilesPerAxisX;
            terrainInputs.Height = extent.TilesPerAxisZ;
            terrainInputs.OriginX = ox;
            terrainInputs.OriginZ = oz;
            terrainInputs.SizeX = extent.WorldSizeX;
            terrainInputs.SizeZ = extent.WorldSizeZ;
            terrainInputs.Splat.HasSplatmap = true;
            terrainInputs.Write = write;
            const uint64 terrainStamp = InputsStamp(terrainInputs, terrainGrass.Version, zonesStamp);
            const uint64 tiledKey = TiledKey(terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration);
            live.push_back(tiledKey);
            TiledState& state = m_Tiled[tiledKey];
            const bool terrainChanged = state.Stamp != terrainStamp;

            std::vector<TileCoord> recomposed;
            for (auto& [coord, tilePtr] : tiled->Tiles)
            {
                if (!tilePtr || tilePtr->LodState == TileLodState::Empty || tilePtr->SplatmapWidth < 2
                    || tilePtr->SplatmapHeight < 2)
                    continue;
                // While its arrival bake runs, a tile keeps the map it showed while Coarse, as the
                // uploaders keep its coarse splat; the bake's end marks the splat changed.
                if (!tilePtr->ArrivalSplatStandIn.empty())
                    continue;
                TerrainTileData& tile = *tilePtr;
                TerrainColorComposeInputs in;
                in.Width = tile.SplatmapWidth;
                in.Height = tile.SplatmapHeight;
                in.OriginX = ox + static_cast<float32>(coord.X) * tileSize;
                in.OriginZ = oz + static_cast<float32>(coord.Z) * tileSize;
                in.SizeX = tileSize;
                in.SizeZ = tileSize;
                in.Splat = {tile.Splatmap.data(), tile.SplatmapWidth, tile.SplatmapHeight, true};
                const bool ownGrass = !unified && tile.GrassField.Initialized && tile.GrassField.Width >= 2;
                in.Grass = ownGrass ? TerrainColorGrassSource{&tile.GrassField, in.OriginX, in.OriginZ, tileSize, tileSize}
                                    : terrainGrassSource;
                in.Write = write;
                in.Zones = zones;
                const uint64 stamp = Hashing::Fnv1a64Value(
                    terrainStamp, ownGrass ? tile.GrassField.Version : ~0ull);
                if (tile.ColorMap.InputsStamp == stamp && !tile.SplatColorDirtyFull && tile.ColorMap.Initialized
                    && tile.ColorMap.Width == in.Width && tile.ColorMap.Height == in.Height)
                    continue;
                ComposeTerrainColorMap(tile.ColorMap, in);
                tile.ColorMap.InputsStamp = stamp;
                tile.SplatColorDirtyFull = false;
                recomposed.push_back(coord);
            }

            const bool residencyChanged = state.Revision != tiled->Revision;
            if (unified)
            {
                TerrainColorMap& map = tiled->ColorUnifiedMap;
                const bool anyTile = std::any_of(tiled->Tiles.begin(), tiled->Tiles.end(), [](const auto& item) {
                    return item.second && item.second->ColorMap.IsActive();
                });
                if (!anyTile)
                {
                    if (map.IsActive())
                    {
                        std::vector<uint8>{}.swap(map.Pixels);
                        ++map.Version;
                        MarkDirty(map, 0, 0, extent.UnifiedWidth, extent.UnifiedHeight);
                    }
                    map.Width = extent.UnifiedWidth;
                    map.Height = extent.UnifiedHeight;
                    map.Initialized = true;
                }
                else
                {
                    // A tile that streams in or upgrades recomposes its own map, which lands
                    // below as a region copy; only a change every tile shares rebuilds the whole.
                    const bool rebuild = !map.IsActive() || map.Width != extent.UnifiedWidth
                        || map.Height != extent.UnifiedHeight || terrainChanged;
                    if (rebuild)
                    {
                        map.Width = extent.UnifiedWidth;
                        map.Height = extent.UnifiedHeight;
                        map.Pixels.assign(static_cast<size_t>(map.Width) * map.Height * kTerrainColorMapChannels, 0);
                    }
                    const uint32 regionDim = tiled->Config.TileConfig.HeightmapWidth;
                    bool changed = rebuild;
                    for (auto& [coord, tilePtr] : tiled->Tiles)
                    {
                        if (!tilePtr || coord.X < 0 || coord.Z < 0)
                            continue;
                        const bool wanted = rebuild
                            || std::find(recomposed.begin(), recomposed.end(), coord) != recomposed.end();
                        if (!wanted)
                            continue;
                        const uint32 dstX = static_cast<uint32>(coord.X) * extent.TileInteriorX;
                        const uint32 dstZ = static_cast<uint32>(coord.Z) * extent.TileInteriorZ;
                        if (CopyTileIntoUnified(tilePtr->ColorMap, regionDim, map, dstX, dstZ))
                        {
                            changed = true;
                            if (!rebuild)
                                MarkDirty(map, dstX, dstZ, std::min(map.Width, dstX + regionDim),
                                          std::min(map.Height, dstZ + regionDim));
                        }
                    }
                    map.Initialized = true;
                    if (rebuild)
                        MarkDirty(map, 0, 0, map.Width, map.Height);
                    if (changed)
                        ++map.Version;
                }
                tiled->ColorMapActive = map.IsActive();
            }
            else
            {
                // The coarse map: the write and the zones evaluated on the coarse splat, whose
                // texels that never streamed read splat layer 0, so the soil continues past the
                // resident window rather than stopping at its frontier.
                if (residencyChanged || terrainChanged || !recomposed.empty() || !tiled->ColorCoarseMap.Initialized)
                {
                    const uint32 cd = kAtlasCoarseFieldDim;
                    const uint8 kCoarseSplatFallback[4] = {255u, 0u, 0u, 0u};
                    BuildCoarseFieldU8(
                        cd, tiled->Config.TilesPerAxisX, tiled->Config.TilesPerAxisZ, 4u, kCoarseSplatFallback,
                        [tiled](int32 tx, int32 tz) -> CoarseTileSourceU8 {
                            auto it = tiled->Tiles.find(TileCoord{tx, tz});
                            if (it == tiled->Tiles.end() || !it->second || it->second->LodState == TileLodState::Empty
                                || it->second->Splatmap.empty())
                                return {};
                            return {it->second->Splatmap.data(), it->second->SplatmapWidth,
                                    it->second->SplatmapHeight, 4u};
                        },
                        m_CoarseSplat);
                    TerrainColorComposeInputs in;
                    in.Width = cd;
                    in.Height = cd;
                    in.OriginX = ox;
                    in.OriginZ = oz;
                    in.SizeX = extent.WorldSizeX;
                    in.SizeZ = extent.WorldSizeZ;
                    in.Splat = {m_CoarseSplat.empty() ? nullptr : m_CoarseSplat.data(), cd, cd, true};
                    in.Grass = terrainGrassSource;
                    in.Write = write;
                    in.Zones = zones;
                    ComposeTerrainColorMap(tiled->ColorCoarseMap, in);
                    tiled->ColorCoarseMap.InputsStamp = terrainStamp;
                }
                tiled->ColorMapActive = tiled->ColorCoarseMap.IsActive()
                    || std::any_of(tiled->Tiles.begin(), tiled->Tiles.end(), [](const auto& item) {
                           return item.second && item.second->ColorMap.IsActive();
                       });
            }
            state.Stamp = terrainStamp;
            state.Revision = tiled->Revision;
        });
    std::erase_if(m_Tiled, [&](const auto& item)
                  { return std::find(live.begin(), live.end(), item.first) == live.end(); });
}

} // namespace GameEngine::TerrainECS
