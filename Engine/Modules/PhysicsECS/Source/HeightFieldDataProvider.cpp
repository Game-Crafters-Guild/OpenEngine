#include "PhysicsECS/HeightFieldDataProvider.h"

#include "PhysicsECS/Components/HeightFieldColliderShape.h"

#include "Components/Transform.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace GameEngine::PhysicsECS
{
namespace
{

// Padding applied to heightfield wake boxes so bodies resting just outside
// the exact region bounds (or above/below the sampled heights) still wake.
constexpr float32 kWakeBoxPaddingMeters = 2.0f;

// The world-space matrix of a collider transform. The physics queries take float32 world boxes, so
// the double position rounds to float32 once here.
Mathematics::Matrix4x4 ColliderWorldMatrix(const Components::WorldTransform& wt)
{
    return wt.ToMatrix4x4(wt.GetPosition() - Mathematics::WorldPosition{});
}

// The surface height at local point (x, z) of a heightfield of `data.sampleCount` samples a side over
// shape.sizeX x shape.sizeZ meters centered on the local origin, bilinear between the four samples
// around the point, in local meters. Empty outside the grid and where a sample is a hole.
std::optional<float32> SampleHeightFieldSurface(const HeightFieldData& data,
                                                const Components::HeightFieldColliderShape& shape, float32 x,
                                                float32 z)
{
    const uint32 last = data.sampleCount - 1u;
    const float32 gridX = (x + shape.sizeX * 0.5f) * static_cast<float32>(last) / shape.sizeX;
    const float32 gridZ = (z + shape.sizeZ * 0.5f) * static_cast<float32>(last) / shape.sizeZ;
    if (!(gridX >= 0.0f && gridZ >= 0.0f && gridX <= static_cast<float32>(last) &&
          gridZ <= static_cast<float32>(last)))
        return std::nullopt;
    const uint32 x0 = std::min(static_cast<uint32>(gridX), last - 1u);
    const uint32 z0 = std::min(static_cast<uint32>(gridZ), last - 1u);
    const float32 fx = gridX - static_cast<float32>(x0);
    const float32 fz = gridZ - static_cast<float32>(z0);
    const size_t row0 = static_cast<size_t>(z0) * data.sampleCount;
    const size_t row1 = row0 + data.sampleCount;
    const float32 s00 = data.samples[row0 + x0];
    const float32 s10 = data.samples[row0 + x0 + 1u];
    const float32 s01 = data.samples[row1 + x0];
    const float32 s11 = data.samples[row1 + x0 + 1u];
    if (s00 == Physics::kHeightFieldNoCollision || s10 == Physics::kHeightFieldNoCollision ||
        s01 == Physics::kHeightFieldNoCollision || s11 == Physics::kHeightFieldNoCollision)
        return std::nullopt;
    const float32 nearRow = Math::Lerp(s00, s10, fx);
    const float32 farRow = Math::Lerp(s01, s11, fx);
    return Math::Lerp(nearRow, farRow, fz) * shape.heightScale;
}

} // namespace

static HeightFieldDataProvider s_Provider = nullptr;

void SetHeightFieldDataProvider(HeightFieldDataProvider provider)
{
    s_Provider = provider;
}

HeightFieldDataProvider GetHeightFieldDataProvider()
{
    return s_Provider;
}

Mathematics::AABB HeightFieldRegionWorldBox(const Components::WorldTransform& wt,
                                            const Components::HeightFieldColliderShape& shape, uint32 sampleCount,
                                            int32 minX, int32 minZ, int32 maxX, int32 maxZ, float32 minRawHeight,
                                            float32 maxRawHeight)
{
    const float32 scaleX = shape.sizeX / static_cast<float32>(sampleCount - 1);
    const float32 scaleZ = shape.sizeZ / static_cast<float32>(sampleCount - 1);

    // The region's local box: the shape offset puts the grid's center at the local origin.
    const Mathematics::Vector3 localLow{-shape.sizeX * 0.5f + static_cast<float32>(minX) * scaleX,
                                        minRawHeight * shape.heightScale,
                                        -shape.sizeZ * 0.5f + static_cast<float32>(minZ) * scaleZ};
    const Mathematics::Vector3 localHigh{-shape.sizeX * 0.5f + static_cast<float32>(maxX) * scaleX,
                                         maxRawHeight * shape.heightScale,
                                         -shape.sizeZ * 0.5f + static_cast<float32>(maxZ) * scaleZ};

    const Mathematics::Matrix4x4 matrix = ColliderWorldMatrix(wt);
    Mathematics::AABB box = Mathematics::AABB::Empty();
    for (uint32 corner = 0; corner < 8u; ++corner)
    {
        const float32 x = (corner & 1u) ? localHigh.x : localLow.x;
        const float32 y = (corner & 2u) ? localHigh.y : localLow.y;
        const float32 z = (corner & 4u) ? localHigh.z : localLow.z;
        box.Expand(matrix.TransformPoint(Mathematics::Vector3{x, y, z}));
    }
    const Mathematics::Vector3 padding{kWakeBoxPaddingMeters, kWakeBoxPaddingMeters, kWakeBoxPaddingMeters};
    box.min = box.min - padding;
    box.max = box.max + padding;
    return box;
}

void SettleBodiesOnHeightField(Physics::PhysicsWorld& physicsWorld, const Components::WorldTransform& wt,
                               const Components::HeightFieldColliderShape& shape, const Mathematics::AABB& wakeBox)
{
    std::vector<Physics::BodyBounds> bodies;
    physicsWorld.GetDynamicBodiesInAABB(wakeBox, bodies);
    const HeightFieldData data =
        bodies.empty() || !s_Provider ? HeightFieldData{}
                                      : s_Provider(shape.dataHandle, shape.dataGeneration, shape.lastBuiltVersion);
    if (data.samples && data.sampleCount >= 2u && shape.sizeX > 0.0f && shape.sizeZ > 0.0f)
    {
        const Mathematics::Matrix4x4 matrix = ColliderWorldMatrix(wt);
        const Mathematics::Matrix4x4 inverse = Mathematics::Inverse(matrix);
        const Mathematics::Vector3 up = Mathematics::Vector3(matrix[1].x, matrix[1].y, matrix[1].z).Normalize();
        const float32 clearance = physicsWorld.GetContactDistance();
        for (const Physics::BodyBounds& body : bodies)
        {
            const Mathematics::Vector3 center = (body.bounds.min + body.bounds.max) * 0.5f;
            const Mathematics::Vector3 halfSize = (body.bounds.max - body.bounds.min) * 0.5f;
            const Mathematics::Vector3 local = inverse.TransformPoint(center);
            const std::optional<float32> surface = SampleHeightFieldSurface(data, shape, local.x, local.z);
            if (!surface)
                continue;
            // The surface and the body's bottom as heights along `up`; the bottom is the lowest point
            // of the body's bounds along `up`.
            const float32 surfaceHeight =
                Mathematics::Vector3::Dot(matrix.TransformPoint({local.x, *surface, local.z}), up);
            const float32 bottom = Mathematics::Vector3::Dot(center, up) -
                                   (halfSize.x * std::abs(up.x) + halfSize.y * std::abs(up.y) +
                                    halfSize.z * std::abs(up.z));
            if (bottom >= surfaceHeight)
                continue;
            Physics::Transform transform = physicsWorld.GetBodyTransform(body.body);
            transform.position = transform.position + up * (surfaceHeight + clearance - bottom);
            physicsWorld.SetBodyTransform(body.body, transform, Physics::ActivationMode::Activate);
        }
    }
    physicsWorld.ActivateBodiesInAABB(wakeBox);
}

} // namespace GameEngine::PhysicsECS
