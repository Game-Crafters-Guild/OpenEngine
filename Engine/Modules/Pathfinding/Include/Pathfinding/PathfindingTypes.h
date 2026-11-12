#pragma once

#include "Mathematics/Quaternion.h"
#include "Mathematics/WorldPosition.h"
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

// Tiles on a navigation chart's side: a chart is kNavChartTilesPerSide x kNavMeshTileCells cells
// square (2,048 m at a 0.5 m cell), so its kNavChartTilePool tiles fill Detour's tile pool exactly.
inline constexpr uint32 kNavChartTilesPerSide = 64;
inline constexpr uint32 kNavChartTilePool = kNavChartTilesPerSide * kNavChartTilesPerSide;

// The unit that owns one Detour mesh: a frame (origin and rotation) and a span. Detour runs in the
// chart's own coordinates with its origin at zero, so every tile's vertices are floats within a
// span of zero wherever the chart sits. Rotation is the frame's orientation; a chart is
// built only with the identity rotation, the frame of a flat world (DetourNavMap::Initialize
// refuses another).
struct NavMeshChart
{
    uint32 Id = 0;
    // Where the chart's frame sits in the world, exactly.
    Mathematics::WorldPosition Origin;
    Mathematics::Quaternion Rotation;
    // Metres along each side.
    float32 Span = 0.0f;

    // World point to chart-local, and back. Both subtract or add the origin in double.
    void ToChart(float64 x, float64 y, float64 z, float32 out[3]) const
    {
        out[0] = static_cast<float32>(x - Origin.X);
        out[1] = static_cast<float32>(y - Origin.Y);
        out[2] = static_cast<float32>(z - Origin.Z);
    }
    void ToWorld(const float32 local[3], float64 out[3]) const
    {
        out[0] = Origin.X + local[0];
        out[1] = Origin.Y + local[1];
        out[2] = Origin.Z + local[2];
    }
    bool HasIdentityRotation() const
    {
        const auto& q = Rotation.GetGLM();
        return q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 1.0f;
    }
    bool operator==(const NavMeshChart& other) const
    {
        return Id == other.Id && Origin == other.Origin && Span == other.Span &&
               Rotation.GetGLM() == other.Rotation.GetGLM();
    }
};

// A navigation mesh tile: its chart and its signed chart-local tile coordinates. Tile (x, z)
// covers chart-local [x, x + 1) x [z, z + 1) tile widths on X and Z. The one key the dirty queue,
// the swap list, the file and the counters speak.
struct NavTileKey
{
    uint32 Chart = 0;
    int32 X = 0;
    int32 Z = 0;

    bool operator==(const NavTileKey&) const = default;
};

struct NavTileKeyHash
{
    std::size_t operator()(const NavTileKey& key) const
    {
        const uint64 xz = (static_cast<uint64>(static_cast<uint32>(key.X)) << 32) | static_cast<uint32>(key.Z);
        return std::hash<uint64>{}(xz ^ (static_cast<uint64>(key.Chart) * 0x9E3779B97F4A7C15ull));
    }
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

// How a grid A* solve ended.
enum class GridSolveEnd : uint8
{
    Reached,     ///< A path reaches the goal cell.
    Refused,     ///< The start or the goal is off the grid or on a blocked cell; no search.
    OtherRegion, ///< The goal is outside the start's region; no search.
    Unreachable  ///< No step joins the start to the goal: the search ran out of cells from the start or from the goal.
};
inline constexpr uint32 kGridSolveEndCount = 4;

// What one grid A* solve did. Cells are grid coordinates; a solve refused
// before its ends map to cells leaves them 0.
struct GridSolveRecord
{
    uint32 StartX = 0, StartZ = 0;
    uint32 GoalX = 0, GoalZ = 0;
    uint32 Expansions = 0;      ///< Cells the search closed, from the start and from the goal.
    uint32 Microseconds = 0;    ///< Wall time of the solve, path smoothing included.
    uint32 AgentCellRadius = 0; ///< The footprint radius the search tested, in cells; 0 tests the cell only.
    GridSolveEnd End = GridSolveEnd::Refused;
};

// One grid path solve for SolveGridPaths: the map and request it reads, and the
// result it writes: its status, for a Complete path its points, and the record
// of the search.
struct GridPathSolve
{
    NavMapHandle Map;
    PathRequest Request;
    PathStatus Status = PathStatus::Invalid;
    std::vector<PathPoint> Points;
    GridSolveRecord Record;
};

} // namespace GameEngine::Pathfinding
