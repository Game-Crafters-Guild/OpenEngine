#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "Components/Transform.h"
#include "Components/TransformDirtyFeed.h"
#include "Components/Rendering/Light.h"
#include "Components/Hierarchy.h"
#include "ECS/World.h"
#include "ECS/Query.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/MatrixOps.h"
#include "DeathTestChild.h"

using namespace GameEngine;
using namespace GameEngine::Components;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;
using GameEngine::Mathematics::WorldPosition;

namespace
{
    constexpr float kDegEpsilon = 0.1f; // generous for composed conversions

    void ExpectEulerNear(float ex, float ey, float ez,
                         float ax, float ay, float az,
                         float eps = kDegEpsilon)
    {
        EXPECT_NEAR(ax, ex, eps);
        EXPECT_NEAR(ay, ey, eps);
        EXPECT_NEAR(az, ez, eps);
    }

	    constexpr float kMatrixEpsilon   = 1e-4f;
	    constexpr float kPositionEpsilon = 1e-4f;
	    constexpr float kScaleEpsilon    = 1e-4f;
}

namespace
{
    // The pose as a float matrix with the translation relative to the world origin.
    template <typename Pose>
    Mathematics::Matrix4x4 PoseMatrix(const Pose& pose)
    {
        return pose.ToMatrix4x4(pose.GetPosition() - WorldPosition{});
    }

    template <typename PoseA, typename PoseB>
    void ExpectPoseMatrixNear(const PoseA& a, const PoseB& b, float epsilon)
    {
        const Mathematics::Matrix4x4 matrixA = PoseMatrix(a);
        const Mathematics::Matrix4x4 matrixB = PoseMatrix(b);
        for (int i = 0; i < 16; ++i)
        {
            EXPECT_NEAR(matrixA.Data()[i], matrixB.Data()[i], epsilon) << "matrix element " << i;
        }
    }

    Vector3 MulPoint(const Transform& t, const Vector3& p)
    {
        return PoseMatrix(t).TransformPoint(p);
    }
}

TEST(TransformMathTests, Identity_PlusZIsEntityForward)
{
    const Transform t = Transform::FromTRS(WorldPosition{}, Quaternion{}, Vector3(1.0f, 1.0f, 1.0f));
    const Vector3 forward = MulPoint(t, Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(forward.x, 0.0f, 1e-4f);
    EXPECT_NEAR(forward.y, 0.0f, 1e-4f);
    EXPECT_NEAR(forward.z, 1.0f, 1e-4f);
}

TEST(TransformMathTests, DefaultSunEuler_PlusZShinesDownward)
{
    const Quaternion q = QuaternionFromEulerXYZDegrees(50.0f, -30.0f, 0.0f);
    const Transform t = Transform::FromTRS(WorldPosition{}, q, Vector3(1.0f, 1.0f, 1.0f));
    const Vector3 shine = MulPoint(t, Vector3(0.0f, 0.0f, 1.0f));
    const Vector3 viaQuat = q.Rotate(Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_LT(shine.y, 0.0f);
    EXPECT_NEAR(shine.x, viaQuat.x, 1e-4f);
    EXPECT_NEAR(shine.y, viaQuat.y, 1e-4f);
    EXPECT_NEAR(shine.z, viaQuat.z, 1e-4f);
}

TEST(TransformMathTests, FromTRS_YawPositiveSendsZTowardX)
{
    const float kHalfPi = 1.57079632679f;
    const Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), kHalfPi);
    const Transform t = Transform::FromTRS(WorldPosition{}, q, Vector3(1.0f, 1.0f, 1.0f));
    const Vector3 rotated = MulPoint(t, Vector3(0.0f, 0.0f, 1.0f));
    const Vector3 viaQuat = q.Rotate(Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(rotated.x, 1.0f, 1e-4f);
    EXPECT_NEAR(rotated.y, 0.0f, 1e-4f);
    EXPECT_NEAR(rotated.z, 0.0f, 1e-4f);
    EXPECT_NEAR(rotated.x, viaQuat.x, 1e-4f);
    EXPECT_NEAR(rotated.y, viaQuat.y, 1e-4f);
    EXPECT_NEAR(rotated.z, viaQuat.z, 1e-4f);
}

// A projective matrix has no transform: FromMatrix4x4 asserts with the fix in the message.
TEST(TransformMathTests, FromMatrix4x4_RefusesAProjectiveBottomRow)
{
    Mathematics::Matrix4x4 projective = Mathematics::Matrix4x4::Identity();
    projective[2][3] = 1.0f;
    projective[3][3] = 0.0f;
    EXPECT_DEBUG_DEATH(
        {
            TestUtils::SuppressCrtDialogsInDeathTestChild();
            [[maybe_unused]] const Transform t = Transform::FromMatrix4x4(projective);
        },
        "affine");
    const Transform affine = Transform::FromMatrix4x4(Mathematics::Matrix4x4::Identity());
    EXPECT_EQ(affine.Position, WorldPosition{});
}

TEST(TransformMathTests, FromTRS_GetRotationMatchesInput)
{
    const float kHalfPi = 1.57079632679f;
    const Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), kHalfPi);
    const Transform t = Transform::FromTRS(WorldPosition{1.0, 2.0, 3.0}, q, Vector3(1.0f, 1.0f, 1.0f));
    const Quaternion back = t.GetRotation();
    const Vector3 a = q.Rotate(Vector3(0.0f, 0.0f, 1.0f));
    const Vector3 b = back.Rotate(Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(a.x, b.x, 1e-4f);
    EXPECT_NEAR(a.y, b.y, 1e-4f);
    EXPECT_NEAR(a.z, b.z, 1e-4f);
}

// Round-trip on compound rotations: a single-axis case cannot catch a
// component swizzle or a dropped term, so pin GetRotation(FromTRS(q)) on
// quats with all components non-zero, checked against two basis vectors.
TEST(TransformMathTests, FromTRS_GetRotationRoundTripsCompoundRotations)
{
    const Quaternion cases[] = {
        QuaternionFromEulerXYZDegrees(50.0f, -30.0f, 0.0f), // the default sun pose
        QuaternionFromEulerXYZDegrees(50.0f, -30.0f, 20.0f),
        Quaternion(0.8923991f, 0.2391176f, 0.3696438f, -0.0990458f), // yaw45 then pitch30
    };
    const Vector3 probes[] = {Vector3(0.0f, 0.0f, 1.0f), Vector3(1.0f, 0.0f, 0.0f)};
    for (const Quaternion& q : cases)
    {
        const Transform t = Transform::FromTRS(WorldPosition{}, q, Vector3(1.0f, 1.0f, 1.0f));
        const Quaternion back = t.GetRotation();
        for (const Vector3& v : probes)
        {
            const Vector3 a = q.Rotate(v);
            const Vector3 b = back.Rotate(v);
            EXPECT_NEAR(a.x, b.x, 1e-4f);
            EXPECT_NEAR(a.y, b.y, 1e-4f);
            EXPECT_NEAR(a.z, b.z, 1e-4f);
        }
    }
}

// The Basic3D template ships this literal; it must be the engine's own
// Euler-to-quaternion output, unit-norm. A hand-derived value with a dropped
// component denormalizes the rotation basis FromTRS builds from it.
TEST(TransformMathTests, DefaultSunSceneLiteralMatchesEulerComposition)
{
    const Quaternion q = QuaternionFromEulerXYZDegrees(
        kDefaultDirectionalLightEulerXDeg,
        kDefaultDirectionalLightEulerYDeg,
        kDefaultDirectionalLightEulerZDeg);
    const auto& g = q.GetGLM();
    EXPECT_NEAR(g.x, 0.408218f, 1e-5f);
    EXPECT_NEAR(g.y, -0.23457f, 1e-5f);
    EXPECT_NEAR(g.z, 0.109382f, 1e-5f);
    EXPECT_NEAR(g.w, 0.875426f, 1e-5f);
    EXPECT_NEAR(g.x * g.x + g.y * g.y + g.z * g.z + g.w * g.w, 1.0f, 1e-5f);
}

// Sanity check: direct Euler -> quaternion -> Euler round-trip
TEST(TransformMathTests, EulerQuaternionRoundTrip_Direct)
{
    struct Sample { float x, y, z; };
    const Sample kSamples[] = {
        { 0.0f, 0.0f, 0.0f },
        { 5.0f, 6.0f, 0.0f },
        { 10.0f, -20.0f, 30.0f },
        { -45.0f, 30.0f, -10.0f },
        { 0.0f, 89.0f, 0.0f },
        { 0.0f, -89.0f, 0.0f },
        { 179.0f, 0.0f, 0.0f },
    };

    for (const auto& s : kSamples)
    {
        Quaternion q = QuaternionFromEulerXYZDegrees(s.x, s.y, s.z);

        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        EulerXYZDegreesFromQuaternion(q, rx, ry, rz);

        ExpectEulerNear(s.x, s.y, s.z, rx, ry, rz, 1e-3f);
    }
}

// Larger and negative angles through TRS, identity scale
TEST(TransformMathTests, TransformTRSRoundTrip_LargeAndNegative_IdentityScale)
{
    struct Sample { float x, y, z; };
    const Sample kSamples[] = {
        { 0.0f, 0.0f, 0.0f },
        { -45.0f, 30.0f, -10.0f },
        { 90.0f, 0.0f, 0.0f },
        { 0.0f, 90.0f, 0.0f },
        { 0.0f, 0.0f, 90.0f },
        { 120.0f, -60.0f, 45.0f },
    };

    Vector3 pos(0.0f, 0.0f, 0.0f);
    Vector3  scale(1.0f, 1.0f, 1.0f);

    for (const auto& s : kSamples)
    {
        Quaternion q = QuaternionFromEulerXYZDegrees(s.x, s.y, s.z);
        Transform t = Transform::FromTRS(WorldPosition::FromVector3(pos), q, scale);

        Quaternion q2 = t.GetRotation();
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        EulerXYZDegreesFromQuaternion(q2, rx, ry, rz);
        ExpectEulerNear(s.x, s.y, s.z, rx, ry, rz);
    }
}

// TRS should not affect translation when extracting rotation/scale
TEST(TransformMathTests, TransformTRS_PreservesTranslation)
{
    Vector3 pos(3.0f, -4.0f, 5.0f);
    Vector3  scale(2.0f, 3.0f, 4.0f);
    Quaternion q = QuaternionFromEulerXYZDegrees(15.0f, -25.0f, 35.0f);

    Transform t = Transform::FromTRS(WorldPosition::FromVector3(pos), q, scale);

    const WorldPosition outPos = t.GetPosition();
    EXPECT_NEAR(outPos.X, pos.x, 1e-4f);
    EXPECT_NEAR(outPos.Y, pos.y, 1e-4f);
    EXPECT_NEAR(outPos.Z, pos.z, 1e-4f);
}

// Full pipeline: Euler -> quaternion -> Transform matrix -> quaternion -> Euler
TEST(TransformMathTests, TransformTRSRoundTrip_SmallAngles_IdentityScale)
{
    const float ex = 5.0f;
    const float ey = 6.0f;
    const float ez = 0.0f;

    Vector3 pos(0.0f, 0.0f, 0.0f);
    Vector3 scale(1.0f, 1.0f, 1.0f);

    Quaternion q = QuaternionFromEulerXYZDegrees(ex, ey, ez);
    Transform t = Transform::FromTRS(WorldPosition::FromVector3(pos), q, scale);

    Quaternion q2 = t.GetRotation();
    EXPECT_NEAR(q2.GetGLM().x, q.GetGLM().x, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().y, q.GetGLM().y, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().z, q.GetGLM().z, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().w, q.GetGLM().w, 1e-4f);
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    EulerXYZDegreesFromQuaternion(q2, rx, ry, rz);

    ExpectEulerNear(ex, ey, ez, rx, ry, rz);
}

TEST(TransformMathTests, TransformTRSRoundTrip_SmallAngles_NonUniformScale)
{
    const float ex = 5.0f;
    const float ey = 6.0f;
    const float ez = 0.0f;

    Vector3 pos(1.0f, -2.0f, 3.0f);
    Vector3 scale(2.0f, 3.0f, 4.0f);

    Quaternion q = QuaternionFromEulerXYZDegrees(ex, ey, ez);
    Transform t = Transform::FromTRS(WorldPosition::FromVector3(pos), q, scale);

    Quaternion q2 = t.GetRotation();
    EXPECT_NEAR(q2.GetGLM().x, q.GetGLM().x, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().y, q.GetGLM().y, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().z, q.GetGLM().z, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().w, q.GetGLM().w, 1e-4f);
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    EulerXYZDegreesFromQuaternion(q2, rx, ry, rz);

    ExpectEulerNear(ex, ey, ez, rx, ry, rz);
}

	// Full TRS round-trip: build a Transform from TRS, decompose back to
	// components, then rebuild and require the matrices to match.
	TEST(TransformMathTests, TransformTRSFullRoundTrip_MatrixAndComponents)
	{
	    struct TrsCase
	    {
	        const char* name;
	        Vector3     position;
	        Quaternion  rotation;
	        Vector3     scale;
	    };

	    const TrsCase kCases[] = {
	        { "Identity",
	          Vector3(0.0f, 0.0f, 0.0f),
	          Quaternion{},
	          Vector3(1.0f, 1.0f, 1.0f) },
	        { "TranslateOnly",
	          Vector3(1.0f, -2.0f, 3.5f),
	          Quaternion{},
	          Vector3(1.0f, 1.0f, 1.0f) },
	        { "RotateOnly_Y90",
	          Vector3(0.0f, 0.0f, 0.0f),
	          QuaternionFromEulerXYZDegrees(0.0f, 90.0f, 0.0f),
	          Vector3(1.0f, 1.0f, 1.0f) },
	        { "TranslateRotate",
	          Vector3(2.0f, 0.0f, -1.0f),
	          QuaternionFromEulerXYZDegrees(15.0f, -25.0f, 35.0f),
	          Vector3(1.0f, 1.0f, 1.0f) },
	        { "UniformScale",
	          Vector3(0.0f, 0.0f, 0.0f),
	          Quaternion{},
	          Vector3(2.0f, 2.0f, 2.0f) },
	        { "TranslateRotateScale",
	          Vector3(1.0f, -3.0f, 4.0f),
	          QuaternionFromEulerXYZDegrees(10.0f, 20.0f, -15.0f),
	          Vector3(2.0f, 3.0f, 4.0f) },
	    };

	    for (const auto& c : kCases)
	    {
	        SCOPED_TRACE(c.name);

	        Transform t0 = Transform::FromTRS(WorldPosition::FromVector3(c.position), c.rotation, c.scale);

	        const WorldPosition p1 = t0.GetPosition();
	        Quaternion r1 = t0.GetRotation();
	        Vector3 s1 = t0.GetScale();

	        // Components should round-trip.
	        EXPECT_NEAR(p1.X, c.position.x, kPositionEpsilon);
	        EXPECT_NEAR(p1.Y, c.position.y, kPositionEpsilon);
	        EXPECT_NEAR(p1.Z, c.position.z, kPositionEpsilon);

	        EXPECT_NEAR(s1.x, c.scale.x, kScaleEpsilon);
	        EXPECT_NEAR(s1.y, c.scale.y, kScaleEpsilon);
	        EXPECT_NEAR(s1.z, c.scale.z, kScaleEpsilon);

	        Transform t1 = Transform::FromTRS(p1, r1, s1);

	        ExpectPoseMatrixNear(t0, t1, kMatrixEpsilon);
	    }
	}

	// Simple hierarchy sanity: a child under a translated root should match
	// manual TRS composition using the same column-major multiply as the
	// TransformHierarchySystem.
	TEST(TransformMathTests, Hierarchy_CompositionMatchesTransformHierarchySystem_TranslateOnly)
	{
	    ECS::World world;

	    ECS::Entity parent = world.Create();
	    ECS::Entity child  = world.Create();

	    Vector3 parentPos(1.0f, 2.0f, 3.0f);
	    Quaternion parentRot{};
	    Vector3 parentScale(1.0f, 1.0f, 1.0f);

	    Vector3 childLocalPos(4.0f, 5.0f, -2.0f);
	    Quaternion childRot{};
	    Vector3 childScale(1.0f, 1.0f, 1.0f);

	    Transform parentLocal = Transform::FromTRS(WorldPosition::FromVector3(parentPos), parentRot, parentScale);
	    Transform childLocal  = Transform::FromTRS(WorldPosition::FromVector3(childLocalPos), childRot, childScale);

	    parent.Set(parentLocal);
	    Parent parentComponent{};
	    parentComponent.Entity = parent.GetHandle();
	    child.Set(childLocal);
	    child.Set(parentComponent);

	    world.ProcessCommands();

	    Engine::Renderer::TransformHierarchySystem system;
	    system.Update(world, 0.0f);

	    auto* parentWorld = world.GetComponent<WorldTransform>(parent.GetHandle());
	    auto* childWorld  = world.GetComponent<WorldTransform>(child.GetHandle());

	    ASSERT_NE(parentWorld, nullptr);
	    ASSERT_NE(childWorld, nullptr);

	    // Root world transform should match its local transform.
	    ExpectPoseMatrixNear(*parentWorld, parentLocal, kMatrixEpsilon);

	    // Child world transform should match manual matrix composition.
	    const Mathematics::Matrix4x4 expectedChildWorld = PoseMatrix(*parentWorld) * PoseMatrix(childLocal);
	    const Mathematics::Matrix4x4 actualChildWorld = PoseMatrix(*childWorld);
	    for (int i = 0; i < 16; ++i)
	    {
	        EXPECT_NEAR(actualChildWorld.Data()[i], expectedChildWorld.Data()[i], kMatrixEpsilon);
	    }
	}

		// More complex hierarchy case: parent and child both have rotation and
		// non-uniform scale. World transforms from TransformHierarchySystem
		// should still match manual column-major matrix composition.
		TEST(TransformMathTests, Hierarchy_CompositionMatchesTransformHierarchySystem_RotateScale)
		{
		    ECS::World world;

		    ECS::Entity parent = world.Create();
		    ECS::Entity child  = world.Create();

		    Vector3   parentPos(1.0f, -2.0f, 3.0f);
		    Quaternion parentRot = QuaternionFromEulerXYZDegrees(20.0f, -30.0f, 15.0f);
		    Vector3   parentScale(2.0f, 1.5f, 0.5f);

		    Vector3   childLocalPos(-1.0f, 0.5f, 2.0f);
		    Quaternion childRot = QuaternionFromEulerXYZDegrees(-10.0f, 45.0f, 5.0f);
		    Vector3   childScale(0.75f, 1.25f, 1.5f);

		    Transform parentLocal = Transform::FromTRS(WorldPosition::FromVector3(parentPos), parentRot, parentScale);
		    Transform childLocal  = Transform::FromTRS(WorldPosition::FromVector3(childLocalPos), childRot, childScale);

		    parent.Set(parentLocal);
		    Parent parentComponent{};
		    parentComponent.Entity = parent.GetHandle();
		    child.Set(childLocal);
		    child.Set(parentComponent);

		    world.ProcessCommands();

		    Engine::Renderer::TransformHierarchySystem system;
		    system.Update(world, 0.0f);

		    auto* parentWorld = world.GetComponent<WorldTransform>(parent.GetHandle());
		    auto* childWorld  = world.GetComponent<WorldTransform>(child.GetHandle());

		    ASSERT_NE(parentWorld, nullptr);
		    ASSERT_NE(childWorld, nullptr);

		    // Root world transform should still match its local transform.
		    ExpectPoseMatrixNear(*parentWorld, parentLocal, kMatrixEpsilon);

		    // Child world transform should match manual matrix composition.
		    const Mathematics::Matrix4x4 expectedChildWorld = PoseMatrix(*parentWorld) * PoseMatrix(childLocal);
		    const Mathematics::Matrix4x4 actualChildWorld = PoseMatrix(*childWorld);
		    for (int i = 0; i < 16; ++i)
		    {
		        EXPECT_NEAR(actualChildWorld.Data()[i], expectedChildWorld.Data()[i], kMatrixEpsilon);
		    }
		}

// Cached topology must make ordinary transform edits proportional to the
// affected subtrees, including multi-selection-style batches. A parent and
// child edited in the same frame overlap and therefore traverse their branch
// only once; independent branches remain independent dirty roots.
TEST(TransformMathTests, Hierarchy_CachedTopologyPropagatesCollapsedDirtySubtrees)
{
    ECS::World world(nullptr);
    ECS::Entity root = world.Create(Transform::FromTRS(
        {10.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity branchA = world.Create(Transform::FromTRS(
        {0.0, 2.0, 0.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity leafA = world.Create(Transform::FromTRS(
        {0.0, 0.0, 3.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity branchB = world.Create(Transform::FromTRS(
        {0.0, -2.0, 0.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity leafB = world.Create(Transform::FromTRS(
        {0.0, 0.0, -3.0}, Quaternion{}, {1, 1, 1}));

    Parent toRoot{}; toRoot.Entity = root.GetHandle();
    Parent toA{}; toA.Entity = branchA.GetHandle();
    Parent toB{}; toB.Entity = branchB.GetHandle();
    branchA.Set(toRoot);
    branchB.Set(toRoot);
    leafA.Set(toA);
    leafB.Set(toB);
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 5u);

    // A clean hierarchy frame neither rebuilds nor walks any node.
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 0u);

    // One leaf edit is one propagated node; the other branch stays untouched.
    const uint32 leafBBefore =
        world.GetComponent<WorldTransform>(leafB.GetHandle())->Version;
    world.GetComponentForWrite<Transform>(leafA.GetHandle())->Translate(1.0f, 0.0f, 0.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 1u);
    EXPECT_EQ(world.GetComponent<WorldTransform>(leafB.GetHandle())->Version,
              leafBBefore);

    // Two independent selected leaves produce two dirty roots.
    world.GetComponentForWrite<Transform>(leafA.GetHandle())->Translate(1.0f, 0.0f, 0.0f);
    world.GetComponentForWrite<Transform>(leafB.GetHandle())->Translate(-1.0f, 0.0f, 0.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 2u);

    // A selected parent plus its selected child collapses to the parent's
    // two-node subtree instead of applying the leaf twice.
    world.GetComponentForWrite<Transform>(branchA.GetHandle())->Translate(0.0f, 1.0f, 0.0f);
    world.GetComponentForWrite<Transform>(leafA.GetHandle())->Translate(0.0f, 0.0f, 1.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 2u);

    // A root edit necessarily visits the entire five-node hierarchy.
    world.GetComponentForWrite<Transform>(root.GetHandle())->Translate(1.0f, 0.0f, 0.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 5u);

    // Parent data edits and unrelated structural moves both invalidate cached
    // pointers/topology and force exactly one rebuild.
    world.GetComponentForWrite<Parent>(leafA.GetHandle())->Entity = branchB.GetHandle();
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 2u);
    const WorldTransform* reparented =
        world.GetComponent<WorldTransform>(leafA.GetHandle());
    ASSERT_NE(reparented, nullptr);
    EXPECT_DOUBLE_EQ(reparented->Position.X, 13.0);
    EXPECT_DOUBLE_EQ(reparented->Position.Y, -2.0);
    EXPECT_DOUBLE_EQ(reparented->Position.Z, 4.0);

    root.Set(HierarchyOrder{7});
    world.ProcessCommands();
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 3u);
}

// A Transform brings its WorldTransform at creation (Version 0: not resolved
// yet). The first resolve marks it resolved even when the pose is identity, so
// a consumer waiting for a placed pose never waits forever; a still frame
// after that leaves the Version alone. Covers both the flat and the hierarchy
// path.
TEST(TransformMathTests, Hierarchy_FirstResolveMarksEveryPoseResolved)
{
    ECS::World world(nullptr);
    ECS::Entity root = world.Create(Transform{});
    ECS::Entity parent = world.Create(Transform{});
    ECS::Entity child = world.Create(Transform{});
    child.Set(Parent{parent.GetHandle()});
    world.ProcessCommands();
    for (ECS::Entity* entity : {&root, &parent, &child})
    {
        const auto* worldTransform = world.GetComponent<WorldTransform>(entity->GetHandle());
        ASSERT_NE(worldTransform, nullptr) << "a Transform entity is created without its WorldTransform";
        EXPECT_EQ(worldTransform->Version, 0u);
    }

    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    hierarchy.Update(world, 0.0f);

    for (ECS::Entity* entity : {&root, &parent, &child})
        EXPECT_EQ(world.GetComponent<WorldTransform>(entity->GetHandle())->Version, 1u)
            << "an identity pose stayed unresolved, or a still frame bumped it again";
}

// A grandchild of a root at 300 km resolves to the exact sum of the three local
// positions, within 0.1 mm. A 32-bit translation has a spacing of 31 mm there.
TEST(TransformMathTests, Hierarchy_GrandchildAt300KmIsExactToATenthOfAMillimeter)
{
    const Vector3 rootPosition(300000.0f, 1200.0f, -299000.0f);
    const Vector3 childPosition(0.1234567f, 2.5f, 0.0137f);
    const Vector3 grandchildPosition(0.0217f, -0.75f, 0.4321f);
    const Quaternion identity(1.0f, 0.0f, 0.0f, 0.0f);
    const Vector3 unitScale(1.0f, 1.0f, 1.0f);

    ECS::World world(nullptr);
    ECS::Entity root = world.Create(Transform::FromTRS(WorldPosition::FromVector3(rootPosition), identity, unitScale));
    ECS::Entity child = world.Create(Transform::FromTRS(WorldPosition::FromVector3(childPosition), identity, unitScale));
    ECS::Entity grandchild =
        world.Create(Transform::FromTRS(WorldPosition::FromVector3(grandchildPosition), identity, unitScale));
    child.Set(Parent{root.GetHandle()});
    grandchild.Set(Parent{child.GetHandle()});
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);

    const auto position = world.GetComponent<WorldTransform>(grandchild.GetHandle())->GetPosition();
    constexpr double kTolerance = 1.0e-4;
    EXPECT_NEAR(position.X, double(rootPosition.x) + double(childPosition.x) + double(grandchildPosition.x), kTolerance);
    EXPECT_NEAR(position.Y, double(rootPosition.y) + double(childPosition.y) + double(grandchildPosition.y), kTolerance);
    EXPECT_NEAR(position.Z, double(rootPosition.z) + double(childPosition.z) + double(grandchildPosition.z), kTolerance);
}

// A required component stays while its requirer does: removing an entity's
// WorldTransform while its Transform stays is refused, so the entity never
// leaves the hierarchy's and extraction's queries for good.
TEST(TransformMathTests, RemovingWorldTransformWhileTransformStaysIsRefused)
{
    ECS::World world(nullptr);
    ECS::Entity entity = world.Create(Transform{});
    world.RemoveComponentImmediate<WorldTransform>(entity.GetHandle());
    EXPECT_NE(world.GetComponent<WorldTransform>(entity.GetHandle()), nullptr);
    world.RemoveComponentImmediate<Transform>(entity.GetHandle());
    world.RemoveComponentImmediate<WorldTransform>(entity.GetHandle());
    EXPECT_EQ(world.GetComponent<WorldTransform>(entity.GetHandle()), nullptr)
        << "with the Transform gone, nothing requires the WorldTransform";
}

// Entities without a Parent resolve on the flat pass even when a hierarchy
// exists elsewhere in the world: one parented pair must not send a thousand
// unrelated roots down the hierarchy walk.
TEST(TransformMathTests, Hierarchy_UnparentedRootsTakeTheFlatPassBesideAHierarchy)
{
    constexpr int kRoots = 1000;
    ECS::World world(nullptr);
    ECS::Entity parent = world.Create(Transform::FromTRS({0.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity child = world.Create(Transform::FromTRS({1.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    child.Set(Parent{parent.GetHandle()});
    std::vector<ECS::Entity> roots;
    for (int i = 0; i < kRoots; ++i)
        roots.push_back(world.Create(Transform::FromTRS(
            {static_cast<double>(i), 0.0, 0.0}, Quaternion{}, {1, 1, 1})));
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    hierarchy.Update(world, 0.0f);

    for (ECS::Entity& root : roots)
        world.GetComponentForWrite<Transform>(root.GetHandle())->Translate(0.0f, 1.0f, 0.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 0u)
        << "unparented roots walked the hierarchy instead of the flat pass";
    for (ECS::Entity& root : roots)
    {
        const auto* local = world.GetComponent<Transform>(root.GetHandle());
        const auto* worldTransform = world.GetComponent<WorldTransform>(root.GetHandle());
        ASSERT_NE(worldTransform, nullptr);
        EXPECT_EQ(0, std::memcmp(worldTransform, local, kTransformPoseBytes));
    }
}

// Every unit a small hierarchy (a root with a child, the shape an imported
// model has): a still frame walks nothing, and moving every root walks each
// root's two-node subtree once, on the job system.
TEST(TransformMathTests, Hierarchy_EveryUnitAHierarchyStillWalksNothingAndMovesWalkOnce)
{
    constexpr int kUnits = 2000; // past the parallel-propagation threshold
    JobSystem::WorkStealingThreadPool pool(4);
    ECS::World world(&pool);
    std::vector<ECS::Entity> roots, children;
    for (int i = 0; i < kUnits; ++i)
    {
        roots.push_back(world.Create(Transform::FromTRS(
            {static_cast<double>(i), 0.0, 0.0}, Quaternion{}, {1, 1, 1})));
        ECS::Entity child = world.Create(Transform::FromTRS({0.0, 1.0, 0.0}, Quaternion{}, {1, 1, 1}));
        child.Set(Parent{roots.back().GetHandle()});
        children.push_back(child);
    }
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 0u) << "a still frame walked the hierarchy";

    for (ECS::Entity& root : roots)
        world.GetComponentForWrite<Transform>(root.GetHandle())->Translate(0.0f, 0.0f, 2.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), static_cast<size_t>(2 * kUnits));
    for (int i = 0; i < kUnits; ++i)
    {
        const auto* childWorld = world.GetComponent<WorldTransform>(children[i].GetHandle());
        ASSERT_NE(childWorld, nullptr);
        EXPECT_DOUBLE_EQ(childWorld->Position.X, static_cast<double>(i));
        EXPECT_DOUBLE_EQ(childWorld->Position.Y, 1.0);
        EXPECT_DOUBLE_EQ(childWorld->Position.Z, 2.0);
    }
}

// Unparented spawns beside a hierarchy leave the cached topology alone: no
// rebuild, and the flat pass resolves only the chunks the spawns landed in. The
// cache stays usable the same frame: a parent moved beside the spawns still
// carries its child.
TEST(TransformMathTests, Hierarchy_UnparentedSpawnsKeepTheTopologyAndResolveOnlyTheirChunks)
{
    constexpr int kFlat = 20000;
    constexpr int kSpawned = 10;
    ECS::World world(nullptr);
    ECS::Entity parent = world.Create(Transform::FromTRS({0.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity child = world.Create(Transform::FromTRS({1.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    child.Set(Parent{parent.GetHandle()});
    for (int i = 0; i < kFlat; ++i)
        world.Create(Transform::FromTRS({static_cast<double>(i), 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    hierarchy.Update(world, 0.0f);
    const size_t builds = hierarchy.GetTopologyBuildCount();

    std::vector<ECS::Entity> spawned;
    for (int i = 0; i < kSpawned; ++i)
        spawned.push_back(world.Create(Transform::FromTRS(
            {-static_cast<double>(i + 1), 5.0, 0.0}, Quaternion{}, {1, 1, 1})));
    world.ProcessCommands();
    world.GetComponentForWrite<Transform>(parent.GetHandle())->Translate(0.0f, 0.0f, 3.0f);
    hierarchy.Update(world, 0.0f);

    // The counts rest on the change filter's gates; with GE_ECS_CHANGE_FILTER=0
    // every frame resolves everything by design, and only the poses are checked.
    if (ECS::ChangeFilter::Enabled())
    {
        EXPECT_EQ(hierarchy.GetTopologyBuildCount(), builds) << "an unparented spawn rebuilt the topology";
        EXPECT_LT(hierarchy.GetLastFlatResolvedCount(), static_cast<size_t>(kFlat / 2))
            << "the flat pass re-resolved the world after a spawn of " << kSpawned;
    }
    const WorldTransform* childWorld = world.GetComponent<WorldTransform>(child.GetHandle());
    ASSERT_NE(childWorld, nullptr);
    EXPECT_DOUBLE_EQ(childWorld->Position.X, 1.0);
    EXPECT_DOUBLE_EQ(childWorld->Position.Z, 3.0);
    EXPECT_DOUBLE_EQ(world.GetComponent<WorldTransform>(parent.GetHandle())->Position.Z, 3.0);
    for (const int i : {0, kSpawned - 1})
    {
        const WorldTransform* spawnedWorld = world.GetComponent<WorldTransform>(spawned[i].GetHandle());
        ASSERT_NE(spawnedWorld, nullptr);
        EXPECT_DOUBLE_EQ(spawnedWorld->Position.X, -static_cast<double>(i + 1));
        EXPECT_DOUBLE_EQ(spawnedWorld->Position.Y, 5.0);
    }
}

// A cached node whose components move without any Parent edit still carries
// its child: an unrelated destroy that swap-moves the node inside its chunk,
// and growth of the node's archetype by thousands of entities. A child whose
// Parent is removed (an archetype move, no Parent column edit) leaves it.
TEST(TransformMathTests, Hierarchy_NodeMovedByAnUnrelatedStructuralChangeStillCarriesItsChild)
{
    ECS::World world(nullptr);
    // The filler and the parent share an archetype chunk, the parent last:
    // destroying the filler swap-moves the parent into the filler's row.
    ECS::Entity filler = world.Create(Transform::FromTRS({9.0, 9.0, 9.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity parent = world.Create(Transform::FromTRS({0.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity child = world.Create(Transform::FromTRS({1.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    child.Set(Parent{parent.GetHandle()});
    world.ProcessCommands();
    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    hierarchy.Update(world, 0.0f);

    const Transform* parentBefore = world.GetComponent<Transform>(parent.GetHandle());
    world.DestroyEntityImmediate(filler.GetHandle());
    ASSERT_NE(world.GetComponent<Transform>(parent.GetHandle()), parentBefore)
        << "the destroy did not move the parent; the case is not exercised";
    world.GetComponentForWrite<Transform>(parent.GetHandle())->Translate(0.0f, 2.0f, 0.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_DOUBLE_EQ(world.GetComponent<WorldTransform>(child.GetHandle())->Position.Y, 2.0)
        << "the child did not follow a parent that a swap-remove moved";

    const Transform* parentBeforeGrowth = world.GetComponent<Transform>(parent.GetHandle());
    for (int i = 0; i < 5000; ++i)
        world.Create(Transform::FromTRS({static_cast<double>(i), 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    world.ProcessCommands();
    RecordProperty("parentMovedByGrowth", world.GetComponent<Transform>(parent.GetHandle()) != parentBeforeGrowth ? 1 : 0);
    world.GetComponentForWrite<Transform>(parent.GetHandle())->Translate(0.0f, 0.0f, 4.0f);
    hierarchy.Update(world, 0.0f);
    const WorldTransform* childWorld = world.GetComponent<WorldTransform>(child.GetHandle());
    EXPECT_DOUBLE_EQ(childWorld->Position.Y, 2.0);
    EXPECT_DOUBLE_EQ(childWorld->Position.Z, 4.0) << "the child did not follow after its archetype grew";

    // Removing the child's Parent moves it to an unparented archetype: it is
    // a root from then on, its world matrix its own local pose.
    world.RemoveComponentImmediate<Parent>(child.GetHandle());
    world.GetComponentForWrite<Transform>(parent.GetHandle())->Translate(5.0f, 0.0f, 0.0f);
    hierarchy.Update(world, 0.0f);
    childWorld = world.GetComponent<WorldTransform>(child.GetHandle());
    EXPECT_DOUBLE_EQ(childWorld->Position.X, 1.0) << "the unparented child still followed its old parent";
    EXPECT_DOUBLE_EQ(childWorld->Position.Y, 0.0);
    EXPECT_DOUBLE_EQ(childWorld->Position.Z, 0.0);
}

// A Parent-holder spawned beside a cached hierarchy is resolved under its
// parent on its first frame. With GE_ECS_CHANGE_FILTER=0 (the ctest entry
// TransformMathTests_ChangeFilterOff) no Parent column gate reports it, so the
// structural change itself has to rebuild the topology.
TEST(TransformMathTests, Hierarchy_ParentHolderSpawnedBesideACachedHierarchyResolves)
{
    ECS::World world(nullptr);
    ECS::Entity parent = world.Create(Transform::FromTRS({0.0, 0.0, 3.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity firstChild = world.Create(Transform::FromTRS({1.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    firstChild.Set(Parent{parent.GetHandle()});
    world.ProcessCommands();
    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    hierarchy.Update(world, 0.0f);

    ECS::Entity secondChild = world.Create(Transform::FromTRS({0.0, 1.0, 0.0}, Quaternion{}, {1, 1, 1}));
    secondChild.Set(Parent{parent.GetHandle()});
    world.ProcessCommands();
    hierarchy.Update(world, 0.0f);
    const WorldTransform* childWorld = world.GetComponent<WorldTransform>(secondChild.GetHandle());
    ASSERT_NE(childWorld, nullptr);
    EXPECT_DOUBLE_EQ(childWorld->Position.X, 0.0);
    EXPECT_DOUBLE_EQ(childWorld->Position.Y, 1.0);
    EXPECT_DOUBLE_EQ(childWorld->Position.Z, 3.0) << "the new child was not resolved under its parent";
}

// The serial seam caches every entity and has no flat pass: a frame that only
// spawns still resolves the new entity.
TEST(TransformMathTests, Hierarchy_SerialSeamResolvesASpawnOnAFrameWithoutDestroys)
{
    ECS::World world(nullptr);
    ECS::Entity parent = world.Create(Transform::FromTRS({0.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity child = world.Create(Transform::FromTRS({1.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    child.Set(Parent{parent.GetHandle()});
    world.ProcessCommands();
    Engine::Renderer::TransformHierarchySystem serial(true);
    serial.Update(world, 0.0f);
    serial.Update(world, 0.0f);

    ECS::Entity spawned = world.Create(Transform::FromTRS({7.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    world.ProcessCommands();
    serial.Update(world, 0.0f);
    EXPECT_DOUBLE_EQ(world.GetComponent<WorldTransform>(spawned.GetHandle())->Position.X, 7.0)
        << "the serial seam left a spawned entity unresolved";
}

namespace
{
// One random forest, built identically into any number of worlds: roots with
// and without children, chains up to six deep, and Parent components whose
// handle is unset (roots that the hierarchy pass, not the flat pass, owns).
struct ForestSpec
{
    std::vector<int> parentOf; // -1: no Parent component, -2: a Parent with an unset handle
    std::vector<Transform> locals;
};

ForestSpec MakeForest(std::mt19937& rng, int count)
{
    std::uniform_real_distribution<float> offset(-5.0f, 5.0f);
    std::uniform_real_distribution<float> angle(-180.0f, 180.0f);
    std::uniform_real_distribution<float> scale(0.5f, 2.0f);
    std::uniform_int_distribution<int> kind(0, 9);
    ForestSpec spec;
    std::vector<int> depth;
    for (int i = 0; i < count; ++i)
    {
        int parent = -1;
        const int k = kind(rng);
        if (k == 0)
            parent = -2;
        else if (k >= 4 && i > 0)
        {
            const int candidate = std::uniform_int_distribution<int>(0, i - 1)(rng);
            if (depth[candidate] < 5)
                parent = candidate;
        }
        spec.parentOf.push_back(parent);
        depth.push_back(parent >= 0 ? depth[parent] + 1 : 0);
        spec.locals.push_back(Transform::FromTRS(
            {offset(rng), offset(rng), offset(rng)},
            QuaternionFromEulerXYZDegrees(angle(rng), angle(rng), angle(rng)),
            {scale(rng), scale(rng), scale(rng)}));
    }
    return spec;
}

std::vector<ECS::EntityHandle> BuildForest(ECS::World& world, const ForestSpec& spec)
{
    std::vector<ECS::EntityHandle> handles;
    for (const Transform& local : spec.locals)
        handles.push_back(world.Create(local).GetHandle());
    for (size_t i = 0; i < spec.parentOf.size(); ++i)
    {
        if (spec.parentOf[i] >= 0)
            world.AddComponentImmediate<Parent>(handles[i], Parent{handles[spec.parentOf[i]]});
        else if (spec.parentOf[i] == -2)
            world.AddComponentImmediate<Parent>(handles[i], Parent{});
    }
    world.ProcessCommands();
    return handles;
}

// The forest's world matrices by direct recursion over the ECS data.
void ReferenceWorld(const ECS::World& world, const std::vector<ECS::EntityHandle>& handles,
                    const ForestSpec& spec, std::vector<std::array<float32, 16>>& out)
{
    out.assign(handles.size(), {});
    std::vector<uint8_t> done(handles.size(), 0u);
    std::vector<size_t> stack;
    for (size_t start = 0; start < handles.size(); ++start)
    {
        for (size_t at = start; done[at] == 0u;)
        {
            const int parent = spec.parentOf[at];
            if (parent >= 0 && done[static_cast<size_t>(parent)] == 0u)
            {
                stack.push_back(at);
                at = static_cast<size_t>(parent);
                continue;
            }
            const Mathematics::Matrix4x4 localMatrix = PoseMatrix(*world.GetComponent<Transform>(handles[at]));
            const float32* local = localMatrix.Data();
            if (parent >= 0)
                Mathematics::MultiplyColumnMajor4x4(out[static_cast<size_t>(parent)].data(), local, out[at].data());
            else
                std::memcpy(out[at].data(), local, sizeof(float32) * 16);
            done[at] = 1u;
            if (stack.empty())
                break;
            at = stack.back();
            stack.pop_back();
        }
    }
}
} // namespace

// I4: the split paths (flat pass for unparented entities, parallel topmost-
// subtree propagation for the rest) produce world matrices bitwise equal to
// the all-serial cached path, and equal to a direct recursion, across random
// moves, a reparenting and frames where hundreds of roots move at once. The
// forest carries one 200-link chain (skeletons and ropes), so the deep walk
// and the topmost filter's climb run on a long spine.
//
// The two legs guard different things: the bitwise leg compares the split
// paths against the same code's serial seam, so it cannot see a math error
// they share; the NEAR leg against a direct recursion is the one that catches
// a wrong multiply order. Neither is redundant.
TEST(TransformMathTests, Hierarchy_SplitPathsMatchTheSerialPathOnRandomForests)
{
    constexpr int kRandomEntities = 3000;
    constexpr int kChainLength = 200;
    constexpr int kFrames = 8;
    std::mt19937 rng(3355u);
    ForestSpec spec = MakeForest(rng, kRandomEntities);
    for (int link = 0; link < kChainLength; ++link)
    {
        // The chain's first link has no Parent (a root the flat pass owns).
        spec.parentOf.push_back(link == 0 ? -1 : static_cast<int>(spec.parentOf.size()) - 1);
        spec.locals.push_back(Transform::FromTRS({0.0, 0.5, 0.1},
                                                 Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 0.05f),
                                                 {1.0f, 1.0f, 1.0f}));
    }
    const int entityCount = static_cast<int>(spec.parentOf.size());

    JobSystem::WorkStealingThreadPool pool(4);
    ECS::World splitWorld(&pool);
    ECS::World serialWorld(nullptr);
    const auto splitHandles = BuildForest(splitWorld, spec);
    const auto serialHandles = BuildForest(serialWorld, spec);
    Engine::Renderer::TransformHierarchySystem split;
    Engine::Renderer::TransformHierarchySystem serial(true);

    std::uniform_int_distribution<int> pick(0, entityCount - 1);
    std::uniform_real_distribution<float> step(-1.0f, 1.0f);
    std::vector<std::array<float32, 16>> reference;
    for (int frame = 0; frame < kFrames; ++frame)
    {
        // Frame 3 moves a third of everything (past the parallel threshold);
        // the others move a handful. Frame 5 reparents one entity.
        const int moves = frame == 3 ? entityCount / 3 : 20;
        for (int m = 0; m < moves; ++m)
        {
            const int i = pick(rng);
            const float dx = step(rng), dy = step(rng), dz = step(rng);
            splitWorld.GetComponentForWrite<Transform>(splitHandles[i])->Translate(dx, dy, dz);
            serialWorld.GetComponentForWrite<Transform>(serialHandles[i])->Translate(dx, dy, dz);
        }
        if (frame == 5)
        {
            for (int i = entityCount - 1; i > 0; --i)
            {
                if (spec.parentOf[i] >= 0)
                {
                    splitWorld.GetComponentForWrite<Parent>(splitHandles[i])->Entity = splitHandles[0];
                    serialWorld.GetComponentForWrite<Parent>(serialHandles[i])->Entity = serialHandles[0];
                    break;
                }
            }
        }
        split.Update(splitWorld, 0.0f);
        serial.Update(serialWorld, 0.0f);

        for (int i = 0; i < entityCount; ++i)
        {
            const auto* a = splitWorld.GetComponent<WorldTransform>(splitHandles[i]);
            const auto* b = serialWorld.GetComponent<WorldTransform>(serialHandles[i]);
            ASSERT_NE(a, nullptr);
            ASSERT_NE(b, nullptr);
            ASSERT_EQ(0, std::memcmp(a, b, kTransformPoseBytes))
                << "frame " << frame << " entity " << i << " differs from the serial path";
        }
    }

    ForestSpec settled = spec;
    for (int i = entityCount - 1; i > 0; --i)
        if (settled.parentOf[i] >= 0) { settled.parentOf[i] = 0; break; }
    ReferenceWorld(splitWorld, splitHandles, settled, reference);
    for (int i = 0; i < entityCount; ++i)
    {
        const Mathematics::Matrix4x4 actualMatrix = PoseMatrix(*splitWorld.GetComponent<WorldTransform>(splitHandles[i]));
        const float32* actual = actualMatrix.Data();
        for (int e = 0; e < 16; ++e)
            ASSERT_NEAR(actual[e], reference[i][e], 1e-3f * (1.0f + std::fabs(reference[i][e])))
                << "entity " << i << " element " << e;
    }
}

// P2 (S8c): with unparented entities spawned and destroyed beside the forest
// every frame (they share archetype chunks with the forest's flat-owned roots,
// so destroys swap-move those roots), the split paths still match the serial
// seam bitwise, frame by frame.
TEST(TransformMathTests, Hierarchy_SplitPathsMatchTheSerialPathWithSpawnsAndDestroys)
{
    constexpr int kForest = 1500;
    constexpr int kFrames = 12;
    std::mt19937 rng(8001u);
    const ForestSpec spec = MakeForest(rng, kForest);

    JobSystem::WorkStealingThreadPool pool(4);
    ECS::World splitWorld(&pool);
    ECS::World serialWorld(nullptr);
    const auto splitHandles = BuildForest(splitWorld, spec);
    const auto serialHandles = BuildForest(serialWorld, spec);
    Engine::Renderer::TransformHierarchySystem split;
    Engine::Renderer::TransformHierarchySystem serial(true);
    std::vector<ECS::EntityHandle> splitExtra, serialExtra;

    std::uniform_int_distribution<int> pick(0, kForest - 1);
    std::uniform_real_distribution<float> step(-1.0f, 1.0f);
    for (int frame = 0; frame < kFrames; ++frame)
    {
        const int spawns = std::uniform_int_distribution<int>(0, 40)(rng);
        for (int s = 0; s < spawns; ++s)
        {
            const Transform local = Transform::FromTRS({step(rng), step(rng), step(rng)}, Quaternion{}, {1, 1, 1});
            splitExtra.push_back(splitWorld.Create(local).GetHandle());
            serialExtra.push_back(serialWorld.Create(local).GetHandle());
        }
        const int destroys = std::min<int>(static_cast<int>(splitExtra.size()),
                                           std::uniform_int_distribution<int>(0, 20)(rng));
        for (int d = 0; d < destroys; ++d)
        {
            const size_t at = std::uniform_int_distribution<size_t>(0, splitExtra.size() - 1)(rng);
            splitWorld.DestroyEntityImmediate(splitExtra[at]);
            serialWorld.DestroyEntityImmediate(serialExtra[at]);
            splitExtra.erase(splitExtra.begin() + static_cast<std::ptrdiff_t>(at));
            serialExtra.erase(serialExtra.begin() + static_cast<std::ptrdiff_t>(at));
        }
        splitWorld.ProcessCommands();
        serialWorld.ProcessCommands();
        for (int m = 0; m < 20; ++m)
        {
            const int i = pick(rng);
            const float dx = step(rng), dy = step(rng), dz = step(rng);
            splitWorld.GetComponentForWrite<Transform>(splitHandles[i])->Translate(dx, dy, dz);
            serialWorld.GetComponentForWrite<Transform>(serialHandles[i])->Translate(dx, dy, dz);
        }
        split.Update(splitWorld, 0.0f);
        serial.Update(serialWorld, 0.0f);

        for (int i = 0; i < kForest; ++i)
            ASSERT_EQ(0, std::memcmp(splitWorld.GetComponent<WorldTransform>(splitHandles[i]),
                                     serialWorld.GetComponent<WorldTransform>(serialHandles[i]), kTransformPoseBytes))
                << "frame " << frame << " forest entity " << i << " differs from the serial path";
        for (size_t i = 0; i < splitExtra.size(); ++i)
            ASSERT_EQ(0, std::memcmp(splitWorld.GetComponent<WorldTransform>(splitExtra[i]),
                                     serialWorld.GetComponent<WorldTransform>(serialExtra[i]), kTransformPoseBytes))
                << "frame " << frame << " spawned entity " << i << " differs from the serial path";
    }
}

TEST(TransformMathTests, DISABLED_Hierarchy_CachedTopologyPerformanceProbe)
{
    constexpr size_t kRoots = 1000;
    constexpr size_t kEntities = 1'000'000;
    ECS::WorldConfig config{};
    config.ExpectedEntityCount = kEntities;
    ECS::World world(config, nullptr);
    std::vector<ECS::EntityHandle> roots;
    std::vector<ECS::EntityHandle> leaves;
    roots.reserve(kRoots);
    leaves.reserve(kEntities - kRoots);

    for (size_t index = 0; index < kRoots; ++index) {
        roots.push_back(world.Create(Transform::FromTRS(
            {static_cast<double>(index), 0.0, 0.0}, Quaternion{}, {1, 1, 1})).GetHandle());
    }
    for (size_t index = kRoots; index < kEntities; ++index) {
        ECS::Entity entity = world.Create(Transform::FromTRS(
            {0.0, static_cast<double>(index % 17), 0.0}, Quaternion{}, {1, 1, 1}));
        entity.Set(Parent{roots[index % kRoots]});
        leaves.push_back(entity.GetHandle());
    }
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    auto milliseconds = [](auto&& operation) {
        const auto start = std::chrono::steady_clock::now();
        operation();
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    };
    const double firstBuildMs = milliseconds([&] { hierarchy.Update(world, 0.0f); });

    auto median = [&](auto&& operation, int runs) {
        std::vector<double> samples;
        samples.reserve(static_cast<size_t>(runs));
        for (int run = 0; run < runs; ++run)
            samples.push_back(milliseconds(operation));
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    };

    const double cleanMs = median([&] { hierarchy.Update(world, 0.0f); }, 21);
    size_t leafStep = 0;
    const double leafMs = median([&] {
        world.GetComponentForWrite<Transform>(leaves[leafStep++ % leaves.size()])
            ->Translate(0.001f, 0.0f, 0.0f);
        hierarchy.Update(world, 0.0f);
        EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 1u);
    }, 21);
    size_t rootStep = 0;
    const double branchMs = median([&] {
        world.GetComponentForWrite<Transform>(roots[rootStep++ % roots.size()])
            ->Translate(0.001f, 0.0f, 0.0f);
        hierarchy.Update(world, 0.0f);
        EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 1000u);
    }, 11);
    bool toggle = false;
    const double rebuildMs = median([&] {
        world.GetComponentForWrite<Parent>(leaves.front())->Entity =
            roots[toggle ? 0 : 1];
        toggle = !toggle;
        hierarchy.Update(world, 0.0f);
    }, 7);

    // A structural frame that moves no node (an unparented spawn) pays the
    // O(nodes) cache check instead of a rebuild; one that moves a node (a leaf
    // given, then relieved of, a component) pays the check and the rebuild.
    const size_t buildsBeforeSpawns = hierarchy.GetTopologyBuildCount();
    const double structuralCheckMs = median([&] {
        world.Create(Transform::FromTRS({-1.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
        world.ProcessCommands();
        hierarchy.Update(world, 0.0f);
    }, 11);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), buildsBeforeSpawns) << "an unparented spawn rebuilt the topology";
    bool ordered = false;
    const double movedNodeMs = median([&] {
        if (ordered)
            world.RemoveComponentImmediate<HierarchyOrder>(leaves.back());
        else
            world.AddComponentImmediate<HierarchyOrder>(leaves.back(), HierarchyOrder{1});
        ordered = !ordered;
        hierarchy.Update(world, 0.0f);
    }, 7);

    std::printf(
        "\n[TransformHierarchyPerf] entities=%zu firstBuild=%.3fms clean=%.3fms "
        "leaf=%.3fms branch1000=%.3fms topologyRebuild=%.3fms structuralCheck=%.3fms movedNode=%.3fms\n",
        kEntities, firstBuildMs, cleanMs, leafMs, branchMs, rebuildMs, structuralCheckMs, movedNodeMs);
}

namespace
{
    // The entities a Changed<WorldTransform> scan visits since `gate`. The
    // scan binds WorldTransform as const, so it stamps nothing it measures.
    std::vector<ECS::EntityHandle> EntitiesStampedSince(ECS::World& world, uint64 gate)
    {
        ECS::ChangeGate changeGate;
        changeGate.LastRunVersion = gate;
        std::vector<ECS::EntityHandle> visited;
        auto q = world.Query<ECS::Read<WorldTransform>>();
        q.Changed<WorldTransform>(changeGate);
        q.Each([&visited](ECS::EntityHandle e, const WorldTransform&) { visited.push_back(e); });
        return visited;
    }

    bool Contains(const std::vector<ECS::EntityHandle>& handles, ECS::EntityHandle wanted)
    {
        return std::find(handles.begin(), handles.end(), wanted) != handles.end();
    }

    // Runs one update and returns whether it reported `entity`'s WorldTransform
    // as changed. The gate is sampled after the caller's edits, so only the
    // update's own stamps count.
    bool UpdateReports(ECS::World& world, Engine::Renderer::TransformHierarchySystem& hierarchy,
                       ECS::EntityHandle entity)
    {
        const uint64 gate = world.GetGlobalSystemVersion();
        hierarchy.Update(world, 0.0f);
        return Contains(EntitiesStampedSince(world, gate), entity);
    }
}

// Change-signaling contract for hierarchy-propagated moves. The hierarchy's
// incremental path writes WorldTransform through pointers cached at topology
// rebuild, so nothing in that write is a query visit — the per-(chunk, column)
// write-grant stamp that Changed<WorldTransform> consumers filter on has to be
// issued explicitly. Consumers that gate on it (the terrain modifier system's
// dirty scan) go blind to every hierarchy-driven move without it, which is the
// difference between moving an entity from the inspector (Transform only) and
// from the viewport gizmo (which writes WorldTransform itself).
//
// Both propagation branches are covered: a child recomposed against its parent
// and a root copied straight from its local matrix.
TEST(TransformMathTests, HierarchyPropagationStampsWorldTransformForChangedGate)
{
    ECS::World world;

    ECS::Entity root  = world.Create(Transform::FromTRS(
        {0.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity child = world.Create(Transform::FromTRS(
        {1.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    Parent toRoot{}; toRoot.Entity = root.GetHandle();
    child.Set(toRoot);
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f); // topology build + first propagation
    hierarchy.Update(world, 0.0f); // quiesce: nothing dirty, nothing walked
    ASSERT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    ASSERT_EQ(hierarchy.GetLastPropagatedNodeCount(), 0u);

    // Positive control: the probe must be capable of seeing a stamped write.
    // A write grant on an untouched entity is the known event injected into
    // the measurement channel — without it a zero below would be unreadable.
    {
        const uint64 gate = world.GetGlobalSystemVersion();
        ASSERT_NE(world.GetComponentForWrite<WorldTransform>(root.GetHandle()), nullptr);
        EXPECT_TRUE(Contains(EntitiesStampedSince(world, gate), root.GetHandle()))
            << "probe cannot see a write-granted WorldTransform; the gate below proves nothing";
    }

    // Child branch: a local Transform edit (the inspector's write shape — a
    // data-only component set, no archetype move, so no topology rebuild).
    {
        const uint64 gate = world.GetGlobalSystemVersion();
        const uint32 versionBefore =
            world.GetComponent<WorldTransform>(child.GetHandle())->Version;

        Transform moved = Transform::FromTRS({7.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1});
        world.AddComponentImmediate(child.GetHandle(), moved);
        hierarchy.Update(world, 0.0f);

        ASSERT_EQ(hierarchy.GetTopologyBuildCount(), 1u) << "test drifted: a rebuild stamps everything";
        ASSERT_EQ(hierarchy.GetLastPropagatedNodeCount(), 1u);
        ASSERT_GT(world.GetComponent<WorldTransform>(child.GetHandle())->Version, versionBefore)
            << "propagation did not run; the stamp assertion below would be vacuous";

        EXPECT_TRUE(Contains(EntitiesStampedSince(world, gate), child.GetHandle()))
            << "hierarchy recomposed the child's WorldTransform without stamping its column";
    }

    // Root branch: the same edit applied to the parent, which propagates
    // through the root path (local matrix copied straight into WorldTransform).
    {
        const uint64 gate = world.GetGlobalSystemVersion();
        const uint32 versionBefore =
            world.GetComponent<WorldTransform>(root.GetHandle())->Version;

        Transform moved = Transform::FromTRS({0.0, 5.0, 0.0}, Quaternion{}, {1, 1, 1});
        world.AddComponentImmediate(root.GetHandle(), moved);
        hierarchy.Update(world, 0.0f);

        ASSERT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
        ASSERT_GT(world.GetComponent<WorldTransform>(root.GetHandle())->Version, versionBefore);

        const std::vector<ECS::EntityHandle> moved2 = EntitiesStampedSince(world, gate);
        EXPECT_TRUE(Contains(moved2, root.GetHandle()))
            << "hierarchy rewrote the root's WorldTransform without stamping its column";
        EXPECT_TRUE(Contains(moved2, child.GetHandle()))
            << "a root move must signal the subtree it recomputed";
    }
}

TEST(TransformMathTests, Hierarchy_ChildrenFollowRootAfterDirectWorldWrite)
{
    for (bool forceSerial : {false, true})
    {
        for (bool publishWorld : {false, true})
        {
            SCOPED_TRACE(::testing::Message() << "forceSerial=" << forceSerial
                                            << " publishWorld=" << publishWorld);
            ECS::World world;
            auto root = world.Create(Transform{});
            Parent parent{};
            parent.Entity = root.GetHandle();
            auto child = world.Create(Transform{}, parent);
            Engine::Renderer::TransformHierarchySystem hierarchy(forceSerial);
            hierarchy.Update(world, 0.0f);
            ASSERT_DOUBLE_EQ(child.Get<WorldTransform>()->Position.Y, 0.0);

            root.GetForWrite<Transform>()->Position.Y = 8.0;
            if (publishWorld)
            {
                auto* composed = root.GetForWrite<WorldTransform>();
                composed->Position.Y = 8.0;
                BumpWorldTransform(world, root.GetHandle(), *composed);
            }
            hierarchy.Update(world, 0.0f);
            EXPECT_DOUBLE_EQ(root.Get<WorldTransform>()->Position.Y, 8.0);
            EXPECT_DOUBLE_EQ(child.Get<WorldTransform>()->Position.Y, 8.0);
        }
    }
}

TEST(TransformMathTests, Hierarchy_GrandchildFollowsRootAfterDirectWorldWrite)
{
    ECS::World world;
    auto root = world.Create(Transform{});
    Transform childLocal{};
    childLocal.Position.Y = 1.0;
    Parent childParent{};
    childParent.Entity = root.GetHandle();
    auto child = world.Create(childLocal, childParent);
    Transform grandchildLocal{};
    grandchildLocal.Position.Y = 2.0;
    Parent grandchildParent{};
    grandchildParent.Entity = child.GetHandle();
    auto grandchild = world.Create(grandchildLocal, grandchildParent);
    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    ASSERT_DOUBLE_EQ(grandchild.Get<WorldTransform>()->Position.Y, 3.0);

    root.GetForWrite<Transform>()->Position.Y = 8.0;
    auto* composed = root.GetForWrite<WorldTransform>();
    composed->Position.Y = 8.0;
    BumpWorldTransform(world, root.GetHandle(), *composed);
    hierarchy.Update(world, 0.0f);
    EXPECT_DOUBLE_EQ(child.Get<WorldTransform>()->Position.Y, 9.0);
    EXPECT_DOUBLE_EQ(grandchild.Get<WorldTransform>()->Position.Y, 11.0);
}

// A spawned Parent-holder rebuilds the topology, and the rebuild resets the flat pass's gate, so both passes
// scan every chunk. Only a changed pose may read as changed to a Changed<WorldTransform> consumer. The gate is
// sampled after the edits, so only the hierarchy's own stamps count.
TEST(TransformMathTests, Hierarchy_SpawnFrameReportsOnlyChangedWorldTransforms)
{
    ECS::World world;
    ECS::Entity still = world.Create(Transform::FromTRS({3.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    ECS::Entity root = world.Create(Transform{});
    Parent toRoot{};
    toRoot.Entity = root.GetHandle();
    ECS::Entity child = world.Create(Transform::FromTRS({1.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}), toRoot);
    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    hierarchy.Update(world, 0.0f);
    ASSERT_EQ(hierarchy.GetTopologyBuildCount(), 1u);

    // HierarchyOrder puts the new entity in a chunk of its own: stamps are per chunk.
    ECS::Entity spawned =
        world.Create(Transform::FromTRS({0.0, 2.0, 0.0}, Quaternion{}, {1, 1, 1}), toRoot, HierarchyOrder{});
    const uint64 gate = world.GetGlobalSystemVersion();
    hierarchy.Update(world, 0.0f);
    ASSERT_EQ(hierarchy.GetTopologyBuildCount(), 2u) << "the spawn did not rebuild, so no gate was reset";

    const std::vector<ECS::EntityHandle> stamped = EntitiesStampedSince(world, gate);
    EXPECT_TRUE(Contains(stamped, spawned.GetHandle())) << "a new entity's first resolved pose was not reported";
    EXPECT_FALSE(Contains(stamped, still.GetHandle())) << "the flat pass reported an unmoved root";
    EXPECT_FALSE(Contains(stamped, root.GetHandle())) << "the flat pass reported an unmoved hierarchy root";
    EXPECT_FALSE(Contains(stamped, child.GetHandle())) << "the rebuild reported an unmoved Parent-holder";
}

// The flat pass reports each real change of a root's pose: the first resolve, a move, a basis-only change,
// and a return to a pose it held before. A move and back within one frame reports nothing.
TEST(TransformMathTests, FlatPass_ReportsEachChangedRootPose)
{
    JobSystem::WorkStealingThreadPool pool(4);
    ECS::World world(&pool);
    const Transform first = Transform::FromTRS({1.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1});
    ECS::Entity mover = world.Create(first);
    const ECS::EntityHandle handle = mover.GetHandle();
    Engine::Renderer::TransformHierarchySystem hierarchy;

    EXPECT_TRUE(UpdateReports(world, hierarchy, handle)) << "first resolve";
    EXPECT_FALSE(UpdateReports(world, hierarchy, handle)) << "idle frame";
    *mover.GetForWrite<Transform>() = Transform::FromTRS({5.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1});
    EXPECT_TRUE(UpdateReports(world, hierarchy, handle)) << "move";
    *mover.GetForWrite<Transform>() = Transform::FromTRS({5.0, 0.0, 0.0}, Quaternion{}, {2, 2, 2});
    EXPECT_TRUE(UpdateReports(world, hierarchy, handle)) << "basis-only change";
    *mover.GetForWrite<Transform>() = first;
    EXPECT_TRUE(UpdateReports(world, hierarchy, handle)) << "return to the first pose";
    EXPECT_DOUBLE_EQ(mover.Get<WorldTransform>()->Position.X, 1.0);
    mover.GetForWrite<Transform>()->Position.X = 9.0;
    *mover.GetForWrite<Transform>() = first;
    EXPECT_FALSE(UpdateReports(world, hierarchy, handle)) << "moved and back within one frame";
}

// A reparent rebuilds the topology; the child's new world pose is reported.
TEST(TransformMathTests, Hierarchy_ReparentReportsTheMovedChild)
{
    ECS::World world;
    ECS::Entity left = world.Create(Transform{});
    ECS::Entity right = world.Create(Transform::FromTRS({10.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    Parent toLeft{};
    toLeft.Entity = left.GetHandle();
    ECS::Entity child = world.Create(Transform::FromTRS({1.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}), toLeft);
    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    hierarchy.Update(world, 0.0f);

    child.GetForWrite<Parent>()->Entity = right.GetHandle();
    EXPECT_TRUE(UpdateReports(world, hierarchy, child.GetHandle()))
        << "the reparented child's new world pose was not reported";
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 2u);
    EXPECT_DOUBLE_EQ(child.Get<WorldTransform>()->Position.X, 11.0);
}

// A world switch resets every gate of the system. The first frame on each
// world reports what changed there and nothing else.
TEST(TransformMathTests, FlatPass_WorldSwitchReportsOnlyChangedPoses)
{
    ECS::World first;
    ECS::World second;
    ECS::Entity mover = first.Create(Transform{});
    ECS::Entity spawned = second.Create(Transform::FromTRS({4.0, 0.0, 0.0}, Quaternion{}, {1, 1, 1}));
    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(first, 0.0f);
    hierarchy.Update(first, 0.0f);

    EXPECT_TRUE(UpdateReports(second, hierarchy, spawned.GetHandle()))
        << "the first resolve on the new world was not reported";
    EXPECT_FALSE(UpdateReports(first, hierarchy, mover.GetHandle())) << "the switch back reported an unmoved pose";

    hierarchy.Update(second, 0.0f);
    mover.GetForWrite<Transform>()->Position.X = 6.0;
    EXPECT_TRUE(UpdateReports(first, hierarchy, mover.GetHandle()))
        << "a pose edited while the other world was active was not reported";
}

// Roots in two chunks move in one pass. The stamp after the pass grants each
// chunk that changed, not only one of them.
TEST(TransformMathTests, FlatPass_ReportsChangedRootsInEachChunk)
{
    JobSystem::WorkStealingThreadPool pool(4);
    ECS::World world(&pool);
    ECS::Entity plain = world.Create(Transform{});
    ECS::Entity ordered = world.Create(Transform{}, HierarchyOrder{});
    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    plain.GetForWrite<Transform>()->Position.X = 1.0;
    ordered.GetForWrite<Transform>()->Position.X = 2.0;
    const uint64 gate = world.GetGlobalSystemVersion();
    hierarchy.Update(world, 0.0f);
    const std::vector<ECS::EntityHandle> stamped = EntitiesStampedSince(world, gate);
    EXPECT_TRUE(Contains(stamped, plain.GetHandle())) << "the move in the first chunk was not reported";
    EXPECT_TRUE(Contains(stamped, ordered.GetHandle())) << "the move in the second chunk was not reported";
}
