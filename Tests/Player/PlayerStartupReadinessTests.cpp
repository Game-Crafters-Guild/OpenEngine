// When the Player's startup scene is ready for its startup line and its --screenshot capture,
// and the game config field that decides whether it waits for the spline recipes.

#include "PlayerStartupReadiness.h"

#include "Engine/Build/GameConfig.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <fstream>

namespace GameEngine
{
namespace
{

constexpr float64 kFrameSeconds = 1.0 / 60.0;

TEST(PlayerStartupReadiness, TheSceneIsReadyWhenTheBatchIsCompleteAndTheRecipesAreSettled)
{
    PlayerStartupReadiness readiness;
    readiness.Begin(/*waitForSplineRecipes=*/true);

    EXPECT_FALSE(readiness.Update(/*batchComplete=*/false, /*recipesSettled=*/true, kFrameSeconds));
    EXPECT_FALSE(readiness.Update(true, false, kFrameSeconds));
    EXPECT_TRUE(readiness.IsPending());
    EXPECT_TRUE(readiness.Update(true, true, kFrameSeconds));

    EXPECT_FALSE(readiness.IsPending());
    EXPECT_EQ(readiness.Frames(), 3u);
    EXPECT_FALSE(readiness.Update(true, true, kFrameSeconds)) << "the scene becomes ready once";
}

TEST(PlayerStartupReadiness, WithoutTheRecipeWaitTheBatchAloneMakesTheSceneReady)
{
    PlayerStartupReadiness readiness;
    readiness.Begin(/*waitForSplineRecipes=*/false);

    EXPECT_TRUE(readiness.Update(/*batchComplete=*/true, /*recipesSettled=*/false, kFrameSeconds));
}

// The wait has no clock of its own: a recipe Held past any time keeps the scene pending, and
// the recipe's own hold budget ends the wait.
TEST(PlayerStartupReadiness, TheRecipeWaitEndsOnlyWhenTheRecipesSettle)
{
    constexpr float64 kLongerThanAnyHoldBudget = 3600.0;
    PlayerStartupReadiness readiness;
    readiness.Begin(/*waitForSplineRecipes=*/true);

    EXPECT_FALSE(readiness.Update(true, false, kLongerThanAnyHoldBudget));
    EXPECT_TRUE(readiness.IsPending());
    EXPECT_TRUE(readiness.Update(true, true, kFrameSeconds));
    EXPECT_FALSE(readiness.IsPending());
}

TEST(PlayerStartupReadiness, TheGameConfigWaitsForTheRecipesUnlessItSaysFalse)
{
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("PlayerStartupReadiness"));
    const std::filesystem::path configPath = root.Path() / "game.config";
    {
        std::ofstream file(configPath);
        file << R"({"startupScene": "Scenes/Main.scene"})";
    }
    EXPECT_TRUE(LoadGameConfig(configPath).waitForSplineRecipes) << "a config without the field waits";

    GameConfig cooked;
    cooked.waitForSplineRecipes = false;
    ASSERT_TRUE(SaveGameConfig(configPath, cooked));
    EXPECT_FALSE(LoadGameConfig(configPath).waitForSplineRecipes);
}

} // namespace
} // namespace GameEngine
