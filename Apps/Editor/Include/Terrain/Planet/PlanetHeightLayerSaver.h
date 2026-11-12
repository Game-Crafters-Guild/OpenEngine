#pragma once

#include "Terrain/TerrainZonePayloadSaver.h"

#include <cstdint>
#include <filesystem>

namespace GameEngine::ECS { class World; }
namespace GameEngine { class AssetRegistry; }
namespace GameEngine::TerrainECS { class TerrainService; }

namespace GameEngine::Editor
{

class EditorChangeNotifications;

/// Writes the active planet's changed sculpt as its height layer (.gepage) with the zones' `mode`
/// contract: the first Save mints <sceneStem>_Zones/<guid>.gepage and sets Terrain.HeightLayerGuid; a
/// backup stages only an existing file; Save of an empty sculpt removes the file and the reference.
/// A change of the reference asks `notifications` (when set) to rebuild the Terrain inspector rows.
/// Returns the number of files written or removed (0 or 1).
uint32_t FlushPlanetHeightLayer(ECS::World& world, const std::filesystem::path& scenePath, AssetRegistry& registry,
                                TerrainECS::TerrainService& service, ZonePayloadFlushMode mode,
                                ZonePayloadStagingLedger* stagingLedger, EditorChangeNotifications* notifications);

} // namespace GameEngine::Editor
