// Font weight matching as CSS Fonts Level 4 §5.2 orders it, over the face sets the editor ships:
// Roboto in Regular (400), Medium (500) and Bold (700), Roboto Mono in Regular and Bold. Both
// the editor's font resolver and UIManager's no-resolver fallback pick faces with this cost, so
// these cases are what a stylesheet weight draws as on either path.

#include "UI/FontWeightMatch.h"

#include <gtest/gtest.h>
#include <initializer_list>

using GameEngine::UI::FontWeightMatchCost;

namespace
{

// The weight of the face with the lowest match cost; the first face wins a tie.
int MatchedWeight(int requestedWeight, std::initializer_list<int> faceWeights)
{
    int best = *faceWeights.begin();
    for (const int weight : faceWeights)
    {
        if (FontWeightMatchCost(requestedWeight, weight) < FontWeightMatchCost(requestedWeight, best))
            best = weight;
    }
    return best;
}

constexpr std::initializer_list<int> kRoboto = {400, 500, 700};
constexpr std::initializer_list<int> kRobotoMono = {400, 700};

} // namespace

TEST(FontWeightMatchTests, RobotoTakesTheNextHeavierFaceAbove500)
{
    EXPECT_EQ(MatchedWeight(600, kRoboto), 700);
    EXPECT_EQ(MatchedWeight(501, kRoboto), 700);
    EXPECT_EQ(MatchedWeight(700, kRoboto), 700);
    EXPECT_EQ(MatchedWeight(900, kRoboto), 700);
}

TEST(FontWeightMatchTests, RobotoTakesMediumFrom401To500)
{
    EXPECT_EQ(MatchedWeight(500, kRoboto), 500);
    EXPECT_EQ(MatchedWeight(450, kRoboto), 500);
    EXPECT_EQ(MatchedWeight(401, kRoboto), 500);
}

TEST(FontWeightMatchTests, RobotoTakesRegularAtAndBelow400)
{
    EXPECT_EQ(MatchedWeight(400, kRoboto), 400);
    EXPECT_EQ(MatchedWeight(300, kRoboto), 400);
    EXPECT_EQ(MatchedWeight(100, kRoboto), 400);
}

TEST(FontWeightMatchTests, RobotoMonoTakesBoldOnlyAbove500)
{
    EXPECT_EQ(MatchedWeight(500, kRobotoMono), 400);
    EXPECT_EQ(MatchedWeight(600, kRobotoMono), 700);
    EXPECT_EQ(MatchedWeight(300, kRobotoMono), 400);
}

TEST(FontWeightMatchTests, AWeightOutsideTheCssRangeIsClamped)
{
    EXPECT_EQ(FontWeightMatchCost(0, 400), FontWeightMatchCost(1, 400));
    EXPECT_EQ(FontWeightMatchCost(5000, 700), FontWeightMatchCost(1000, 700));
}
