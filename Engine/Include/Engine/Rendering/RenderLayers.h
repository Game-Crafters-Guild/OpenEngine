#pragma once

#include "Types/Types.h"

#include <optional>
#include <string>
#include <string_view>

// The project's render layer names, as a game reads them. The names are set in Settings > Project >
// Render Layers and ship with the game; scenes store layer numbers, so a rename never touches a scene.
// Call these on the main thread or at a system's start, never per frame.
namespace GameEngine::Rendering
{

/// The layer (0 to 31) that is called `name`, or no value when no layer has that name. The match is
/// exact and case-sensitive, against the names the editor shows: an unnamed layer answers to "Default"
/// (layer 0) or "Layer N".
std::optional<uint32> FindRenderLayer(std::string_view name);

/// The name of `layer` (0 to 31): its project name, else "Default" for layer 0 and "Layer N" for the
/// others. A copy, so a later rename does not change it.
std::string RenderLayerName(uint32 layer);

} // namespace GameEngine::Rendering
