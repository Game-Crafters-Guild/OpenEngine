#include "AI/ResponseCurve.h"

#include "Mathematics/Curve.h"
#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::AI
{

namespace
{

// Keeps the logit away from x = 0 and x = 1, where it is infinite.
constexpr float32 kLogitEdge = 1.0e-4f;

float32 SampleX(uint32 index)
{
    return static_cast<float32>(index) / static_cast<float32>(ResponseCurve::kSampleCount - 1);
}

float32 EvaluatePreset(CurvePreset preset, float32 shape, float32 midpoint, float32 x)
{
    switch (preset)
    {
        case CurvePreset::Linear:
            return x;
        case CurvePreset::Polynomial:
            return std::pow(x, shape);
        case CurvePreset::Logistic:
            return 1.0f / (1.0f + std::exp(-shape * (x - midpoint)));
        case CurvePreset::Logit:
        {
            const float32 edged = std::clamp(x, kLogitEdge, 1.0f - kLogitEdge);
            return midpoint + std::log(edged / (1.0f - edged)) / shape;
        }
        case CurvePreset::Step:
            return x >= midpoint ? 1.0f : 0.0f;
    }
    return 0.0f;
}

} // namespace

ResponseCurve ResponseCurve::FromKeys(std::span<const Math::CurveKey> keys)
{
    ResponseCurve curve;
    for (uint32 index = 0; index < kSampleCount; ++index)
        curve.m_Samples[index] = Math::EvaluateCurveKeys(keys.data(), static_cast<uint32>(keys.size()), SampleX(index));
    return curve;
}

ResponseCurve ResponseCurve::FromPreset(CurvePreset preset, float32 shape, float32 midpoint)
{
    ResponseCurve curve;
    for (uint32 index = 0; index < kSampleCount; ++index)
        curve.m_Samples[index] = Math::Clamp01(EvaluatePreset(preset, shape, midpoint, SampleX(index)));
    return curve;
}

} // namespace GameEngine::AI
