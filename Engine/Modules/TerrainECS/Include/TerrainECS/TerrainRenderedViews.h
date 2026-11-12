#pragma once

#include "Mathematics/Vector3.h"
#include "Mathematics/Vector3Int.h"
#include "TerrainECS/PageLevelRule.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::Engine::Renderer
{
class ViewRegistry;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::TerrainECS
{

/// The views the terrain streams tiles and requests pages for this frame: the views the pipeline renders.
struct TerrainRenderedViews
{
    std::vector<Mathematics::Vector3> CameraPositions;
    /// Each view's position, forward and horizontal half field: the surface pages' wedges.
    std::vector<PageRequestCamera> PageCameras;
    /// The first view's XZ look direction, for the tile in-front priority.
    Mathematics::Vector3 Direction{0.0f, 0.0f, 1.0f};
    /// The largest far clip of the views, for the LOD range clamp.
    float32 MaxFarClip = 1000.0f;
    /// The largest 1 / tan(vfov / 2) of the views: the page level rule serves the narrowest field of view.
    float32 PageFocalScale = 0.0f;
    /// The render-origin sector of the first view.
    Mathematics::Vector3Int RenderOriginSector;

    /// Collects the views the pipeline renders this frame (ViewDesc::RenderedByPipeline). A parked view
    /// and a view off the pipeline are not collected, so they request no terrain page.
    static TerrainRenderedViews Collect(const Engine::Renderer::ViewRegistry& views);
};

} // namespace GameEngine::TerrainECS
