#include <gtest/gtest.h>

#include "DebugServer/Physics/PhysicsRaycastRequest.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <string>

namespace
{
using GameEngine::Editor::ReadRayCastQuery;
using GameEngine::Physics::RayCastQuery;
using json = nlohmann::json;

// The defect these lock: physics_raycast accepted any JSON number for origin, direction and
// maxDistance. A number outside float range parses as a number and narrows to inf, and the
// backend casts against `origin + direction * maxDistance` — one non-finite input makes the
// whole segment NaN math with no downstream guard. Same disease TerrainStrokeTargetTests
// locks for the sculpt path; the rejection belongs where the parameter is read.

TEST(PhysicsRaycastRequestTests, NonFiniteOriginIsRejected)
{
    // The origin is read as a double, and text out of double range does not parse: the infinity is built.
    json params = json::object();
    params["origin"] = {std::numeric_limits<double>::infinity(), 0.0, 0.0};
    params["direction"] = {0.0, 1.0, 0.0};
    ASSERT_TRUE(params["origin"][0].is_number())
        << "an infinity must still be a JSON number, or this test is not reaching the guard at all";

    RayCastQuery query{};
    std::string err;
    EXPECT_FALSE(ReadRayCastQuery(params, query, err));
    EXPECT_NE(err.find("origin"), std::string::npos)
        << "the error must name the key the caller supplied, got: " << err;
    EXPECT_NE(err.find("not finite"), std::string::npos) << err;
}

TEST(PhysicsRaycastRequestTests, NonFiniteDirectionIsRejected)
{
    const json params = json::parse(R"({"origin":[0,0,0],"direction":[0,-1e39,0]})");
    RayCastQuery query{};
    std::string err;

    EXPECT_FALSE(ReadRayCastQuery(params, query, err));
    EXPECT_NE(err.find("direction"), std::string::npos)
        << "the error must name the key the caller supplied, got: " << err;
    EXPECT_NE(err.find("not finite"), std::string::npos) << err;
}

// isfinite, not isinf: a NaN component is equally unusable, so an implementation that only
// rejected infinities would still let one through.
TEST(PhysicsRaycastRequestTests, NaNOriginIsRejected)
{
    json params = json::object();
    params["origin"] = {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
    params["direction"] = {0.0, 1.0, 0.0};
    ASSERT_TRUE(params["origin"][0].is_number());
    ASSERT_TRUE(std::isnan(params["origin"][0].get<float>()));

    RayCastQuery query{};
    std::string err;
    EXPECT_FALSE(ReadRayCastQuery(params, query, err));
    EXPECT_NE(err.find("not finite"), std::string::npos) << err;
}

// The discriminating control: the guard is a finiteness predicate, not a magnitude cap. An
// over-strict implementation could pass every rejection test above by capping magnitude and
// only this test would catch it. The origin is a double: 1e300 passes, and 300 km keeps its millimeters.
TEST(PhysicsRaycastRequestTests, LargeButFiniteOriginIsAccepted)
{
    const json params = json::parse(R"({"origin":[1e300,-1e300,300000.004],"direction":[0,1,0]})");

    RayCastQuery query{};
    std::string err;
    ASSERT_TRUE(ReadRayCastQuery(params, query, err)) << err;
    EXPECT_TRUE(err.empty());
    EXPECT_DOUBLE_EQ(query.origin.X, 1e300);
    EXPECT_DOUBLE_EQ(query.origin.Y, -1e300);
    EXPECT_DOUBLE_EQ(query.origin.Z, 300000.004);
}

TEST(PhysicsRaycastRequestTests, MissingOriginAsksForIt)
{
    const json params = json::parse(R"({"direction":[0,1,0]})");
    RayCastQuery query{};
    std::string err;

    EXPECT_FALSE(ReadRayCastQuery(params, query, err));
    EXPECT_NE(err.find("origin"), std::string::npos) << err;
    EXPECT_NE(err.find("[x,y,z]"), std::string::npos)
        << "an absent required key must be told what to pass, got: " << err;
}

TEST(PhysicsRaycastRequestTests, MissingDirectionAsksForIt)
{
    const json params = json::parse(R"({"origin":[0,0,0]})");
    RayCastQuery query{};
    std::string err;

    EXPECT_FALSE(ReadRayCastQuery(params, query, err));
    EXPECT_NE(err.find("direction"), std::string::npos) << err;
    EXPECT_NE(err.find("[x,y,z]"), std::string::npos) << err;
}

TEST(PhysicsRaycastRequestTests, MalformedOriginIsReported)
{
    const json params = json::parse(R"({"origin":[1,2],"direction":[0,1,0]})");
    RayCastQuery query{};
    std::string err;

    EXPECT_FALSE(ReadRayCastQuery(params, query, err));
    EXPECT_NE(err.find("origin"), std::string::npos) << err;
    EXPECT_NE(err.find("[x,y,z]"), std::string::npos) << err;
}

TEST(PhysicsRaycastRequestTests, MaxDistanceDefaultsWhenAbsent)
{
    const json params = json::parse(R"({"origin":[0,0,0],"direction":[0,1,0]})");
    RayCastQuery query{};
    std::string err;

    ASSERT_TRUE(ReadRayCastQuery(params, query, err)) << err;
    EXPECT_FLOAT_EQ(query.maxDistance, 1000.0f);
}

// 1e39 is a finite double but narrows to inf in the query's float32 field — the same
// narrowing hole as the vector components, reached through a scalar.
TEST(PhysicsRaycastRequestTests, NonFiniteMaxDistanceIsRejected)
{
    const json params = json::parse(R"({"origin":[0,0,0],"direction":[0,1,0],"maxDistance":1e39})");
    ASSERT_TRUE(params["maxDistance"].is_number());
    ASSERT_FALSE(std::isfinite(params["maxDistance"].get<float>()));

    RayCastQuery query{};
    std::string err;
    EXPECT_FALSE(ReadRayCastQuery(params, query, err));
    EXPECT_NE(err.find("maxDistance"), std::string::npos) << err;
}

TEST(PhysicsRaycastRequestTests, NegativeMaxDistanceIsRejected)
{
    const json params = json::parse(R"({"origin":[0,0,0],"direction":[0,1,0],"maxDistance":-5})");
    RayCastQuery query{};
    std::string err;

    EXPECT_FALSE(ReadRayCastQuery(params, query, err));
    EXPECT_NE(err.find("maxDistance"), std::string::npos) << err;
    EXPECT_NE(err.find("positive"), std::string::npos)
        << "the error must state the fix, got: " << err;
}

// Zero rides the same rejection as negative: an empty segment can never hit, and answering
// it `hit: false` would be indistinguishable from a genuine miss — the exact ambiguity this
// collision oracle exists to resolve.
TEST(PhysicsRaycastRequestTests, ZeroMaxDistanceIsRejected)
{
    const json params = json::parse(R"({"origin":[0,0,0],"direction":[0,1,0],"maxDistance":0})");
    RayCastQuery query{};
    std::string err;

    EXPECT_FALSE(ReadRayCastQuery(params, query, err));
    EXPECT_NE(err.find("maxDistance"), std::string::npos) << err;
}

TEST(PhysicsRaycastRequestTests, NonNumberMaxDistanceIsRejected)
{
    const json params = json::parse(R"({"origin":[0,0,0],"direction":[0,1,0],"maxDistance":"far"})");
    RayCastQuery query{};
    std::string err;

    EXPECT_FALSE(ReadRayCastQuery(params, query, err));
    EXPECT_NE(err.find("maxDistance"), std::string::npos) << err;
}

// The scalar half of the magnitude-cap control: 1e30 is far beyond any sane search cap but
// finite, so it must be accepted as given — not clamped, not defaulted.
TEST(PhysicsRaycastRequestTests, LargeButFiniteMaxDistanceIsAccepted)
{
    const json params = json::parse(R"({"origin":[0,0,0],"direction":[0,1,0],"maxDistance":1e30})");
    RayCastQuery query{};
    std::string err;

    ASSERT_TRUE(ReadRayCastQuery(params, query, err)) << err;
    EXPECT_FLOAT_EQ(query.maxDistance, 1e30f);
}

TEST(PhysicsRaycastRequestTests, ValidRequestPassesThroughExactly)
{
    const json params = json::parse(
        R"({"origin":[1,2,3],"direction":[0,-1,0.5],"maxDistance":250,"layerMask":5})");
    RayCastQuery query{};
    std::string err;

    ASSERT_TRUE(ReadRayCastQuery(params, query, err)) << err;
    EXPECT_TRUE(err.empty());
    EXPECT_DOUBLE_EQ(query.origin.X, 1.0);
    EXPECT_DOUBLE_EQ(query.origin.Y, 2.0);
    EXPECT_DOUBLE_EQ(query.origin.Z, 3.0);
    EXPECT_FLOAT_EQ(query.direction.x, 0.0f);
    EXPECT_FLOAT_EQ(query.direction.y, -1.0f);
    EXPECT_FLOAT_EQ(query.direction.z, 0.5f);
    EXPECT_FLOAT_EQ(query.maxDistance, 250.0f);
    EXPECT_EQ(query.filter.layerMask, 5u);
}

TEST(PhysicsRaycastRequestTests, LayerMaskDefaultsToAllLayers)
{
    const json params = json::parse(R"({"origin":[0,0,0],"direction":[0,1,0]})");
    RayCastQuery query{};
    std::string err;

    ASSERT_TRUE(ReadRayCastQuery(params, query, err)) << err;
    EXPECT_EQ(query.filter.layerMask, 0xFFFFFFFFu);
}

} // namespace
