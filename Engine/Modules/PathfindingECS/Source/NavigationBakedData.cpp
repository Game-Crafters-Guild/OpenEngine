#include "NavigationBakedData.h"

#include "PathfindingECS/Components/NavigationSurface.h"

#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/NavSurfaceFile.h"
#include "Types/Fnv1a.h"

#include <vector>

namespace GameEngine::PathfindingECS
{

namespace
{

using Components::NavigationSurface;

// The surface-wide settings a tile's bytes depend on besides the agent: the cell size and the mode.
uint64 BakeSettingsHash(const NavigationSurface& surface)
{
    uint64 hash = Hashing::Fnv1a64Value(Hashing::kFnv1a64OffsetBasis, surface.CellSize);
    return Hashing::Fnv1a64Value(hash, Components::NavigationSurfaceMode::Mesh);
}

// The agent values a profile's tiles depend on.
uint64 ProfileSettingsHash(const NavigationSurface& surface)
{
    uint64 hash = Hashing::kFnv1a64OffsetBasis;
    for (const float32 value : {surface.AgentRadius, surface.AgentHeight, surface.MaxSlope, surface.StepHeight})
        hash = Hashing::Fnv1a64Value(hash, value);
    return hash;
}

} // namespace

Pathfinding::NavSurfaceData MakeNavigationBakedData(const NavigationSurface& surface,
                                                    const Pathfinding::DetourNavMap& mesh)
{
    Pathfinding::NavSurfaceData data;
    data.BakeSettingsHash = BakeSettingsHash(surface);
    data.Areas = Pathfinding::DefaultNavSurfaceAreas();
    data.FlagNames = Pathfinding::DefaultNavSurfaceFlagNames();
    Pathfinding::NavSurfaceProfile& profile = data.Profiles.emplace_back();
    profile.Radius = surface.AgentRadius;
    profile.Height = surface.AgentHeight;
    profile.MaxSlope = surface.MaxSlope;
    profile.StepHeight = surface.StepHeight;
    profile.SettingsHash = ProfileSettingsHash(surface);
    Pathfinding::NavSurfaceChart& chart = profile.Charts.emplace_back();
    chart.Chart = mesh.GetChart();

    std::vector<Pathfinding::NavMeshTileData> tiles;
    mesh.CopyTiles(tiles);
    chart.Tiles.reserve(tiles.size());
    for (Pathfinding::NavMeshTileData& tile : tiles)
    {
        Pathfinding::NavSurfaceTile& saved = chart.Tiles.emplace_back();
        saved.X = tile.Key.X;
        saved.Z = tile.Key.Z;
        saved.PolygonCount = tile.PolygonCount;
        saved.ContentHash = tile.ContentHash;
        saved.Data = std::move(tile.Data);
    }
    return data;
}

bool NavigationBakedDataMatches(const Pathfinding::NavSurfaceData& data, const NavigationSurface& surface,
                                const Pathfinding::NavMeshChart& chart)
{
    return data.BakeSettingsHash == BakeSettingsHash(surface) && data.Profiles.size() == 1 &&
           data.Profiles[0].SettingsHash == ProfileSettingsHash(surface) && data.Profiles[0].Charts.size() == 1 &&
           data.Profiles[0].Charts[0].Chart == chart;
}

} // namespace GameEngine::PathfindingECS
