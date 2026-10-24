#pragma once

// The RenderLayers script API: the project's render layers by name. The names are set in Settings >
// Project > Render Layers and ship with the game; scenes store layer numbers, so a rename never touches a
// scene. Every marked declaration is generated into the C# binding (RenderLayers); a native script calls
// the same functions. The bodies are in Engine (Engine/Source/Engine/Rendering/RenderLayerNames.cpp),
// beside the name table they read.

#include "Rendering/RenderLayerMask.h"

#include <string>
#include <string_view>

namespace GameEngine::Rendering::Script
{

/// The layer called `name`, as a mask of that one layer, or no layer (RenderLayerMask.None) when no
/// layer has that name. The match is exact and case-sensitive, against the names the editor shows: an
/// unnamed layer answers to "Default" (layer 0) or "Layer N". Look a name up once, at a system's start,
/// and keep the mask.
// @ge-script @ge-engine-thread
RenderLayerMask Find(std::string_view name);

/// The name of the one layer in `layer`: its project name, else "Default" for layer 0 and "Layer N" for
/// the others. A mask of no layer or of several layers has no name: the text is empty.
// @ge-script @ge-engine-thread
std::string NameOf(RenderLayerMask layer);

} // namespace GameEngine::Rendering::Script
