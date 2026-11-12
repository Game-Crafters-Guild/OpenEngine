#pragma once

#include "AI/ResponseCurve.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::AI
{

/// The options an agent chooses among by utility, with their considerations and curves.
/// Immutable after it is built; any number of threads may call Select at once.
class OptionSet
{
public:
    /// The option index Select writes for a row with no option above zero.
    static constexpr uint16 kNoOption = 0xFFFF;

    /// One axis of an option: the input in column `Column`, mapped from [Min, Max] to [0, 1]
    /// and then through the curve at index `Curve`.
    struct Consideration
    {
        float32 Min = 0.0f;
        float32 Max = 1.0f;
        uint16 Column = 0;
        uint16 Curve = 0;
    };

    /// One choice. Its score is Weight times the compensated product of its considerations.
    /// A higher Rank wins over any score of a lower rank. `Action` is the caller's id for
    /// what the option does; the set does not read it.
    struct Option
    {
        float32 Weight = 1.0f;
        uint16 FirstConsideration = 0;
        uint16 ConsiderationCount = 0;
        uint16 Action = 0;
        uint8 Rank = 0;
        /// The ticks after the option stops before it can be chosen again. The caller keeps
        /// the tick it is ready again in Row::ReadyTicks.
        uint16 CooldownTicks = 0;
    };

    /// One agent's state before the choice: its key for the noise draw, its active option with
    /// the tick that option started, and per option the first tick it can be chosen again.
    /// Empty ReadyTicks means no option cools down.
    struct Row
    {
        uint64 Agent = 0;
        uint32 EnteredTick = 0;
        uint16 Active = kNoOption;
        std::span<const uint32> ReadyTicks;
    };

    std::vector<ResponseCurve> Curves;
    std::vector<Consideration> Considerations;
    std::vector<Option> Options;
    /// A challenger of the active option's rank must score above the active score plus this value.
    float32 CommitmentMargin = 0.0f;
    /// The ticks an active option holds against a challenger of the same rank.
    uint32 MinimumTicks = 0;
    /// The options of the winning rank that score above 0 and within this value of the best score
    /// share a seeded draw. At 0 the best score wins.
    float32 NoiseMargin = 0.0f;

    /// Chooses per row the highest rank above 0, then the best score or the draw Hashing::UnitDraw(seed,
    /// row.Agent, tick, 0) within NoiseMargin; ties go to the lower index; kNoOption when none scores. An option
    /// before its ready tick scores 0. columns[c * rows.size() + r] is column c of row r.
    void Select(std::span<const float32> columns, std::span<const Row> rows, uint32 tick, uint64 seed,
                std::span<uint16> chosen, std::span<float32> scores) const;

private:
    float32 Score(uint16 option, std::span<const float32> columns, uint32 rowCount, uint32 row) const;
    // Score, or 0 while the option cools down for the row.
    float32 ReadyScore(uint16 option, std::span<const float32> columns, std::span<const Row> rows, uint32 row,
                       uint32 tick) const;
    uint16 DrawNearBest(std::span<const float32> columns, std::span<const Row> rows, uint32 row, uint32 tick, uint8 rank,
                        float32 bestScore, float32 draw) const;
};

} // namespace GameEngine::AI
