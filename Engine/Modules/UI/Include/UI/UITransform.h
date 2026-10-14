#pragma once

#include "UI/ResolvedStyle.h"

namespace GameEngine::UI
{

// A 2D affine map in CSS matrix(a, b, c, d, e, f) order:
//   x' = A x + C y + E
//   y' = B x + D y + F
struct Affine2D
{
    float A = 1.0f, B = 0.0f, C = 0.0f, D = 1.0f, E = 0.0f, F = 0.0f;

    static Affine2D Translation(float x, float y) { return {1.0f, 0.0f, 0.0f, 1.0f, x, y}; }

    // this ∘ rhs: applies rhs first. CSS composes a transform list this way,
    // left to right.
    Affine2D operator*(const Affine2D& rhs) const
    {
        return {A * rhs.A + C * rhs.B,
                B * rhs.A + D * rhs.B,
                A * rhs.C + C * rhs.D,
                B * rhs.C + D * rhs.D,
                A * rhs.E + C * rhs.F + E,
                B * rhs.E + D * rhs.F + F};
    }

    void Apply(float& x, float& y) const
    {
        const float nx = A * x + C * y + E;
        y = B * x + D * y + F;
        x = nx;
    }

    // False (and `out` untouched) when the map is singular, as scale(0) is.
    bool Inverse(Affine2D& out) const;
};

// The element's own transform: the map from its untransformed layout space
// into the space its parent paints in, pivoting about transform-origin.
// `x, y, w, h` is the border box in CSS logical px (the layout rect);
// `scale` is the output unit per CSS px — the content scale for physical
// pixels (paint), 1 for logical (hit-testing). Percentages in translate()
// and transform-origin resolve against w and h. False for `transform: none`.
bool ComputeElementTransform(const VisualStyle& vs, float x, float y, float w, float h, float scale,
                             Affine2D& out);

} // namespace GameEngine::UI
