// The NavigationAgent inspector: the Surface row and the status of the agent's request with its
// reason, which follows the agent while the simulation runs and stands for the hidden runtime state.
#include <gtest/gtest.h>

#include "Editor/Entities/EditorComponentTraits.h"
#include "InspectorRegistry.h"
#include "Inspectors/NavigationAgentInspector.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace GameEngine;
using Components::NavigationAgent;
using Components::NavigationAgentState;

namespace
{

void CollectLabels(const UIElement& element, std::vector<std::string>& out)
{
    if (const auto* label = dynamic_cast<const Label*>(&element))
        out.push_back(label->GetText());
    for (const auto& child : element.GetChildren())
        CollectLabels(*child, out);
}

const EditorUI::InspectorNotice* FindNotice(const UIElement& element)
{
    if (const auto* notice = dynamic_cast<const EditorUI::InspectorNotice*>(&element))
        return notice;
    for (const auto& child : element.GetChildren())
        if (const auto* found = FindNotice(*child))
            return found;
    return nullptr;
}

bool Contains(const std::vector<std::string>& texts, const std::string& text)
{
    return std::find(texts.begin(), texts.end(), text) != texts.end();
}

class NavigationAgentInspectorTest : public ::testing::Test
{
protected:
    // The Surface row is an entity picker, which reads the engine.
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

    void Build(const NavigationAgentState& state)
    {
        RegisterNavigationAgentInspector();
        entity = world.Create(NavigationAgent{}).GetHandle();
        world.ProcessCommands();
        *world.GetComponentForWrite<NavigationAgentState>(entity) = state;
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<NavigationAgent>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext ctx;
        ctx.Parent = &parent;
        ctx.World = &world;
        ctx.Entity = entity;
        ctx.SimulationRefreshCallbacks = &refreshes;
        (*inspector)(ctx);
    }

    std::vector<std::string> Texts()
    {
        std::vector<std::string> texts;
        CollectLabels(parent, texts);
        return texts;
    }

    ECS::World world;
    ECS::EntityHandle entity;
    UIElement parent;
    std::vector<std::function<void()>> refreshes;
};

} // namespace

TEST_F(NavigationAgentInspectorTest, ShowsTheSurfaceRowAndTheReasonOfAFailedOrder)
{
    NavigationAgentState state{};
    state.Status = Pathfinding::PathStatus::Failed;
    state.Reason = Navigation::StatusReason::NoRoute;
    Build(state);

    const std::vector<std::string> texts = Texts();
    EXPECT_TRUE(Contains(texts, "Surface"));
    EXPECT_TRUE(Contains(texts, "The only surface in the scene"));
    EXPECT_TRUE(Contains(texts, "Failed: no walkable route to the destination"));
    const auto* notice = FindNotice(parent);
    ASSERT_NE(notice, nullptr);
    EXPECT_TRUE(notice->HasClass("inspector-notice-error"));

    // The status line stands for the runtime state, which has no section of its own.
    Editor::EditorComponentTraits traits;
    ASSERT_TRUE(Editor::EditorComponentTraitsRegistry::Get().TryGet(ECS::GetComponentTypeId<NavigationAgentState>(), traits));
    EXPECT_TRUE(traits.HideInInspector);
}

TEST_F(NavigationAgentInspectorTest, TheStatusFollowsTheOrderWhileTheSimulationRuns)
{
    NavigationAgentState state{};
    state.HasDestination = true;
    state.Status = Pathfinding::PathStatus::Pending;
    state.Reason = Navigation::StatusReason::SurfaceNotBaked;
    state.PendingTiles = 212;
    Build(state);
    EXPECT_TRUE(Contains(Texts(), "Waiting: the surface is baking, 212 tiles left"));

    world.GetComponentForWrite<NavigationAgentState>(entity)->PendingTiles = 12;
    ASSERT_FALSE(refreshes.empty());
    for (auto& refresh : refreshes)
        refresh();
    EXPECT_TRUE(Contains(Texts(), "Waiting: the surface is baking, 12 tiles left"));
}

TEST_F(NavigationAgentInspectorTest, AWalkingAgentAndAnArrivedOneReadDifferently)
{
    NavigationAgentState state{};
    state.HasDestination = true;
    state.Status = Pathfinding::PathStatus::Complete;
    Build(state);
    EXPECT_TRUE(Contains(Texts(), "Walking"));

    world.GetComponentForWrite<NavigationAgentState>(entity)->HasDestination = false;
    ASSERT_FALSE(refreshes.empty());
    for (auto& refresh : refreshes)
        refresh();
    EXPECT_TRUE(Contains(Texts(), "Arrived"));
}
