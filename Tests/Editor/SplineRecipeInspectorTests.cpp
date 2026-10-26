// The spline recipe inspectors: a notice with the recipe's build state and the reflected rows.

#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplineFence.h"
#include "Components/Spline/SplinePlacement.h"
#include "Components/Spline/SplineWall.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "InspectorLayoutFixture.h"
#include "InspectorRegistry.h"
#include "Inspectors/SplineFenceInspector.h"
#include "Inspectors/SplineInspector.h"
#include "Inspectors/SplinePlacementInspector.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include <gtest/gtest.h>

namespace GameEngine
{
namespace
{

using InspectorLayoutTesting::FindByClass;

constexpr const char* kPendingText = "Not built yet / It builds once its inputs stop changing.";

UIElement* FindById(UIElement& element, const std::string& id)
{
    if (element.GetId() == id)
        return &element;
    for (const auto& child : element.GetChildren())
        if (UIElement* found = FindById(*child, id))
            return found;
    return nullptr;
}

class SplineRecipeInspectorTest : public ::testing::Test
{
protected:
    // The recipe rows include asset pickers, which read the engine's asset registry.
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

    // The registration order of the editor: the fence and placement inspectors replace any
    // inspector registered before them for their components.
    template <typename RecipeT>
    void Build()
    {
        RegisterSplineRecipeInspectors();
        RegisterSplinePlacementInspector();
        RegisterSplineFenceInspector();
        entity = world.Create().GetHandle();
        world.AddComponentImmediate(entity, Components::SplineComponent{});
        world.AddComponentImmediate(entity, RecipeT{});
        world.AddComponentImmediate(entity, Components::WorldTransform{});
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<RecipeT>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext context;
        context.Parent = &parent;
        context.World = &world;
        context.Entity = entity;
        (*inspector)(context);
    }

    // The title and text of the build state notice, or "(none)" when the inspector shows none.
    std::string StateText()
    {
        UIElement* notice = FindById(parent, "spline-recipe-build-state");
        const auto* title = notice ? dynamic_cast<const Label*>(FindByClass(notice, "inspector-notice-title")) : nullptr;
        const auto* text = notice ? dynamic_cast<const Label*>(FindByClass(notice, "inspector-notice-text")) : nullptr;
        return title && text ? title->GetText() + " / " + text->GetText() : std::string("(none)");
    }

    ECS::World world;
    ECS::EntityHandle entity;
    UIElement parent;
};

} // namespace

// With no recipe system running, the recipe has not built: the notice says so above the rows.
TEST_F(SplineRecipeInspectorTest, TheExtrudeShowsItsBuildStateAboveItsRows)
{
    Build<Components::SplineExtrude>();
    ASSERT_GT(parent.GetChildren().size(), 1u) << "the reflected rows come after the notice";
    EXPECT_EQ(parent.GetChildren().front()->GetId(), "spline-recipe-build-state");
    EXPECT_EQ(StateText(), kPendingText);
}

TEST_F(SplineRecipeInspectorTest, EveryRecipeKindShowsItsBuildState)
{
    Build<Components::SplineWall>();
    EXPECT_EQ(StateText(), kPendingText);
    parent.RemoveAllChildren();
    Build<Components::SplineFence>();
    EXPECT_EQ(StateText(), kPendingText);
    parent.RemoveAllChildren();
    Build<Components::SplinePlacement>();
    EXPECT_EQ(StateText(), kPendingText);
}

} // namespace GameEngine
