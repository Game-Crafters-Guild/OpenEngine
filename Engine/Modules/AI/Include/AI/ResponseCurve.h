#pragma once

#include "Mathematics/Interpolation.h"
#include "Types/Types.h"

#include <algorithm>
#include <array>
#include <span>

namespace GameEngine::Math
{
struct CurveKey;
}

namespace GameEngine::AI
{

/// The shape of a preset response curve. Each maps x in [0, 1] to a value in [0, 1].
enum class CurvePreset : uint8
{
    Linear,     ///< y = x.
    Polynomial, ///< y = x ^ shape.
    Logistic,   ///< An S curve with its center at x = midpoint; shape sets the steepness.
    Logit,      ///< The inverse of Logistic; shape sets the steepness.
    Step,       ///< 0 below x = midpoint, 1 at and above it.
};

/// A response curve over x in [0, 1], baked to kSampleCount samples. Evaluate reads the table and
/// interpolates linearly. It calls no math library function, so every compiler gives the same bits.
class ResponseCurve
{
public:
    static constexpr uint32 kSampleCount = 64;

    /// Bakes Time-sorted keys with Math::EvaluateCurveKeys at kSampleCount equal steps of [0, 1].
    static ResponseCurve FromKeys(std::span<const Math::CurveKey> keys);

    /// Bakes a preset. `shape` is the exponent (Polynomial) or the steepness (Logistic, Logit).
    /// `midpoint` is the center (Logistic, Logit) or the threshold (Step). Values clamp to [0, 1].
    static ResponseCurve FromPreset(CurvePreset preset, float32 shape, float32 midpoint);

    /// The curve at x. x clamps to [0, 1].
    float32 Evaluate(float32 x) const
    {
        const float32 position = Math::Clamp01(x) * static_cast<float32>(kSampleCount - 1);
        const uint32 index = std::min(static_cast<uint32>(position), kSampleCount - 2);
        const float32 fraction = position - static_cast<float32>(index);
        return Math::Lerp(m_Samples[index], m_Samples[index + 1], fraction);
    }

private:
    std::array<float32, kSampleCount> m_Samples{};
};

} // namespace GameEngine::AI
