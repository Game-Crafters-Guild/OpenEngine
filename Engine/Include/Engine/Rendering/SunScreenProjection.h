#pragma once

#include "Mathematics/Vector3.h"
#include "Rendering/CameraTypes.h"

#include <optional>

namespace GameEngine::Engine::Renderer
{

// Screen projection of an infinitely distant directional source (the sun), shared by every
// pass that needs to know where the sun lands on screen: the lens flare's per-source
// placement and occlusion probe, and the sun glare's occlusion probe.
//
// Kept in one place because the two callers must agree — a glare that fades on a silhouette
// the flare thinks it is clear of is a bug nothing would catch.

// Project a world position through the camera's column-major view-projection matrix to NDC
// (x, y, depth). Empty when the point is at or behind the camera (w at or below 1e-5).
std::optional<Mathematics::Vector3> ProjectToNdc(const ::GameEngine::Rendering::CameraData& camera,
                                                 const Mathematics::Vector3& position);

// Project an infinite directional source. Perspective views can transform the direction
// directly with homogeneous w = 0, avoiding camera-position precision loss in large worlds.
// Editor 2D views are orthographic, so use a finite camera-relative point there. Sun depth
// is always the reverse-Z far plane (0), allowing any scene geometry at the source screen
// position to occlude it.
//
// `direction` is the toward-sun direction (surface -> sun), the same convention
// SkySettings::scatteringSunDir carries. Empty when the sun is behind the view.
std::optional<Mathematics::Vector3> ProjectSunDirectionToNdc(const ::GameEngine::Rendering::CameraData& camera,
                                                             const Mathematics::Vector3& direction);

} // namespace GameEngine::Engine::Renderer
