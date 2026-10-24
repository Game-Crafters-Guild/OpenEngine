#pragma once

#include "Ocean/OceanDepthCacheAsset.h"
#include "Rendering/RenderLayerMask.h"
#include "Types/Types.h"

#include <filesystem>
#include <string>

namespace GameEngine
{
namespace ECS { class World; }

namespace Engine::Renderer
{

struct OceanSceneDepthBakeOptions
{
    Rendering::RenderLayerMask Layers = Rendering::RenderLayerMask::All();
    bool LoadMissingAssets = true;
    bool IncludeDisabledRenderers = false;
    bool IncludeSkinnedMeshes = false;
    bool IncludeTerrainHeightfields = true;
    bool OnlyOceanMeshDepthContributors = false;
};

struct OceanSceneDepthBakeStats
{
    uint32 CandidateRenderers = 0;
    uint32 MeshSources = 0;
    uint32 TerrainSources = 0;
    uint32 Triangles = 0;
    uint32 SkippedLayer = 0;
    uint32 SkippedAsset = 0;
    uint32 SkippedMesh = 0;
    uint32 SkippedSkinned = 0;
    uint32 SkippedTerrain = 0;
    uint32 SkippedUntagged = 0;
    uint32 UnsampledTerrainTexels = 0; ///< texels on a terrain with no finest height yet: the bake fails
};

// Bakes the current scene's MeshRenderer geometry and planar terrains into a
// saved ocean depth cache. Sources are filtered by Layers, transformed by
// WorldTransform, and sampled top-down into Ocean's .oceandepth format. A terrain
// is read at its finest height (SurfaceHeightQuery, RequireFinest), streamed out
// or not; a texel on a terrain whose height store is still cooking fails the bake.
bool BakeOceanDepthCacheFromSceneMeshes(ECS::World& world,
                                        const Ocean::OceanDepthCacheBakeDesc& desc,
                                        const OceanSceneDepthBakeOptions& options,
                                        Ocean::OceanDepthCacheAsset& outCache,
                                        OceanSceneDepthBakeStats* outStats = nullptr,
                                        std::string* error = nullptr);

bool SaveOceanDepthCacheFromSceneMeshes(ECS::World& world,
                                        const Ocean::OceanDepthCacheBakeDesc& desc,
                                        const OceanSceneDepthBakeOptions& options,
                                        const std::filesystem::path& path,
                                        OceanSceneDepthBakeStats* outStats = nullptr,
                                        std::string* error = nullptr);

} // namespace Engine::Renderer
} // namespace GameEngine
