#pragma once

#include "Types/Types.h"

#include <string>

namespace GameEngine
{
namespace ECS
{
class World;
struct EntityHandle;
} // namespace ECS

namespace Components
{
struct Terrain;
struct TerrainPlanetRelief;
} // namespace Components

namespace CBTTerrainECS
{
enum class TerrainPreset : uint8;
} // namespace CBTTerrainECS

namespace Editor
{
class EditorChangeNotifications;
class UndoRedoService;

// The engine's terrain provisioning (TerrainECS::ProvisionTerrainEntity), shared by the create
// menu and the create_terrain / create_entity IPC, with the default surface-rules volume it may
// spawn placed at the bottom of the hierarchy. Returns the refusal, empty when the terrain was
// provisioned.
std::string ProvisionTerrainEntity(ECS::World& world, ECS::EntityHandle entity,
                                   const Components::Terrain& config,
                                   const Components::TerrainPlanetRelief& relief);

// Makes the Create menu's terrain at the origin, named "Terrain" ("Planet" when spherical), and switches
// off each entity that has a TemplateGroundPlaceholder, with a notice that can switch it back on. With
// `undo` set, it is one undo step. Returns the new entity, invalid if none was made.
ECS::EntityHandle CreateTerrainEntity(ECS::World& world, UndoRedoService* undo,
                                      EditorChangeNotifications* notifications, CBTTerrainECS::TerrainPreset preset);

// Makes "Terrain Noise" (TerrainECS::SpawnGeneratedNoiseVolume), the relief of every Flat planar terrain, at
// the bottom of the hierarchy. With `undo` set, it is one undo step. Returns the volume, invalid if none was made.
ECS::EntityHandle CreateGeneratedNoiseVolume(ECS::World& world, UndoRedoService* undo,
                                             EditorChangeNotifications* notifications);

} // namespace Editor
} // namespace GameEngine
