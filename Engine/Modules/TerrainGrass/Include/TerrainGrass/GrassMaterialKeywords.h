#pragma once

#include "Rendering/Materials/ShaderVariantKey.h"

namespace GameEngine::TerrainGrass
{

// The material keywords every grass draw mode registers its material with: clustered lighting with
// shadows and image-based light, the instance-driven procedural blade, and diffuse transmission, the
// light through a thin blade (terrain_grass_surface.glsl sets the share and its albedo). The feature
// registers with these and the material variant cook requests them, so a cooked-only runtime finds
// the variant the feature asks for.
inline constexpr Rendering::MaterialKeyword kGrassMaterialKeywords =
    Rendering::MaterialKeyword::ForwardPlus | Rendering::MaterialKeyword::Shadows |
    Rendering::MaterialKeyword::Instanced | Rendering::MaterialKeyword::IBL |
    Rendering::MaterialKeyword::ProceduralVertexOutput |
    Rendering::MaterialKeyword::DiffuseTransmission;

} // namespace GameEngine::TerrainGrass
