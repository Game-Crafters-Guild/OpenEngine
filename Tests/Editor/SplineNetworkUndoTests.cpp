#include <gtest/gtest.h>

#include "UndoRedo/SplineNetworkRecordCommand.h"
#include "UndoRedo/SplineUndoHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Name.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Spline/SplineNetworkOverrides.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Mathematics/Vector3.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineNetwork.h"
#include "SplineECS/SplineService.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using Components::SplineNetworkOverride;
using Components::SplineNetworkOverrides;
using Components::SplineNodeRule;
using Editor::SplineNetworkRecordCommand;
using Editor::UndoRedoService;
using Mathematics::Vector3;
using SplineECS::SplineNetwork;
using SplineECS::SplineNode;

namespace
{

constexpr float32 kHalfWidth = 3.0f;

// The undo steps of the junction records, against the network the editor runs: each step
// changes one record and finds it again by where its junction is.
class SplineNetworkUndoTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
        m_OwnsNetwork = !SplineNetwork::IsInitialized();
        if (m_OwnsNetwork)
            SplineNetwork::Initialize();
        m_World = std::make_unique<ECS::World>();
    }

    void TearDown() override
    {
        m_World.reset();
        if (m_OwnsNetwork)
            SplineNetwork::Shutdown();
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
    }

    // A Linear Road member through `points`, in world meters.
    ECS::EntityHandle AddRoad(const char* name, const std::vector<Vector3>& points)
    {
        SplineECS::SplineService& splines = SplineECS::SplineService::Get();
        const SplineECS::SplineHandle handle = splines.CreateSpline(Spline::SplineType::Linear, false);
        Spline::SplineData* data = splines.GetSplineData(handle);
        for (const Vector3& point : points)
            data->AddPoint(point, kHalfWidth);
        splines.RebuildCache(handle);
        const ECS::EntityHandle entity = m_World->CreateEntity();
        Components::SplineComponent spline{};
        spline.SplineDataIndex = handle.Index();
        spline.SplineDataGeneration = handle.Generation();
        Components::Name label{};
        std::strncpy(label.value, name, sizeof(label.value) - 1);
        m_World->AddComponentImmediate(entity, spline);
        m_World->AddComponentImmediate(entity, Components::WorldTransform{});
        m_World->AddComponentImmediate(entity, label);
        m_World->AddComponentImmediate(entity, Components::SplineNetworkMember{Components::SplineNetworkMemberKind::Road});
        return entity;
    }

    SplineECS::SplineHandle SplineOf(ECS::EntityHandle entity) const
    {
        const auto* spline = m_World->GetComponent<Components::SplineComponent>(entity);
        return {spline->SplineDataIndex, spline->SplineDataGeneration};
    }

    // A knot drag as the spline tool commits it: one undo step of the spline's points.
    void Drag(ECS::EntityHandle entity, uint32 point, const Vector3& position)
    {
        Editor::SplineUndo::CommitSplineDataOneShot(&SplineECS::SplineService::Get(), SplineOf(entity), m_World.get(), entity,
                                        nullptr, &m_Undo, "Move Spline Point",
                                        [&](Spline::SplineData* data) { data->SetPointPosition(point, position); });
    }

    void Derive() { SplineNetwork::Get().Update(*m_World); }

    const SplineNode* NodeAt(float32 x, float32 z) const
    {
        for (const SplineNode& node : SplineNetwork::Get().GetNodes(*m_World))
            if (std::hypot(node.Position.x - x, node.Position.z - z) <= 1.0f)
                return &node;
        return nullptr;
    }

    uint32 RecordCount(ECS::EntityHandle holder) const
    {
        const auto* overrides = m_World->GetComponent<SplineNetworkOverrides>(holder);
        uint32 count = 0;
        if (overrides)
            for (const SplineNetworkOverride& record : overrides->Records)
                count += record.Rule != SplineNodeRule::None ? 1u : 0u;
        return count;
    }

    void Execute(std::unique_ptr<SplineNetworkRecordCommand> command, const char* expectedName)
    {
        ASSERT_NE(command, nullptr);
        m_Undo.Execute(std::move(command));
        EXPECT_STREQ(m_Undo.PeekUndoName(), expectedName);
        Derive();
    }

    std::unique_ptr<ECS::World> m_World;
    UndoRedoService m_Undo;

  private:
    bool m_OwnsSplines = false;
    bool m_OwnsNetwork = false;
};

// The rule rides on its meeting through a drag that ends it, a clear of the held record, and
// the undo and redo of both.
TEST_F(SplineNetworkUndoTest, AnOverriddenCrossingDraggedApartKeepsItsRuleThroughUndoAndRedo)
{
    const ECS::EntityHandle high = AddRoad("High Street", {{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle ford = AddRoad("Ford Road", {{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    Derive();
    ASSERT_NE(NodeAt(10.0f, 0.0f), nullptr);
    Execute(SplineNetworkRecordCommand::JoinAtGrade(*m_World, *NodeAt(10.0f, 0.0f), high), "Join High Street at Grade");
    ASSERT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->RuleHolder, high);

    Drag(ford, 0, {10.0f, 0.0f, 20.0f});
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f), nullptr);
    ASSERT_EQ(SplineNetwork::Get().GetHeldRecords(*m_World).size(), 1u);
    EXPECT_EQ(SplineNetwork::Get().GetHeldRecords(*m_World)[0].Holder, high);

    m_Undo.Undo();
    Derive();
    ASSERT_NE(NodeAt(10.0f, 0.0f), nullptr);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_TRUE(SplineNetwork::Get().GetHeldRecords(*m_World).empty());

    m_Undo.Redo();
    Derive();
    ASSERT_EQ(SplineNetwork::Get().GetHeldRecords(*m_World).size(), 1u);
    const uint32 heldSlot = SplineNetwork::Get().GetHeldRecords(*m_World)[0].Slot;
    Execute(SplineNetworkRecordCommand::Clear(*m_World, high, heldSlot), "Clear Junction Override");
    EXPECT_TRUE(SplineNetwork::Get().GetHeldRecords(*m_World).empty());
    EXPECT_EQ(RecordCount(high), 0u);

    m_Undo.Undo();
    Derive();
    EXPECT_EQ(SplineNetwork::Get().GetHeldRecords(*m_World).size(), 1u);
    m_Undo.Undo();
    Derive();
    ASSERT_NE(NodeAt(10.0f, 0.0f), nullptr);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->RuleHolder, high);
    EXPECT_EQ(RecordCount(high), 1u);
}

// Two meetings of one pair, each with a rule: after an upstream drag of 20 m re-keyed both
// records, undoing the second Join at Grade removes that record alone.
TEST_F(SplineNetworkUndoTest, UndoingAJoinAtGradeAfterAnUpstreamDragRemovesThatRecordAlone)
{
    const ECS::EntityHandle high = AddRoad("High Street", {{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle loop = AddRoad("Mill Loop", {{-10.0f, 0.0f, -30.0f}, {-10.0f, 0.0f, 30.0f},
                                                         {20.0f, 0.0f, 30.0f}, {20.0f, 0.0f, -30.0f}});
    Derive();
    ASSERT_NE(NodeAt(-10.0f, 0.0f), nullptr);
    ASSERT_NE(NodeAt(20.0f, 0.0f), nullptr);
    Execute(SplineNetworkRecordCommand::JoinAtGrade(*m_World, *NodeAt(-10.0f, 0.0f), high), "Join High Street at Grade");

    Drag(high, 0, {-70.0f, 0.0f, 0.0f});
    Derive();
    ASSERT_EQ(NodeAt(-10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    const SplineNetworkOverride first = m_World->GetComponent<SplineNetworkOverrides>(high)->Records[0];
    EXPECT_NEAR(first.ArcLengthMetres, 60.0f, 0.05f);

    Execute(SplineNetworkRecordCommand::JoinAtGrade(*m_World, *NodeAt(20.0f, 0.0f), loop), "Join Mill Loop at Grade");
    ASSERT_EQ(NodeAt(20.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(20.0f, 0.0f)->RuleHolder, loop);

    m_Undo.Undo();
    Derive();
    EXPECT_EQ(NodeAt(20.0f, 0.0f)->Rule, SplineNodeRule::None);
    EXPECT_EQ(RecordCount(loop), 0u);
    ASSERT_EQ(NodeAt(-10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(-10.0f, 0.0f)->RuleHolder, high);
    EXPECT_EQ(m_World->GetComponent<SplineNetworkOverrides>(high)->Records[0], first);

    m_Undo.Redo();
    Derive();
    EXPECT_EQ(NodeAt(20.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(-10.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_TRUE(SplineNetwork::Get().GetHeldRecords(*m_World).empty());
}

// Attach makes the T junction an end left short does not; Detach removes it; both undo.
TEST_F(SplineNetworkUndoTest, AttachAndDetachAreUndoSteps)
{
    const ECS::EntityHandle high = AddRoad("High Street", {{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle church = AddRoad("Church Walk", {{10.0f, 0.0f, 40.0f}, {10.0f, 0.0f, 8.0f}});
    Derive();
    EXPECT_TRUE(SplineNetwork::Get().GetNodes(*m_World).empty());

    Execute(SplineNetworkRecordCommand::Attach(*m_World, church, 32.0f, high, 60.0f, {10.0f, 0.0f, 0.0f}),
            "Attach Church Walk to High Street");
    ASSERT_NE(NodeAt(10.0f, 0.0f), nullptr);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Kind, SplineECS::SplineNodeKind::Attachment);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::Attach);

    Execute(SplineNetworkRecordCommand::Detach(*m_World, *NodeAt(10.0f, 0.0f)), "Detach Church Walk");
    EXPECT_EQ(NodeAt(10.0f, 0.0f), nullptr);
    EXPECT_EQ(RecordCount(church), 0u);

    m_Undo.Undo();
    Derive();
    ASSERT_NE(NodeAt(10.0f, 0.0f), nullptr);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::Attach);
    m_Undo.Undo();
    Derive();
    EXPECT_EQ(NodeAt(10.0f, 0.0f), nullptr);
    EXPECT_EQ(RecordCount(church), 0u);
    m_Undo.Redo();
    Derive();
    ASSERT_NE(NodeAt(10.0f, 0.0f), nullptr);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->RuleHolder, church);
}

// A record with two meetings of its pair in reach is held until Keep lands it on one; undo
// holds it again between the two, and redo lands it on the same one.
TEST_F(SplineNetworkUndoTest, KeepLandsAHeldRecordAndUndoHoldsItAgain)
{
    const ECS::EntityHandle high = AddRoad("High Street", {{-50.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}});
    const ECS::EntityHandle loop = AddRoad("Mill Loop", {{10.0f, 0.0f, -50.0f}, {10.0f, 0.0f, 50.0f}});
    Derive();
    Execute(SplineNetworkRecordCommand::JoinAtGrade(*m_World, *NodeAt(10.0f, 0.0f), high), "Join High Street at Grade");
    Spline::SplineData* data = SplineECS::SplineService::Get().GetSplineData(SplineOf(loop));
    data->AddPoint({4.0f, 0.0f, 50.0f}, kHalfWidth);
    data->AddPoint({4.0f, 0.0f, -50.0f}, kHalfWidth);
    SplineECS::SplineService::Get().RebuildCache(SplineOf(loop));
    Derive();
    ASSERT_EQ(SplineNetwork::Get().GetHeldRecords(*m_World).size(), 1u);
    ASSERT_EQ(SplineNetwork::Get().GetHeldRecords(*m_World)[0].CandidateNodes.size(), 2u);
    const uint32 slot = SplineNetwork::Get().GetHeldRecords(*m_World)[0].Slot;

    Execute(SplineNetworkRecordCommand::Keep(*m_World, high, slot, NodeAt(4.0f, 0.0f)->Id), "Keep Junction Rule");
    EXPECT_TRUE(SplineNetwork::Get().GetHeldRecords(*m_World).empty());
    EXPECT_EQ(NodeAt(4.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::None);

    m_Undo.Undo();
    Derive();
    ASSERT_EQ(SplineNetwork::Get().GetHeldRecords(*m_World).size(), 1u);
    EXPECT_EQ(SplineNetwork::Get().GetHeldRecords(*m_World)[0].CandidateNodes.size(), 2u);
    EXPECT_EQ(NodeAt(4.0f, 0.0f)->Rule, SplineNodeRule::None);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::None);

    m_Undo.Redo();
    Derive();
    EXPECT_TRUE(SplineNetwork::Get().GetHeldRecords(*m_World).empty());
    EXPECT_EQ(NodeAt(4.0f, 0.0f)->Rule, SplineNodeRule::AtGrade);
    EXPECT_EQ(NodeAt(10.0f, 0.0f)->Rule, SplineNodeRule::None);
    EXPECT_EQ(RecordCount(high), 1u);
}

} // namespace
