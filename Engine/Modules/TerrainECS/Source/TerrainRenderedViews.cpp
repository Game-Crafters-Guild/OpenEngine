#include "TerrainECS/TerrainRenderedViews.h"

#include "Engine/Rendering/ViewRegistry.h"
#include "Rendering/Camera/CameraTypes.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TileStreamingManager.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainECS
{

TerrainRenderedViews TerrainRenderedViews::Collect(const Engine::Renderer::ViewRegistry& views)
{
    TerrainRenderedViews rendered;
    for (const auto& view : views.GetViews())
    {
        if (!view.RenderedByPipeline())
            continue;
        const auto* camData = views.FindCameraData(view.cameraId);
        if (!camData)
            continue;

        rendered.CameraPositions.push_back(ExtractCameraPosition(*camData));
        rendered.PageFocalScale = std::max(rendered.PageFocalScale, std::abs(camData->proj[1][1]));
        // LH view: row 2 is forward; proj[0][0] is 1 / tan(horizontal half field).
        const Mathematics::Vector3 forward{camData->view[0][2], camData->view[1][2], camData->view[2][2]};
        rendered.PageCameras.push_back(PageRequestCamera{rendered.CameraPositions.back(), forward,
                                                         std::atan(1.0f / std::abs(camData->proj[0][0])), true});
        if (rendered.CameraPositions.size() == 1)
        {
            rendered.Direction = TilePriorityViewDirectionXZ(camData->view);
            // ResolveCameraData, not camData: a stored camera without SetCameraEye has no sector
            // until ResolveCameraData sets it.
            const auto resolved = views.ResolveCameraData(view.id);
            const int32* sector = resolved.renderOriginSector;
            rendered.RenderOriginSector = Mathematics::Vector3Int(sector[0], sector[1], sector[2]);
        }

        // Far clip from a perspective projection (LH, ZO): proj[2][2] = zFar / (zFar - zNear) and
        // proj[3][2] = -(zNear * zFar) / (zFar - zNear), so zFar = -proj[3][2] / (proj[2][2] - 1).
        const float32 p10 = camData->proj[2][2];
        const float32 p14 = camData->proj[3][2];
        if (std::abs(p10 - 1.0f) > 1e-6f)
        {
            const float32 farClip = -p14 / (p10 - 1.0f);
            if (farClip > 0.0f)
                rendered.MaxFarClip = std::max(rendered.MaxFarClip, farClip);
        }
    }
    return rendered;
}

} // namespace GameEngine::TerrainECS
