#include "AI/OptionSet.h"

#include "Types/Fnv1a.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::AI
{

float32 OptionSet::Score(uint16 option, std::span<const float32> columns, uint32 rowCount, uint32 row) const
{
    const Option& entry = Options[option];
    if (entry.ConsiderationCount == 0)
        return entry.Weight;

    // The compensation factor keeps a product of many axes from sinking below a product of few:
    // each axis value moves up by (1 - value) * (1 - 1 / count) of itself.
    const float32 compensation = 1.0f - 1.0f / static_cast<float32>(entry.ConsiderationCount);
    float32 product = 1.0f;
    for (uint16 index = 0; index < entry.ConsiderationCount; ++index)
    {
        const Consideration& axis = Considerations[entry.FirstConsideration + index];
        assert(static_cast<size_t>(axis.Column + 1) * rowCount <= columns.size());
        const float32 input = columns[static_cast<size_t>(axis.Column) * rowCount + row];
        const float32 range = axis.Max - axis.Min;
        const float32 normalized = range != 0.0f ? (input - axis.Min) / range : 0.0f;
        const float32 value = Curves[axis.Curve].Evaluate(normalized);
        product *= value + (1.0f - value) * compensation * value;
        if (product <= 0.0f)
            return 0.0f;
    }
    return entry.Weight * product;
}

float32 OptionSet::ReadyScore(uint16 option, std::span<const float32> columns, std::span<const Row> rows, uint32 row,
                              uint32 tick) const
{
    const std::span<const uint32> readyTicks = rows[row].ReadyTicks;
    assert(readyTicks.empty() || readyTicks.size() >= Options.size());
    if (!readyTicks.empty() && tick < readyTicks[option])
        return 0.0f;
    return Score(option, columns, static_cast<uint32>(rows.size()), row);
}

uint16 OptionSet::DrawNearBest(std::span<const float32> columns, std::span<const Row> rows, uint32 row, uint32 tick,
                               uint8 rank, float32 bestScore, float32 draw) const
{
    // A candidate scores above 0 and within NoiseMargin of the best.
    const float32 floor = bestScore - NoiseMargin;
    uint32 candidateCount = 0;
    for (uint16 option = 0; option < static_cast<uint16>(Options.size()); ++option)
    {
        const float32 score = ReadyScore(option, columns, rows, row, tick);
        if (Options[option].Rank == rank && score > 0.0f && score >= floor)
            ++candidateCount;
    }
    // The draw is in [0, 1), so the pick is below candidateCount; candidates count in option order.
    uint32 pick = static_cast<uint32>(draw * static_cast<float32>(candidateCount));
    for (uint16 option = 0; option < static_cast<uint16>(Options.size()); ++option)
    {
        const float32 score = ReadyScore(option, columns, rows, row, tick);
        if (Options[option].Rank != rank || score <= 0.0f || score < floor)
            continue;
        if (pick == 0)
            return option;
        --pick;
    }
    return kNoOption;
}

void OptionSet::Select(std::span<const float32> columns, std::span<const Row> rows, uint32 tick, uint64 seed,
                       std::span<uint16> chosen, std::span<float32> scores) const
{
    const uint32 rowCount = static_cast<uint32>(rows.size());
    assert(chosen.size() >= rowCount && scores.size() >= rowCount);
    for (uint32 row = 0; row < rowCount; ++row)
    {
        const Row& state = rows[row];
        uint16 best = kNoOption;
        float32 bestScore = 0.0f;
        float32 activeScore = 0.0f;
        for (uint16 option = 0; option < static_cast<uint16>(Options.size()); ++option)
        {
            const float32 score = ReadyScore(option, columns, rows, row, tick);
            if (option == state.Active)
                activeScore = score;
            if (score <= 0.0f)
                continue;
            const bool higherRank = best == kNoOption || Options[option].Rank > Options[best].Rank;
            const bool sameRankBetter = best != kNoOption && Options[option].Rank == Options[best].Rank && score > bestScore;
            if (higherRank || sameRankBetter)
            {
                best = option;
                bestScore = score;
            }
        }

        if (best != kNoOption && NoiseMargin > 0.0f)
        {
            const float32 draw = Hashing::UnitDraw(seed, state.Agent, tick, 0);
            best = DrawNearBest(columns, rows, row, tick, Options[best].Rank, bestScore, draw);
            bestScore = ReadyScore(best, columns, rows, row, tick);
        }

        // The active option holds against a challenger of its own rank until the minimum time
        // passes and the challenger beats it by the margin. A lower rank cannot win while it scores.
        const bool activeHolds = state.Active != kNoOption && activeScore > 0.0f && best != state.Active &&
                                 Options[best].Rank == Options[state.Active].Rank &&
                                 (tick - state.EnteredTick < MinimumTicks || bestScore <= activeScore + CommitmentMargin);
        chosen[row] = activeHolds ? state.Active : best;
        scores[row] = activeHolds ? activeScore : bestScore;
    }
}

} // namespace GameEngine::AI
