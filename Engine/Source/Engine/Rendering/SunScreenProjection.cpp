#include "Engine/Rendering/SunScreenProjection.h"

namespace GameEngine::Engine::Renderer
{

using namespace ::GameEngine::Rendering;

std::optional<Mathematics::Vector3> ProjectToNdc(const CameraData& camera, const Mathematics::Vector3& position)
{
    const float x = position.x;
    const float y = position.y;
    const float z = position.z;
    const float cx = camera.viewProj[0] * x + camera.viewProj[4] * y + camera.viewProj[8] * z + camera.viewProj[12];
    const float cy = camera.viewProj[1] * x + camera.viewProj[5] * y + camera.viewProj[9] * z + camera.viewProj[13];
    const float cz = camera.viewProj[2] * x + camera.viewProj[6] * y + camera.viewProj[10] * z + camera.viewProj[14];
    const float cw = camera.viewProj[3] * x + camera.viewProj[7] * y + camera.viewProj[11] * z + camera.viewProj[15];
    if (cw <= 1e-5f)
        return std::nullopt;
    return Mathematics::Vector3(cx / cw, cy / cw, cz / cw);
}

std::optional<Mathematics::Vector3> ProjectSunDirectionToNdc(const CameraData& camera, const Mathematics::Vector3& direction)
{
    if (camera.cameraPos[3] < 0.5f)
    {
        const float cx = camera.viewProj[0] * direction.x + camera.viewProj[4] * direction.y +
                         camera.viewProj[8] * direction.z;
        const float cy = camera.viewProj[1] * direction.x + camera.viewProj[5] * direction.y +
                         camera.viewProj[9] * direction.z;
        const float cw = camera.viewProj[3] * direction.x + camera.viewProj[7] * direction.y +
                         camera.viewProj[11] * direction.z;
        if (cw <= 1e-5f)
            return std::nullopt;
        return Mathematics::Vector3(cx / cw, cy / cw, 0.0f);
    }

    constexpr float kOrthoSunDistance = 1000.0f;
    const Mathematics::Vector3 cameraPosition(camera.cameraPos[0], camera.cameraPos[1], camera.cameraPos[2]);
    const std::optional<Mathematics::Vector3> ndc = ProjectToNdc(camera, cameraPosition + direction * kOrthoSunDistance);
    if (!ndc)
        return std::nullopt;
    return Mathematics::Vector3(ndc->x, ndc->y, 0.0f);
}

} // namespace GameEngine::Engine::Renderer
