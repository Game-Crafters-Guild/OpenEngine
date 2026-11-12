#pragma once

#include <algorithm>

namespace GameEngine
{
namespace Mathematics
{

/// Axis-aligned rectangle. Coordinates are in the caller's space
/// (UI, graph, screen — the type does not pick one).
struct Rect
{
    float X = 0.0f;
    float Y = 0.0f;
    float Width = 0.0f;
    float Height = 0.0f;

    bool operator==(const Rect&) const = default;

    float Right() const { return X + Width; }
    float Bottom() const { return Y + Height; }

    /// For callers that think in edges rather than origin and size.
    static Rect FromEdges(float left, float top, float right, float bottom)
    {
        return Rect{left, top, right - left, bottom - top};
    }

    /// True when interiors intersect. Edges that only touch do not overlap.
    bool Overlaps(const Rect& other) const
    {
        return X < other.Right() && Right() > other.X &&
               Y < other.Bottom() && Bottom() > other.Y;
    }

    /// True when the rect has no area: a width or height of zero, negative or NaN.
    bool IsEmpty() const { return !(Width > 0.0f && Height > 0.0f); }

    /// The area both rects cover. Empty (IsEmpty) when they do not overlap.
    Rect Intersection(const Rect& other) const
    {
        return FromEdges(std::max(X, other.X), std::max(Y, other.Y),
                         std::min(Right(), other.Right()), std::min(Bottom(), other.Bottom()));
    }

    /// Grow (or shrink, if margin is negative) equally on every side.
    Rect Inflated(float margin) const
    {
        return Rect{X - margin, Y - margin, Width + 2.0f * margin, Height + 2.0f * margin};
    }
};

} // namespace Mathematics
} // namespace GameEngine

