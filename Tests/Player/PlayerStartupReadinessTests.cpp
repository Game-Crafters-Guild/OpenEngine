// The Player's startup load, and when its startup scene is ready for its startup line and its --screenshot
// capture: after the reveal, when the spline recipes are settled.

#include "PlayerStartupReadiness.h"

#include "Engine/Loading/SceneLoadRequest.h"

#include <gtest/gtest.h>

namespace GameEngine
{
namespace
{

constexpr float64 kFrameSeconds = 1.0 / 60.0;

TEST(PlayerStartupReadiness, TheSceneIsReadyWhenItIsRevealedAndTheRecipesAreSettled)
{
    PlayerStartupReadiness readiness;
    readiness.Begin();

    EXPECT_FALSE(readiness.Update(/*revealed=*/false, /*recipesSettled=*/true, kFrameSeconds));
    EXPECT_FALSE(readiness.Update(true, false, kFrameSeconds));
    EXPECT_TRUE(readiness.IsPending());
    EXPECT_TRUE(readiness.Update(true, true, kFrameSeconds));

    EXPECT_FALSE(readiness.IsPending());
    EXPECT_EQ(readiness.Frames(), 3u);
    EXPECT_FALSE(readiness.Update(true, true, kFrameSeconds)) << "the scene becomes ready once";
}

// The wait has no clock of its own: a recipe Held past any time keeps the scene pending, and
// the recipe's own hold budget ends the wait.
TEST(PlayerStartupReadiness, TheRecipeWaitEndsOnlyWhenTheRecipesSettle)
{
    constexpr float64 kLongerThanAnyHoldBudget = 3600.0;
    PlayerStartupReadiness readiness;
    readiness.Begin();

    EXPECT_FALSE(readiness.Update(true, false, kLongerThanAnyHoldBudget));
    EXPECT_TRUE(readiness.IsPending());
    EXPECT_TRUE(readiness.Update(true, true, kFrameSeconds));
    EXPECT_FALSE(readiness.IsPending());
}

// The world first draws when the startup scene is ready to show: the Player reveals the load at Ready,
// and SceneLoaderTests prove what PauseWorldDrawing does to the cameras.
TEST(PlayerStartupReadiness, TheStartupLoadPausesWorldDrawingUntilTheSceneIsReadyToShow)
{
    const SceneLoadRequest request = MakeStartupSceneLoadRequest("Scenes/Main.scene", [](SceneLoadInstantiated&) {});

    EXPECT_TRUE(request.PauseWorldDrawing);
    EXPECT_EQ(request.Reveal, SceneRevealPolicy::AtReady) << "nothing in the Player releases a held load";
    EXPECT_EQ(request.Mode, SceneLoadMode::Single);
    EXPECT_EQ(request.Scene, std::filesystem::path("Scenes/Main.scene"));
    EXPECT_TRUE(request.OnInstantiated);
}

} // namespace
} // namespace GameEngine
