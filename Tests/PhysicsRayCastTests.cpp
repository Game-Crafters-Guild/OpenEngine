#include <gtest/gtest.h>
#include "Physics/PhysicsWorld.h"

#include <memory>
#include <stdexcept>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Physics;

namespace
{
// Every test casts one ray from the origin along +Z; bodies sit on that line.
class PhysicsRayCastTests : public ::testing::Test
{
protected:
    std::unique_ptr<PhysicsWorld> world;
    std::vector<RayCastResult> hits;

    void SetUp() override
    {
        PhysicsWorldSettings settings;
        settings.numThreads = 1;
        settings.maxBodies = settings.maxBodyPairs = settings.maxContactConstraints = 1024;
        settings.tempAllocatorBytes = 16u * 1024u * 1024u;
        world = std::make_unique<PhysicsWorld>(settings);
    }

    BodyHandle Body(const ShapeDefinition& definition, float z, uint64 data, CollisionLayer layer = Layers::Static,
                    bool sensor = false)
    {
        BodySettings settings;
        settings.shape = world->CreateShape(definition);
        if (!settings.shape.IsValid()) throw std::runtime_error("actual shape creation failed");
        settings.position = {0, 0, z};
        settings.motionType = MotionType::Static;
        settings.layer = layer;
        settings.isSensor = sensor;
        settings.userData = data;
        auto body = world->CreateBody(settings);
        if (!body.IsValid()) throw std::runtime_error("actual body creation failed");
        return body;
    }

    // A unit box centered at z, so the ray enters it at z - 0.5.
    BodyHandle Box(float z, uint64 data, CollisionLayer layer = Layers::Static, bool sensor = false)
    {
        BoxShapeDef box;
        box.halfExtents = {.5f, .5f, .5f};
        return Body(box, z, data, layer, sensor);
    }

    static RayCastQuery Forward()
    {
        RayCastQuery query;
        query.ray.origin = {0, 0, 0};
        query.ray.direction = {0, 0, 1};
        query.maxDistance = 100;
        return query;
    }
};

TEST_F(PhysicsRayCastTests, LayerMaskPassesThroughExcludedBodies)
{
    Box(3, 7, Layers::UserStart);
    const auto wall = Box(6, 11);

    RayCastQuery query = Forward();
    query.filter.layerMask = 1u << Layers::Static;
    RayCastResult hit;
    ASSERT_TRUE(world->RayCast(query, hit));
    EXPECT_EQ(hit.body, wall);
    EXPECT_EQ(hit.userData, 11u);
    EXPECT_NEAR(hit.distance, 5.5f, 1e-3f);

    ASSERT_TRUE(world->RayCastAll(query, hits));
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].body, wall);

    query.filter.layerMask = 0;
    EXPECT_FALSE(world->RayCast(query, hit));
    EXPECT_FALSE(hit.HasHit());
    EXPECT_FALSE(world->RayCastAll(query, hits));
    EXPECT_TRUE(hits.empty());
}

TEST_F(PhysicsRayCastTests, SensorsAreSkippedOnlyWhenAsked)
{
    const auto trigger = Box(3, 7, Layers::Static, true);
    const auto wall = Box(6, 11);

    RayCastQuery query = Forward();
    RayCastResult hit;
    ASSERT_TRUE(world->RayCast(query, hit));
    EXPECT_EQ(hit.body, trigger);

    query.filter.ignoreSensors = true;
    ASSERT_TRUE(world->RayCast(query, hit));
    EXPECT_EQ(hit.body, wall);
    EXPECT_NEAR(hit.distance, 5.5f, 1e-3f);

    ASSERT_TRUE(world->RayCastAll(query, hits));
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].body, wall);
}

TEST_F(PhysicsRayCastTests, RayCastAllReturnsEveryBodyNearestFirst)
{
    // Created far to near so the result order cannot come from creation order.
    const auto far = Box(9, 3);
    const auto middle = Box(6, 2);
    const auto near = Box(3, 1);

    ASSERT_TRUE(world->RayCastAll(Forward(), hits));
    ASSERT_EQ(hits.size(), 3u);
    const BodyHandle expected[] = {near, middle, far};
    for (size_t i = 0; i < 3; ++i)
    {
        EXPECT_EQ(hits[i].body, expected[i]) << i;
        EXPECT_EQ(hits[i].userData, i + 1) << i;
        EXPECT_NEAR(hits[i].distance, 2.5f + 3.0f * float(i), 1e-3f) << i;
        EXPECT_NEAR(hits[i].hitPoint.z, hits[i].distance, 1e-3f) << i;
        EXPECT_NEAR(hits[i].hitNormal.z, -1.0f, 1e-3f) << i;
    }
}

// Layer 0 bodies sit in the Static broad-phase tree and the others in the Dynamic tree; each tree
// reports its own bodies, so the order across them comes from the sort.
TEST_F(PhysicsRayCastTests, RayCastAllSortsAcrossBroadPhaseTrees)
{
    const auto wall = Box(6, 2);
    const auto trigger = Box(3, 1, Layers::Trigger, true);

    ASSERT_TRUE(world->RayCastAll(Forward(), hits));
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].body, trigger);
    EXPECT_EQ(hits[1].body, wall);
}

// Bodies hit at the same distance come back in one fixed order, by body, whatever the broad
// phase's tree layout: a replay or a lockstep game sees the same list every time.
TEST_F(PhysicsRayCastTests, RayCastAllOrdersEqualDistancesByBody)
{
    const BodyHandle bodies[] = {Box(3, 1), Box(3, 2), Box(3, 3)};

    ASSERT_TRUE(world->RayCastAll(Forward(), hits));
    ASSERT_EQ(hits.size(), 3u);
    for (size_t i = 0; i < 3; ++i)
        EXPECT_EQ(hits[i].body, bodies[i]) << i;
}

TEST_F(PhysicsRayCastTests, RayCastAllKeepsTheNearestMaxResults)
{
    Box(9, 3);
    const auto middle = Box(6, 2);
    const auto near = Box(3, 1);

    ASSERT_TRUE(world->RayCastAll(Forward(), hits, 2));
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].body, near);
    EXPECT_EQ(hits[1].body, middle);

    hits.resize(5);
    EXPECT_FALSE(world->RayCastAll(Forward(), hits, 0));
    EXPECT_TRUE(hits.empty());
}

// A world with no backend (moved from) finds nothing and leaves the caller's lists empty, as a query that
// hits nothing does: a list the caller reuses never keeps the results of an earlier query.
TEST_F(PhysicsRayCastTests, WorldWithoutABackendEmptiesTheLists)
{
    Box(3, 1);
    ASSERT_TRUE(world->RayCastAll(Forward(), hits));
    std::vector<OverlapResult> overlaps;
    BoxOverlapQuery box;
    box.center = {0, 0, 3};
    ASSERT_TRUE(world->BoxOverlap(box, overlaps));

    const PhysicsWorld moved = std::move(*world);
    EXPECT_FALSE(world->RayCastAll(Forward(), hits));
    EXPECT_TRUE(hits.empty());
    EXPECT_FALSE(world->BoxOverlap(box, overlaps));
    EXPECT_TRUE(overlaps.empty());
}

TEST_F(PhysicsRayCastTests, RayCastAllReportsAMeshBodyOnceAtItsNearestSurface)
{
    // Two facing quads, both windings so either side is a front face.
    MeshShapeDef mesh;
    for (float z : {0.0f, 1.0f})
        for (Vector3 v : {Vector3{-1, -1, z}, Vector3{1, -1, z}, Vector3{1, 1, z}, Vector3{-1, 1, z}})
            mesh.vertices.push_back(v);
    for (uint32 base : {0u, 4u})
        for (uint32 i : {0u, 1u, 2u, 0u, 2u, 3u, 0u, 2u, 1u, 0u, 3u, 2u})
            mesh.indices.push_back(base + i);
    const auto body = Body(mesh, 4, 5);
    const auto wall = Box(9, 6);

    ASSERT_TRUE(world->RayCastAll(Forward(), hits));
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].body, body);
    EXPECT_NEAR(hits[0].distance, 4.0f, 1e-3f);
    EXPECT_EQ(hits[1].body, wall);
}
} // namespace

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
