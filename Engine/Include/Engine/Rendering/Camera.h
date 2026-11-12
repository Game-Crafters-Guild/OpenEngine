#pragma once

#include "Components/Rendering/Camera.h"
#include "ECS/Entity.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/WorldPosition.h"
#include "Rendering/Camera/CameraTypes.h"

#include <optional>
#include "Rendering/Camera/CameraDerivation.h"

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::CameraData;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine {
namespace ECS { class World; }
} // namespace GameEngine

namespace GameEngine::Engine::Renderer {

/// Full camera state with computed matrices, suitable for rendering.
/// Distinct from Components::Camera (ECS component holding just fovY/nearZ/farZ parameters).
/// The double eye is the one source of the camera's position: SetPose writes the transform's
/// translation from it, so the two cannot disagree.
class Camera
{
public:
    Components::Camera params; // projection, clipping, culling, and post-process params
    ECS::EntityHandle entity;

    /// Set the pose: the rotation and scale of `basis` at the exact `eye` in world meters.
    /// The translation of `basis` is ignored.
    void SetPose(const Mathematics::Matrix4x4& basis, const Mathematics::WorldPosition& eye);

    /// The exact eye in world meters.
    const Mathematics::WorldPosition& Eye() const { return m_Eye; }

    /// The pose as a matrix. Its translation is the eye rounded to fp32.
    const Mathematics::Matrix4x4& WorldTransform() const { return m_WorldTransform; }

    /// Compute the view matrix: the inverse of WorldTransform().
    Mathematics::Matrix4x4 ComputeViewMatrix() const;

    /// Compute projection matrix for a given viewport aspect ratio.
    /// When orthoHeightOverride is set and the camera is orthographic, that vertical
    /// world-space extent is used instead of params.OrthographicSize (pixel-perfect mode).
    Mathematics::Matrix4x4 ComputeProjectionMatrix(float aspectRatio,
                                                   std::optional<float> orthoHeightOverride = std::nullopt) const;

    /// Compute combined view-projection matrix.
    Mathematics::Matrix4x4 ComputeViewProjectionMatrix(float aspectRatio,
                                                       std::optional<float> orthoHeightOverride = std::nullopt) const;

    /// Fill a CameraData struct ready for ViewRegistry::SetCameraData().
    Rendering::CameraData ToCameraData(float aspectRatio,
                                       std::optional<float> orthoHeightOverride = std::nullopt) const;

    /// The near distance and culling range for ViewRegistry::SetCameraData().
    /// A NearZ at or below zero, or a FarZ at or below NearZ, logs one error per entity and
    /// gives the component's default NearZ and FarZ.
    Rendering::CameraRanges ToCameraRanges() const;

private:
    Mathematics::Matrix4x4 m_WorldTransform;
    Mathematics::WorldPosition m_Eye;
};

/// Find the active camera in the ECS world.
///
/// Currently returns the first entity with a Camera component and WorldTransform
/// (with Transform as fallback). Returns std::nullopt if no camera entity exists.
///
/// Future: support priority/tag-based camera selection (e.g., MainCamera component).
std::optional<Camera> FindActiveCamera(ECS::World& world);

class ViewRegistry;

/// Declares a game view for this frame once its world's active camera is resolved: the view draws the
/// camera's Visible Layers, or no world while the camera's WorldDrawingPaused is set, and is armed for
/// the frame. With no camera nothing is armed, so an on-demand view sleeps: its effective layers are
/// None, and no extraction, culling or pass reads it. The view's world pause then follows the camera
/// from the next extraction on (ApplyActiveCameraWorldPause).
void DeclareGameCameraView(ViewRegistry& views, Rendering::ViewId view, const Camera* activeCamera);

/// Copies the WorldDrawingPaused of the world's active camera into the game views of the world that
/// DeclareGameCameraView declared. Extraction calls it before it reads the views, so the frame that
/// resumes the camera extracts the world for the view that it draws.
void ApplyActiveCameraWorldPause(ViewRegistry& views, ECS::World& world);

} // namespace GameEngine::Engine::Renderer
