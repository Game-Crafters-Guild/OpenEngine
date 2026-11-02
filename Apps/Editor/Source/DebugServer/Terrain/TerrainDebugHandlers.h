#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

void RegisterTerrainDebugHandlers(EditorDebugServer& server, EditorApplication& app);

/// Registers planet_page_overlay: a raised-cosine bump on one face of the active planet's height
/// pages (TerrainService::WritePlanetPageLayer), or its removal (TerrainService::ClearPlanetPageLayer).
void RegisterPlanetPageOverlayHandler(EditorDebugServer& server);

} // namespace GameEngine
