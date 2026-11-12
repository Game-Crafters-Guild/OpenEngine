#include "SplineRecipes/Paving/SplinePavingSanitize.h"

#include <gtest/gtest.h>

#include <limits>

namespace GameEngine::SplineRecipes
{
namespace
{

using Components::SplinePaving;

TEST(SplinePavingRecipeTests, ANonFiniteBorderFallsBackSoTheRecipeEqualsItself)
{
    SplinePaving authored;
    authored.BorderWidth = std::numeric_limits<float32>::quiet_NaN();
    authored.BorderHeight = std::numeric_limits<float32>::infinity();

    const SplinePaving clean = SanitizePavingRecipe(authored);

    EXPECT_FLOAT_EQ(clean.BorderWidth, SplinePaving{}.BorderWidth);
    EXPECT_FLOAT_EQ(clean.BorderHeight, SplinePaving{}.BorderHeight);
    EXPECT_EQ(clean, SanitizePavingRecipe(authored));
}

TEST(SplinePavingRecipeTests, AnUnknownSeamTakesBlendAndANonFiniteBandTakesTwoTiles)
{
    SplinePaving authored;
    authored.Seam = static_cast<Components::SplinePavingSeam>(9);
    authored.SeamBandTiles = std::numeric_limits<float32>::quiet_NaN();
    authored.SeamCourseWidth = 40.0f;
    const SplinePaving clean = SanitizePavingRecipe(authored);
    EXPECT_EQ(clean.Seam, Components::SplinePavingSeam::Blend);
    EXPECT_EQ(clean.SeamBandTiles, 2.0f);
    EXPECT_EQ(clean.SeamCourseWidth, 2.0f);
}

TEST(SplinePavingRecipeTests, MembersPastTheCountAreClearedAndTheCountIsBounded)
{
    SplinePaving authored;
    authored.MemberCount = 40;
    authored.Members[3].Mode = static_cast<Components::SplinePavingMemberMode>(9);
    SplinePaving shorter = authored;
    shorter.MemberCount = 2;
    shorter.Members[5].Entity = ECS::EntityHandle{12, 1};

    EXPECT_EQ(SanitizePavingRecipe(authored).MemberCount, Components::kMaxPavingMembers);
    EXPECT_EQ(SanitizePavingRecipe(authored).Members[3].Mode, Components::SplinePavingMemberMode::Exclude);
    EXPECT_FALSE(SanitizePavingRecipe(shorter).Members[5].Entity.IsValid());
}

TEST(SplinePavingRecipeTests, ALookEditLeavesTheGeometryKeyAndAGeometryEditChangesIt)
{
    const SplinePaving recipe;
    SplinePaving look = recipe;
    look.Material.Set(GUID::Derive(GUID::Null(), "test/paving"));
    look.CastShadows = false;
    SplinePaving geometry = recipe;
    geometry.Seed = 7;

    EXPECT_EQ(PavingGeometryKey(look), PavingGeometryKey(recipe));
    EXPECT_NE(PavingGeometryKey(geometry), PavingGeometryKey(recipe));
}

} // namespace
} // namespace GameEngine::SplineRecipes
