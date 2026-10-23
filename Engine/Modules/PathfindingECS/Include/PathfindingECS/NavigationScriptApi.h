#pragma once

// The Navigation script API: what a game asks of a NavigationSurface and of a NavigationAgent. Every
// marked declaration is generated into the C# binding (Navigation, and entity.Navigation); a native
// script calls the same functions. Points and boxes are in world meters; +Y is up.

#include "ECS/ECS.h"
#include "Mathematics/Vector3.h"

#include <cstdint>

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Navigation
{

/// Where an agent's order stands.
// @ge-script
enum class AgentState : std::uint8_t
{
    /// The agent has no order: it was never given one, or ClearDestination dropped it.
    Idle = 0,
    /// The agent has an order and stands: its path comes on a later frame, or Reason names what
    /// it waits for.
    Waiting = 1,
    /// The agent walks its path to the destination.
    Walking = 2,
    /// The agent reached its destination, within its Stopping Distance. With PartialPath, it
    /// reached the reachable point nearest the destination.
    Arrived = 3,
    /// The agent cannot reach its destination; Reason says why. A NoRoute order tries again a few
    /// times a second.
    Failed = 4,
};

/// Why an agent waits or failed. None while it walks, has arrived or has no order, and while its
/// order waits only for its path.
// @ge-script
enum class StatusReason : std::uint8_t
{
    /// Nothing holds the order back.
    None = 0,
    /// The agent's Surface names an entity with no NavigationSurface, or Surface is empty and the
    /// scene has no surface or several. The order waits, and goes on once the agent has a surface.
    NoSurface = 1,
    /// The surface has no baked tiles yet. The order waits, and PendingTiles counts the tiles still
    /// to bake.
    SurfaceNotBaked = 2,
    /// The destination has no walkable ground within 5 m above or below it and twice the agent's
    /// Radius across.
    DestinationOffSurface = 3,
    /// The agent stands too far from walkable ground.
    StartOffSurface = 4,
    /// No walkable ground connects the agent to the destination.
    NoRoute = 5,
    /// The agent has a dynamic physics body, which would fight the movement: make the body
    /// kinematic or remove it. The order waits, and goes on once the body is kinematic.
    DynamicBody = 6,
};

/// An agent's order: where it stands, why it waits or failed, whether its path ends short of the
/// destination, and how many surface tiles it waits for.
// @ge-script
struct AgentStatus
{
    /// Where the order stands.
    AgentState State = AgentState::Idle;
    /// Why the order waits or failed; None otherwise.
    StatusReason Reason = StatusReason::None;
    /// The path ends at the reachable point nearest the destination, not at the destination.
    bool PartialPath = false;
    /// The surface tiles left to bake while the order waits with SurfaceNotBaked; 0 otherwise.
    std::uint32_t PendingTiles = 0;
};

/// Orders the entity's NavigationAgent to walk to `worldPoint`, in place of any order it has. The
/// agent walks on the surface its Surface names, or on the only surface in the scene when Surface is
/// empty. The order is always stored: GetStatus says, on this frame and later ones, whether it waits,
/// walks, arrived or failed, and why. The destination moves onto the walkable ground under or beside
/// it; when there is none within 5 m above or below and twice the agent's Radius across, the order
/// fails with DestinationOffSurface. Returns false only when the entity has no NavigationAgent.
// @ge-script @ge-context
bool SetDestination(ECS::World& world, ECS::EntityHandle agent, Mathematics::Vector3 worldPoint);

/// Stops the entity's NavigationAgent where it stands and drops its order: the agent is Idle.
// @ge-script @ge-context
void ClearDestination(ECS::World& world, ECS::EntityHandle agent);

/// The order of the entity's NavigationAgent. Idle, with Reason None, when it has no order or the
/// entity has no agent. Read it each frame: an order is solved on a later frame than the one that
/// gave it.
// @ge-script @ge-context
AgentStatus GetStatus(ECS::World& world, ECS::EntityHandle agent);

/// Asks the entity's NavigationSurface to bake again from the current terrain, static collider
/// shapes and mesh renderers. The bake starts on the next frame; a mesh keeps its current tiles until
/// each new one is built. Returns false when the entity has no
/// NavigationSurface.
// @ge-script @ge-context
bool RequestBake(ECS::World& world, ECS::EntityHandle surface);

/// Rebakes the tiles of the entity's mesh NavigationSurface under the box from `min` to `max` (X
/// and Z; a tile spans every height, so Y is not read), for a change the surface does not watch:
/// geometry a game placed or moved that is not a carving obstacle. The surface watches the terrain
/// and its carving obstacles itself. The tiles join the surface's dirty queue, also while it bakes
/// or loads; a few a frame, each whose content changed is rebuilt and replaces the old one, which
/// serves until then, and agents whose path crosses a replaced tile replan. Returns false when the
/// entity has no NavigationSurface in Mesh mode: a grid surface rebakes through RequestBake only.
// @ge-script @ge-context
bool MarkDirty(ECS::World& world, ECS::EntityHandle surface, Mathematics::Vector3 min, Mathematics::Vector3 max);

} // namespace GameEngine::Navigation
