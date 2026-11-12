#pragma once

#include "ECS/RequiredComponents.h"
#include "Types/Types.h"
#include "Mathematics/Angle.h"
#include "Mathematics/MulAdd.h"
#include "Mathematics/Matrix3x3.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/WorldPosition.h"

#include <cassert>
#include <cmath>
#include <cstddef>

namespace GameEngine
{

using Mathematics::Vector3;
using Mathematics::Quaternion;

namespace Components
{

// Local placement relative to Parent: a position in double and a 3x3 basis.
//
// Authoring/editor code should treat this as the local transform. World-space
// consumers (rendering, bounds, physics, etc.) are expected to read from
// WorldTransform, which is derived from Transform + Parent hierarchy by
// TransformHierarchySystem (see docs/scripting/transform-hierarchy.mdx).
// @ge-no-add  every entity always has a Transform; not user-addable via the menu.
struct Transform
{
    // Structure, not a feature: an entity's placement has no off state.
    static constexpr bool NotToggleable = true;

    // Meters, relative to the parent. The bottom row is always 0, 0, 0, 1.
    Mathematics::WorldPosition Position;
    // Rotation times scale, column-major.
    Mathematics::Matrix3x3 Basis;
    // Zero, so a byte compare of two transforms compares defined bytes.
    uint32 Padding = 0u;  // @ge-hidden

    Transform() = default;

    // LH TRS: matrix * v == rot.Rotate(v) for a unit quaternion; a non-unit one rotates by
    // rot / |rot| without scaling. +Y yaw sends +Z toward +X. The position keeps its doubles;
    // a caller with a Vector3 writes WorldPosition::FromVector3.
    static Transform FromTRS(const Mathematics::WorldPosition& pos, const Quaternion& rot, const Vector3& scale)
    {
        Transform transform = MakeBasis(rot, scale);
        transform.Position = pos;
        return transform;
    }

    /// The transform of an affine 4x4 matrix: its 3x3 and its translation, which widens exactly.
    /// A projective matrix (a bottom row more than 1e-5 from 0, 0, 0, 1) asserts: apply a projection after the transform.
    static Transform FromMatrix4x4(const Mathematics::Matrix4x4& matrix)
    {
        // A matrix read from a file carries rounding. C# Transform.FromMatrix4x4 uses the same value.
        constexpr float kAffineTolerance = 1e-5f;
        [[maybe_unused]] const bool affine = std::abs(matrix[0][3]) <= kAffineTolerance && std::abs(matrix[1][3]) <= kAffineTolerance &&
                                             std::abs(matrix[2][3]) <= kAffineTolerance && std::abs(matrix[3][3] - 1.0f) <= kAffineTolerance;
        assert(affine && "Transform::FromMatrix4x4 takes an affine matrix: its bottom row is not 0, 0, 0, 1. Build the transform with FromTRS, or apply the projection after the transform.");
        Transform transform;
        transform.Basis = Mathematics::Matrix3x3(glm::mat3(matrix.GetGLM()));
        transform.Position = Mathematics::WorldPosition::FromVector3(Vector3(glm::vec3(matrix[3])));
        return transform;
    }

    Mathematics::WorldPosition GetPosition() const { return Position; }

    /// The 4x4 matrix of the basis with `translation`; the caller chooses the 32-bit translation.
    Mathematics::Matrix4x4 ToMatrix4x4(const Vector3& translation) const
    {
        return Mathematics::Matrix4x4::FromBasis(Basis, translation);
    }

    // Quaternion whose Rotate() matches this basis (FromTRS convention).
    //
    // GetRotation, GetScale and FromTRS evaluate in double and round once to float. Evaluated in
    // float, decomposing a composed basis lands several ulps away from the inputs and a scene
    // save/load cycle random-walks the scale; in double the cycle jitters within a bounded
    // distance of its first save instead. The SceneIO round-trip test poses stay within that test's
    // bound (scale 2 ulp, rotation 2^-23 per component); this is not guaranteed for every pose. A
    // float basis cannot carry the low bits of a small quaternion component, so bit-exact
    // recovery is not reachable while Transform stores a basis.
    Quaternion GetRotation() const
    {
        // Extract rotation by removing scale from the column-major basis.
        // First compute per-axis scales from column lengths.
        const float64 sx = ColumnLength(0);
        const float64 sy = ColumnLength(1);
        const float64 sz = ColumnLength(2);

        if (sx <= 0.0 || sy <= 0.0 || sz <= 0.0)
        {
            return Quaternion{}; // Degenerate, return identity
        }

        // Build a pure rotation 3x3 (rows) from the scaled columns
        const float64 r00 = Basis[0].x / sx;
        const float64 r10 = Basis[0].y / sx;
        const float64 r20 = Basis[0].z / sx;

        const float64 r01 = Basis[1].x / sy;
        const float64 r11 = Basis[1].y / sy;
        const float64 r21 = Basis[1].z / sy;

        const float64 r02 = Basis[2].x / sz;
        const float64 r12 = Basis[2].y / sz;
        const float64 r22 = Basis[2].z / sz;

        const float64 trace = r00 + r11 + r22;
        float64 qw, qx, qy, qz;

        if (trace > 0.0)
        {
            const float64 s = std::sqrt(trace + 1.0) * 2.0;
            qw = 0.25 * s;
            qx = (r21 - r12) / s;
            qy = (r02 - r20) / s;
            qz = (r10 - r01) / s;
        }
        else if (r00 > r11 && r00 > r22)
        {
            const float64 s = std::sqrt(1.0 + r00 - r11 - r22) * 2.0;
            qw = (r21 - r12) / s;
            qx = 0.25 * s;
            qy = (r01 + r10) / s;
            qz = (r02 + r20) / s;
        }
        else if (r11 > r22)
        {
            const float64 s = std::sqrt(1.0 + r11 - r00 - r22) * 2.0;
            qw = (r02 - r20) / s;
            qx = (r01 + r10) / s;
            qy = 0.25 * s;
            qz = (r12 + r21) / s;
        }
        else
        {
            const float64 s = std::sqrt(1.0 + r22 - r00 - r11) * 2.0;
            qw = (r10 - r01) / s;
            qx = (r02 + r20) / s;
            qy = (r12 + r21) / s;
            qz = 0.25 * s;
        }

        return Quaternion(static_cast<float32>(qw), static_cast<float32>(qx), static_cast<float32>(qy),
                          static_cast<float32>(qz));
    }

    Vector3 GetScale() const
    {
        return Vector3{static_cast<float32>(ColumnLength(0)), static_cast<float32>(ColumnLength(1)),
                       static_cast<float32>(ColumnLength(2))};
    }

    void SetIdentity()
    {
        *this = Transform{};
    }

    void Translate(float32 x, float32 y, float32 z)
    {
        Position = Position + Vector3(x, y, z);
    }

    void Rotate(float32 x, float32 y, float32 z, float32 w)
    {
        *this = FromTRS(Position, Quaternion(w, x, y, z), GetScale());
    }

    void SetScale(float32 x, float32 y, float32 z)
    {
        // Replaces the basis diagonal; it does not multiply.
        Basis[0].x = x;
        Basis[1].y = y;
        Basis[2].z = z;
    }

  private:
    // Length of a basis column, in double (see GetRotation).
    float64 ColumnLength(int column) const
    {
        const float64 a = Basis[column].x;
        const float64 b = Basis[column].y;
        const float64 c = Basis[column].z;
        return std::sqrt(Math::MulAdd(c, c, Math::MulAdd(a, a, b * b)));
    }

    // The LH rotation-times-scale basis of FromTRS, at the origin.
    static Transform MakeBasis(const Quaternion& rot, const Vector3& scale)
    {
        Transform transform;

        const auto& q = rot.GetGLM();
        const float64 x = q.x;
        const float64 y = q.y;
        const float64 z = q.z;
        const float64 w = q.w;

        // Rotates by q / |q|: scaling the products by 2 / |q|^2 keeps a non-unit quaternion's norm
        // out of the scale columns. Rounding a normalised quaternion to float before composing
        // would put a 1 +- 2^-24 factor there instead, which a save/load cycle accumulates. A
        // zero quaternion composes as the identity rotation.
        const float64 normSquared = Math::MulAdd(w, w, Math::MulAdd(z, z, Math::MulAdd(x, x, y * y)));
        const float64 s = normSquared > 0.0 ? 2.0 / normSquared : 0.0;
        const float64 x2 = x * s;
        const float64 y2 = y * s;
        const float64 z2 = z * s;

        const float64 xx = x * x2;
        const float64 xy = x * y2;
        const float64 xz = x * z2;
        const float64 yy = y * y2;
        const float64 yz = y * z2;
        const float64 zz = z * z2;
        const float64 wx = w * x2;
        const float64 wy = w * y2;
        const float64 wz = w * z2;

        // Hamilton (q*v) 3x3.
        const float64 r00 = 1.0 - (yy + zz);
        const float64 r11 = 1.0 - (xx + zz);
        const float64 r22 = 1.0 - (xx + yy);
        const float64 r01 = xy - wz;
        const float64 r02 = xz + wy;
        const float64 r10 = xy + wz;
        const float64 r12 = yz - wx;
        const float64 r20 = xz - wy;
        const float64 r21 = yz + wx;

        const float64 sx = scale.x;
        const float64 sy = scale.y;
        const float64 sz = scale.z;

        transform.Basis[0] = glm::vec3(static_cast<float32>(r00 * sx), static_cast<float32>(r10 * sx),
                                       static_cast<float32>(r20 * sx));
        transform.Basis[1] = glm::vec3(static_cast<float32>(r01 * sy), static_cast<float32>(r11 * sy),
                                       static_cast<float32>(r21 * sy));
        transform.Basis[2] = glm::vec3(static_cast<float32>(r02 * sz), static_cast<float32>(r12 * sz),
                                       static_cast<float32>(r22 * sz));

        return transform;
    }
};
static_assert(sizeof(Transform) == 64);

// World-space transform derived from local Transform + Parent by
// TransformHierarchySystem. Its Position and Basis have Transform's layout;
// Version takes the padding.
//
// CANONICAL WRITER: TransformHierarchySystem. Other systems should mutate
// the local Components::Transform and let the hierarchy propagate. This is
// the recommended path because the hierarchy auto-bumps Version on actual
// content change (memcmp-gated).
//
// DIRECT WRITERS (rare; today: PhysicsWritebackSystem, NavigationMovementSystem,
// GamepadCameraControllerSystem, editor tools) must call
// Components::BumpWorldTransform(world, entity, wt)
// (Components/TransformDirtyFeed.h) after mutating the pose bytes — it bumps
// Version AND emits the entity into the world's dirty feed in one step, so
// the two cannot fork. A bare `++wt.Version;` bump keeps Version-polling
// consumers working but silently starves feed consumers (extraction's
// patch lane). Audit any new direct writer
// against this contract.
//
// A THIRD channel is the per-(chunk, column) write grant that Changed<WorldTransform> consumers filter on. A
// Write<WorldTransform> query visit or GetComponentForWrite issues it. A writer through a pointer it got some
// other way stamps with World::StampComponentWriteBatch, as the transform hierarchy does.
//
// VERSION SEMANTICS: monotonic uint32, may wrap at ~4 billion frames (>2 years
// at 60 FPS). Consumers compare cached vs current value; equality means "no
// change since last observation." On an entity with a Transform, Version 0
// means TransformHierarchySystem has not resolved it yet: the component is
// placed with the Transform (it is required), so a consumer that needs the
// placed pose before the first resolve waits for a nonzero Version, not for the
// component. On an entity with no Transform, nothing resolves it, and whatever
// it holds is the entity's pose. Wrap is not handled in v1 — if it ever
// matters, switch to uint64 or rebuild detection logic.
// [DoNotSerialize] — derived world pose, recomputed from Transform each frame; never authored.
// @ge-readonly TransformHierarchySystem
struct WorldTransform
{
    // Derived from Transform and the Parent chain: it has no off state of its own.
    static constexpr bool NotToggleable = true;

    // World meters. The bottom row is always 0, 0, 0, 1.
    Mathematics::WorldPosition Position;
    // Rotation times scale, column-major.
    Mathematics::Matrix3x3 Basis;
    uint32 Version = 0u;

    Mathematics::WorldPosition GetPosition() const { return Position; }

    /// The 4x4 matrix of the basis with `translation`; the caller chooses the 32-bit translation.
    Mathematics::Matrix4x4 ToMatrix4x4(const Vector3& translation) const
    {
        return Mathematics::Matrix4x4::FromBasis(Basis, translation);
    }
};
static_assert(sizeof(WorldTransform) == 64);
// The bytes of a pose: Position and Basis, the same offsets in both components.
inline constexpr size_t kTransformPoseBytes = offsetof(WorldTransform, Version);
// Padding fills the tail, so a byte compare over sizeof(Transform) reads no indeterminate bytes.
// The size assert alone does not catch a dropped Padding: WorldPosition's 8-byte alignment keeps 64.
static_assert(offsetof(Transform, Padding) == kTransformPoseBytes);

// The world pose of `local` under `parent`: parent * local in affine form. The basis
// multiplies in float in MultiplyColumnMajor4x4's order; the position adds in double.
inline void ComposeWorldPose(const WorldTransform& parent, const Transform& local, WorldTransform& out)
{
    const float32* a = parent.Basis.Data();
    const float32* b = local.Basis.Data();
    float32* basis = out.Basis.Data();
    for (int column = 0; column < 3; ++column)
    {
        const float32* bc = b + column * 3;
        for (int row = 0; row < 3; ++row)
            basis[column * 3 + row] = Math::MulAdd(a[6 + row], bc[2], Math::MulAdd(a[row], bc[0], a[3 + row] * bc[1]));
    }
    const Mathematics::WorldPosition& p = local.Position;
    const Mathematics::WorldPosition& q = parent.Position;
    out.Position = Mathematics::WorldPosition{
        q.X + Math::MulAdd(float64(a[6]), p.Z, Math::MulAdd(float64(a[3]), p.Y, float64(a[0]) * p.X)),
        q.Y + Math::MulAdd(float64(a[7]), p.Z, Math::MulAdd(float64(a[4]), p.Y, float64(a[1]) * p.X)),
        q.Z + Math::MulAdd(float64(a[8]), p.Z, Math::MulAdd(float64(a[5]), p.Y, float64(a[2]) * p.X))};
}

// Helper utilities for quaternion  Euler angle conversions (degrees, XYZ = roll, pitch, yaw).
inline Quaternion QuaternionFromEulerXYZDegrees(float32 xDeg, float32 yDeg, float32 zDeg)
{
    float32 roll = Mathematics::ToRadians(xDeg);
    float32 pitch = Mathematics::ToRadians(yDeg);
    float32 yaw = Mathematics::ToRadians(zDeg);

    float32 cy = std::cos(yaw * 0.5f);
    float32 sy = std::sin(yaw * 0.5f);
    float32 cp = std::cos(pitch * 0.5f);
    float32 sp = std::sin(pitch * 0.5f);
    float32 cr = std::cos(roll * 0.5f);
    float32 sr = std::sin(roll * 0.5f);

    float32 w = cr * cp * cy + sr * sp * sy;
    float32 x = sr * cp * cy - cr * sp * sy;
    float32 y = cr * sp * cy + sr * cp * sy;
    float32 z = cr * cp * sy - sr * sp * cy;
    return Quaternion(w, x, y, z);
}

inline void EulerXYZDegreesFromQuaternion(const Quaternion& q, float32& xDeg, float32& yDeg, float32& zDeg)
{
    const auto& g = q.GetGLM();

    // roll (X-axis rotation)
    float32 sinr_cosp = 2.0f * (g.w * g.x + g.y * g.z);
    float32 cosr_cosp = 1.0f - 2.0f * (g.x * g.x + g.y * g.y);
    float32 roll = std::atan2(sinr_cosp, cosr_cosp);

    // pitch (Y-axis rotation)
    float32 sinp = 2.0f * (g.w * g.y - g.z * g.x);
    float32 pitch;
    if (std::fabs(sinp) >= 1.0f)
    {
        pitch = (sinp > 0.0f ? 1.0f : -1.0f) * (3.14159265358979323846f * 0.5f);
    }
    else
    {
        pitch = std::asin(sinp);
    }

    // yaw (Z-axis rotation)
    float32 siny_cosp = 2.0f * (g.w * g.z + g.x * g.y);
    float32 cosy_cosp = 1.0f - 2.0f * (g.y * g.y + g.z * g.z);
    float32 yaw = std::atan2(siny_cosp, cosy_cosp);

    xDeg = Mathematics::ToDegrees(roll);
    yDeg = Mathematics::ToDegrees(pitch);
    zDeg = Mathematics::ToDegrees(yaw);
}

// Convenience alias for clarity at callsites where we want to emphasize
// that Transform is local-space.
using LocalTransform = Transform;

} // namespace Components
} // namespace GameEngine

// Every entity with a Transform holds its WorldTransform from creation, so
// nothing adds it a frame later with an archetype move per entity.
template <>
struct GameEngine::ECS::Detail::RequiredComponents<GameEngine::Components::Transform>
{
    using type = TypeList<GameEngine::Components::WorldTransform>;
};
