#pragma once

#include "Editor/Registries/StatusBarItemRegistry.h"

#include <cstddef>
#include <functional>
#include <string>

namespace GameEngine::Editor
{

/// What the status bar's texture item counts.
struct TexturePreparingCounts
{
    /// Textures whose import cook runs now (TextureCookActivity).
    size_t Cooking = 0;
    /// Distinct textures a material binding still waits on (TextureService), which covers the
    /// loads still queued, the cooks running and the uploads not yet bound.
    size_t AwaitedByMaterials = 0;
};

/// "Preparing N textures", with the tooltip "N textures still preparing; clears when done",
/// while any texture cooks or any material waits on one; empty otherwise. N is the larger
/// count: each is a lower bound on the textures not yet ready, and a texture can be in both.
StatusBarItemDisplay TexturePreparingStatusDisplay(const TexturePreparingCounts& counts);

/// The status bar item that reads `counts` at each poll.
StatusBarItem MakeTexturePreparingStatusItem(std::function<TexturePreparingCounts()> counts);

/// Registers the item over the engine's counts: the asset manager's cooks and the render
/// services' pending material texture binds.
void RegisterTexturePreparingStatusItem(StatusBarItemRegistry& registry);

} // namespace GameEngine::Editor
