#include "AI/OptionSet.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

using namespace GameEngine;

namespace
{

// One linear curve; option i reads the columns in columnsPerOption[i] through it.
AI::OptionSet MakeSet(const std::vector<std::vector<uint16>>& columnsPerOption, const std::vector<uint8>& ranks)
{
    AI::OptionSet set;
    set.Curves.push_back(AI::ResponseCurve::FromPreset(AI::CurvePreset::Linear, 1.0f, 0.5f));
    for (size_t option = 0; option < columnsPerOption.size(); ++option)
    {
        AI::OptionSet::Option entry;
        entry.FirstConsideration = static_cast<uint16>(set.Considerations.size());
        entry.ConsiderationCount = static_cast<uint16>(columnsPerOption[option].size());
        entry.Rank = ranks[option];
        for (uint16 column : columnsPerOption[option])
            set.Considerations.push_back({0.0f, 1.0f, column, 0});
        set.Options.push_back(entry);
    }
    return set;
}

} // namespace

// Three axes at 0.5 each: every axis rises to 0.5 + 0.5 * (2/3) * 0.5 = 2/3, so the score is 8/27.
TEST(OptionSetTests, CompensationFactorRaisesEachAxisByTheAxisCount)
{
    const AI::OptionSet set = MakeSet({{0, 0, 0}}, {0});
    const std::array<float32, 1> columns{0.5f};
    const std::array<AI::OptionSet::Row, 1> rows{};
    std::array<uint16, 1> chosen{};
    std::array<float32, 1> scores{};
    set.Select(columns, rows, 0, 0, chosen, scores);

    EXPECT_EQ(chosen[0], 0);
    EXPECT_NEAR(scores[0], 8.0f / 27.0f, 1.0e-5f);
}

// Row 0: option 1 (rank 1) scores 0.1 and wins over option 0 (rank 0) at 0.9.
// Row 1: option 1 scores 0, so the lower rank's option 0 wins.
TEST(OptionSetTests, HigherRankWinsWhenItScoresAboveZero)
{
    const AI::OptionSet set = MakeSet({{0}, {1}}, {0, 1});
    const std::array<float32, 4> columns{0.9f, 0.9f, 0.1f, 0.0f};
    const std::array<AI::OptionSet::Row, 2> rows{};
    std::array<uint16, 2> chosen{};
    std::array<float32, 2> scores{};
    set.Select(columns, rows, 0, 0, chosen, scores);

    EXPECT_EQ(chosen[0], 1);
    EXPECT_NEAR(scores[0], 0.1f, 1.0e-3f);
    EXPECT_EQ(chosen[1], 0);
    EXPECT_NEAR(scores[1], 0.9f, 1.0e-3f);
}

TEST(OptionSetTests, TieGoesToTheLowerOptionIndex)
{
    const AI::OptionSet set = MakeSet({{0}, {0}}, {0, 0});
    const std::array<float32, 1> columns{0.7f};
    const std::array<AI::OptionSet::Row, 1> rows{};
    std::array<uint16, 1> chosen{};
    std::array<float32, 1> scores{};
    set.Select(columns, rows, 0, 0, chosen, scores);

    EXPECT_EQ(chosen[0], 0);
}

TEST(OptionSetTests, NoOptionAboveZeroChoosesNone)
{
    const AI::OptionSet set = MakeSet({{0}, {0}}, {0, 1});
    const std::array<float32, 1> columns{0.0f};
    std::array<uint16, 1> chosen{};
    std::array<float32, 1> scores{1.0f};
    const std::array<AI::OptionSet::Row, 1> rows{};
    set.Select(columns, rows, 0, 0, chosen, scores);

    EXPECT_EQ(chosen[0], AI::OptionSet::kNoOption);
    EXPECT_FLOAT_EQ(scores[0], 0.0f);
}

// Option 0 is active at 0.5 since tick 10; option 1 (same rank) scores 0.6 in row 0 and 0.8 in
// row 1. With a margin of 0.2, only row 1's challenger replaces the active option.
TEST(OptionSetTests, SameRankChallengerMustBeatTheCommitmentMargin)
{
    AI::OptionSet set = MakeSet({{0}, {1}}, {0, 0});
    set.CommitmentMargin = 0.2f;
    const std::array<float32, 4> columns{0.5f, 0.5f, 0.6f, 0.8f};
    const std::array<AI::OptionSet::Row, 2> rows{{{1, 10, 0}, {2, 10, 0}}};
    std::array<uint16, 2> chosen{};
    std::array<float32, 2> scores{};
    set.Select(columns, rows, 20, 0, chosen, scores);

    EXPECT_EQ(chosen[0], 0);
    EXPECT_NEAR(scores[0], 0.5f, 1.0e-3f);
    EXPECT_EQ(chosen[1], 1);
}

// Option 0 entered at tick 10 with a minimum of 5 ticks: a better option 1 of the same rank waits
// until tick 15. Option 2 has a higher rank and preempts at once.
TEST(OptionSetTests, MinimumTimeHoldsOnlyAgainstTheSameRank)
{
    AI::OptionSet set = MakeSet({{0}, {1}, {2}}, {0, 0, 1});
    set.MinimumTicks = 5;
    const std::array<float32, 3> sameRank{0.2f, 0.9f, 0.0f};
    const std::array<float32, 3> higherRank{0.2f, 0.9f, 0.1f};
    const std::array<AI::OptionSet::Row, 1> rows{{{1, 10, 0}}};
    std::array<uint16, 1> chosen{};
    std::array<float32, 1> scores{};

    set.Select(sameRank, rows, 14, 0, chosen, scores);
    EXPECT_EQ(chosen[0], 0);
    set.Select(sameRank, rows, 15, 0, chosen, scores);
    EXPECT_EQ(chosen[0], 1);
    set.Select(higherRank, rows, 11, 0, chosen, scores);
    EXPECT_EQ(chosen[0], 2);
}

// Option 1 scores best but is ready again only at tick 20: option 0 wins before it, option 1 from it.
TEST(OptionSetTests, OptionWaitsForItsReadyTick)
{
    AI::OptionSet set = MakeSet({{0}, {1}}, {0, 0});
    const std::array<float32, 2> columns{0.3f, 0.9f};
    const std::array<uint32, 2> readyTicks{0, 20};
    const std::array<AI::OptionSet::Row, 1> rows{{{1, 0, AI::OptionSet::kNoOption, readyTicks}}};
    std::array<uint16, 1> chosen{};
    std::array<float32, 1> scores{};

    set.Select(columns, rows, 19, 0, chosen, scores);
    EXPECT_EQ(chosen[0], 0);
    set.Select(columns, rows, 20, 0, chosen, scores);
    EXPECT_EQ(chosen[0], 1);
}

// Options 0 and 1 score within the noise margin of the best, option 2 does not. The draw is a pure
// function of seed, agent and tick: two calls agree, and over 64 agents both near options win.
TEST(OptionSetTests, NoiseDrawsAmongOptionsNearTheBest)
{
    AI::OptionSet set = MakeSet({{0}, {1}, {2}}, {0, 0, 0});
    set.NoiseMargin = 0.1f;
    constexpr uint32 kAgents = 64;
    std::vector<float32> columns(3 * kAgents);
    std::vector<AI::OptionSet::Row> rows(kAgents);
    for (uint32 agent = 0; agent < kAgents; ++agent)
    {
        columns[agent] = 0.9f;
        columns[kAgents + agent] = 0.85f;
        columns[2 * kAgents + agent] = 0.5f;
        rows[agent].Agent = agent;
    }
    std::vector<uint16> first(kAgents);
    std::vector<uint16> second(kAgents);
    std::vector<float32> scores(kAgents);
    set.Select(columns, rows, 7, 42, first, scores);
    set.Select(columns, rows, 7, 42, second, scores);

    EXPECT_EQ(first, second);
    std::array<uint32, 3> wins{};
    for (uint16 option : first)
        ++wins[option];
    EXPECT_GT(wins[0], 0u);
    EXPECT_GT(wins[1], 0u);
    EXPECT_EQ(wins[2], 0u);
}
