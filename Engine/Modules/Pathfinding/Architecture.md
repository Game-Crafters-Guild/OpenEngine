# Pathfinding Module Architecture

## Overview

The pathfinding system is split into two modules following the Physics/PhysicsECS pattern:

- **Pathfinding** (`Engine/Modules/Pathfinding/`) -- Core library. No ECS dependency. Owns navigation maps, solvers, path storage, and obstacle avoidance.
- **PathfindingECS** (`Engine/Modules/PathfindingECS/`) -- ECS integration. Components, systems, and the `NavigationService` singleton that bridges the core library to the entity world.

### Dependency Chain

```
Pathfinding --> Logger, Types, Mathematics, JobSystem
  + optional: RecastNavigation (Recast, Detour, DetourCrowd) via GE_PATHFINDING_BACKEND_DETOUR

PathfindingECS --> Pathfinding, ECS, ECSComponents, Types, Mathematics, Logger
```

Both modules are linked into the `Engine` target.

---

## Core Module (`Pathfinding`)

### Directory Layout

```
Include/Pathfinding/
  PathfindingTypes.h       Types, enums, handles (PathPoint, PathHandle, NavMapHandle, NavLink, etc.)
  PathBuffer.h             Pool of variable-length path buffers (mutex-guarded)
  INavigationMap.h         Abstract interface for all navigation maps
  GridMap.h                Square + hex grid map (costs, heights, blocked, reservations)
  DetourNavMap.h           Recast baking + Detour runtime queries (Pimpl)
  ObstacleAvoidance.h      CrowdManager wrapping dtCrowd
  NavigationWorld.h        Facade: owns maps, PathBuffer, CrowdManager, NavLinks, batched grid solves

Source/
  PathBuffer.cpp
  GridMap.cpp
  AStarSolver.h / .cpp     Private. A* with scratch reused across solves; GridMap keeps one solver per thread.
  HexGridHelpers.h / .cpp  Private. Odd-r offset hex coordinate math.
  DetourNavMap.cpp          Full Recast pipeline + Detour queries (behind #if guard)
  ObstacleAvoidance.cpp     dtCrowd wrapper (behind #if guard)
  NavigationWorld.cpp       Facade with map/link management, path queries, the parallel grid solve
```

### Key Abstractions

#### INavigationMap

Abstract interface implemented by all navigation map types.

```
FindPath(request, pathBuffer, outPath) --> PathStatus
IsPointNavigable(x, y, z, radius) --> bool
GetClosestNavigablePoint(x, y, z, searchRadius, out) --> bool
Raycast(start, end, hitPoint) --> bool
```

#### GridMap

In-house grid-based navigation map. Supports both square (8-neighbor) and hex (6-neighbor, odd-r offset) grids.

**Data layout**: Three flat contiguous arrays indexed by `cellZ * width + cellX`:
- `m_Costs` (`float32`) -- per-cell traversal cost multiplier (default 1.0)
- `m_Heights` (`float32`) -- per-cell world Y height
- `m_Blocked` (`uint8`) -- per-cell blocked flag

**Reservations**: A parallel flat array `m_Reservations` stores `(agentId, expirationTime)` per cell. Agents reserve cells along their path within a time horizon. The A* solver adds a penalty cost for cells reserved by other agents (expensive, not impassable). A `shared_mutex` protects the reservation array: writers (reserve/clear) take it exclusive, `GetCellReservation` and `IsOccupiedByOther` take it shared per call, and a path-solve window takes it shared once for all of its solves.

**Read view**: path solves read the map through `GridMap::ReadView`, a value holding the settings and raw pointers to the five arrays, with inline accessors and no lock. `View()` hands one out only inside a path-solve window (`OpenPathSolveWindow` / `ClosePathSolveWindow`; `FindPath` opens one per solve, `NavigationWorld::SolveGridPaths` one per map per batch on the calling thread, held across the join). The window holds the reservation lock shared, and every writer asserts (Debug, DebugFast) while one is open; `AStarSolver` and `PathSmoother` take the view, not the map.

**Coordinate conversion**: `WorldToCell` / `CellToWorld` handle both square and hex layouts. Hex uses odd-r offset coordinates with flat-top orientation.

**Baking**: the grid holds what it is given. `NavigationSurfaceSystem` (PathfindingECS) bakes a grid surface by writing each cell's height and blocked flag from the terrain's heightfield and the static geometry (`NavigationGridBake.cpp`).

#### A* Solver

Private class whose working buffers belong to the solver instance; a solve never yields, so it finishes on the thread that started it. The buffers (11 bytes per cell) grow to exactly the largest grid the solver has solved on. A `NavigationWorld` owns its grid solvers (`AStarSolverPool`): a grid solve of the world (`FindPath`, `FindPathAcrossLinks`) leases an idle solver, or a new one when every solver is leased, and returns it when it finishes; `SolveGridPaths` leases one solver per lane (at most one per pool worker plus the caller), and each lane claims the batch's solves one at a time on it. The world therefore keeps one solver per solve or lane it has run at once, and the pool mutex is taken twice per solve or lane. Removing or replacing a grid map frees the scratch of the world's idle solvers beyond what its largest remaining grid map needs, and destroying the world frees all of them; a leased solver is never trimmed, and no other world's or thread's scratch is touched. `GridMap::FindPath` called directly, on a standalone grid map or on a world's grid map through `GetGridMap`/`GetMap`, solves on the calling thread's solver (`static thread_local`). That scratch lives as long as the thread, and the world neither frees nor reports it. Solve through `NavigationWorld::FindPath` or `SolveGridPaths` to use the world's solvers. `NavigationWorld::GetSolverScratchStatistics` reports the world's solvers, the bytes they hold and the most one has held (the editor's `get_navigation_stats`, `solverScratch`).

- Open list: array-backed binary heap (`std::vector<Node>`)
- Closed set: flat `std::vector<uint8>`
- gScore + parent: flat `std::vector<float32>` / `std::vector<uint32>`
- Visit stamps: a `uint16` per cell; a cell's gScore, parent and closed flag count only when its stamp equals the solve's generation, so a solve does not clear the buffers (the stamps are cleared once every 65,535 solves)
- Square: 8 neighbors, octile heuristic, diagonal cost = 1.414
- Hex: 6 neighbors, hex distance heuristic
- Step cost = `base_distance * cell_cost * (1 + kReservationPenalty)` if reserved
- Terrain checks: slope angle vs MaxSlope, height difference vs MaxStepHeight
- Expansion limit: `cellCount` (each cell is expanded at most once, so a path that exists is always found; an unreachable goal on a 1024 x 1024 grid costs about 210 ms in Release)

#### DetourNavMap

Wraps Recast (baking) and Detour (runtime queries) behind a Pimpl.

- `Initialize(settings, layout)` -- an empty tiled dtNavMesh: tiles of `kNavMeshTileCells` (64) cells, the agent and the one `CellSize` dial in `NavMeshSettings`; every other Recast parameter is derived from them
- `BuildTile(settings, layout, x, z, geometry)` -- the Recast pipeline for one tile (heightfield, filtering, compact heightfield, erosion, regions, contours, poly mesh, detail mesh, Detour tile data); reads only its arguments, so it runs on job threads
- `AddTile` / `RemoveTile` -- swap one tile in the mesh on the querying thread
- `FindPath` -- `dtNavMeshQuery::findPath` + `findStraightPath`, results stored in PathBuffer
- Debug mesh extraction for visualization

All behind `#if GE_PATHFINDING_BACKEND_DETOUR`. When disabled, all methods return false/empty.

#### CrowdManager

Wraps Detour's `dtCrowd` for navmesh obstacle avoidance.

- `Initialize(navMap, maxAgents)` -- sets up dtCrowd with 4 quality presets
- `AddAgent` / `RemoveAgent` -- manage crowd agents
- `SetAgentTarget` / `GetAgentTarget` -- destination management
- `GetAgentPosition` / `GetAgentVelocity` -- read simulation results
- `Update(deltaTime)` -- steps the crowd simulation

#### NavigationWorld

Top-level facade that owns everything.

- **Map management**: `AddGridMap`, `AddDetourNavMap`, `RemoveMap` with generational handle validation
- **Sync queries**: `FindPath(mapHandle, request, outPath)` -- blocks, for editor/tools
- **Batched grid solves**: `SolveGridPaths(pool, solves)` -- solves every grid request with `JobSystem::ParallelFor` on the given pool, one unit per lane, each lane on one leased solver claiming solves one at a time (null pool: on the caller, in order) and returns once all are done; each solve writes only its own status and points, so the caller allocates the paths in its own order. `HasActivePathJobs()` is true only during this call, and every `GridMap` writer and every map add, remove or replace asserts (Debug, DebugFast) while it runs
- **Cross-map**: `FindPathAcrossLinks(request, outPath)` -- cheapest map sequence over the links (Dijkstra on `TraversalCost`), then solves per segment and joins them; blocks
- **NavLinks**: `AddNavLink`, `RemoveNavLink`, `GetNavLink`, `ForEachNavLink`, `AutoGenerateBoundaryLinks`
- **PathBuffer**: Thread-safe pool of variable-length path buffers

#### PathBuffer

Pool that stores full waypoint sequences. Each path is a `std::vector<PathPoint>` in a slot with a generation counter. A `std::mutex` guards all operations.

- `AllocatePath(points, count)` --> `PathHandle` (index + generation)
- `FreePath(handle)` -- returns slot to free list
- `GetPoint(handle, index)` -- returns a copy (safe across threads)
- `CopyPoints(handle, outBuffer, capacity)` -- bulk copy into caller-owned buffer
- `GetPointCount(handle)` -- count of waypoints

---

## ECS Module (`PathfindingECS`)

### Directory Layout

```
Include/PathfindingECS/
  NavigationService.h                Singleton service + NavDebugData struct
  Components/
    NavigationAgent.h                Agent config, path state, velocity, crowd handle
    NavigationSurface.h              A walkable surface (Mesh or Grid), its bake settings and bake state
    NavigationObstacle.h             Dynamic obstacle marker
    NavigationDebugSettings.h        Debug visualization toggles
  Systems/
    NavigationSurfaceSystem.h             Bakes navigation surfaces
    NavigationPathfindingSystem.h           Computes paths for agents
    NavigationMovementSystem.h              Steers agents along paths
    NavigationDebugSystem.h             Generates debug visualization data
    RegisterPathfindingSystems.h     Schedule registration

Source/
  NavigationService.cpp
  NavigationSourceCollector.cpp      What a surface bakes: terrain lattice, static collider shapes, mesh renderers
  NavigationMeshBake.cpp             One mesh bake: tile snapshots and their jobs
  NavigationGridBake.cpp             One grid bake
  Systems/
    NavigationSurfaceSystem.cpp
    NavigationPathfindingSystem.cpp
    NavigationMovementSystem.cpp
    NavigationDebugSystem.cpp
    RegisterPathfindingSystems.cpp
```

### Components (namespace `GameEngine::components`, all POD)

| Component | Purpose |
|-----------|---------|
| **NavigationAgent** | Speed, acceleration, radius, height, avoidance settings, nav map handle, destination, path handle, cached waypoints, velocity, crowd handle, agent ID |
| **NavigationSurface** | Mode (Mesh or Grid), agent radius/height/slope/step, cell size, bounds (fitted or explicit), authored grid cells; runtime: nav map handle, bake state |
| **NavigationObstacle** | Radius, height, enabled flag |
| **NavigationDebugSettings** | ShowGrid, ShowNavMesh, ShowPaths, ShowAgentRadii, ShowCellCosts, ShowReservations, line thicknesses |

### Systems

| System | Phase | Order | Dependencies | Purpose |
|--------|-------|-------|-------------|---------|
| **NavigationSurfaceSystem** | Early | 10 | TransformHierarchy, PhysicsWriteback, TerrainModifiers (optional), TerrainExtraction (optional) | Bakes each NavigationSurface into a GridMap or a tiled DetourNavMap (tiles on the job system), publishes its bake state, stamps obstacles into grids |
| **NavigationPathfindingSystem** | Early | 20 | NavigationSurfaceSystem | Exclusive. Queues every request (HasDestination + PathDirty, or the replan timer's ReplanRequested) and solves the oldest `MaxPathSolvesPerFrame` (default 64) each frame, the grid requests on the world's job pool and the rest on the caller, applying the results in queue order before it returns; the rest wait in order, a PathDirty request as Pending with its path released, a timer refresh still walking its current path. `GetStatistics()` reports the queue (the editor's `get_navigation_stats`) |
| **NavigationMovementSystem** | Early | 30 | NavigationPathfindingSystem | Steers agents along paths, manages reservations, reads crowd positions |
| **NavigationDebugSystem** | Render | 5 | NavigationMovementSystem | Populates NavDebugData with lines/triangles for editor consumption |

### Data Flow

```
1. Entity has NavigationSurface --> NavigationSurfaceSystem bakes its map, stores the handle
2. Entity has NavigationAgent + HasDestination=true + PathDirty (or ReplanRequested) --> NavigationPathfindingSystem queues it and computes the path
   within ceil(backlog / MaxPathSolvesPerFrame) frames, oldest request first
3. Path stored in PathBuffer, handle + cached waypoints written to NavigationAgent
4. NavigationMovementSystem steers agent toward waypoints, reserves grid cells
5. NavigationDebugSystem generates visualization --> NavDebugData --> Editor gizmos
```

### NavigationService

Singleton following `PhysicsWorldService` pattern.

- `Initialize()` / `Shutdown()` -- creates/destroys the `NavigationWorld`
- `Get()` / `TryGet()` -- access the world
- `GetGeneration()` -- monotonic counter, increments on init/shutdown
- `GetDebugData()` -- returns `NavDebugData` (lines + triangles for editor rendering)

### NavDebugData

Generated each frame by `NavigationDebugSystem`. Contains:
- **Lines**: grid edges (color-coded by cost), reservation outlines (orange), path strips (blue), agent radius circles (cyan)
- **Triangles**: navmesh faces (semi-transparent green)

The Editor layer is responsible for consuming this data and drawing it via `GizmoRenderContext`.

---

## Thread Safety

| Resource | Protection | Details |
|----------|-----------|---------|
| PathBuffer | `std::mutex` | All alloc/free/read operations lock |
| GridMap reservations | `std::shared_mutex` | A path-solve window holds it shared for all of its solves (A* reads through the `ReadView`, unlocked); `GetCellReservation` and `IsOccupiedByOther` share it per call; writers (Reserve, Clear) take it exclusive and assert (Debug, DebugFast) while a window is open |
| GridMap cell data | None; no writer inside a path-solve window | Written on the main thread outside `SolveGridPaths`; every writer asserts (Debug, DebugFast) while a window is open |
| A* solver | World: `std::mutex` per world's solver pool; outside a world: per-thread instance | A world's grid solve, or a `SolveGridPaths` lane, leases one of the world's solvers (the pool mutex is taken to lease and to return) and shares no scratch with another solve; the scratch release trims only idle solvers of that world, under the same mutex. `GridMap::FindPath` outside a world uses one `thread_local` solver per thread |
| NavigationWorld maps | None (single-threaded access assumed) | Map add/remove/replace on the main thread, gated on `HasActivePathJobs()`; `SolveGridPaths`' solves only read the map table, and each of those writers asserts (Debug, DebugFast) while it runs |
| NavigationPathfindingSystem | Exclusive update | No other managed system runs while its solves read the grids; results are applied on its own thread |

---

## Recast/Detour Integration

Added via vcpkg (`recastnavigation` port, community-maintained repo). Guarded by `GE_PATHFINDING_BACKEND_DETOUR` CMake option (ON by default).

When disabled, `DetourNavMap` and `CrowdManager` compile as stubs returning false/empty for all operations. Grid-based pathfinding works independently of this flag.

---

## Surface Baking

`NavigationSurfaceSystem` bakes a surface when it first appears (a new component, a loaded scene, a world restored from bytes whose maps were released) and on `Navigation::RequestBake`. It needs no physics world. `NavigationSourceCollector` reads three CPU sources: the first planar terrain's finest height through `TerrainECS::SurfaceHeightQuery` (`RequireFinest`: a bake waits on a height page not on the CPU yet and keeps a CPU window there until it lands), the box, sphere, capsule and plane collider shapes of `Static` bodies as triangles, and each `MeshRenderer`'s own submesh (model assets, and primitives generated from their GUID) at its `WorldTransform`. Moving bodies and navigation agents are not baked.

A mesh bake cuts the bounds into 64-cell tiles. Each update snapshots up to `kTileSnapshotsPerUpdate` tiles' terrain lattice on the main thread and builds them as jobs that own their input; finished tiles replace the old ones as they land. A grid bake builds the whole grid in one update: a cell's ground is the highest of the terrain and the up-facing static triangles under its centre, and cells steeper than `MaxSlope` to a neighbour are blocked.
