#include "TerrainGrassCompose.h"

#include <algorithm>
#include <chrono>

namespace GameEngine::TerrainECS
{

void SnapshotTerrainGrassModifiers(std::span<const ResolvedModifier> modifiers, TerrainGrassComposeRequest& request)
{
    request.Modifiers.assign(modifiers.begin(), modifiers.end());
    for (ResolvedModifier& mod : request.Modifiers)
    {
        mod.Route = nullptr;
        if (!mod.ResolvedSpline) continue;
        request.Splines.push_back(*mod.ResolvedSpline);
        mod.ResolvedSpline = &request.Splines.back();
    }
}

void ComposeTerrainGrassRequest(TerrainGrassComposeRequest& request, std::span<const ResolvedModifier> modifiers,
                                JobSystem::WorkStealingThreadPool* jobs)
{
    for (TerrainGrassComposeTarget& target : request.Targets)
    {
        if (!target.Compose) continue;
        const auto start = std::chrono::steady_clock::now();
        ComposeTerrainGrassField(target.Field, target.Width, target.Height, target.SizeX, target.SizeZ,
                                 target.OriginX, target.OriginZ, modifiers, target.MinX, target.MinZ,
                                 target.MaxX, target.MaxZ, jobs);
        target.ComposeTime = std::chrono::steady_clock::now() - start;
    }
    request.Composed.store(true, std::memory_order_release);
}

TerrainGrassField* FindTerrainGrassField(TerrainService& service, const TerrainGrassComposeTarget& target)
{
    if (target.Owner == TerrainGrassFieldOwner::Single)
    {
        TerrainData* data = service.GetTerrainData(target.Terrain);
        return data ? &data->GrassField : nullptr;
    }
    TiledTerrainData* tiled = service.GetTiledTerrainData(target.Tiled);
    if (!tiled) return nullptr;
    switch (target.Owner)
    {
    case TerrainGrassFieldOwner::TiledUnified: return &tiled->GrassUnifiedField;
    case TerrainGrassFieldOwner::TiledCoarse: return &tiled->GrassCoarseField;
    case TerrainGrassFieldOwner::TiledTile:
    {
        const auto tile = tiled->Tiles.find(target.Tile);
        if (tile == tiled->Tiles.end() || !tile->second || tile->second->LodState == TileLodState::Empty)
            return nullptr;
        return &tile->second->GrassField;
    }
    case TerrainGrassFieldOwner::Single: break;
    }
    return nullptr;
}

void ApplyTerrainGrassComposeTarget(TerrainGrassComposeTarget& target, TerrainGrassField& live)
{
    TerrainGrassField& result = target.Field;
    if (live.Dirty && result.Width == live.Width && result.Height == live.Height)
    {
        if (result.Dirty)
        {
            result.DirtyMinX = std::min(result.DirtyMinX, live.DirtyMinX);
            result.DirtyMinZ = std::min(result.DirtyMinZ, live.DirtyMinZ);
            result.DirtyMaxX = std::max(result.DirtyMaxX, live.DirtyMaxX);
            result.DirtyMaxZ = std::max(result.DirtyMaxZ, live.DirtyMaxZ);
        }
        else
        {
            result.DirtyMinX = live.DirtyMinX;
            result.DirtyMinZ = live.DirtyMinZ;
            result.DirtyMaxX = live.DirtyMaxX;
            result.DirtyMaxZ = live.DirtyMaxZ;
        }
        result.Dirty = true;
    }
    live = std::move(result);
}

} // namespace GameEngine::TerrainECS
