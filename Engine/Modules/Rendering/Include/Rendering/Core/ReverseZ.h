#pragma once

#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"

namespace GameEngine
{
namespace Rendering
{

/// The engine's depth convention in pipeline terms. The engine renders reverse-Z only: NDC depth
/// is [0, 1], the near plane maps to 1.0, the far plane to 0.0, and the nearer surface has the
/// larger depth. Depth state, sampler compares, depth clears and caster bias name these values
/// instead of a literal. Projection and linearization live in `Engine/Rendering/CameraUtils.h`,
/// `Mathematics/MatrixOps.h` and, for shaders, `Shaders/Includes/reverse_z.glsl`.
namespace ReverseZ
{

/// Pass when the fragment is nearer than or at the stored depth. The default for every depth
/// test: a pass that redraws the prepass geometry writes the same depth, and the equal half of
/// the compare keeps those fragments. The shadow comparison sampler uses it for "lit when the
/// receiver is at or nearer than the occluder".
constexpr CompareOp kNearerOrEqual = CompareOp::GreaterOrEqual;

/// Pass when the fragment is strictly farther than the stored depth: the x-ray passes that draw
/// gizmos and markups where scene geometry hides them.
constexpr CompareOp kFarther = CompareOp::Less;

/// The depth a depth attachment clears to: the far plane, so the first fragment anywhere wins.
constexpr float kClearFar = 0.0f;

/// Converts a rasterizer depth bias magnitude (constant or slope factor) into the signed value
/// that pushes a fragment away from the viewer. The hardware adds the bias to depth, and under
/// reverse-Z "away" is a smaller depth, so the sign is negative.
constexpr float CasterBiasAwayFromViewer(float magnitude)
{
    return -magnitude;
}

} // namespace ReverseZ

// Defaults in headers that do not include this one.
static_assert(DepthStencilState{}.depthCompareOp == ReverseZ::kNearerOrEqual,
              "DepthStencilState must default to the reverse-Z compare (ReverseZ::kNearerOrEqual)");
static_assert(ViewTargets{}.clearDepthValue == ReverseZ::kClearFar,
              "ViewTargets must default its depth clear to the far plane (ReverseZ::kClearFar)");
static_assert(ViewClearConfig{}.clearDepthValue == ReverseZ::kClearFar,
              "ViewClearConfig must default its depth clear to the far plane (ReverseZ::kClearFar)");

} // namespace Rendering
} // namespace GameEngine
