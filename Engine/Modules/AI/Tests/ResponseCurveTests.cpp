#include "AI/ResponseCurve.h"

#include "Mathematics/Curve.h"

#include <gtest/gtest.h>

#include <array>

using namespace GameEngine;

namespace
{

// A smooth curve between samples differs from its linear interpolation by at most
// h^2 / 8 * max|f''|; for these keys and h = 1/63 that is under this bound.
constexpr float32 kBakeTolerance = 1.0e-3f;

} // namespace

TEST(ResponseCurveTests, KeysBakeToTheirEvaluationWithinTheStatedError)
{
    std::array<Math::CurveKey, 3> keys{};
    keys[0] = {0.0f, 0.0f, 0.0f, 0.0f, Math::CurveInterp::Smooth};
    keys[1] = {0.4f, 0.9f, 0.0f, 0.0f, Math::CurveInterp::Smooth};
    keys[2] = {1.0f, 0.2f, 0.0f, 0.0f, Math::CurveInterp::Smooth};
    const AI::ResponseCurve curve = AI::ResponseCurve::FromKeys(keys);

    for (uint32 step = 0; step <= 1000; ++step)
    {
        const float32 x = static_cast<float32>(step) / 1000.0f;
        EXPECT_NEAR(curve.Evaluate(x), Math::EvaluateCurveKeys(keys.data(), 3, x), kBakeTolerance) << "x = " << x;
    }
}

TEST(ResponseCurveTests, PresetsHaveTheirShapes)
{
    EXPECT_FLOAT_EQ(AI::ResponseCurve::FromPreset(AI::CurvePreset::Linear, 1.0f, 0.5f).Evaluate(0.25f), 0.25f);
    EXPECT_NEAR(AI::ResponseCurve::FromPreset(AI::CurvePreset::Polynomial, 2.0f, 0.5f).Evaluate(0.5f), 0.25f, 1.0e-3f);

    const AI::ResponseCurve logistic = AI::ResponseCurve::FromPreset(AI::CurvePreset::Logistic, 12.0f, 0.5f);
    EXPECT_NEAR(logistic.Evaluate(0.5f), 0.5f, 1.0e-3f);
    EXPECT_LT(logistic.Evaluate(0.1f), 0.01f);
    EXPECT_GT(logistic.Evaluate(0.9f), 0.99f);

    const AI::ResponseCurve logit = AI::ResponseCurve::FromPreset(AI::CurvePreset::Logit, 12.0f, 0.5f);
    EXPECT_NEAR(logit.Evaluate(0.5f), 0.5f, 1.0e-3f);
    EXPECT_FLOAT_EQ(logit.Evaluate(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(logit.Evaluate(1.0f), 1.0f);

    const AI::ResponseCurve step = AI::ResponseCurve::FromPreset(AI::CurvePreset::Step, 1.0f, 0.5f);
    EXPECT_FLOAT_EQ(step.Evaluate(0.4f), 0.0f);
    EXPECT_FLOAT_EQ(step.Evaluate(0.6f), 1.0f);
}

TEST(ResponseCurveTests, InputClampsToTheUnitRange)
{
    const AI::ResponseCurve linear = AI::ResponseCurve::FromPreset(AI::CurvePreset::Linear, 1.0f, 0.5f);
    EXPECT_FLOAT_EQ(linear.Evaluate(-3.0f), 0.0f);
    EXPECT_FLOAT_EQ(linear.Evaluate(7.0f), 1.0f);
}
