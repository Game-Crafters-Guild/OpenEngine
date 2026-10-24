#pragma once
// A game's own Curve: the bare name finds it from the component, never Math::Curve.

namespace GameEngine
{
struct Curve
{
    float Value = 0.0f;
};
} // namespace GameEngine

namespace GameEngine::Components
{
struct FixtureBad
{
    Curve Shape;
};
} // namespace GameEngine::Components
