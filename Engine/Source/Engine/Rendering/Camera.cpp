#include "Engine/Rendering/Camera.h"

#include "Components/Transform.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "ECS/ECSTemplates.h"
#include "Mathematics/Angle.h"
#include "Mathematics/MatrixOps.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Math.h"

#include <algorithm>
#include <atomic>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer {
using namespace ::GameEngine::Rendering;

void Camera::SetPose(const Mathematics::Matrix4x4& basis, const Mathematics::WorldPosition& eye)
{
    m_WorldTransform = basis;
    m_WorldTransform[3][0] = static_cast<float>(eye.X);
    m_WorldTransform[3][1] = static_cast<float>(eye.Y);
    m_WorldTransform[3][2] = static_cast<float>(eye.Z);
    m_Eye = eye;
}

Mathematics::Matrix4x4 Camera::ComputeViewMatrix() const
{
    return Mathematics::Inverse(m_WorldTransform);
}

Mathematics::Matrix4x4 Camera::ComputeProjectionMatrix(float aspectRatio,
                                                       std::optional<float> orthoHeightOverride) const
{
    const float safeAspect = std::max(aspectRatio, 0.0001f);
    if (params.Perspective)
    {
        const float fovRad = Mathematics::ToRadians(params.FovY);
        const float squeeze = std::clamp(params.AnamorphicSqueeze, 1.0f, 4.0f);
        return Mathematics::MakePerspectiveLH_ZO_ReverseZ(fovRad, safeAspect * squeeze, params.NearZ, params.FarZ);
    }

    const float requestedHeight = orthoHeightOverride.value_or(params.OrthographicSize);
    const float orthoHeight = std::max(requestedHeight, 0.0001f);
    const float halfH = orthoHeight * 0.5f;
    const float halfW = halfH * safeAspect;
    return Mathematics::MakeOrthographicLH_ZO_ReverseZ(-halfW, halfW, -halfH, halfH, params.NearZ, params.FarZ);
}

Mathematics::Matrix4x4 Camera::ComputeViewProjectionMatrix(float aspectRatio,
                                                           std::optional<float> orthoHeightOverride) const
{
    return ComputeProjectionMatrix(aspectRatio, orthoHeightOverride) * ComputeViewMatrix();
}

Rendering::CameraData Camera::ToCameraData(float aspectRatio,
                                           std::optional<float> orthoHeightOverride) const
{
    const Mathematics::Matrix4x4 viewM = ComputeViewMatrix();
    const Mathematics::Matrix4x4 projM = ComputeProjectionMatrix(aspectRatio, orthoHeightOverride);
    const Mathematics::Matrix4x4 viewProjM = projM * viewM;

    Rendering::CameraData data{};
    data.view = viewM;
    data.proj = projM;
    data.viewProj = viewProjM;

    data.cameraPos.w = 0.0f;
    Engine::Renderer::SetCameraEye(data, m_Eye);

    return data;
}

Rendering::CameraRanges Camera::ToCameraRanges() const
{
    if (params.NearZ > 0.0f && params.FarZ > params.NearZ)
        return {params.NearZ, params.FarZ};

    // The game camera calls this every frame: report each entity once, not once per frame.
    static std::atomic<uint32> s_LastReportedEntity{ECS::kInvalidEntity};
    const Components::Camera defaults{};
    if (s_LastReportedEntity.exchange(entity.id) != entity.id)
    {
        Logger::Log::Error("Camera on entity={} has NearZ {} and FarZ {}: NearZ must be above 0 and FarZ above "
                           "NearZ. Set them in the Camera component; until then it draws with NearZ {} and FarZ {}.",
                           entity.id, params.NearZ, params.FarZ, defaults.NearZ, defaults.FarZ);
    }
    return {defaults.NearZ, defaults.FarZ};
}

std::optional<Camera> FindActiveCamera(ECS::World& world)
{
    Camera result{};
    bool found = false;

    auto q = world.Query<ECS::Read<Components::Camera>>();
    q.Each([&](ECS::EntityHandle e, const Components::Camera& cam)
    {
        if (found)
            return;

        // Extract world transform (prefer WorldTransform, fallback to Transform).
        // A WorldTransform the hierarchy has not resolved yet (Version 0 beside
        // a Transform) holds no pose; the camera's own Transform does.
        const auto* worldXf = world.GetComponent<Components::WorldTransform>(e);
        const bool hasLocal = world.HasComponent<Components::Transform>(e);
        if (worldXf && (worldXf->Version != 0u || !hasLocal))
        {
            result.SetPose(worldXf->ToMatrix4x4(Mathematics::Vector3{}), worldXf->GetPosition());
        }
        else if (hasLocal)
        {
            if (auto* xf = world.GetComponent<Components::Transform>(e))
            {
                result.SetPose(xf->ToMatrix4x4(Mathematics::Vector3{}), xf->GetPosition());
            }
        }
        result.params = cam;
        result.entity = e;
        found = true;
    });

    if (!found)
        return std::nullopt;
    return result;
}

void DeclareGameCameraView(ViewRegistry& views, Rendering::ViewId view, const Camera* activeCamera)
{
    if (!activeCamera)
        return;
    views.RequestViewFrame(view);
    views.SetViewRenderLayerMask(view, activeCamera->params.VisibleLayers);
    views.SetViewWorldPaused(view, activeCamera->params.WorldDrawingPaused);
    views.SetViewWorldPauseFollowsActiveCamera(view);
}

void ApplyActiveCameraWorldPause(ViewRegistry& views, ECS::World& world)
{
    if (const std::optional<Camera> activeCamera = FindActiveCamera(world))
        views.SetActiveCameraViewsWorldPaused(world.GetWorldId(), activeCamera->params.WorldDrawingPaused);
}

} // namespace GameEngine::Engine::Renderer
