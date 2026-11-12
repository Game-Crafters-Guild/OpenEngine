#include <cmath>

#include <gtest/gtest.h>

#include "Components/Animation/ValueCurve.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/ValueCurveSystem.h"

using namespace GameEngine;
using namespace GameEngine::Components;

namespace
{
Math::CurveKey MakeKey(float t, float v, Math::CurveInterp interp = Math::CurveInterp::Linear,
                       Math::CurveTangentMode mode = Math::CurveTangentMode::Auto)
{
    Math::CurveKey key;
    key.Time = t;
    key.Value = v;
    key.Interp = interp;
    key.TangentMode = mode;
    return key;
}
} // namespace

TEST(ValueCurveEvaluator, LinearShapeIsPolyline)
{
    ValueCurve curve;
    curve.Shape = Math::Curve{};
    curve.Shape.TryInsert(MakeKey(0.0f, 0.0f));
    curve.Shape.TryInsert(MakeKey(0.5f, 0.5f));
    curve.Shape.TryInsert(MakeKey(1.0f, 1.0f));
    EXPECT_NEAR(EvaluateValueCurve01(curve, 0.25f), 0.25f, 1.0e-5f);
    EXPECT_NEAR(EvaluateValueCurve01(curve, 0.75f), 0.75f, 1.0e-5f);
}

TEST(ValueCurveEvaluator, StepShapeHolds)
{
    ValueCurve curve;
    curve.Shape = Math::Curve{};
    curve.Shape.TryInsert(MakeKey(0.0f, 0.0f, Math::CurveInterp::Step));
    curve.Shape.TryInsert(MakeKey(1.0f, 1.0f, Math::CurveInterp::Step));
    EXPECT_NEAR(EvaluateValueCurve01(curve, 0.5f), 0.0f, 1.0e-5f);
    EXPECT_NEAR(EvaluateValueCurve01(curve, 0.999f), 0.0f, 1.0e-5f);
}

TEST(ValueCurveEvaluator, SmoothFlatEndpointsTraceSmoothstep)
{
    ValueCurve curve;
    curve.Shape = Math::Curve{};
    curve.Shape.TryInsert(MakeKey(0.0f, 0.0f, Math::CurveInterp::Smooth, Math::CurveTangentMode::Flat));
    curve.Shape.TryInsert(MakeKey(1.0f, 1.0f, Math::CurveInterp::Smooth, Math::CurveTangentMode::Flat));
    // Flat tangents at both ends produce an S: midpoint stays 0.5, quarter dips below, three-quarter rises above.
    EXPECT_NEAR(EvaluateValueCurve01(curve, 0.5f), 0.5f, 1.0e-4f);
    EXPECT_LT(EvaluateValueCurve01(curve, 0.25f), 0.25f);
    EXPECT_GT(EvaluateValueCurve01(curve, 0.75f), 0.75f);
}

TEST(ValueCurveEvaluator, DefaultShapeIsLinearRamp)
{
    ValueCurve curve; // default Shape is the 0 -> 1 ramp
    EXPECT_NEAR(EvaluateValueCurve01(curve, 0.0f), 0.0f, 1.0e-5f);
    EXPECT_NEAR(EvaluateValueCurve01(curve, 0.5f), 0.5f, 1.0e-5f);
    EXPECT_NEAR(EvaluateValueCurve01(curve, 1.0f), 1.0f, 1.0e-5f);
}

TEST(ValueCurveEvaluator, BuiltinEasingSourceUsesAnalyticEasing)
{
    ValueCurve curve;
    curve.Shape = Math::Curve{};
    curve.Shape.TryInsert(MakeKey(0.0f, 0.0f));
    curve.Shape.TryInsert(MakeKey(1.0f, 1.0f));
    curve.UseBuiltinEasing = true;
    curve.BuiltinEasing = Math::TweenEasing::QuadIn;

    EXPECT_NEAR(EvaluateValueCurve01(curve, 0.5f),
                Math::EvalTweenEasing(Math::TweenEasing::QuadIn, 0.5f), 1.0e-5f);
}

TEST(ValueCurveEvaluator, CubicBezierModeUsesDedicatedHandles)
{
    ValueCurve curve;
    curve.Shape = Math::Curve{};
    curve.Shape.TryInsert(MakeKey(0.0f, 1.0f));
    curve.Shape.TryInsert(MakeKey(1.0f, 1.0f));
    curve.ShapeMode = ValueCurveShapeMode::CubicBezier;
    curve.CubicBezierAnchorStartY = 0.0f;
    curve.CubicBezierAnchorEndY = 1.0f;
    curve.CubicBezierControl1X = 0.0f;
    curve.CubicBezierControl1Y = 0.0f;
    curve.CubicBezierControl2X = 1.0f;
    curve.CubicBezierControl2Y = 1.0f;

    EXPECT_NEAR(EvaluateValueCurve01(curve, 0.0f), 0.0f, 1.0e-5f);
    EXPECT_NEAR(EvaluateValueCurve01(curve, 0.5f), 0.5f, 1.0e-3f);
    EXPECT_NEAR(EvaluateValueCurve01(curve, 1.0f), 1.0f, 1.0e-5f);
}

TEST(ValueCurveEvaluator, RemapsStartToEndAcrossDuration)
{
    ValueCurve curve;
    curve.StartValue = 10.0f;
    curve.EndValue = 20.0f;
    curve.DurationSeconds = 2.0f;
    EXPECT_NEAR(EvaluateValueCurveValue(curve, 0.0f), 10.0f, 1.0e-3f);
    EXPECT_NEAR(EvaluateValueCurveValue(curve, 1.0f), 15.0f, 1.0e-3f);
    EXPECT_NEAR(EvaluateValueCurveValue(curve, 2.0f), 20.0f, 1.0e-3f);
    // Clamps past the end when not looping.
    EXPECT_NEAR(EvaluateValueCurveValue(curve, 5.0f), 20.0f, 1.0e-3f);
}

TEST(ValueCurveEvaluator, LoopWraps)
{
    ValueCurve curve;
    curve.Loop = true;
    curve.DurationSeconds = 1.0f;
    EXPECT_NEAR(EvaluateValueCurveValue(curve, 1.25f), 0.25f, 1.0e-3f);
    EXPECT_NEAR(EvaluateValueCurveValue(curve, 3.5f), 0.5f, 1.0e-3f);
}

TEST(ValueCurveEvaluator, PingPongReverses)
{
    ValueCurve curve;
    curve.Loop = true;
    curve.PingPong = true;
    curve.DurationSeconds = 1.0f;
    // 1.5 cycles: forward to 1.0 then back to 0.5.
    EXPECT_NEAR(EvaluateValueCurveValue(curve, 1.5f), 0.5f, 1.0e-3f);
}

TEST(ValueCurveEvaluator, DisabledReturnsStartValue)
{
    ValueCurve curve;
    curve.Enabled = false;
    curve.StartValue = 7.0f;
    curve.EndValue = 99.0f;
    EXPECT_NEAR(EvaluateValueCurveValue(curve, 0.5f), 7.0f, 1.0e-5f);
}

// The scheduler hands the curve system a delta of 0 while the world clock is paused, so a
// playing curve holds its elapsed time.
TEST(ValueCurveSystem, APausedClockHoldsThePlayingCurve)
{
    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    ValueCurve curve{};
    curve.Loop = true;
    world.AddComponentImmediate(entity, curve);
    ECS::SystemManager systems;
    systems.AddSystem<Engine::Renderer::ValueCurveSystem>();
    systems.Update(world, 0.1f);
    const float32 held = world.GetComponent<ValueCurve>(entity)->ElapsedSeconds;
    ASSERT_GT(held, 0.0f);

    world.GetSimulationClock().SetTimeScale(0.0);
    for (int frame = 0; frame < 5; ++frame)
        systems.Update(world, 0.1f);
    EXPECT_EQ(world.GetComponent<ValueCurve>(entity)->ElapsedSeconds, held) << "a paused clock holds the curve";
}
