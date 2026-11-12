#pragma once

#include "Engine/Rendering/RenderOrigin.h"
#include "Rendering/Camera/CameraTypes.h"

namespace GameEngine::Tests
{

/// A test camera with its eye set from its own cameraPos, as ViewRegistry::SetCameraData requires.
inline Rendering::CameraData TestCameraWithEye(Rendering::CameraData camera)
{
    Engine::Renderer::SetCameraEye(camera, Mathematics::WorldPosition::FromVector3(Mathematics::Vector3{
                                               camera.cameraPos.x, camera.cameraPos.y, camera.cameraPos.z}));
    return camera;
}

/// The linear depth of an exact world position in a camera's view, in double: the rotation of the relative view
/// applied to the position minus the camera's double eye.
inline double ExactViewDepth(const Rendering::CameraData& camera, const Mathematics::WorldPosition& point)
{
    const Mathematics::WorldPosition eye = Engine::Renderer::CameraEye(camera);
    const Mathematics::Matrix4x4& view = camera.viewRel;
    return view[0][2] * (point.X - eye.X) + view[1][2] * (point.Y - eye.Y) + view[2][2] * (point.Z - eye.Z);
}

} // namespace GameEngine::Tests
