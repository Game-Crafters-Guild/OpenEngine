// The NavigationSurface inspector: its rows per mode, the bake status notice and its Bake action,
// the header badge, and the surface's Scene View overlay the inspector registers.
#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "InspectorRegistry.h"
#include "Inspectors/NavigationSurfaceInspector.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Components/Navigation/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"
#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "Pathfinding/DetourNavMap.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "SceneView/ComponentGizmoRegistry.h"
#include "SceneView/SceneViewGizmos.h"
#include "Components/Transform.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/InspectorSection.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;
using Components::NavigationSurface;
using InspectorLayoutTesting::FindByClass;

namespace
{

void CollectLabels(const UIElement& element, std::vector<std::string>& out)
{
    if (const auto* label = dynamic_cast<const Label*>(&element); label && label->HasClass("inspector-label"))
        out.push_back(label->GetText());
    for (const auto& child : element.GetChildren())
        CollectLabels(*child, out);
}

// The texts of the captions under rows (the Cell Size caption among them).
void CollectCaptions(const UIElement& element, std::vector<std::string>& out)
{
    if (const auto* label = dynamic_cast<const Label*>(&element); label && label->HasClass("inspector-text"))
        out.push_back(label->GetText());
    for (const auto& child : element.GetChildren())
        CollectCaptions(*child, out);
}

EditorUI::InspectorNotice* FindNotice(UIElement& element)
{
    if (auto* notice = dynamic_cast<EditorUI::InspectorNotice*>(&element))
        return notice;
    for (const auto& child : element.GetChildren())
        if (auto* found = FindNotice(*child))
            return found;
    return nullptr;
}

// The inspector row holding the label `text`, or null.
const UIElement* RowOfLabel(const UIElement& element, const std::string& text)
{
    if (const auto* label = dynamic_cast<const Label*>(&element); label && label->GetText() == text)
    {
        for (const UIElement* up = label->GetParent(); up; up = up->GetParent())
            if (up->HasClass("inspector-row"))
                return up;
        return nullptr;
    }
    for (const auto& child : element.GetChildren())
        if (const auto* row = RowOfLabel(*child, text))
            return row;
    return nullptr;
}

// True when every element of `row` is disabled: its look follows each element's own state.
bool WholeRowDisabled(const UIElement& row)
{
    if (row.IsEnabled())
        return false;
    for (const auto& child : row.GetChildren())
        if (!WholeRowDisabled(*child))
            return false;
    return true;
}

bool Contains(const std::vector<std::string>& labels, const std::string& text)
{
    return std::find(labels.begin(), labels.end(), text) != labels.end();
}

class NavigationSurfaceInspectorTest : public ::testing::Test
{
protected:
    // The Authored Cells row is an asset picker, which reads the engine's asset registry.
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void Build(const NavigationSurface& surface)
    {
        RegisterNavigationSurfaceInspector();
        entity = world.Create(surface).GetHandle();
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<NavigationSurface>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext ctx;
        ctx.Parent = &parent;
        ctx.World = &world;
        ctx.Entity = entity;
        ctx.Section = &section;
        (*inspector)(ctx);
        CollectLabels(parent, labels);
    }

    // The header badge's classes, empty when the section shows none.
    const UIElement* Badge() { return FindByClass(&section, "inspector-section-header-badge"); }

    ECS::World world;
    ECS::EntityHandle entity;
    InspectorSection section{"Navigation Surface"};
    UIElement parent;
    std::vector<std::string> labels;
};

} // namespace

TEST_F(NavigationSurfaceInspectorTest, MeshSurfaceShowsItsRowsAndTheBakeStatus)
{
    Build(NavigationSurface{});

    // Each quantity's row names its unit.
    for (const char* row : {"Mode", "Agent Radius (m)", "Agent Height (m)", "Max Slope (\xC2\xB0)", "Step Height (m)",
                            "Cell Size (m)", "Fit Bounds", "Center", "Extent"})
        EXPECT_TRUE(Contains(labels, row)) << "missing row " << row;
    EXPECT_FALSE(Contains(labels, "Center.X")) << "the bounds are one Vector3 row each, not three scalar rows";
    EXPECT_FALSE(Contains(labels, "Grid Type")) << "a mesh surface has no grid rows";
    EXPECT_FALSE(Contains(labels, "Authored Cells"));
    // Fit Bounds is on by default, so the explicit bounds read as inactive, values included, and say why.
    const UIElement* centerRow = RowOfLabel(parent, "Center");
    ASSERT_NE(centerRow, nullptr);
    EXPECT_TRUE(WholeRowDisabled(*centerRow));
    EXPECT_NE(centerRow->GetTooltip().find("Fit Bounds is on"), std::string::npos);
    const UIElement* radiusRow = RowOfLabel(parent, "Agent Radius (m)");
    ASSERT_NE(radiusRow, nullptr);
    EXPECT_TRUE(radiusRow->IsEnabled());

    auto* notice = FindNotice(parent);
    ASSERT_NE(notice, nullptr);
    const auto* text = dynamic_cast<const Label*>(FindByClass(notice, "inspector-notice-text"));
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->GetText(), "Not baked");
}

TEST_F(NavigationSurfaceInspectorTest, GridSurfaceShowsItsGridRows)
{
    NavigationSurface surface{};
    surface.Mode = Components::NavigationSurfaceMode::Grid;
    Build(surface);

    EXPECT_TRUE(Contains(labels, "Grid Type"));
    EXPECT_TRUE(Contains(labels, "Authored Cells"));
}

TEST_F(NavigationSurfaceInspectorTest, BakeActionRequestsABake)
{
    Build(NavigationSurface{});
    auto* notice = FindNotice(parent);
    ASSERT_NE(notice, nullptr);
    auto* bake = dynamic_cast<Button*>(FindByClass(notice, "inspector-notice-action"));
    ASSERT_NE(bake, nullptr);
    EXPECT_EQ(bake->GetText(), "Bake");
    ASSERT_FALSE(world.GetComponent<NavigationSurface>(entity)->BakeRequested);

    UIEvent click{};
    click.Id = kEventButtonClick;
    click.Target = bake;
    click.CurrentTarget = bake;
    bake->DispatchEvent(click);

    EXPECT_TRUE(world.GetComponent<NavigationSurface>(entity)->BakeRequested);
}

// The Cell Size caption counts what a bake of the shown mode over the current bounds makes, before
// any bake: after a switch to Grid it counts cells, never the mesh bake's tiles.
TEST_F(NavigationSurfaceInspectorTest, CellSizeCaptionCountsTheShownModeWithoutABake)
{
    NavigationSurface surface{};
    surface.Mode = Components::NavigationSurfaceMode::Grid;
    surface.CellSize = 1.0f;
    surface.FitBounds = false;
    surface.Extent = {50.0f, 1.0f, 50.0f};
    surface.BakePhase = Components::NavigationBakePhase::Baked;
    surface.BakedMode = Components::NavigationSurfaceMode::Mesh;
    surface.BakeTotal = 1024;
    Build(surface);

    std::vector<std::string> captions;
    CollectCaptions(parent, captions);
    EXPECT_TRUE(Contains(captions, "10,000 cells"));
    for (const std::string& caption : captions)
        EXPECT_EQ(caption.find("1,024"), std::string::npos) << caption;
}

// The badge reads the bake state's weight: green when current, amber when it needs attention, red
// when it failed, with the failure's notice in the error style.
TEST_F(NavigationSurfaceInspectorTest, HeaderBadgeColorFollowsTheBakeState)
{
    NavigationSurface surface{};
    surface.BakePhase = Components::NavigationBakePhase::Baked;
    Build(surface);
    const UIElement* badge = Badge();
    ASSERT_NE(badge, nullptr);
    EXPECT_TRUE(badge->HasClass("inspector-section-header-badge-success"));

    surface.BakePhase = Components::NavigationBakePhase::Failed;
    world.DestroyEntity(entity);
    world.ProcessCommands();
    Build(surface);
    badge = Badge();
    ASSERT_NE(badge, nullptr);
    EXPECT_TRUE(badge->HasClass("inspector-section-header-badge-error"));
    EXPECT_FALSE(badge->HasClass("inspector-section-header-badge-success"));
    // The failure's notice reads in the error style under the red badge.
    const auto* notice = dynamic_cast<const EditorUI::InspectorNotice*>(parent.GetChildren().back().get());
    ASSERT_NE(notice, nullptr);
    EXPECT_TRUE(notice->HasClass("inspector-notice-error"));
}

// A selected surface's overlay is a faint walkable tint with its edge outlined opaquely, so an
// exclusion reads as an outlined hole: one outline segment per boundary edge of the baked mesh.
TEST_F(NavigationSurfaceInspectorTest, OverlayOutlinesTheWalkableSurfaceEdge)
{
    if (!PathfindingECS::NavigationService::IsInitialized())
        PathfindingECS::NavigationService::Initialize();
    PathfindingECS::RegisterNavigationWorldHooks(world);
    auto ground = world.Create();
    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Static;
    ground.Set(body);
    ground.Set(Components::PhysicsCollider{});
    Components::BoxColliderShape box{};
    box.halfExtentsX = box.halfExtentsZ = 10.0f;
    box.halfExtentsY = 0.5f;
    ground.Set(box);
    ground.Set(Components::WorldTransform{});
    world.ProcessCommands();
    NavigationSurface surface{};
    surface.FitBounds = false;
    surface.Extent = {8.0f, 5.0f, 8.0f};
    Build(surface);

    PathfindingECS::NavigationSurfaceSystem system;
    // The tiles build on the job system: update until they land, at most about four seconds.
    for (int i = 0; i < 400; ++i)
    {
        const auto phase = world.GetComponent<NavigationSurface>(entity)->BakePhase;
        if (phase == Components::NavigationBakePhase::Baked || phase == Components::NavigationBakePhase::Failed)
            break;
        system.Update(world, 1.0f / 60.0f);
        world.ProcessCommands();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto* baked = world.GetComponent<NavigationSurface>(entity);
    ASSERT_EQ(baked->BakePhase, Components::NavigationBakePhase::Baked) << Navigation::FormatBakeStatus(*baked);
    const auto* mesh = PathfindingECS::ResolveNavigationSurfaceMesh(world, *baked);
    ASSERT_NE(mesh, nullptr);
    ASSERT_GT(mesh->GetDebugBoundaryEdgeCount(), 0u);

    using namespace Editor::SceneTools;
    constexpr Rendering::ViewId kView = 31;
    ResetGizmoLineGroups(kView);
    ResetGizmoTriangleGroups(kView);
    // Looking down on the surface, so no outline segment is seen end-on.
    const Mathematics::Vector3 camera(0.0f, 40.0f, -1.0f);
    GizmoRenderContext context(kView, 31, &camera);
    ComponentGizmoRegistry::Get().Draw(context, world, entity);

    // The outline is one screen-facing quad (six vertices) per boundary edge, opaque; the fill is the
    // rest, faint.
    const auto* triangles = GetGizmoTriangleGroups(kView);
    ASSERT_NE(triangles, nullptr);
    std::size_t outlineVertices = 0;
    std::size_t fillGroups = 0;
    for (const auto& group : *triangles)
    {
        if (group.color.a >= 0.999f)
            outlineVertices += group.vertices.size() / 3;
        else
        {
            ++fillGroups;
            EXPECT_LT(group.color.a, 0.3f) << "the walkable tint stays faint so the ground reads through it";
        }
    }
    EXPECT_EQ(outlineVertices, 6u * mesh->GetDebugBoundaryEdgeCount());
    EXPECT_EQ(fillGroups, 1u);
}
