#pragma once

#include "Pathfinding/PathfindingTypes.h"
#include "Types/StringId.h"
#include "Types/Types.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::Pathfinding
{

// The .navsurface container: a navigation surface's baked tiles with what they were baked for.
//
// On disk, little-endian and packed: the header; the profile table; the area table; the 16 flag
// names; per profile, the chart table; per chart, the index of its tiles in Morton order of their
// coordinates, so tiles near each other on the ground are near each other in the file; then the
// tile bytes exactly as Detour built them, chart-local, added to a mesh unchanged. Offsets are
// from the start of the file. A loader reads only kNavSurfaceFormatVersion: a file of any other
// version is refused, and the surface bakes again.

/// The one format version a reader accepts. Changes with every change to the layout or to what
/// decides a tile's bytes or its content hash, so an older file is refused rather than misread.
inline constexpr uint32 kNavSurfaceFormatVersion = 1u;
/// Bits a polygon's flags hold, one name per bit in the flag-name table.
inline constexpr uint32 kNavSurfaceFlagBits = 16u;
/// Detour's area ids: 0 is the null area (not walkable, what a carve marks) and 63 the default
/// walkable area; 1 to 62 are a project's own.
inline constexpr uint8 kNavAreaNull = 0u;
inline constexpr uint8 kNavAreaWalkable = 63u;

/// Whether the file is a cooked surface, complete over its bounds, or a cache of tiles baked on
/// demand, whose index holds only the tiles baked so far.
enum class NavSurfaceLifecycle : uint8
{
    Cooked = 0,
    Cache = 1,
};

/// One baked tile: its chart-local coordinates, its Detour bytes and the hash of the content it
/// was built from (the terrain and the sources under it), never of a counter or a handle.
struct NavSurfaceTile
{
    int32 X = 0;
    int32 Z = 0;
    uint32 PolygonCount = 0;
    uint64 ContentHash = 0;
    std::vector<uint8> Data;
};

struct NavSurfaceChart
{
    NavMeshChart Chart;
    std::vector<NavSurfaceTile> Tiles;
};

/// The agent a profile's tiles are baked for; one profile, Default (id 0), until surfaces bake
/// for several agents.
struct NavSurfaceProfile
{
    uint32 Id = 0;
    float32 Radius = 0.0f;
    float32 Height = 0.0f;
    float32 MaxSlope = 0.0f;
    float32 StepHeight = 0.0f;
    bool AlwaysBake = false;
    uint64 SettingsHash = 0;
    std::vector<NavSurfaceChart> Charts;
};

/// A traversal area the tiles' polygons name by id, with its cost and flags before a game
/// changes them.
struct NavSurfaceArea
{
    uint8 Id = 0;
    StringId Name = 0;
    float32 DefaultCost = 1.0f;
    uint16 DefaultFlags = 0;
};

struct NavSurfaceData
{
    /// Hash of the surface-wide settings the tiles were baked with (the cell size and the mode).
    uint64 BakeSettingsHash = 0;
    uint16 TileCells = static_cast<uint16>(kNavMeshTileCells);
    NavSurfaceLifecycle Lifecycle = NavSurfaceLifecycle::Cooked;
    std::vector<NavSurfaceProfile> Profiles;
    std::vector<NavSurfaceArea> Areas;
    /// One name per flag bit; 0 for an unnamed bit.
    std::array<StringId, kNavSurfaceFlagBits> FlagNames{};
};

/// The area table every surface carries today: the null area and the default walkable area.
std::vector<NavSurfaceArea> DefaultNavSurfaceAreas();

/// The flag names every surface carries today: bit 0 is Walk, the flag every baked polygon has.
std::array<StringId, kNavSurfaceFlagBits> DefaultNavSurfaceFlagNames();

/// Writes `data` as a .navsurface file into `out`. False when a count does not fit its field.
bool WriteNavSurface(const NavSurfaceData& data, std::vector<uint8>& out);

/// Reads a .navsurface file. False, with `outError` saying why and `out` left empty, when the
/// bytes are not a .navsurface file, are of another format version, or are truncated or
/// inconsistent. Never throws.
bool ReadNavSurface(std::span<const uint8> bytes, NavSurfaceData& out, std::string& outError);

} // namespace GameEngine::Pathfinding
