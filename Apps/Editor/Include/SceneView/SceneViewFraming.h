#pragma once

#include <cmath>

#include "Mathematics/Geometry.h"

namespace GameEngine
{
struct SceneViewCameraPose;
namespace ECS
{
class World;
struct EntityHandle;
} // namespace ECS
} // namespace GameEngine

namespace GameEngine::Editor
{

// World-space bounds of an entity and all of its descendants, for camera
// framing. Multi-submesh model roots typically carry no LocalBounds of their
// own — the union over the Parent-linked subtree is what "frame this model"
// actually means.
struct SubtreeWorldBounds
{
    // Union of every LocalBounds in the subtree, transformed to world space.
    // Only meaningful when HasBounds is true.
    Mathematics::AABB Box{};
    // Root's world position — the framing target when no node carries bounds.
    Mathematics::Vector3 FallbackPosition{};
    bool HasBounds = false;

    Mathematics::Vector3 Center() const { return (Box.min + Box.max) * 0.5f; }

    // Radius of the bounding sphere enclosing Box.
    float Radius() const
    {
        const Mathematics::Vector3 halfExtents = (Box.max - Box.min) * 0.5f;
        return std::sqrt(Mathematics::Vector3::Dot(halfExtents, halfExtents));
    }
};

// Computes the world-space AABB union over `root` and all of its descendants
// (children discovered via Parent components), transforming each node's
// LocalBounds by its WorldTransform (Arvo's method — rotation and scale are
// handled exactly, not scale-only). Nodes without usable bounds contribute
// nothing. Returns false when `root` is not a valid entity; returns true with
// HasBounds == false when no node in the subtree carries bounds, in which
// case FallbackPosition holds the root's world position.
bool ComputeSubtreeWorldBounds(ECS::World& world, ECS::EntityHandle root, SubtreeWorldBounds& out);

// The Scene View pose that looks along `direction` (from the camera toward the target,
// any length) at `target` from `distance` away, in the camera's yaw/pitch convention:
// look = (cos(pitch)cos(yaw), sin(pitch), cos(pitch)sin(yaw)), yaw 0 looking down +X.
// False, leaving `out` unchanged, when `direction` has no length.
bool ComputeLookAtPose(const Mathematics::Vector3& target, const Mathematics::Vector3& direction, float distance,
                       SceneViewCameraPose& out);

// The perspective pose that frames `bounds`: every corner inside the view with a 10% margin,
// the box center as the orbit pivot, the yaw and pitch of `currentPose` kept. A flat box (one
// extent under a tenth of the largest) seen within 30 degrees of edge-on is viewed from 30 degrees
// off its plane instead: from the low side only when `currentPose` is past the box's low face,
// from the high side otherwise. `fieldOfViewDeg` is the vertical
// field of view in degrees, `aspect` the view's width over its height, and `nearClip` the
// closest the camera may come to any corner.
SceneViewCameraPose ComputePerspectiveFramingPose(const Mathematics::AABB& bounds,
                                                  const SceneViewCameraPose& currentPose,
                                                  float fieldOfViewDeg, float aspect, float nearClip);

// The Scene View's far clip for a camera `distance` from its orbit pivot: the configured
// `farClip`, or twice the distance when that is larger, so a framed object is never clipped.
float ExpandFarClipForFraming(float farClip, float distance);

} // namespace GameEngine::Editor
