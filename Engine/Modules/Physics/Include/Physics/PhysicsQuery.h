#pragma once

#include "Physics/PhysicsTypes.h"

#include <vector>

namespace GameEngine::Physics
{
/// Which bodies a query sees.
struct CollisionFilter
{
    /// Bit n set: bodies on collision layer n are seen; the others are passed through as if absent.
    uint32 layerMask = 0xFFFFFFFFu;
    /// Sensors (trigger bodies) are passed through when set, and seen like solid bodies when not.
    bool ignoreSensors = false;

    bool PassesLayer(CollisionLayer layer) const
    {
        const uint32 l = static_cast<uint32>(layer);
        if (l >= 32)
        {
            // Out of supported mask range.
            return false;
        }
        return (layerMask & (1u << l)) != 0u;
    }
};

struct RayCastQuery
{
    /// The start of the ray: an exact world position.
    WorldPosition origin{};
    /// The direction of the ray, of any length above zero.
    Vector3 direction{0.0f, 0.0f, 1.0f};
    float32 maxDistance = 1000.0f;
    CollisionFilter filter{};
};

struct RayCastResult
{
    BodyHandle body{};
    WorldPosition hitPoint{};
    Vector3 hitNormal{0.0f, 1.0f, 0.0f};
    float32 distance = 0.0f;
    uint64 userData = 0;

    bool HasHit() const { return body.IsValid(); }
};

struct BoxOverlapQuery
{
    WorldPosition center{};
    Vector3 halfExtents{0.5f, 0.5f, 0.5f};
    Quaternion rotation = Quaternion::Identity();
    CollisionFilter filter{};
};

struct OverlapResult
{
    BodyHandle body{};
    uint64 userData = 0;

    bool HasHit() const { return body.IsValid(); }
};

/// A body and its world-space bounding box.
struct BodyBounds
{
    BodyHandle body{};
    AABB bounds{};
};

} // namespace GameEngine::Physics

