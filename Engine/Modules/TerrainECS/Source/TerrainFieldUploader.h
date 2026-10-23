#pragma once

#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainFieldData.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainService.h"
#include <unordered_map>

namespace GameEngine::TerrainECS
{
class TerrainRenderFeature;

// One field's upload facts: its GPU texture set, its texel size and neutral byte, and where its
// CPU maps live on a tiled terrain.
struct TerrainFieldUploadDesc
{
    TerrainField Kind = TerrainField::GrassControl;
    uint32 Channels = 0;
    uint8 Neutral = 0;
    TerrainFieldData& (*TileField)(TerrainTileData& tile) = nullptr;
    TerrainFieldData& (*CoarseField)(TiledTerrainData& tiled) = nullptr;
    bool (*Active)(const TiledTerrainData& tiled) = nullptr;
};

// The grass control field: RG8, neutral (1, 1) where no region is authored.
const TerrainFieldUploadDesc& GrassFieldUploadDesc();
// The colour map: RGBA8, neutral 0 (identity).
const TerrainFieldUploadDesc& ColorMapUploadDesc();

// Extraction-owned upload cache for one field. Source generations and slot ownership decide
// uploads; idle views do not touch GPU field textures.
class TerrainFieldUploader
{
public:
    explicit TerrainFieldUploader(const TerrainFieldUploadDesc& desc) : m_Desc(desc) {}

    bool UpdateUnified(TerrainRenderFeature& feature, TerrainHandle handle, TerrainFieldData& field);
    bool UpdateAtlas(TerrainRenderFeature& feature, TerrainHandle handle, TiledTerrainData& tiled,
                     const AtlasGeometry& geometry, std::span<const TileAtlasSlot> rows);
    void Forget(TerrainHandle handle);
    void ForgetRetired(const TerrainService& service);

private:
    struct SlotState
    {
        int32 TileX = -1, TileZ = -1;
        uint32 Generation = 0, Width = 0, Height = 0;
        uint64 Version = 0, CoarseVersion = 0;
        bool Initialized = false, Valid = false;
    };
    struct AtlasState
    {
        uint64 TextureId = 0;
        std::vector<SlotState> Slots;
    };
    uint8 SampleCoarse(const TerrainFieldData& field, float32 u, float32 v, uint32 channel) const;

    const TerrainFieldUploadDesc& m_Desc;
    std::unordered_map<uint64, AtlasState> m_Atlases;
    std::vector<uint8> m_Source;
    std::vector<uint8> m_Patch;
    static uint64 Key(TerrainHandle handle) { return (static_cast<uint64>(handle.Generation) << 32) | handle.Index; }
};
} // namespace GameEngine::TerrainECS
