#pragma once

#include <cstdint>
#include <filesystem>

namespace GameEngine
{
class AssetManager;
namespace ECS { class World; }
} // namespace GameEngine

namespace GameEngine::Editor
{

// The scene save's navigation step: writes each baked mesh surface's tiles (.navsurface) beside
// the scene and points the surface's BakedData at the file, so the saved scene loads its tiles
// instead of baking them. The file is named after the scene and the surface's scene tag. A surface
// whose BakedData names this scene's file, which no other surface writes, rewrites it, and only when
// its bytes changed; a copied surface and a scene saved under a new name get their own. Called
// before the scene file is written, so the reference is saved with it. Returns the number of files
// written.
uint32_t WriteNavigationBakedDataForSave(ECS::World& world, const std::filesystem::path& scenePath,
                                         AssetManager& assets);

} // namespace GameEngine::Editor
