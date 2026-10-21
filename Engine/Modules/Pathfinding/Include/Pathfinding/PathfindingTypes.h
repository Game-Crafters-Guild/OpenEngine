#pragma once

#include "Types/Types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace GameEngine::Pathfinding
{

enum class PathStatus : uint8
{
    Invalid,
    Pending,
    Complete,
    Partial,
    Failed
};

enum class GridType : uint8
{
    Square,
    Hex
};

struct PathPoint
{
    float32 X = 0.0f;
    float32 Y = 0.0f;
    float32 Z = 0.0f;
};

struct PathHandle
{
    uint32 Index = 0;
    uint32 Generation = 0;
    bool IsValid() const { return Generation != 0; }
};

struct PathRequest
{
    float32 StartX = 0.0f, StartY = 0.0f, StartZ = 0.0f;
    float32 EndX = 0.0f, EndY = 0.0f, EndZ = 0.0f;
    float32 AgentRadius = 0.5f;
    float32 AgentHeight = 2.0f;
    uint32 ExcludeAgentId = 0; // reservation owner to ignore during A*
    float32 CurrentTime = 0.0f; // game time for reservation expiration checks
    bool SmoothPath = true;    // Apply string-pulling to simplify grid paths
};

struct GridSettings
{
    GridType Type = GridType::Square;
    float32 CellSize = 1.0f;
    float32 OriginX = 0.0f, OriginY = 0.0f, OriginZ = 0.0f;
    uint32 Width = 64;
    uint32 Depth = 64;
    float32 MaxSlope = 45.0f;
    float32 MaxStepHeight = 0.4f;
};

// The agent a navigation mesh is built for and its one resolution dial. Recast's other parameters
// are derived from these (DetourNavMap.cpp, MakeRecastConfig): the cell height, the region and edge
// limits and the detail sampling all scale with CellSize.
struct NavMeshSettings
{
    float32 CellSize = 0.5f;    // metres per voxel column side
    float32 AgentRadius = 0.5f; // metres
    float32 AgentHeight = 1.8f; // metres
    float32 MaxSlope = 45.0f;   // degrees from horizontal
    float32 StepHeight = 0.4f;  // metres an agent climbs without a slope
};

// The height an agent climbs between two points `distance` metres apart: a step up to stepHeight,
// or the rise of a maxSlope slope over that distance, whichever is more. Max Slope governs slopes and
// Step Height governs steps: a surface sampled at cells cannot tell a slope from a staircase of
// cell-sized steps, so the climb between neighbouring cells has to allow the slope's rise.
inline float32 WalkableClimb(float32 stepHeight, float32 maxSlopeDegrees, float32 distance)
{
    constexpr float32 kDegreesToRadians = 3.14159265358979323846f / 180.0f;
    return std::max(stepHeight, std::tan(maxSlopeDegrees * kDegreesToRadians) * distance);
}

// Cells on a navigation mesh tile's side: a tile is kNavMeshTileCells x CellSize metres square.
inline constexpr uint32 kNavMeshTileCells = 64;

// How a navigation mesh is cut into Detour tiles: tile (0, 0)'s minimum corner in world metres and
// the tile count along X and Z. OriginY is the lowest height any tile is built at.
struct NavMeshTileLayout
{
    float32 OriginX = 0.0f;
    float32 OriginY = 0.0f;
    float32 OriginZ = 0.0f;
    uint32 TilesX = 0;
    uint32 TilesZ = 0;
};

struct NavMapHandle
{
    uint32 Index = 0;
    uint32 Generation = 0;
    bool IsValid() const { return Generation != 0; }
    bool operator==(const NavMapHandle& other) const { return Index == other.Index && Generation == other.Generation; }
    bool operator!=(const NavMapHandle& other) const { return !(*this == other); }
};

struct NavMapHandleHash
{
    std::size_t operator()(const NavMapHandle& h) const
    {
        return std::hash<uint64>{}(static_cast<uint64>(h.Index) | (static_cast<uint64>(h.Generation) << 32));
    }
};

struct NavLinkId
{
    uint32 Index = 0;
    uint32 Generation = 0;
    bool IsValid() const { return Generation != 0; }
};

struct NavLink
{
    NavMapHandle SourceMap;
    float32 SourceX = 0.0f, SourceY = 0.0f, SourceZ = 0.0f;
    NavMapHandle TargetMap;
    float32 TargetX = 0.0f, TargetY = 0.0f, TargetZ = 0.0f;
    float32 TraversalCost = 1.0f;
    bool Bidirectional = true;
};

struct CellReservation
{
    uint32 AgentId = 0;
    float32 ExpirationTime = 0.0f;
    // 1 = agent body is in this cell now. A* taxes it; movement refuses
    // it (except dest stand-off). 0 = lookahead tax only.
    uint8 Occupancy = 0;
    bool IsActive(float32 currentTime) const { return AgentId != 0 && ExpirationTime > currentTime; }
};

// One grid path solve for SolveGridPaths: the map and request it reads, and the
// result it writes, its status and, for a Complete path, its points.
struct GridPathSolve
{
    NavMapHandle Map;
    PathRequest Request;
    PathStatus Status = PathStatus::Invalid;
    std::vector<PathPoint> Points;
};

} // namespace GameEngine::Pathfinding
