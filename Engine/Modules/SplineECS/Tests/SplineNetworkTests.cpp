#include "SplineECS/SplineNetwork.h"

#include "SplineECS/SplineService.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Spline/SplineNetworkOverrides.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <span>
#include <vector>

using namespace GameEngine;
using Components::SplineNetworkMemberKind;
using Components::SplineNetworkOverride;
using Components::SplineNetworkOverrides;
using Components::SplineNodeRule;
using Mathematics::Vector3;
using SplineECS::SplineNetwork;
using SplineECS::SplineNode;
using SplineECS::SplineNodeKind;

namespace
{

constexpr float32 kHalfWidth = 3.0f;
constexpr float32 kArcTolerance = 0.05f;

class SplineNetworkTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
    }

    void TearDown() override
    {
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
    }

    // A Linear spline through `points` (world meters, identity transform) with a half-width.
    ECS::EntityHandle AddSpline(ECS::World& world, const std::vector<Vector3>& points, bool closed, float32 halfWidth)
    {
        SplineECS::SplineService& splines = SplineECS::SplineService::Get();
        const SplineECS::SplineHandle handle = splines.CreateSpline(Spline::SplineType::Linear, closed);
        Spline::SplineData* data = splines.GetSplineData(handle);
        for (const Vector3& point : points)
            data->AddPoint(point, halfWidth);
        Spline::RebuildSplineCache(*data);
        const ECS::EntityHandle entity = world.CreateEntity();
        Components::SplineComponent spline{};
        spline.SplineDataIndex = handle.Index();
        spline.SplineDataGeneration = handle.Generation();
        world.AddComponentImmediate(entity, spline);
        world.AddComponentImmediate(entity, Components::WorldTransform{});
        return entity;
    }

    ECS::EntityHandle AddRoad(const std::vector<Vector3>& points, bool closed = false, ECS::World* world = nullptr)
    {
        ECS::World& target = world ? *world : World;
        const ECS::EntityHandle road = AddSpline(target, points, closed, kHalfWidth);
        target.AddComponentImmediate(road, Components::SplineNetworkMember{SplineNetworkMemberKind::Road});
        return road;
    }

    ECS::EntityHandle AddArea(const std::vector<Vector3>& points, bool closed = true)
    {
        const ECS::EntityHandle area = AddSpline(World, points, closed, 0.0f);
        World.AddComponentImmediate(area, Components::SplineNetworkMember{SplineNetworkMemberKind::Area});
        return area;
    }

    Spline::SplineData& DataOf(ECS::EntityHandle entity, ECS::World* world = nullptr)
    {
        const auto* spline = (world ? *world : World).GetComponent<Components::SplineComponent>(entity);
        return *SplineECS::SplineService::Get().GetSplineData(
            SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
    }

    void SetPoints(ECS::EntityHandle entity, const std::vector<Vector3>& points)
    {
        Spline::SplineData& data = DataOf(entity);
        data.Points.clear();
        for (const Vector3& point : points)
            data.AddPoint(point, kHalfWidth);
        Spline::RebuildSplineCache(data);
    }

    // A record written from outside the service, as a load, a script or an undo writes it.
    void WriteRecord(ECS::EntityHandle holder, uint32 slot, ECS::EntityHandle other, SplineNodeRule rule,
                     float32 arc, float32 otherArc, ECS::World* world = nullptr)
    {
        ECS::World& target = world ? *world : World;
        SplineNetworkOverrides overrides{};
        if (const auto* existing = target.GetComponent<SplineNetworkOverrides>(holder))
            overrides = *existing;
        overrides.Records[slot] = {other, arc, otherArc, rule, {}};
        target.AddComponentImmediate(holder, overrides);
    }

    const SplineNetworkOverride& RecordOf(ECS::EntityHandle holder, uint32 slot, ECS::World* world = nullptr)
    {
        return (world ? *world : World).GetComponent<SplineNetworkOverrides>(holder)->Records[slot];
    }

    // Updates the world, World by default; the queries below read the world updated last.
    void Derive(ECS::World* world = nullptr)
    {
        m_Derived = world ? world : &World;
        Network.Update(*m_Derived);
    }

    std::span<const SplineNode> Nodes() const { return Network.GetNodes(*m_Derived); }
    std::span<const SplineECS::SplineHeldRecord> Held() const { return Network.GetHeldRecords(*m_Derived); }

    // The node within 1 m of (x, z), or null.
    const SplineNode* NodeAt(float32 x, float32 z) const
    {
        for (const SplineNode& node : Nodes())
            if (std::hypot(node.Position.x - x, node.Position.z - z) <= 1.0f)
                return &node;
        return nullptr;
    }

    // A copy of every network entity of World in a fresh world, created in reverse order so every
    // handle changes, with each record's Other remapped: what a scene reload does.
    void CopyReversedInto(ECS::World& target, const std::vector<ECS::EntityHandle>& entities,
                          std::vector<ECS::EntityHandle>& copies)
    {
        copies.assign(entities.size(), {});
        for (size_t i = entities.size(); i-- > 0;)
        {
            const Spline::SplineData& data = DataOf(entities[i]);
            std::vector<Vector3> points;
            for (const auto& point : data.Points)
                points.push_back(point.Position);
            const auto kind = World.GetComponent<Components::SplineNetworkMember>(entities[i]);
            copies[i] = AddSpline(target, points, data.Closed, kHalfWidth);
            if (kind)
                target.AddComponentImmediate(copies[i], *kind);
        }
        for (size_t i = 0; i < entities.size(); ++i)
        {
            const auto* overrides = World.GetComponent<SplineNetworkOverrides>(entities[i]);
            if (!overrides)
                continue;
            SplineNetworkOverrides copy = *overrides;
            for (SplineNetworkOverride& record : copy.Records)
                for (size_t j = 0; j < entities.size(); ++j)
                    if (record.Rule != SplineNodeRule::None && record.Other == entities[j])
                    {
                        record.Other = copies[j];
                        break;
                    }
            target.AddComponentImmediate(copies[i], copy);
        }
    }

    void ExpectReloadKeepsTheRule(ECS::EntityHandle a, ECS::EntityHandle b, size_t holder)
    {
        Derive();
        ECS::World reloaded;
        std::vector<ECS::EntityHandle> copies;
        CopyReversedInto(reloaded, {a, b}, copies);
        ASSERT_LT(copies[1].id, copies[0].id);
        Derive(&reloaded);
        ASSERT_EQ(Nodes().size(), 1u);
        EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
        EXPECT_EQ(NodeAt(10.0f, 0.0f)->RuleHolder, copies[holder]);
    }

    ECS::World World;
    SplineNetwork Network;

  private:
    bool m_OwnsSplines = false;
    ECS::World* m_Derived = &World;
};

// A road along +X through the origin and one along +Z: they cross at the origin.
TEST_F(SplineNetworkTest, TwoRoadsCrossingFormOneCrossingWithFourArms)
{
    AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    AddRoad({{0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, 50.0f}});
    Derive();
    ASSERT_EQ(Nodes().size(), 1u);
    const SplineNode& node = Nodes()[0];
    EXPECT_EQ(node.Kind, SplineNodeKind::Crossing);
    EXPECT_EQ(node.ArmCount, 4u);
    EXPECT_NEAR(node.ArcLengthMetres[0], 50.0f, kArcTolerance);
    EXPECT_NEAR(node.ArcLengthMetres[1], 50.0f, kArcTolerance);
    EXPECT_FALSE(node.IsSplitLevel());
}

TEST_F(SplineNetworkTest, AnEndOnARibbonIsAnAttachmentAndAnEndShortOfItIsNoNode)
{
    const ECS::EntityHandle body = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    // Inside the 3 m band: a T.
    const ECS::EntityHandle onRibbon = AddRoad({{10.0f, 0.0f, 40.0f}, {10.0f, 0.0f, kHalfWidth}});
    // 1.5 m beyond the band's edge: nothing joins it, whatever the editor's snap would do.
    AddRoad({{-20.0f, 0.0f, 40.0f}, {-20.0f, 0.0f, kHalfWidth + 1.5f}});
    Derive();
    ASSERT_EQ(Nodes().size(), 1u);
    const SplineNode& node = Nodes()[0];
    EXPECT_EQ(node.Kind, SplineNodeKind::Attachment);
    EXPECT_EQ(node.Members[0], onRibbon);
    EXPECT_EQ(node.Members[1], body);
    EXPECT_EQ(node.ArmCount, 3u);
    EXPECT_NEAR(node.Position.x, 10.0f, kArcTolerance);
    EXPECT_NEAR(node.Position.z, 0.0f, kArcTolerance);
}

TEST_F(SplineNetworkTest, ARoadThroughASquareEntersItTwiceAndOneEndingOnTheRimOnce)
{
    const ECS::EntityHandle square =
        AddArea({{0.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f}});
    AddRoad({{-30.0f, 0.0f, 20.0f}, {70.0f, 0.0f, 20.0f}});
    AddRoad({{20.0f, 0.0f, -30.0f}, {20.0f, 0.0f, 0.0f}});
    Derive();
    ASSERT_EQ(Nodes().size(), 3u);
    for (const SplineNode& node : Nodes())
    {
        EXPECT_EQ(node.Kind, SplineNodeKind::Entry);
        EXPECT_EQ(node.Members[1], square);
    }
    EXPECT_NE(NodeAt(0.0f, 20.0f), nullptr);
    EXPECT_NE(NodeAt(40.0f, 20.0f), nullptr);
    ASSERT_NE(NodeAt(20.0f, 0.0f), nullptr);
    EXPECT_EQ(NodeAt(20.0f, 0.0f)->ArmCount, 3u);
}

TEST_F(SplineNetworkTest, TwoRoadsMeetingEndToEndFormOneMerge)
{
    AddRoad({{-50.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}});
    AddRoad({{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 50.0f}});
    Derive();
    ASSERT_EQ(Nodes().size(), 1u);
    EXPECT_EQ(Nodes()[0].Kind, SplineNodeKind::Merge);
    EXPECT_EQ(Nodes()[0].ArmCount, 2u);
}

TEST_F(SplineNetworkTest, OnlyMembersMeet)
{
    AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    // A fence loop, a lake outline and a river carry no member component.
    AddSpline(World, {{-10.0f, 0.0f, -10.0f}, {10.0f, 0.0f, -10.0f}, {10.0f, 0.0f, 10.0f}, {-10.0f, 0.0f, 10.0f}}, true, 0.1f);
    AddSpline(World, {{20.0f, 0.0f, -5.0f}, {30.0f, 0.0f, -5.0f}, {30.0f, 0.0f, 5.0f}, {20.0f, 0.0f, 5.0f}}, true, 0.0f);
    AddSpline(World, {{-30.0f, 0.0f, -50.0f}, {-30.0f, 0.0f, 50.0f}}, false, 8.0f);
    // An open Area is reported and ignored.
    const ECS::EntityHandle openArea = AddArea({{40.0f, 0.0f, -5.0f}, {40.0f, 0.0f, 5.0f}}, false);
    Derive();
    EXPECT_TRUE(Nodes().empty());
    ASSERT_EQ(Network.GetMemberIssues(World).size(), 1u);
    EXPECT_EQ(Network.GetMemberIssues(World)[0].Member, openArea);
    // A path is a Road member too: it crosses the road.
    AddRoad({{0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, 50.0f}});
    Derive();
    EXPECT_EQ(Nodes().size(), 1u);
}

// Arms run counter-clockwise seen from above with +X right and +Z up the screen: -Z, +X, +Z, -X.
TEST_F(SplineNetworkTest, ArmsSortCounterClockwiseFromAboveWhicheverMemberComesFirst)
{
    for (const float32 mirror : {1.0f, -1.0f})
    {
        ECS::World world;
        AddRoad({{0.0f, 0.0f, -50.0f}, {0.0f, 0.0f, 50.0f}}, false, &world);
        AddRoad({{-50.0f * mirror, 0.0f, 0.0f}, {50.0f * mirror, 0.0f, 0.0f}}, false, &world);
        Derive(&world);
        ASSERT_EQ(Nodes().size(), 1u);
        const SplineNode& node = Nodes()[0];
        ASSERT_EQ(node.ArmCount, 4u);
        const float32 expected[4] = {-0.5f, 0.0f, 0.5f, 1.0f};
        for (uint32 i = 0; i < 4; ++i)
            EXPECT_NEAR(node.Arms[i].AngleRadians / std::numbers::pi_v<float32>, expected[i], 1e-3f) << i;
        EXPECT_NEAR(node.Arms[0].Forward.z, -1.0f, 1e-3f);
        EXPECT_NEAR(node.Arms[1].Forward.x, 1.0f, 1e-3f);
    }
}

TEST_F(SplineNetworkTest, ABridgeIsSplitLevelUntilAnAtGradeRuleJoinsIt)
{
    const ECS::EntityHandle road = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle bridge = AddRoad({{0.0f, 6.0f, -50.0f}, {0.0f, 6.0f, 50.0f}});
    const ECS::EntityHandle low = AddRoad({{20.0f, 0.2f, -50.0f}, {20.0f, 0.2f, 50.0f}});
    Derive();
    ASSERT_NE(NodeAt(0.0f, 0.0f), nullptr);
    EXPECT_TRUE(NodeAt(0.0f, 0.0f)->IsSplitLevel());
    EXPECT_NEAR(NodeAt(0.0f, 0.0f)->HeightDifferenceMetres * (NodeAt(0.0f, 0.0f)->Members[0] == road ? 1.0f : -1.0f),
                6.0f, 1e-3f);
    // 0.2 m is under the 0.25 m level check.
    ASSERT_NE(NodeAt(20.0f, 0.0f), nullptr);
    EXPECT_FALSE(NodeAt(20.0f, 0.0f)->IsSplitLevel());
    (void)low;
    WriteRecord(bridge, 0, road, SplineNodeRule::AtGrade, 50.0f, 50.0f);
    Derive();
    EXPECT_EQ(NodeAt(0.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_FALSE(NodeAt(0.0f, 0.0f)->IsSplitLevel());
}

TEST_F(SplineNetworkTest, AKnotInsertAndAnUpstreamDragKeepTheRuleAndReKeyIt)
{
    const ECS::EntityHandle a = AddRoad({{-60.0f, 0.0f, 0.0f}, {-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 70.0f, 50.0f);
    Derive();
    ASSERT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    const uint32 id = NodeAt(10.0f, 0.0f)->Id;
    // A knot inserted before the node adds about 50.8 m of length upstream of it.
    SetPoints(a, {{-60.0f, 0.0f, 0.0f}, {-55.0f, 0.0f, 30.0f}, {-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    Derive();
    const SplineNode* node = NodeAt(10.0f, 0.0f);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->Id, id);
    EXPECT_EQ(node->Rule, SplineNodeRule::AtGrade);
    // The node lies 40 m before A's end, in the spline's own arc length.
    EXPECT_NEAR(RecordOf(a, 0).ArcLengthMetres, DataOf(a).TotalArcLength - 40.0f, kArcTolerance);
    EXPECT_GT(RecordOf(a, 0).ArcLengthMetres, 110.0f);
    EXPECT_NEAR(RecordOf(a, 0).OtherArcLengthMetres, 50.0f, kArcTolerance);
    // The first knot dragged 20 m upstream, the meeting count unchanged.
    SetPoints(a, {{-80.0f, 0.0f, 0.0f}, {-55.0f, 0.0f, 30.0f}, {-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Id, id);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_TRUE(Held().empty());
}

TEST_F(SplineNetworkTest, ASecondMeetingOfThePairTakesNoRule)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}, {-30.0f, 0.0f, 50.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::Closed, 60.0f, 50.0f);
    Derive();
    ASSERT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::Closed);
    // B turns back and crosses A again 40 m before the first meeting along A.
    SetPoints(b, {{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}, {-30.0f, 0.0f, 50.0f}, {-30.0f, 0.0f, -50.0f}});
    Derive();
    ASSERT_EQ(Nodes().size(), 2u);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::Closed);
    EXPECT_EQ(NodeAt(-30.0f, 0.0f)->Rule, SplineNodeRule::None);
}

// One edit that ends one meeting and begins another: up to 10 m it is the meeting moved, as a
// drag moves it; beyond 10 m the old rule is held with no candidate and the new meeting has no
// rule.
TEST_F(SplineNetworkTest, AMeetingThatJumpsKeepsItsRuleWithinReachAndIsHeldBeyondIt)
{
    for (const float32 jump : {6.0f, 9.9f, 10.1f, 15.0f})
    {
        ECS::World world;
        const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}}, false, &world);
        const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}}, false, &world);
        WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 60.0f, 50.0f, &world);
        Derive(&world);
        Spline::SplineData& data = DataOf(b, &world);
        data.SetPointPosition(0, {10.0f + jump, 0.0f, -50.0f});
        data.SetPointPosition(1, {10.0f + jump, 0.0f, 50.0f});
        Spline::RebuildSplineCache(data);
        Derive(&world);
        const SplineNode* node = NodeAt(10.0f + jump, 0.0f);
        ASSERT_NE(node, nullptr) << "jump " << jump;
        if (jump < 10.0f)
        {
            EXPECT_EQ(node->Rule, SplineNodeRule::AtGrade) << "jump " << jump;
            EXPECT_TRUE(Held().empty()) << "jump " << jump;
            continue;
        }
        EXPECT_EQ(node->Rule, SplineNodeRule::None) << "jump " << jump;
        ASSERT_EQ(Held().size(), 1u) << "jump " << jump;
        EXPECT_EQ(Held()[0].Holder, a);
        EXPECT_TRUE(Held()[0].CandidateNodes.empty());
        EXPECT_NEAR(Held()[0].LastPosition.x, 10.0f, kArcTolerance);
    }
}

TEST_F(SplineNetworkTest, ARecordWithTwoMeetingsInReachIsHeldWithBothUntilKept)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 60.0f, 50.0f);
    Derive();
    SetPoints(b, {{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}, {4.0f, 0.0f, 50.0f}, {4.0f, 0.0f, -50.0f}});
    Derive();
    ASSERT_EQ(Nodes().size(), 2u);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::None);
    EXPECT_EQ(NodeAt(4.0f, 0.0f)->Rule, SplineNodeRule::None);
    ASSERT_EQ(Held().size(), 1u);
    EXPECT_EQ(Held()[0].CandidateNodes.size(), 2u);
    ASSERT_TRUE(Network.KeepRecord(World, a, 0, NodeAt(4.0f, 0.0f)->Id));
    Derive();
    EXPECT_EQ(NodeAt(4.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::None);
    EXPECT_TRUE(Held().empty());
}

// Saved with the record on A (or on B), then reloaded with the two entities created in the other
// order so B takes the lower handle: the node carries the rule.
TEST_F(SplineNetworkTest, AReloadThatSwapsTheHandlesMatchesTheSavedKeysOnTheFirstMember)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 60.0f, 50.0f);
    ExpectReloadKeepsTheRule(a, b, 0);
}

TEST_F(SplineNetworkTest, AReloadThatSwapsTheHandlesMatchesTheSavedKeysOnTheSecondMember)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    WriteRecord(b, 0, a, SplineNodeRule::AtGrade, 50.0f, 60.0f);
    ExpectReloadKeepsTheRule(a, b, 1);
}

TEST_F(SplineNetworkTest, ACrossingNearARingRoadsStartMatchesAcrossTheStartOnAReload)
{
    // The ring starts at (20, 0); its closing segment runs from (0, 0) back to the start.
    const ECS::EntityHandle ring = AddRoad(
        {{20.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 0.0f}}, true);
    const ECS::EntityHandle road = AddRoad({{19.5f, 0.0f, -30.0f}, {19.5f, 0.0f, -10.0f}, {19.5f, 0.0f, 30.0f}});
    WriteRecord(ring, 0, road, SplineNodeRule::Closed, 159.5f, 30.0f);
    Derive();
    ASSERT_EQ(NodeAt(19.5f, 0.0f)->Rule, SplineNodeRule::Closed);
    // Saved, then the road moved 1 m across the ring's start: arc 159.5 is now 0.5.
    ECS::World reloaded;
    std::vector<ECS::EntityHandle> copies;
    SetPoints(road, {{20.5f, 0.0f, -30.0f}, {20.5f, 0.0f, -10.0f}, {20.5f, 0.0f, 30.0f}});
    CopyReversedInto(reloaded, {ring, road}, copies);
    Derive(&reloaded);
    ASSERT_NE(NodeAt(20.5f, 0.0f), nullptr);
    EXPECT_EQ(NodeAt(20.5f, 0.0f)->Rule, SplineNodeRule::Closed);
    EXPECT_NEAR(RecordOf(copies[0], 0, &reloaded).ArcLengthMetres, 0.5f, kArcTolerance);
}

TEST_F(SplineNetworkTest, ARoadDraggedOffItsPartnerHoldsTheRecordAndItReturnsOnUndo)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 60.0f, 50.0f);
    Derive();
    SetPoints(b, {{10.0f, 0.0f, 20.0f}, {10.0f, 0.0f, 120.0f}});
    Derive();
    EXPECT_TRUE(Nodes().empty());
    ASSERT_EQ(Held().size(), 1u);
    EXPECT_EQ(RecordOf(a, 0).Rule, SplineNodeRule::AtGrade);
    SetPoints(b, {{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_TRUE(Held().empty());
}

TEST_F(SplineNetworkTest, ARecordOnBothMembersForOneNodeIsHeldOnBoth)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 60.0f, 50.0f);
    WriteRecord(b, 0, a, SplineNodeRule::Closed, 50.0f, 60.0f);
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::None);
    ASSERT_EQ(Held().size(), 2u);
    for (const auto& held : Held())
        EXPECT_EQ(held.CandidateNodes.size(), 1u);
    ASSERT_TRUE(Network.KeepRecord(World, b, 0, NodeAt(10.0f, 0.0f)->Id));
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::Closed);
    ASSERT_EQ(Held().size(), 1u);
    EXPECT_EQ(Held()[0].Holder, a);
}

TEST_F(SplineNetworkTest, AnAttachRecordMakesTheEntryTheGeometryDoesNot)
{
    const ECS::EntityHandle square =
        AddArea({{0.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f}});
    // Ends 3 m short of the square's south edge.
    const ECS::EntityHandle road = AddRoad({{20.0f, 0.0f, -30.0f}, {20.0f, 0.0f, -3.0f}});
    Derive();
    EXPECT_TRUE(Nodes().empty());
    WriteRecord(road, 0, square, SplineNodeRule::Attach, 27.0f, 20.0f);
    Derive();
    ASSERT_EQ(Nodes().size(), 1u);
    const SplineNode& node = Nodes()[0];
    EXPECT_EQ(node.Kind, SplineNodeKind::Entry);
    EXPECT_EQ(node.Rule, SplineNodeRule::Attach);
    EXPECT_EQ(node.Members[1], square);
    EXPECT_NEAR(node.Position.x, 20.0f, kArcTolerance);
    EXPECT_NEAR(node.Position.z, 0.0f, kArcTolerance);
}

TEST_F(SplineNetworkTest, AnAttachRecordJoinsARoadWhoseBoundsMissTheSquare)
{
    const ECS::EntityHandle square =
        AddArea({{0.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f}});
    // Ends 10 m short of the square, beyond the half-width: the pair's bounds do not overlap.
    const ECS::EntityHandle road = AddRoad({{20.0f, 0.0f, -30.0f}, {20.0f, 0.0f, -10.0f}});
    WriteRecord(road, 0, square, SplineNodeRule::Attach, 20.0f, 20.0f);
    Derive();
    ASSERT_EQ(Nodes().size(), 1u);
    EXPECT_EQ(Nodes()[0].Kind, SplineNodeKind::Entry);
    EXPECT_EQ(Nodes()[0].Rule, SplineNodeRule::Attach);
}

TEST_F(SplineNetworkTest, AReversedSplineAndAMovedLoopStartKeepEachRuleOnItsMeeting)
{
    const ECS::EntityHandle ring =
        AddRoad({{0.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f}}, true);
    const ECS::EntityHandle road = AddRoad({{10.0f, 0.0f, -30.0f}, {10.0f, 0.0f, 70.0f}});
    WriteRecord(ring, 0, road, SplineNodeRule::AtGrade, 10.0f, 30.0f);
    WriteRecord(ring, 1, road, SplineNodeRule::Closed, 110.0f, 70.0f);
    Derive();
    ASSERT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    ASSERT_EQ(NodeAt(10.0f, 40.0f)->Rule, SplineNodeRule::Closed);
    SetPoints(road, {{10.0f, 0.0f, 70.0f}, {10.0f, 0.0f, -30.0f}});
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(10.0f, 40.0f)->Rule, SplineNodeRule::Closed);
    EXPECT_NEAR(RecordOf(ring, 0).OtherArcLengthMetres, 70.0f, kArcTolerance);
    SetPoints(ring, {{40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}});
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(10.0f, 40.0f)->Rule, SplineNodeRule::Closed);
    EXPECT_NEAR(RecordOf(ring, 1).ArcLengthMetres, 30.0f, kArcTolerance);
    EXPECT_TRUE(Held().empty());
}

TEST_F(SplineNetworkTest, AHeldRecordSurvivesAReloadAndIsReportedAfterIt)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, 20.0f}, {10.0f, 0.0f, 120.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 60.0f, 50.0f);
    Derive();
    ASSERT_EQ(Held().size(), 1u);
    ECS::World reloaded;
    std::vector<ECS::EntityHandle> copies;
    CopyReversedInto(reloaded, {a, b}, copies);
    Derive(&reloaded);
    ASSERT_EQ(Held().size(), 1u);
    EXPECT_EQ(Held()[0].Holder, copies[0]);
    EXPECT_EQ(RecordOf(copies[0], 0, &reloaded).Rule, SplineNodeRule::AtGrade);
}

// A snapshot applied after a move, and a record a script writes: both are loads, matched by the
// saved keys at the next derivation.
TEST_F(SplineNetworkTest, ASnapshotAppliedAndAScriptWriteAreMatchedByTheirKeys)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 60.0f, 50.0f);
    Derive();
    const SplineNetworkOverrides snapshot = *World.GetComponent<SplineNetworkOverrides>(a);
    SetPoints(b, {{14.0f, 0.0f, -50.0f}, {14.0f, 0.0f, 50.0f}});
    Derive();
    SetPoints(b, {{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    World.AddComponentImmediate(a, snapshot);
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    const ECS::EntityHandle c = AddRoad({{-20.0f, 0.0f, -50.0f}, {-20.0f, 0.0f, 50.0f}});
    Derive();
    WriteRecord(c, 0, a, SplineNodeRule::Closed, 52.0f, 29.0f);
    Derive();
    EXPECT_EQ(NodeAt(-20.0f, 0.0f)->Rule, SplineNodeRule::Closed);
    EXPECT_NEAR(RecordOf(c, 0).ArcLengthMetres, 50.0f, kArcTolerance);
    EXPECT_NEAR(RecordOf(c, 0).OtherArcLengthMetres, 30.0f, kArcTolerance);
}

TEST_F(SplineNetworkTest, TheChangeNoticeNamesAddedChangedAndRemovedNodes)
{
    std::vector<SplineECS::SplineNodeChange> seen;
    const EventSubscription subscription = Network.NodesChanged.Subscribe(
        [&seen](const ECS::World&, std::span<const SplineECS::SplineNodeChange> changes) {
            seen.assign(changes.begin(), changes.end());
        });
    AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    Derive();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].Kind, SplineECS::SplineNodeChangeKind::Added);
    const uint32 id = seen[0].NodeId;
    SetPoints(b, {{12.0f, 0.0f, -50.0f}, {12.0f, 0.0f, 50.0f}});
    Derive();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].Kind, SplineECS::SplineNodeChangeKind::Changed);
    EXPECT_EQ(seen[0].NodeId, id);
    SetPoints(b, {{12.0f, 0.0f, 20.0f}, {12.0f, 0.0f, 50.0f}});
    Derive();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].Kind, SplineECS::SplineNodeChangeKind::Removed);
}

// A cleared world starts a new run of handles: the scene that fills it again is another scene,
// whose junctions are new ones, even where the same roads lie at the same place.
TEST_F(SplineNetworkTest, AClearedWorldsJunctionsAreNewOnes)
{
    std::vector<SplineECS::SplineNodeChange> seen;
    const EventSubscription subscription = Network.NodesChanged.Subscribe(
        [&seen](const ECS::World&, std::span<const SplineECS::SplineNodeChange> changes) {
            seen.assign(changes.begin(), changes.end());
        });
    AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    Derive();
    ASSERT_EQ(seen.size(), 1u);
    const uint32 id = seen[0].NodeId;
    World.Clear();
    AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    Derive();
    ASSERT_EQ(Nodes().size(), 1u);
    EXPECT_NE(Nodes()[0].Id, id);
}

// Two meetings of one pair 6 m apart; the ordering member (A, the longer) is reversed. Each rule
// stays on its own meeting: the match is by position, never by the order along A alone.
TEST_F(SplineNetworkTest, ReversingTheOrderingMemberKeepsTwoNearRulesOnTheirMeetings)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b =
        AddRoad({{10.0f, 0.0f, -20.0f}, {10.0f, 0.0f, 20.0f}, {4.0f, 0.0f, 20.0f}, {4.0f, 0.0f, -20.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 54.0f, 66.0f);
    WriteRecord(a, 1, b, SplineNodeRule::Closed, 60.0f, 20.0f);
    Derive();
    ASSERT_EQ(Nodes().size(), 2u);
    ASSERT_EQ(NodeAt(4.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    ASSERT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::Closed);
    SetPoints(a, {{50.0f, 0.0f, 0.0f}, {-50.0f, 0.0f, 0.0f}});
    Derive();
    ASSERT_EQ(Nodes().size(), 2u);
    EXPECT_EQ(NodeAt(4.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::Closed);
}

// Two meetings of one pair 6 m apart on a ring road's edge; the loop's start moves between them.
TEST_F(SplineNetworkTest, MovingTheLoopStartBetweenTwoNearMeetingsKeepsEachRule)
{
    const ECS::EntityHandle ring = AddRoad(
        {{0.0f, 0.0f, 0.0f}, {13.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f}}, true);
    const ECS::EntityHandle b =
        AddRoad({{10.0f, 0.0f, -20.0f}, {10.0f, 0.0f, 20.0f}, {16.0f, 0.0f, 20.0f}, {16.0f, 0.0f, -20.0f}});
    WriteRecord(ring, 0, b, SplineNodeRule::AtGrade, 10.0f, 20.0f);
    WriteRecord(ring, 1, b, SplineNodeRule::Closed, 16.0f, 66.0f);
    Derive();
    ASSERT_EQ(Nodes().size(), 2u);
    ASSERT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    ASSERT_EQ(NodeAt(16.0f, 0.0f)->Rule, SplineNodeRule::Closed);
    SetPoints(ring,
              {{13.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 0.0f}});
    Derive();
    ASSERT_EQ(Nodes().size(), 2u);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(16.0f, 0.0f)->Rule, SplineNodeRule::Closed);
}

// A crosses a U-shaped B at x = 5 and x = 9; the U is narrowed so the second meeting lands 1.5 m
// from the first. The order along both members is unchanged, so each rule stays on its meeting.
TEST_F(SplineNetworkTest, NarrowingAUShapedPartnerKeepsEachRuleOnItsMeeting)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b =
        AddRoad({{9.0f, 0.0f, -20.0f}, {9.0f, 0.0f, 20.0f}, {5.0f, 0.0f, 20.0f}, {5.0f, 0.0f, -20.0f}});
    WriteRecord(b, 0, a, SplineNodeRule::AtGrade, 64.0f, 55.0f);
    WriteRecord(b, 1, a, SplineNodeRule::SplitLevel, 20.0f, 59.0f);
    Derive();
    ASSERT_EQ(Nodes().size(), 2u);
    SetPoints(b, {{6.5f, 0.0f, -20.0f}, {6.5f, 0.0f, 20.0f}, {5.0f, 0.0f, 20.0f}, {5.0f, 0.0f, -20.0f}});
    Derive();
    ASSERT_EQ(Nodes().size(), 2u);
    ASSERT_NE(NodeAt(5.0f, 0.0f), nullptr);
    ASSERT_NE(NodeAt(6.5f, 0.0f), nullptr);
    EXPECT_EQ(NodeAt(5.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(6.5f, 0.0f)->Rule, SplineNodeRule::SplitLevel);
    EXPECT_TRUE(Held().empty());
}

// A record on A rides on the node; a script writes a second record for the same node on B. Both
// are held with the node as their candidate; Keep lands B's, and A's stays held beside it, still
// offering a Keep. A Keep of A's then holds B's beside it.
TEST_F(SplineNetworkTest, ASecondRecordWrittenOnThePartnerIsHeldWithTheFirstUntilOneIsKept)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 60.0f, 50.0f);
    Derive();
    ASSERT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    WriteRecord(b, 0, a, SplineNodeRule::SplitLevel, 50.0f, 60.0f);
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::None);
    ASSERT_EQ(Held().size(), 2u);
    for (const auto& held : Held())
        EXPECT_EQ(held.CandidateNodes.size(), 1u);
    // A small drag of A keeps both held.
    SetPoints(a, {{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.1f}});
    Derive();
    EXPECT_EQ(Held().size(), 2u);
    const uint32 id = NodeAt(10.0f, 0.0f)->Id;
    ASSERT_TRUE(Network.KeepRecord(World, b, 0, id));
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::SplitLevel);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->RuleHolder, b);
    ASSERT_EQ(Held().size(), 1u);
    EXPECT_EQ(Held()[0].Holder, a);
    EXPECT_EQ(Held()[0].CandidateNodes.size(), 1u);
    ASSERT_TRUE(Network.KeepRecord(World, a, 0, id));
    for (int32 pass = 0; pass < 2; ++pass)
    {
        Derive();
        EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade) << pass;
        EXPECT_EQ(NodeAt(10.0f, 0.0f)->RuleHolder, a) << pass;
        ASSERT_EQ(Held().size(), 1u) << pass;
        EXPECT_EQ(Held()[0].Holder, b) << pass;
        EXPECT_EQ(Held()[0].CandidateNodes, std::vector<uint32>{id}) << pass;
    }
}

// A holds two records; A gains 20 m upstream and, in the same epoch, an undo clears the first
// record. The second record was bound by the service, so it keeps following its node.
TEST_F(SplineNetworkTest, ClearingOneRecordDuringADragKeepsTheHoldersOtherBindings)
{
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    const ECS::EntityHandle c = AddRoad({{30.0f, 0.0f, -50.0f}, {30.0f, 0.0f, 50.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 60.0f, 50.0f);
    WriteRecord(a, 1, c, SplineNodeRule::AtGrade, 80.0f, 50.0f);
    Derive();
    ASSERT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    ASSERT_EQ(NodeAt(30.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    const Vector3 position = NodeAt(10.0f, 0.0f)->Position;
    SetPoints(a, {{-70.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    Network.SetRecord(World, a, 0, SplineNetworkOverride{}, position);
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::None);
    EXPECT_EQ(NodeAt(30.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_TRUE(Held().empty());
}

// A road ends 3 m short of a square and is attached by a record; a knot is inserted on the
// square's outline before the attach point. The attach point stays where it was.
TEST_F(SplineNetworkTest, AnAttachPointStaysPutWhenTheBodyGainsAnUpstreamKnot)
{
    const ECS::EntityHandle area =
        AddArea({{0.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f}});
    const ECS::EntityHandle road = AddRoad({{20.0f, 0.0f, -30.0f}, {20.0f, 0.0f, -3.0f}});
    WriteRecord(road, 0, area, SplineNodeRule::Attach, 27.0f, 20.0f);
    Derive();
    ASSERT_EQ(Nodes().size(), 1u);
    ASSERT_NE(NodeAt(20.0f, 0.0f), nullptr);
    Spline::SplineData& data = DataOf(area);
    data.Points.clear();
    for (const Vector3& point : std::vector<Vector3>{{0.0f, 0.0f, 0.0f}, {5.0f, 0.0f, -10.0f}, {10.0f, 0.0f, 0.0f},
                                                     {40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 40.0f}, {0.0f, 0.0f, 40.0f}})
        data.AddPoint(point, 0.0f);
    Spline::RebuildSplineCache(data);
    Derive();
    ASSERT_EQ(Nodes().size(), 1u);
    const SplineNode& node = Nodes()[0];
    EXPECT_EQ(node.Rule, SplineNodeRule::Attach);
    EXPECT_NEAR(node.Position.x, 20.0f, kArcTolerance);
    EXPECT_NEAR(node.Position.z, 0.0f, kArcTolerance);
    // The record is re-keyed to where the point now lies along the longer outline.
    const Vector3 keyed = Spline::EvaluateAtDistance(DataOf(area), RecordOf(road, 0).OtherArcLengthMetres).Position;
    EXPECT_NEAR(keyed.x, 20.0f, kArcTolerance);
    EXPECT_NEAR(keyed.z, 0.0f, kArcTolerance);
}

// A crossing exactly at a sample point of both centerlines, under many rotations: found once.
TEST_F(SplineNetworkTest, ACrossingThroughSamplePointsIsFoundOnceAtEveryAngle)
{
    constexpr int32 kAngles = 180;
    uint32 missed = 0;
    for (int32 step = 0; step < kAngles; ++step)
    {
        ECS::World world;
        const float32 angle = static_cast<float32>(step) * std::numbers::pi_v<float32> / kAngles + 0.013f;
        const Vector3 direction{std::cos(angle), 0.0f, std::sin(angle)};
        const Vector3 normal{-direction.z, 0.0f, direction.x};
        const Vector3 origin{3.7f, 0.0f, -1.3f};
        AddRoad({origin, origin + direction * 100.0f}, false, &world);
        const Vector3 crossing = origin + direction * 50.0f;
        AddRoad({crossing - normal * 50.0f, crossing + normal * 50.0f}, false, &world);
        Derive(&world);
        missed += Nodes().size() == 1 ? 0u : 1u;
    }
    EXPECT_EQ(missed, 0u);
}

// A road ending on the true rim of a round square of radius 5 m, between two samples of the rim,
// at sixteen places: an Entry at every one. The touch is measured against the curve.
TEST_F(SplineNetworkTest, AnEndOnACurvedRimBetweenSamplesIsAnEntry)
{
    constexpr float32 kRadius = 5.0f;
    constexpr int32 kPlaces = 16;
    SplineECS::SplineService& splines = SplineECS::SplineService::Get();
    const SplineECS::SplineHandle handle = splines.CreateSpline(Spline::SplineType::CatmullRom, true);
    Spline::SplineData* rim = splines.GetSplineData(handle);
    for (int32 i = 0; i < 8; ++i)
    {
        const float32 a = static_cast<float32>(i) * std::numbers::pi_v<float32> / 4.0f;
        rim->AddPoint({kRadius * std::cos(a), 0.0f, kRadius * std::sin(a)}, 0.0f);
    }
    Spline::RebuildSplineCache(*rim);
    const ECS::EntityHandle area = World.CreateEntity();
    Components::SplineComponent spline{};
    spline.SplineDataIndex = handle.Index();
    spline.SplineDataGeneration = handle.Generation();
    World.AddComponentImmediate(area, spline);
    World.AddComponentImmediate(area, Components::WorldTransform{});
    World.AddComponentImmediate(area, Components::SplineNetworkMember{SplineNetworkMemberKind::Area});
    uint32 found = 0;
    for (int32 k = 0; k < kPlaces; ++k)
    {
        // Creating a road may move the service's spline storage: look the rim up again each time.
        const Spline::SplineData& outline = *splines.GetSplineData(handle);
        const float32 distance = (static_cast<float32>(k) + 0.37f) * outline.TotalArcLength / kPlaces;
        const Vector3 end = Spline::EvaluateAtDistance(outline, distance).Position;
        const Vector3 outward = Vector3{end.x, 0.0f, end.z} * (1.0f / std::hypot(end.x, end.z));
        const ECS::EntityHandle road = AddRoad({end + outward * 20.0f, end});
        Derive();
        const SplineNode* node = NodeAt(end.x, end.z);
        found += node && node->Kind == SplineNodeKind::Entry ? 1u : 0u;
        World.DestroyEntityImmediate(road);
    }
    EXPECT_EQ(found, static_cast<uint32>(kPlaces));
}

// Two worlds updated in turn through the one service each keep their own nodes' identities.
TEST_F(SplineNetworkTest, TwoWorldsUpdatedInTurnKeepTheirIdentities)
{
    ECS::World other;
    const ECS::EntityHandle a = AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle b = AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    WriteRecord(a, 0, b, SplineNodeRule::AtGrade, 60.0f, 50.0f);
    AddRoad({{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}}, false, &other);
    AddRoad({{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}}, false, &other);
    Derive();
    const uint32 id = NodeAt(10.0f, 0.0f)->Id;
    Derive(&other);
    Derive();
    ASSERT_NE(NodeAt(10.0f, 0.0f), nullptr);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Id, id);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
}

// One crossing at each of the grid's 100 by 100 places, 20 m apart; one road may lean by up to
// `lean` meters along Z.
void ExpectOneCrossingAtEveryGridPlace(std::span<const SplineNode> nodes, float32 lean, const char* step)
{
    ASSERT_EQ(nodes.size(), 10000u) << step;
    std::vector<bool> taken(10000, false);
    for (const SplineNode& node : nodes)
    {
        const int32 column = static_cast<int32>(std::lround(node.Position.x / 20.0f));
        const int32 row = static_cast<int32>(std::lround(node.Position.z / 20.0f));
        ASSERT_TRUE(column >= 0 && column < 100 && row >= 0 && row < 100) << step;
        EXPECT_EQ(node.Kind, SplineNodeKind::Crossing) << step;
        EXPECT_NEAR(node.Position.x, static_cast<float32>(column) * 20.0f, kArcTolerance) << step;
        EXPECT_NEAR(node.Position.z, static_cast<float32>(row) * 20.0f, lean + kArcTolerance) << step;
        EXPECT_FALSE(taken[row * 100 + column]) << step;
        taken[row * 100 + column] = true;
    }
}

// The derivation's node set over a 200-member grid (100 by 100 roads, 10,000 crossings), first and
// after one knot moved on one road; and, for information, its cost from a test-side clock in this
// build's configuration.
TEST_F(SplineNetworkTest, ATwoHundredMemberGridDerivesEveryCrossing)
{
    std::vector<ECS::EntityHandle> roads;
    for (int32 i = 0; i < 100; ++i)
    {
        const float32 offset = static_cast<float32>(i) * 20.0f;
        roads.push_back(AddRoad({{-10.0f, 0.0f, offset}, {2000.0f, 0.0f, offset}}));
        roads.push_back(AddRoad({{offset, 0.0f, -10.0f}, {offset, 0.0f, 2000.0f}}));
    }
    auto start = std::chrono::steady_clock::now();
    Derive();
    const std::chrono::duration<double, std::milli> first = std::chrono::steady_clock::now() - start;
    ExpectOneCrossingAtEveryGridPlace(Nodes(), 0.0f, "first");
    Spline::SplineData& data = DataOf(roads[100]);
    data.SetPointPosition(1, {2000.0f, 0.0f, 1000.5f});
    Spline::RebuildSplineCache(data);
    start = std::chrono::steady_clock::now();
    Derive();
    const std::chrono::duration<double, std::milli> edit = std::chrono::steady_clock::now() - start;
    start = std::chrono::steady_clock::now();
    Derive();
    const std::chrono::duration<double, std::milli> idle = std::chrono::steady_clock::now() - start;
    std::printf("[ info ] 200 members, %zu nodes: first derivation %.1f ms, one edit %.1f ms, idle %.3f ms\n",
                Nodes().size(), first.count(), edit.count(), idle.count());
    ExpectOneCrossingAtEveryGridPlace(Nodes(), 0.5f, "after the edit");
}

// 2000 short roads 10 m apart meet nowhere; one knot moved on one road. No node, first or after
// the edit; and, for information, the cost from a test-side clock in this build's configuration.
TEST_F(SplineNetworkTest, TwoThousandRoadsApartFormNoNode)
{
    std::vector<ECS::EntityHandle> roads;
    for (int32 i = 0; i < 2000; ++i)
    {
        const float32 x = static_cast<float32>(i) * 10.0f;
        roads.push_back(AddRoad({{x, 0.0f, 0.0f}, {x, 0.0f, 5.0f}}));
    }
    auto start = std::chrono::steady_clock::now();
    Derive();
    const std::chrono::duration<double, std::milli> first = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(Nodes().empty());
    Spline::SplineData& data = DataOf(roads[1000]);
    data.SetPointPosition(1, {10000.0f, 0.0f, 4.0f});
    Spline::RebuildSplineCache(data);
    start = std::chrono::steady_clock::now();
    Derive();
    const std::chrono::duration<double, std::milli> edit = std::chrono::steady_clock::now() - start;
    std::printf("[ info ] 2000 members, no node: first derivation %.1f ms, one edit %.1f ms\n", first.count(),
                edit.count());
    EXPECT_TRUE(Nodes().empty());
}

} // namespace
