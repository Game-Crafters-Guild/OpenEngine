#pragma once

#if !defined(__AVX2__)
#error "Avx2Float8.h needs a build with AVX2. Include Mathematics/Float8.h: it selects the implementation."
#endif

#include <cstddef>
#include <span>

#include <immintrin.h>

namespace GameEngine::Mathematics::Float8Detail
{

/// Eight float lanes in one AVX2 register: Float8 on a build with AVX2. Each operation has the bits of
/// the same ScalarFloat8 operation; the build turns contraction off, so no multiply and add fuse.
/// Use Float8; name this type only to compare the two implementations.
class Avx2Float8
{
public:
    static constexpr std::size_t kLaneCount = 8;

    /// The lanes where a comparison of two Avx2Float8 holds. Each lane is all bits set or all bits clear.
    class Mask
    {
    public:
        /// True when the comparison holds in at least one lane.
        bool Any() const
        {
            return _mm256_movemask_ps(m_Lanes) != 0;
        }

    private:
        friend class Avx2Float8;

        explicit Mask(__m256 lanes)
            : m_Lanes(lanes)
        {
        }

        __m256 m_Lanes;
    };

    /// All lanes 0.
    Avx2Float8() = default;

    /// `value` in every lane.
    static Avx2Float8 Broadcast(float value)
    {
        return Avx2Float8(_mm256_set1_ps(value));
    }

    /// Lane i is `values[i]`. No alignment is necessary.
    static Avx2Float8 Load(std::span<const float, kLaneCount> values)
    {
        return Avx2Float8(_mm256_loadu_ps(values.data()));
    }

    /// Writes lane i to `out[i]`. No alignment is necessary.
    void Store(std::span<float, kLaneCount> out) const
    {
        _mm256_storeu_ps(out.data(), m_Lanes);
    }

    /// `value` with its sign bit flipped in each lane where bit 0 of the bit pattern of `bits` is 1.
    static Avx2Float8 FlipSignWhereLowBitSet(Avx2Float8 value, Avx2Float8 bits)
    {
        const __m256 sign = _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_castps_si256(bits.m_Lanes), 31));
        return Avx2Float8(_mm256_xor_ps(value.m_Lanes, sign));
    }

    /// Lane i is `ifSet[i]` where `mask` holds in lane i, else `ifClear[i]`.
    static Avx2Float8 Select(Mask mask, Avx2Float8 ifSet, Avx2Float8 ifClear)
    {
        return Avx2Float8(_mm256_blendv_ps(ifClear.m_Lanes, ifSet.m_Lanes, mask.m_Lanes));
    }

    /// `value` clamped to [0, 1] in each lane. Each lane has the bits of Math::Clamp01(float): a NaN
    /// and a negative zero pass through.
    static Avx2Float8 Clamp01(Avx2Float8 value)
    {
        const Avx2Float8 zero = Broadcast(0.0f);
        const Avx2Float8 one = Broadcast(1.0f);
        return Select(value < zero, zero, Select(value > one, one, value));
    }

    /// The ordered comparisons: a lane where either operand is NaN does not hold, as in scalar C++.
    Mask operator<(Avx2Float8 other) const
    {
        return Mask(_mm256_cmp_ps(m_Lanes, other.m_Lanes, _CMP_LT_OQ));
    }

    Mask operator<=(Avx2Float8 other) const
    {
        return Mask(_mm256_cmp_ps(m_Lanes, other.m_Lanes, _CMP_LE_OQ));
    }

    Mask operator>(Avx2Float8 other) const
    {
        return Mask(_mm256_cmp_ps(m_Lanes, other.m_Lanes, _CMP_GT_OQ));
    }

    Mask operator>=(Avx2Float8 other) const
    {
        return Mask(_mm256_cmp_ps(m_Lanes, other.m_Lanes, _CMP_GE_OQ));
    }

    friend Avx2Float8 operator+(Avx2Float8 a, Avx2Float8 b)
    {
        return Avx2Float8(_mm256_add_ps(a.m_Lanes, b.m_Lanes));
    }

    friend Avx2Float8 operator-(Avx2Float8 a, Avx2Float8 b)
    {
        return Avx2Float8(_mm256_sub_ps(a.m_Lanes, b.m_Lanes));
    }

    friend Avx2Float8 operator*(Avx2Float8 a, Avx2Float8 b)
    {
        return Avx2Float8(_mm256_mul_ps(a.m_Lanes, b.m_Lanes));
    }

    friend Avx2Float8 operator/(Avx2Float8 a, Avx2Float8 b)
    {
        return Avx2Float8(_mm256_div_ps(a.m_Lanes, b.m_Lanes));
    }

private:
    explicit Avx2Float8(__m256 lanes)
        : m_Lanes(lanes)
    {
    }

    __m256 m_Lanes = _mm256_setzero_ps();
};

} // namespace GameEngine::Mathematics::Float8Detail
