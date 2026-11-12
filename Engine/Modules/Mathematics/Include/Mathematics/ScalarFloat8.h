#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace GameEngine::Mathematics::Float8Detail
{

/// Eight float lanes as eight plain floats: Float8 on a build without AVX2, and the reference that
/// Avx2Float8 must match. Each operation is the scalar operation in each lane, so each lane has the
/// bits of the same scalar code. Use Float8; name this type only to compare the two implementations.
class ScalarFloat8
{
public:
    static constexpr std::size_t kLaneCount = 8;

    /// The lanes where a comparison of two ScalarFloat8 holds.
    class Mask
    {
    public:
        /// True when the comparison holds in at least one lane.
        bool Any() const
        {
            for (const bool lane : m_Lanes)
                if (lane)
                    return true;
            return false;
        }

    private:
        friend class ScalarFloat8;

        Mask() = default;

        std::array<bool, kLaneCount> m_Lanes{};
    };

    /// All lanes 0.
    ScalarFloat8() = default;

    /// `value` in every lane.
    static ScalarFloat8 Broadcast(float value)
    {
        ScalarFloat8 result;
        result.m_Lanes.fill(value);
        return result;
    }

    /// Lane i is `values[i]`. No alignment is necessary.
    static ScalarFloat8 Load(std::span<const float, kLaneCount> values)
    {
        ScalarFloat8 result;
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            result.m_Lanes[lane] = values[lane];
        return result;
    }

    /// Writes lane i to `out[i]`. No alignment is necessary.
    void Store(std::span<float, kLaneCount> out) const
    {
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            out[lane] = m_Lanes[lane];
    }

    /// `value` with its sign bit flipped in each lane where bit 0 of the bit pattern of `bits` is 1.
    static ScalarFloat8 FlipSignWhereLowBitSet(ScalarFloat8 value, ScalarFloat8 bits)
    {
        ScalarFloat8 result;
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            result.m_Lanes[lane] = std::bit_cast<float>(std::bit_cast<std::uint32_t>(value.m_Lanes[lane]) ^
                                                        (std::bit_cast<std::uint32_t>(bits.m_Lanes[lane]) << 31u));
        return result;
    }

    /// Lane i is `ifSet[i]` where `mask` holds in lane i, else `ifClear[i]`.
    static ScalarFloat8 Select(Mask mask, ScalarFloat8 ifSet, ScalarFloat8 ifClear)
    {
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            if (mask.m_Lanes[lane])
                ifClear.m_Lanes[lane] = ifSet.m_Lanes[lane];
        return ifClear;
    }

    /// `value` clamped to [0, 1] in each lane. Each lane has the bits of Math::Clamp01(float): a NaN
    /// and a negative zero pass through.
    static ScalarFloat8 Clamp01(ScalarFloat8 value)
    {
        const ScalarFloat8 zero = Broadcast(0.0f);
        const ScalarFloat8 one = Broadcast(1.0f);
        return Select(value < zero, zero, Select(value > one, one, value));
    }

    /// The ordered comparisons: a lane where either operand is NaN does not hold, as in scalar C++.
    Mask operator<(ScalarFloat8 other) const
    {
        Mask result;
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            result.m_Lanes[lane] = m_Lanes[lane] < other.m_Lanes[lane];
        return result;
    }

    Mask operator<=(ScalarFloat8 other) const
    {
        Mask result;
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            result.m_Lanes[lane] = m_Lanes[lane] <= other.m_Lanes[lane];
        return result;
    }

    Mask operator>(ScalarFloat8 other) const
    {
        Mask result;
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            result.m_Lanes[lane] = m_Lanes[lane] > other.m_Lanes[lane];
        return result;
    }

    Mask operator>=(ScalarFloat8 other) const
    {
        Mask result;
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            result.m_Lanes[lane] = m_Lanes[lane] >= other.m_Lanes[lane];
        return result;
    }

    friend ScalarFloat8 operator+(ScalarFloat8 a, ScalarFloat8 b)
    {
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            a.m_Lanes[lane] = a.m_Lanes[lane] + b.m_Lanes[lane];
        return a;
    }

    friend ScalarFloat8 operator-(ScalarFloat8 a, ScalarFloat8 b)
    {
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            a.m_Lanes[lane] = a.m_Lanes[lane] - b.m_Lanes[lane];
        return a;
    }

    friend ScalarFloat8 operator*(ScalarFloat8 a, ScalarFloat8 b)
    {
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            a.m_Lanes[lane] = a.m_Lanes[lane] * b.m_Lanes[lane];
        return a;
    }

    friend ScalarFloat8 operator/(ScalarFloat8 a, ScalarFloat8 b)
    {
        for (std::size_t lane = 0; lane < kLaneCount; ++lane)
            a.m_Lanes[lane] = a.m_Lanes[lane] / b.m_Lanes[lane];
        return a;
    }

private:
    // Lane storage of a SIMD register's shape, not a vector: no engine type holds eight floats.
    std::array<float, kLaneCount> m_Lanes{};
};

} // namespace GameEngine::Mathematics::Float8Detail
