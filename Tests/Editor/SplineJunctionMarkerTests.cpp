#include <gtest/gtest.h>

#include "Components/Name.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Spline/SplineNetworkOverrides.h"
#include "Components/Spline/SplinePaving.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/CpuProfiler.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Inspectors/SplineRecipe/Paving/SplinePavingInspector.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "SceneView/SplineTool/SplineJunctionMarkers.h"
#include "SceneView/SplineTool/SplineSceneGizmo.h"
#include "SceneView/SplineTool/SplineTool.h"
#include "Scripting/ScriptsConfig.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineNetwork.h"
#include "SplineECS/SplineService.h"
#include "SplineRecipes/Paving/PavingSources.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{

using namespace GameEngine;
using Editor::SceneTools::PointerButton;
using Editor::SceneTools::PointerPhase;
using Editor::SceneTools::ScenePointerEvent;
using Editor::SceneTools::SplineJunctionMarkers;
using Editor::SceneTools::SplineSceneGizmo;
using Editor::SceneTools::SplineTool;
using Mathematics::Vector3;
using SplineECS::SplineNetwork;
using SplineECS::SplineNode;

class SplineJunctionMarkerTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ScriptsConfig scriptsConfig{};
            scriptsConfig.disableClr = true;
            scriptsConfig.enableHotReload = false;
            scriptsConfig.enableAsyncHotReload = false;
            scriptsConfig.enableAutoProjectGeneration = false;
            engine.SetScriptsConfig(scriptsConfig);
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
        if (!SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Initialize();
        if (!SplineNetwork::IsInitialized())
            SplineNetwork::Initialize();
    }

    void SetUp() override
    {
        m_World = EngineCore::GetInstance().EnsurePrimaryWorld();
        ASSERT_NE(m_World, nullptr);
        m_World->Clear();
    }

    // A Linear network member through world points, 1 m wide on each side, named `name`.
    ECS::EntityHandle AddMember(const char* name, const std::vector<Vector3>& points, bool closed,
                                Components::SplineNetworkMemberKind kind)
    {
        SplineECS::SplineService& splines = SplineECS::SplineService::Get();
        const auto handle = splines.CreateSpline(Spline::SplineType::Linear, closed);
        Spline::SplineData* data = splines.GetSplineData(handle);
        for (const Vector3& point : points)
            data->AddPoint(point, 1.0f);
        splines.RebuildCache(handle);
        ECS::Entity entity = m_World->Create();
        entity.Set(Components::Transform::FromTRS(Mathematics::WorldPosition{}, Mathematics::Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
                                                  Vector3(1.0f, 1.0f, 1.0f)));
        entity.Set(Components::WorldTransform{});
        Components::SplineComponent spline{};
        spline.SplineDataIndex = handle.Index();
        spline.SplineDataGeneration = handle.Generation();
        entity.Set(spline);
        Components::Name entityName{};
        std::strncpy(entityName.value, name, sizeof(entityName.value) - 1);
        entity.Set(entityName);
        entity.Set(Components::SplineNetworkMember{kind});
        m_World->ProcessCommands();
        return entity.GetHandle();
    }

    const SplineNode* NodeNear(float x, float z) const
    {
        for (const SplineNode& node : SplineNetwork::Get().GetNodes(*m_World))
        {
            if (std::abs(node.Position.x - x) < 0.01f && std::abs(node.Position.z - z) < 0.01f)
                return &node;
        }
        return nullptr;
    }

    // A pointer straight down from 10 m above `ground`, no button held.
    static ScenePointerEvent HoverAbove(const Vector3& ground)
    {
        ScenePointerEvent event{};
        event.phase = PointerPhase::Move;
        event.button = PointerButton::None;
        event.viewW = 800.0f;
        event.viewH = 600.0f;
        event.ray.origin = ground + Vector3(0.0f, 10.0f, 0.0f);
        event.ray.direction = Vector3(0.0f, -1.0f, 0.0f);
        event.cameraPos = event.ray.origin;
        event.cameraForward = Vector3(0.0f, -1.0f, 0.0f);
        event.cameraUp = Vector3(0.0f, 0.0f, 1.0f);
        event.cameraRight = Vector3(1.0f, 0.0f, 0.0f);
        event.tanHalfFovY = 0.5f;
        return event;
    }

    ECS::World* m_World = nullptr;
};

using Components::SplineNetworkMemberKind;

// The record of the first junction that the paving on `area` reads, or null.
const SplineRecipes::PavingJunctionStatus* FirstJunctionOf(const SplineRecipes::PavingWorld& pavings,
                                                           ECS::EntityHandle area)
{
    for (const SplineRecipes::PavingCandidate& candidate : pavings.Candidates)
    {
        if (candidate.Entity == area && !candidate.Junctions.empty())
            return &candidate.Junctions[0];
    }
    return nullptr;
}

// A marker's hover text names the kind, the pair and the state in a user's words.
TEST_F(SplineJunctionMarkerTests, AMarkersTooltipNamesTheKindThePairAndTheState)
{
    AddMember("High Street", {{-40.0f, 0.0f, 10.0f}, {60.0f, 0.0f, 10.0f}}, false, SplineNetworkMemberKind::Road);
    AddMember("Ford Road", {{-20.0f, 0.0f, -40.0f}, {-20.0f, 0.0f, 60.0f}}, false, SplineNetworkMemberKind::Road);
    AddMember("Smithy Lane", {{-60.0f, 0.0f, -20.0f}, {-21.0f, 0.0f, -20.0f}}, false, SplineNetworkMemberKind::Road);
    AddMember("Market Square", {{0.0f, 0.0f, 30.0f}, {20.0f, 0.0f, 30.0f}, {20.0f, 0.0f, 50.0f}, {0.0f, 0.0f, 50.0f}},
              true, SplineNetworkMemberKind::Area);
    AddMember("Mill Lane", {{10.0f, -0.41f, 80.0f}, {10.0f, -0.41f, 50.0f}}, false, SplineNetworkMemberKind::Road);
    SplineNetwork::Get().Update(*m_World);

    const SplineNode* crossing = NodeNear(-20.0f, 10.0f);
    ASSERT_NE(crossing, nullptr);
    const std::string crossingText = SplineJunctionMarkers::Tooltip(*m_World, *crossing);
    EXPECT_TRUE(crossingText == "Crossing · High Street and Ford Road · joined" ||
                crossingText == "Crossing · Ford Road and High Street · joined")
        << crossingText;

    const SplineNode* tJunction = NodeNear(-20.0f, -20.0f);
    ASSERT_NE(tJunction, nullptr);
    EXPECT_EQ(SplineJunctionMarkers::Tooltip(*m_World, *tJunction), "T junction · Smithy Lane on Ford Road · joined");

    const SplineNode* entry = NodeNear(10.0f, 50.0f);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(SplineJunctionMarkers::Tooltip(*m_World, *entry), "Entry · Mill Lane into Market Square · 0.41 m below");
}

// The ring and the hover text agree with the paving: a road that enters an area that grades its ground
// is paved at any height; an area that leaves the ground alone keeps the road apart.
TEST_F(SplineJunctionMarkerTests, AnEntryIntoAnAreaThatGradesItsGroundReadsPaved)
{
    const ECS::EntityHandle owns = AddMember(
        "Market Square", {{0.0f, 0.0f, 30.0f}, {20.0f, 0.0f, 30.0f}, {20.0f, 0.0f, 50.0f}, {0.0f, 0.0f, 50.0f}}, true,
        SplineNetworkMemberKind::Area);
    const ECS::EntityHandle leaves = AddMember(
        "Kiln Yard", {{40.0f, 0.0f, 30.0f}, {60.0f, 0.0f, 30.0f}, {60.0f, 0.0f, 50.0f}, {40.0f, 0.0f, 50.0f}}, true,
        SplineNetworkMemberKind::Area);
    Components::SplinePaving paving{};
    paving.GroundAuthority = Components::SplineGroundAuthority::Owns;
    m_World->AddComponentImmediate(owns, paving);
    paving.GroundAuthority = Components::SplineGroundAuthority::None;
    m_World->AddComponentImmediate(leaves, paving);
    AddMember("Mill Lane", {{10.0f, 0.5f, 80.0f}, {10.0f, 0.5f, 50.0f}}, false, SplineNetworkMemberKind::Road);
    AddMember("Kiln Lane", {{50.0f, 0.5f, 80.0f}, {50.0f, 0.5f, 50.0f}}, false, SplineNetworkMemberKind::Road);
    SplineNetwork::Get().Update(*m_World);
    const SplineRecipes::PavingWorld pavings =
        SplineRecipes::GatherPavings(*m_World, SplineRecipes::PavingGatherDepth::Full);

    const SplineNode* graded = NodeNear(10.0f, 50.0f);
    const SplineRecipes::PavingJunctionStatus* gradedJunction = FirstJunctionOf(pavings, owns);
    ASSERT_NE(graded, nullptr);
    ASSERT_NE(gradedJunction, nullptr);
    EXPECT_TRUE(graded->IsSplitLevel());
    EXPECT_FALSE(SplineRecipes::JunctionHeightsDiffer(*graded, gradedJunction));
    EXPECT_EQ(PavingJunctionStateWords(*m_World, *gradedJunction), "paved");

    const SplineNode* apart = NodeNear(50.0f, 50.0f);
    const SplineRecipes::PavingJunctionStatus* apartJunction = FirstJunctionOf(pavings, leaves);
    ASSERT_NE(apart, nullptr);
    ASSERT_NE(apartJunction, nullptr);
    EXPECT_TRUE(SplineRecipes::JunctionHeightsDiffer(*apart, apartJunction));
    EXPECT_EQ(PavingJunctionStateWords(*m_World, *apartJunction), "0.5 m above the paving");
}

// The pointer over a marker shows its text with no spline tool active, and over an end knot that
// sits on the node; away from every marker, none.
TEST_F(SplineJunctionMarkerTests, ThePointerOverAMarkerShowsItsTooltip)
{
    AddMember("High Street", {{-40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}}, false, SplineNetworkMemberKind::Road);
    AddMember("Ford Road", {{0.0f, 0.0f, -40.0f}, {0.0f, 0.0f, 40.0f}}, false, SplineNetworkMemberKind::Road);
    AddMember("Market Square", {{10.0f, 0.0f, 10.0f}, {30.0f, 0.0f, 10.0f}, {30.0f, 0.0f, 30.0f}, {10.0f, 0.0f, 30.0f}},
              true, SplineNetworkMemberKind::Area);
    const ECS::EntityHandle road =
        AddMember("Church Walk", {{20.0f, 0.0f, 50.0f}, {20.0f, 0.0f, 30.0f}}, false, SplineNetworkMemberKind::Road);
    SplineNetwork::Get().Update(*m_World);
    const SplineNode* crossing = NodeNear(0.0f, 0.0f);
    const SplineNode* entry = NodeNear(20.0f, 30.0f);
    ASSERT_NE(crossing, nullptr);
    ASSERT_NE(entry, nullptr);

    // The overlay that draws the markers with every tool; no spline tool exists here.
    SplineSceneGizmo gizmo(*m_World);
    gizmo.UpdateMarkerHover(HoverAbove({0.1f, 0.0f, 0.1f}).ray);
    EXPECT_EQ(gizmo.GetMarkerHoverText(), SplineJunctionMarkers::Tooltip(*m_World, *crossing));
    gizmo.UpdateMarkerHover(HoverAbove({20.0f, 0.0f, -20.0f}).ray);
    EXPECT_TRUE(gizmo.GetMarkerHoverText().empty());

    // The spline tool hovers the road's end knot on the entry; the marker's text still shows.
    SplineTool tool(*m_World);
    tool.SetSelectionListQuery([road] { return std::vector<ECS::EntityHandle>{road}; });
    const ScenePointerEvent overEnd = HoverAbove(entry->Position);
    tool.OnPointerEvent(overEnd);
    EXPECT_EQ(Editor::SceneTools::AcquireSplineInteractionState(*m_World)->Hover().Kind, 1);
    gizmo.UpdateMarkerHover(overEnd.ray);
    EXPECT_EQ(gizmo.GetMarkerHoverText(), SplineJunctionMarkers::Tooltip(*m_World, *entry));
    EXPECT_TRUE(tool.GetHoverText().empty());
}

// A marker under a still pointer keeps its hover text: the text is built again only when the
// pointer moves to another marker or the junction's state or names change. Each build is one call
// of the profiler scope "SplineJunctionMarkers.HoverText".
TEST_F(SplineJunctionMarkerTests, AHoveredMarkerBuildsItsTextOnlyWhenItChanges)
{
    const ECS::EntityHandle high =
        AddMember("High Street", {{-40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}}, false, SplineNetworkMemberKind::Road);
    const ECS::EntityHandle ford =
        AddMember("Ford Road", {{0.0f, 0.0f, -40.0f}, {0.0f, 0.0f, 40.0f}}, false, SplineNetworkMemberKind::Road);
    AddMember("Market Square", {{10.0f, 0.0f, 10.0f}, {30.0f, 0.0f, 10.0f}, {30.0f, 0.0f, 30.0f}, {10.0f, 0.0f, 30.0f}},
              true, SplineNetworkMemberKind::Area);
    AddMember("Church Walk", {{20.0f, 0.0f, 50.0f}, {20.0f, 0.0f, 30.0f}}, false, SplineNetworkMemberKind::Road);
    SplineNetwork::Get().Update(*m_World);
    ASSERT_NE(NodeNear(0.0f, 0.0f), nullptr);
    const SplineNode* entry = NodeNear(20.0f, 30.0f);
    ASSERT_NE(entry, nullptr);
    const Vector3 entryPosition = entry->Position;

    SplineSceneGizmo gizmo(*m_World);
    Profiling::CpuProfiler& profiler = Profiling::CpuProfiler::Get();
    profiler.SetEnabled(true);
    profiler.BeginFrame();
    // Ten frames under a still pointer on the crossing, then a rename of one of its roads.
    for (int frame = 0; frame < 10; ++frame)
        gizmo.UpdateMarkerHover(HoverAbove({0.1f, 0.0f, 0.1f}).ray);
    Components::Name renamed{};
    std::strncpy(renamed.value, "Ford Lane", sizeof(renamed.value) - 1);
    ECS::Entity(m_World, ford).Set(renamed);
    m_World->ProcessCommands();
    for (int frame = 0; frame < 10; ++frame)
        gizmo.UpdateMarkerHover(HoverAbove({0.1f, 0.0f, 0.1f}).ray);
    const std::string crossingText = gizmo.GetMarkerHoverText();
    // Then a rule that keeps the two roads apart: the state words change.
    Components::SplineNetworkOverrides overrides{};
    overrides.Records[0] = {ford, 40.0f, 40.0f, Components::SplineNodeRule::SplitLevel, {}};
    m_World->AddComponentImmediate(high, overrides);
    SplineNetwork::Get().Update(*m_World);
    for (int frame = 0; frame < 10; ++frame)
        gizmo.UpdateMarkerHover(HoverAbove({0.1f, 0.0f, 0.1f}).ray);
    const std::string keptApartText = gizmo.GetMarkerHoverText();
    // Then the entry, and away from every marker.
    for (int frame = 0; frame < 10; ++frame)
        gizmo.UpdateMarkerHover(HoverAbove(entryPosition).ray);
    gizmo.UpdateMarkerHover(HoverAbove({20.0f, 0.0f, -20.0f}).ray);
    profiler.BeginFrame();
    std::vector<Profiling::CpuProfiler::TreeRow> rows;
    profiler.CopyFrameTree(rows);
    profiler.SetEnabled(false);

    uint32 builds = 0;
    for (const Profiling::CpuProfiler::TreeRow& row : rows)
    {
        if (row.name == "SplineJunctionMarkers.HoverText")
            builds += row.callCount;
    }
    EXPECT_EQ(builds, 4u);
    EXPECT_NE(crossingText.find("Ford Lane"), std::string::npos);
    EXPECT_NE(keptApartText.find("not joined"), std::string::npos);
    EXPECT_TRUE(gizmo.GetMarkerHoverText().empty());
}

// While a dragged end is snapped, the ring marks where it lands and the pill says where it attaches.
TEST_F(SplineJunctionMarkerTests, ASnappedDragShowsTheRingAndSaysWhereTheEndAttaches)
{
    AddMember("Market Square", {{0.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 20.0f}, {0.0f, 0.0f, 20.0f}},
              true, SplineNetworkMemberKind::Area);
    const ECS::EntityHandle road =
        AddMember("Church Walk", {{10.0f, 0.0f, 40.0f}, {10.0f, 0.0f, 25.0f}}, false, SplineNetworkMemberKind::Road);
    SplineNetwork::Get().Update(*m_World);

    SplineTool tool(*m_World);
    tool.SetSelectionListQuery([road] { return std::vector<ECS::EntityHandle>{road}; });
    ScenePointerEvent down = HoverAbove({10.0f, 0.0f, 25.0f});
    down.phase = PointerPhase::Down;
    down.button = PointerButton::Left;
    down.viewX = 100.0f;
    tool.OnPointerEvent(down);
    ScenePointerEvent move = HoverAbove({10.0f, 0.0f, 20.3f});
    move.button = PointerButton::Left;
    move.viewX = 140.0f;
    tool.OnPointerEvent(move);

    EXPECT_EQ(tool.GetHoverText(), "Snap to Market Square");
    const auto& preview = Editor::SceneTools::AcquireSplineInteractionState(*m_World)->SnapPreview();
    EXPECT_TRUE(preview.Target.IsValid());
    EXPECT_NEAR(preview.Point.z, 20.0f, 1.0e-3f);

    ScenePointerEvent up = move;
    up.phase = PointerPhase::Up;
    tool.OnPointerEvent(up);
    EXPECT_TRUE(tool.GetHoverText().empty());
    EXPECT_FALSE(Editor::SceneTools::AcquireSplineInteractionState(*m_World)->SnapPreview().Target.IsValid());
}

} // namespace
