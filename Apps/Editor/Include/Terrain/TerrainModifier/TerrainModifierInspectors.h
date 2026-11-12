#pragma once

#include "Types/Types.h"

#include <string>

namespace GameEngine
{
namespace Components
{
enum class TerrainNoisePreset : uint8;
} // namespace Components

void RegisterTerrainModifierInspectors();

// The Noise Preset dropdown's label of `preset`: "Smooth", "Rolling" or "Rugged".
const std::string& TerrainNoisePresetLabel(Components::TerrainNoisePreset preset);

} // namespace GameEngine
