#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{
enum class TerrainNoisePreset : uint8;
}

namespace GameEngine::TerrainECS
{

// The relief of a Flat terrain under the scene's Generated noise volume (ResolveGeneratedNoiseRelief).
// Weight is the volume's Weight: it multiplies the preset's relief height.
struct GeneratedNoiseRelief
{
    Components::TerrainNoisePreset Preset{};
    float32 Weight = 1.0f;

    bool operator==(const GeneratedNoiseRelief&) const = default;
};

} // namespace GameEngine::TerrainECS
