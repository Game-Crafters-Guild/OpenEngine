#include "SmartShape2D/EdgeQuad.h"

namespace GameEngine::SmartShape2D
{
using Mathematics::Vector2;

float EdgeQuad::Length() const { return ((D - A).Length() + (C - B).Length()) * 0.5f; }
float EdgeQuad::Height() const { return ((B - A).Length() + (C - D).Length()) * 0.5f; }

EdgeQuad MakeEdgeCorner(Vector2 previous, Vector2 point, Vector2 next,
    float width, float previousWidth, float offset, bool flip, QuadCorner corner)
{
    const auto incoming = (point - previous).Normalize();
    const auto outgoing = (next - point).Normalize();
    const float sign = flip ? -1.0f : 1.0f;
    const auto first = incoming * (width * 0.5f * sign);
    const auto second = outgoing * (previousWidth * 0.5f * sign);
    const auto shift = (incoming - outgoing) * (offset * 0.5f * sign);
    const float mirror = corner == QuadCorner::Inner ? -1.0f : 1.0f;
    EdgeQuad result;
    result.A = point + (first * -1.0f - second + shift) * mirror;
    result.B = point + (second - first + shift) * mirror;
    result.C = point + (first + second + shift) * mirror;
    result.D = point + (first - second + shift) * mirror;
    result.Corner = corner;
    return result;
}

void WeldEdgeQuads(EdgeQuad& previous, EdgeQuad& next)
{
    if (previous.IgnoreWeldNext)
        return;
    if (previous.Corner == QuadCorner::Outer)
    {
        next.A = previous.C;
        next.B = previous.B;
    }
    else if (previous.Corner == QuadCorner::Inner)
    {
        next.A = previous.D;
        next.B = previous.A;
    }
    else if (next.Corner == QuadCorner::Outer)
    {
        previous.D = next.A;
        previous.C = next.B;
    }
    else if (next.Corner == QuadCorner::Inner)
    {
        previous.D = next.D;
        previous.C = next.C;
    }
    else
    {
        const float height = (previous.Height() + next.Height()) * 0.5f;
        auto top = (previous.D + next.A) * 0.5f;
        auto bottom = (previous.C + next.B) * 0.5f;
        const auto center = (top + bottom) * 0.5f;
        const auto half = (bottom - center).Normalize() * (height * 0.5f);
        if (half.Length() > 0.0f)
        {
            bottom = center + half;
            top = center - half;
        }
        previous.D = next.A = top;
        previous.C = next.B = bottom;
    }
}

bool SplitEdgeTaper(EdgeQuad& edge, EdgeQuad& taper, Vector2 size, size_t part, bool end)
{
    if (size.x > edge.Length())
    {
        edge.Part = part;
        edge.TileLength = size.x;
        edge.Taper = true;
        return false;
    }
    taper = edge;
    taper.Part = part;
    taper.TileLength = size.x;
    taper.Stretch = true;
    taper.Taper = true;
    taper.Corner = QuadCorner::None;
    const auto direction = (edge.D - edge.A).Normalize();
    const Vector2 offset{direction.x * size.x, direction.y * size.y};
    if (end)
    {
        taper.A = taper.D - offset;
        taper.B = taper.C - offset;
        edge.D = taper.A;
        edge.C = taper.B;
    }
    else
    {
        taper.D = taper.A + offset;
        taper.C = taper.B + offset;
        edge.A = taper.D;
        edge.B = taper.C;
    }
    return true;
}
}
