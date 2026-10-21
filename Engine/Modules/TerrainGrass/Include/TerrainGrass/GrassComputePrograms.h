#pragma once

#include "Rendering/Materials/ShaderCompileService.h"

#include <filesystem>

namespace GameEngine::TerrainGrass
{

/// One compute program of the grass placement: its stage source in the engine shader tree's
/// TerrainGrass directory, and the name its compile is logged under.
struct GrassComputeProgram
{
    const char* ShaderFile;
    const char* DebugName;
};

inline constexpr GrassComputeProgram kGrassClassifyProgram{"terrain_grass_classify.comp",
                                                           "TerrainGrassClassify_Pipeline"};
inline constexpr GrassComputeProgram kGrassPlanProgram{"terrain_grass_plan.comp", "TerrainGrassPlan_Pipeline"};
inline constexpr GrassComputeProgram kGrassPlaceProgram{"terrain_grass_place.comp", "TerrainGrassPlace_Pipeline"};

/// Every compute program the placement creates a pipeline from.
inline constexpr GrassComputeProgram kGrassComputePrograms[] = {kGrassClassifyProgram, kGrassPlanProgram,
                                                                kGrassPlaceProgram};

/// The compile request of `program` under `engineShaders`, the engine shader tree (the install
/// assets root's Shaders directory, or the tree an export ships). One builder for its two consumers:
/// TerrainGrassRenderFeature compiles the program at runtime, and MaterialVariantCook cooks it into
/// the cache a packaged game ships. The cache key folds the stage's source, its defines and the
/// content of its include closure, so the two must build the request alike. `compat` selects the
/// compatibility profile's named-binding arm of the source. The caller sets the cache roots.
Rendering::ShaderProgramCompileRequest MakeGrassComputeCompileRequest(const std::filesystem::path& engineShaders,
                                                                      const GrassComputeProgram& program,
                                                                      bool compat);

} // namespace GameEngine::TerrainGrass
