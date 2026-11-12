#include "PhysicsPoseWriteback.h"

#include "PhysicsECS/Components/PhysicsWritebackRecord.h"

#include "Components/Transform.h"
#include "Components/TransformDirtyFeed.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Physics/PhysicsTypes.h"

#include <cstring>

namespace GameEngine::PhysicsECS
{
namespace
{
// Bit equality: the skip must hold exactly when composing would see the same inputs.
bool SamePose(const Physics::Transform& a, const Physics::Transform& b)
{
    return std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0 &&
           std::memcmp(&a.rotation.GetGLM(), &b.rotation.GetGLM(), sizeof(a.rotation.GetGLM())) == 0;
}

// Bit equality of two poses: the position doubles and the basis floats.
bool SamePoseBytes(const Mathematics::WorldPosition& position, const Mathematics::Matrix3x3& basis,
                   const Mathematics::WorldPosition& otherPosition, const Mathematics::Matrix3x3& otherBasis)
{
    return std::memcmp(&position, &otherPosition, sizeof(position)) == 0 &&
           std::memcmp(&basis.GetGLM(), &otherBasis.GetGLM(), sizeof(basis.GetGLM())) == 0;
}

bool HoldsWrittenPose(const Mathematics::WorldPosition& position, const Mathematics::Matrix3x3& basis,
                      const Components::PhysicsWritebackRecord& record)
{
    return SamePoseBytes(position, basis, record.WrittenPosition, record.WrittenBasis);
}

bool IsUnchanged(const Physics::Transform& current,
                 const Physics::Transform* interpolateFrom,
                 const Components::Transform& transform,
                 const Components::WorldTransform& worldTransform,
                 const Components::PhysicsWritebackRecord& record)
{
    return record.Valid && SamePose(current, record.AppliedPose) &&
           (interpolateFrom == nullptr || SamePose(*interpolateFrom, record.AppliedPose)) &&
           HoldsWrittenPose(transform.Position, transform.Basis, record) &&
           HoldsWrittenPose(worldTransform.Position, worldTransform.Basis, record);
}

Physics::Transform BlendPose(const Physics::Transform& from, const Physics::Transform& to, float32 alpha)
{
    Physics::Transform pose = to;
    pose.position = from.position + (to.position - from.position) * alpha;
    pose.rotation = Physics::Quaternion::Slerp(from.rotation, to.rotation, alpha);
    return pose;
}
} // namespace

bool WritePhysicsPose(ECS::World& world,
                      ECS::EntityHandle entity,
                      const Physics::Transform& current,
                      const Physics::Transform* interpolateFrom,
                      float32 alpha,
                      Components::Transform& transform,
                      Components::WorldTransform& worldTransform,
                      Components::PhysicsWritebackRecord& record)
{
    if (IsUnchanged(current, interpolateFrom, transform, worldTransform, record))
        return false;

    const Physics::Transform pose = interpolateFrom ? BlendPose(*interpolateFrom, current, alpha) : current;

    const auto scale = transform.GetScale();
    const Mathematics::WorldPosition& position = pose.position;
    const auto& rotationGlm = pose.rotation.GetGLM();
    const Mathematics::Quaternion rotation{static_cast<float32>(rotationGlm.w),
                                           static_cast<float32>(rotationGlm.x),
                                           static_cast<float32>(rotationGlm.y),
                                           static_cast<float32>(rotationGlm.z)};
    const Components::Transform composed = Components::Transform::FromTRS(position, rotation, scale);

    // Each target is gated on its own bytes, so a component that already holds the pose keeps its
    // Version and emits no dirty-feed entry.
    if (!SamePoseBytes(transform.Position, transform.Basis, composed.Position, composed.Basis))
        transform = composed;

    if (!SamePoseBytes(worldTransform.Position, worldTransform.Basis, composed.Position, composed.Basis))
    {
        worldTransform.Position = composed.Position;
        worldTransform.Basis = composed.Basis;
        // A direct WorldTransform write: bump Version and emit the dirty-feed entry so this frame's
        // consumers (Scene TLAS, motion vectors, GPUScene upload) see the move; the hierarchy would
        // only re-derive it from the local Transform on the next frame.
        Components::BumpWorldTransform(world, entity, worldTransform);
    }

    record.Valid = interpolateFrom == nullptr || SamePose(*interpolateFrom, current);
    record.AppliedPose = current;
    record.WrittenPosition = composed.Position;
    record.WrittenBasis = composed.Basis;
    return true;
}

} // namespace GameEngine::PhysicsECS
