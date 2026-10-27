// Writing a fence recipe's span overrides: what the scene view's span section and the fence's
// override list commit. The table is fixed-capacity, so every write is either placed or refused,
// never dropped.

#include <gtest/gtest.h>

#include "Components/Spline/SplineFence.h"
#include "SplineRecipes/FenceSpanOverrides.h"

#include <utility>
#include <vector>

using namespace GameEngine;
using Components::SplineFence;
using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;

TEST(FenceSpanOverrides, ANewOverrideTakesTheFirstEmptySlotAndIsRewrittenInPlace)
{
    SplineFence recipe;
    recipe.Overrides[0] = {1u, 0u, SplineSpanOverrideKind::ExplicitPart, 2u};
    ASSERT_TRUE(SplineRecipes::SetSpanOverride(recipe, 3u, 2u, SplineSpanOverrideKind::Gate, 0u));
    EXPECT_EQ(recipe.Overrides[1], (SplineSpanOverride{3u, 2u, SplineSpanOverrideKind::Gate, 0u}));
    EXPECT_EQ(SplineRecipes::CountSpanOverrides(recipe), 2u);

    // The same span again rewrites its slot rather than taking another.
    ASSERT_TRUE(SplineRecipes::SetSpanOverride(recipe, 3u, 2u, SplineSpanOverrideKind::ExplicitPart, 4u));
    EXPECT_EQ(recipe.Overrides[1],
              (SplineSpanOverride{3u, 2u, SplineSpanOverrideKind::ExplicitPart, 4u}));
    EXPECT_EQ(SplineRecipes::CountSpanOverrides(recipe), 2u);
    EXPECT_EQ(recipe.Overrides[0], (SplineSpanOverride{1u, 0u, SplineSpanOverrideKind::ExplicitPart, 2u}));
}

TEST(FenceSpanOverrides, FindReturnsTheFirstFilledSlotNamingTheSpan)
{
    SplineFence recipe;
    EXPECT_EQ(SplineRecipes::FindSpanOverride(recipe, 0u, 0u), nullptr)
        << "an empty table is sixteen empty slots, not sixteen gates on the first span";
    recipe.Overrides[2] = {5u, 1u, SplineSpanOverrideKind::Gate, 0u};
    recipe.Overrides[6] = {5u, 1u, SplineSpanOverrideKind::ExplicitPart, 3u};
    const SplineSpanOverride* found = SplineRecipes::FindSpanOverride(recipe, 5u, 1u);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found, &recipe.Overrides[2]);
    EXPECT_EQ(SplineRecipes::FindSpanOverride(recipe, 5u, 2u), nullptr);
}

TEST(FenceSpanOverrides, NoneClearsEveryOverrideOnTheSpanAndNothingElse)
{
    SplineFence recipe;
    recipe.Overrides[2] = {5u, 1u, SplineSpanOverrideKind::Gate, 0u};
    recipe.Overrides[6] = {5u, 1u, SplineSpanOverrideKind::ExplicitPart, 3u};
    recipe.Overrides[7] = {5u, 2u, SplineSpanOverrideKind::Gate, 0u};
    ASSERT_TRUE(SplineRecipes::SetSpanOverride(recipe, 5u, 1u, SplineSpanOverrideKind::None, 0u));
    EXPECT_EQ(recipe.Overrides[2], SplineSpanOverride{});
    EXPECT_EQ(recipe.Overrides[6], SplineSpanOverride{});
    EXPECT_EQ(recipe.Overrides[7], (SplineSpanOverride{5u, 2u, SplineSpanOverrideKind::Gate, 0u}));
}

TEST(FenceSpanOverrides, AFullTableRefusesANewSpanAndLeavesTheRecipeUntouched)
{
    SplineFence recipe;
    for (uint32 i = 0; i < Components::kSplineFenceMaxSpanOverrides; ++i)
        recipe.Overrides[i] = {0u, static_cast<uint16>(i), SplineSpanOverrideKind::Gate, 0u};
    const SplineFence before = recipe;
    EXPECT_FALSE(SplineRecipes::SetSpanOverride(recipe, 1u, 0u, SplineSpanOverrideKind::Gate, 0u));
    EXPECT_TRUE(recipe == before);
    // A span that already has a slot can still be changed.
    EXPECT_TRUE(SplineRecipes::SetSpanOverride(recipe, 0u, 3u, SplineSpanOverrideKind::ExplicitPart, 1u));
    EXPECT_EQ(recipe.Overrides[3].Kind, SplineSpanOverrideKind::ExplicitPart);
}

namespace
{

Components::ModelRef TestMesh(uint8 tag)
{
    GUID::Data data{};
    data[0] = tag;
    data[15] = 0x5A;
    return Components::ModelRef(GUID(data));
}

} // namespace

// Remove and Move keep the list in order with no gap, so row n of the list is slot n.
TEST(FenceSpanOverrides, RemoveAndMoveKeepTheListInOrderWithNoGap)
{
    SplineFence recipe;
    recipe.Overrides[0] = {0u, 1u, SplineSpanOverrideKind::Gate, 0u};
    recipe.Overrides[3] = {0u, 2u, SplineSpanOverrideKind::ExplicitPart, 1u};
    recipe.Overrides[5] = {1u, 0u, SplineSpanOverrideKind::Gate, 0u};
    SplineRecipes::SwapSpanOverrides(recipe, 1u, 2u);
    EXPECT_EQ(recipe.Overrides[1], (SplineSpanOverride{1u, 0u, SplineSpanOverrideKind::Gate, 0u}));
    EXPECT_EQ(recipe.Overrides[2], (SplineSpanOverride{0u, 2u, SplineSpanOverrideKind::ExplicitPart, 1u}));
    SplineRecipes::RemoveSpanOverride(recipe, 0u);
    EXPECT_EQ(recipe.Overrides[0], (SplineSpanOverride{1u, 0u, SplineSpanOverrideKind::Gate, 0u}));
    EXPECT_EQ(recipe.Overrides[1], (SplineSpanOverride{0u, 2u, SplineSpanOverrideKind::ExplicitPart, 1u}));
    EXPECT_EQ(SplineRecipes::CountSpanOverrides(recipe), 2u);
    SplineRecipes::SwapSpanOverrides(recipe, 1u, 2u);
    EXPECT_EQ(recipe.Overrides[1].Kind, SplineSpanOverrideKind::ExplicitPart) << "no row 2 to swap with";
    SplineRecipes::SetSpanOverrideKind(recipe, 1u, SplineSpanOverrideKind::Gate);
    EXPECT_EQ(recipe.Overrides[1], (SplineSpanOverride{0u, 2u, SplineSpanOverrideKind::Gate, 0u}))
        << "a new kind takes the first part of its pool";
}

// With a build, Add Override takes the first standing span with no override after the last
// override, then wraps to the first one; with no build, the next span with no override.
TEST(FenceSpanOverrides, AddTakesAStandingSpanWithNoOverride)
{
    using Components::SplineFencePartKind;
    SplineFence recipe;
    recipe.Parts[0] = {TestMesh(1), SplineFencePartKind::Span};
    const std::vector<std::pair<uint32, uint32>> standing = {{1u, 1u}, {0u, 0u}, {0u, 1u}, {1u, 0u}};
    ASSERT_TRUE(SplineRecipes::AddSpanOverride(recipe, standing));
    EXPECT_EQ(recipe.Overrides[0], (SplineSpanOverride{0u, 0u, SplineSpanOverrideKind::ExplicitPart, 0u}))
        << "no Gate part, so an explicit part";
    ASSERT_TRUE(SplineRecipes::AddSpanOverride(recipe, standing));
    EXPECT_EQ(recipe.Overrides[1], (SplineSpanOverride{0u, 1u, SplineSpanOverrideKind::ExplicitPart, 0u}));
    ASSERT_TRUE(SplineRecipes::AddSpanOverride(recipe, standing));
    EXPECT_EQ(recipe.Overrides[2], (SplineSpanOverride{1u, 0u, SplineSpanOverrideKind::ExplicitPart, 0u}))
        << "past the end of the segment from point 0, the next segment";
    recipe.Overrides[0] = SplineSpanOverride{};
    recipe.Overrides[2].PointIndex = 1u;
    recipe.Overrides[2].SpanOrdinal = 1u;
    ASSERT_TRUE(SplineRecipes::AddSpanOverride(recipe, standing));
    EXPECT_EQ(recipe.Overrides[2], (SplineSpanOverride{0u, 0u, SplineSpanOverrideKind::ExplicitPart, 0u}))
        << "after the last standing span, the first free one";

    SplineFence unbuilt;
    unbuilt.Parts[0] = {TestMesh(2), SplineFencePartKind::Gate};
    ASSERT_TRUE(SplineRecipes::AddSpanOverride(unbuilt, {}));
    EXPECT_EQ(unbuilt.Overrides[0], (SplineSpanOverride{0u, 0u, SplineSpanOverrideKind::Gate, 0u}));
    ASSERT_TRUE(SplineRecipes::AddSpanOverride(unbuilt, {}));
    EXPECT_EQ(unbuilt.Overrides[1], (SplineSpanOverride{0u, 1u, SplineSpanOverrideKind::Gate, 0u}));

    SplineFence full;
    for (uint32 i = 0; i < Components::kSplineFenceMaxSpanOverrides; ++i)
        full.Overrides[i] = {0u, static_cast<uint16>(i), SplineSpanOverrideKind::Gate, 0u};
    const SplineFence before = full;
    EXPECT_FALSE(SplineRecipes::AddSpanOverride(full, standing));
    EXPECT_TRUE(full == before);
}

// An edit of the Parts list keeps each override on the part it drew. A removed part leaves its
// override with no part, not with the next part of the pool.
TEST(FenceSpanOverrides, AnOverrideKeepsItsPartThroughAPartsListEdit)
{
    using Components::SplineFencePartKind;
    SplineFence recipe;
    recipe.Parts[0] = {TestMesh(1), SplineFencePartKind::Span};
    recipe.Parts[1] = {TestMesh(2), SplineFencePartKind::Gate};
    recipe.Parts[2] = {TestMesh(3), SplineFencePartKind::Span};
    recipe.Overrides[0] = {0u, 1u, SplineSpanOverrideKind::ExplicitPart, 1u};
    recipe.Overrides[1] = {0u, 2u, SplineSpanOverrideKind::Gate, 0u};
    const SplineRecipes::SpanOverridePartRows drawn = SplineRecipes::PartRowsOfSpanOverrides(recipe);
    EXPECT_EQ(drawn[0], 2u);
    EXPECT_EQ(drawn[1], 1u);
    EXPECT_EQ(drawn[2], SplineRecipes::kNoPartRow);

    // Remove row 0: the Gate moves to row 0 and the second Span to row 1.
    SplineRecipes::PartRowMoves moved{};
    moved[0] = SplineRecipes::kNoPartRow;
    moved[1] = 0u;
    moved[2] = 1u;
    recipe.Parts[0] = recipe.Parts[1];
    recipe.Parts[1] = recipe.Parts[2];
    recipe.Parts[2] = Components::SplineFencePart{};
    SplineRecipes::KeepSpanOverridesOnTheirParts(recipe, drawn, moved);
    EXPECT_EQ(recipe.Overrides[0].PoolSlot, 0u) << "its Span part is now the first of the pool";
    EXPECT_EQ(recipe.Overrides[1].PoolSlot, 0u);

    const SplineRecipes::SpanOverridePartRows again = SplineRecipes::PartRowsOfSpanOverrides(recipe);
    SplineRecipes::PartRowMoves removed{};
    removed[0] = 0u;
    removed[1] = SplineRecipes::kNoPartRow;
    recipe.Parts[1] = Components::SplineFencePart{};
    SplineRecipes::KeepSpanOverridesOnTheirParts(recipe, again, removed);
    EXPECT_EQ(recipe.Overrides[0].PoolSlot, SplineRecipes::kRemovedPartSlot);
    EXPECT_EQ(recipe.Overrides[1].PoolSlot, 0u);
}
