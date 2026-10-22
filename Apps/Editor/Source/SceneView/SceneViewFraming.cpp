#include "SceneView/SceneViewFraming.h"

#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Mathematics/VectorOps.h"
#include "SceneView/SceneViewCameraRig.h"
#include "SceneViewController.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

void AccumulateWorldBounds(ECS::World& world, ECS::EntityHandle entity, SubtreeWorldBounds& out)
{
    if (!entity.IsValid() || !world.IsValid(entity))
        return;

    const auto* bounds = world.GetComponent<Components::LocalBounds>(entity);
    if (!bounds || bounds->Box.RadiusSquared() <= 0.0f)
        return;

    const auto* worldXf = world.GetComponent<Components::WorldTransform>(entity);
    if (!worldXf)
        return;

    const Mathematics::AABB aabb = bounds->Box.TransformToAABB(worldXf->matrix);
    if (!out.HasBounds)
    {
        out.Box = aabb;
        out.HasBounds = true;
        return;
    }

    out.Box.min.x = std::min(out.Box.min.x, aabb.min.x);
    out.Box.min.y = std::min(out.Box.min.y, aabb.min.y);
    out.Box.min.z = std::min(out.Box.min.z, aabb.min.z);
    out.Box.max.x = std::max(out.Box.max.x, aabb.max.x);
    out.Box.max.y = std::max(out.Box.max.y, aabb.max.y);
    out.Box.max.z = std::max(out.Box.max.z, aabb.max.z);
}

} // namespace

bool ComputeSubtreeWorldBounds(ECS::World& world, ECS::EntityHandle root, SubtreeWorldBounds& out)
{
    out = {};
    if (!root.IsValid() || !world.IsValid(root))
        return false;

    // Fallback target for the no-bounds case. WorldTransform is the rendered
    // truth; the local Transform position covers entities the hierarchy
    // system has not visited yet.
    if (const auto* worldXf = world.GetComponent<Components::WorldTransform>(root))
    {
        out.FallbackPosition = {worldXf->matrix[12], worldXf->matrix[13], worldXf->matrix[14]};
    }
    else if (const auto* xf = world.GetComponent<Components::Transform>(root))
    {
        out.FallbackPosition = xf->GetPosition();
    }

    AccumulateWorldBounds(world, root, out);

    std::vector<ECS::EntityHandle> descendants;
    Components::DescendantsOf(world, root, descendants);
    for (ECS::EntityHandle descendant : descendants)
        AccumulateWorldBounds(world, descendant, out);

    return true;
}

bool ComputeLookAtPose(const Mathematics::Vector3& target, const Mathematics::Vector3& direction, float distance,
                       SceneViewCameraPose& out)
{
    const float length = std::sqrt(Mathematics::Vector3::Dot(direction, direction));
    if (length < 1.0e-6f)
        return false;
    const Mathematics::Vector3 look(direction.x / length, direction.y / length, direction.z / length);
    constexpr float kDegreesPerRadian = 180.0f / Mathematics::Pi;
    out.Pos[0] = target.x - look.x * distance;
    out.Pos[1] = target.y - look.y * distance;
    out.Pos[2] = target.z - look.z * distance;
    out.YawDeg = std::atan2(look.z, look.x) * kDegreesPerRadian;
    out.PitchDeg = std::asin(look.y) * kDegreesPerRadian;
    out.Distance = distance;
    return true;
}

SceneViewCameraPose ComputePerspectiveFramingPose(const Mathematics::AABB& bounds,
                                                const SceneViewCameraPose& currentPose,
                                                float fieldOfViewDeg, float aspect, float nearClip)
{
    using Mathematics::Vector3;
    const auto box = Mathematics::BoundingBox::FromMinMax(bounds.min, bounds.max);
    const Vector3 center = box.center;
    const Vector3 halfExtents = box.halfExtents;
    const auto forward = CameraRig::LookDirection(currentPose.YawDeg, currentPose.PitchDeg);
    Vector3 look(static_cast<float>(forward.X), static_cast<float>(forward.Y), static_cast<float>(forward.Z));

    constexpr float kPlanarThicknessRatio = 0.001f;
    constexpr float kMinimumPlaneViewSine = 0.5f;
    const float planarThickness = std::max({halfExtents.x, halfExtents.y, halfExtents.z}) * kPlanarThicknessRatio;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        if (halfExtents[axis] > planarThickness ||
            halfExtents[(axis + 1) % 3] <= planarThickness ||
            halfExtents[(axis + 2) % 3] <= planarThickness ||
            std::abs(look[axis]) >= kMinimumPlaneViewSine)
            continue;

        look[axis] = 0.0f;
        look = look.NormalizeOrZero() * std::sqrt(1.0f - kMinimumPlaneViewSine * kMinimumPlaneViewSine);
        look[axis] = currentPose.Pos[axis] < center[axis] ? kMinimumPlaneViewSine : -kMinimumPlaneViewSine;
        break;
    }

    SceneViewCameraPose result = currentPose;
    ComputeLookAtPose(center, look, nearClip, result);
    float view[16];
    CameraRig::BuildViewMatrixLH({}, result.YawDeg, result.PitchDeg, view);
    const Vector3 right(view[0], view[4], view[8]);
    const Vector3 up(view[1], view[5], view[9]);
    look = Vector3(view[2], view[6], view[10]);

    constexpr float kFramingPadding = 1.1f;
    const float tanHalfVertical = std::tan(fieldOfViewDeg * Mathematics::Pi / 360.0f);
    const float tanHalfHorizontal = tanHalfVertical * aspect;
    float distance = box.Radius() + nearClip;
    for (uint32_t corner = 0; corner < 8; ++corner)
    {
        const Vector3 offset((corner & 1) ? halfExtents.x : -halfExtents.x,
                             (corner & 2) ? halfExtents.y : -halfExtents.y,
                             (corner & 4) ? halfExtents.z : -halfExtents.z);
        const float depth = Vector3::Dot(offset, look);
        distance = std::max({distance, nearClip - depth,
                             kFramingPadding * std::abs(Vector3::Dot(offset, right)) / tanHalfHorizontal - depth,
                             kFramingPadding * std::abs(Vector3::Dot(offset, up)) / tanHalfVertical - depth});
    }

    const auto position = CameraRig::OrbitCameraPosition({center.x, center.y, center.z},
                                                        result.YawDeg, result.PitchDeg, distance);
    result.Pos[0] = static_cast<float>(position.X);
    result.Pos[1] = static_cast<float>(position.Y);
    result.Pos[2] = static_cast<float>(position.Z);
    result.Distance = distance;
    return result;
}

float ExpandFarClipForFraming(float farClip, float distance)
{
    return std::max(farClip, distance * 2.0f);
}

} // namespace GameEngine::Editor
