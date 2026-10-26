#pragma once

#include "Rendering/Core/Device.h"

namespace GameEngine
{
namespace Rendering
{

/// The engine's reverse-Z depth convention in pipeline terms: near maps to 1.0, far to 0.0, and
/// the nearer surface has the larger depth. Production depth tests, sampler compares and depth
/// clears use these values. Shaders use `Shaders/Includes/reverse_z.glsl`.
namespace ReverseZ
{

/// Pass when the fragment is nearer than or at the stored depth. The default depth test, so a
/// pass that redraws the prepass depth keeps its fragments; also the shadow comparison sampler.
constexpr CompareOp kNearerOrEqual = CompareOp::GreaterOrEqual;

/// Pass when the fragment is strictly farther than the stored depth: the x-ray passes that draw
/// gizmos and markups behind scene geometry.
constexpr CompareOp kFarther = CompareOp::Less;

/// The depth a depth attachment clears to: the far plane, so the first fragment anywhere wins.
constexpr float kClearFar = 0.0f;

} // namespace ReverseZ

static_assert(DepthStencilState{}.depthCompareOp == ReverseZ::kNearerOrEqual,
              "DepthStencilState must default to the reverse-Z compare (ReverseZ::kNearerOrEqual)");

} // namespace Rendering
} // namespace GameEngine
