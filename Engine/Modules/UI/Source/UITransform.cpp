#include "UI/UITransform.h"

#include <cmath>

namespace GameEngine::UI
{

bool Affine2D::Inverse(Affine2D& out) const
{
    const float det = A * D - B * C;
    if (std::abs(det) < 1e-12f)
        return false;
    const float inv = 1.0f / det;
    out.A = D * inv;
    out.B = -B * inv;
    out.C = -C * inv;
    out.D = A * inv;
    out.E = (C * F - D * E) * inv;
    out.F = (B * E - A * F) * inv;
    return true;
}

namespace
{

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

Affine2D OpMatrix(const TransformOp& op, float w, float h, float scale)
{
    const float* a = op.Args;
    switch (op.Function)
    {
    case TransformFunction::Translate:
    {
        const float tx = op.XIsPercent ? a[0] * 0.01f * w : a[0];
        const float ty = op.YIsPercent ? a[1] * 0.01f * h : a[1];
        return Affine2D::Translation(tx * scale, ty * scale);
    }
    case TransformFunction::Scale:
        return {a[0], 0.0f, 0.0f, a[1], 0.0f, 0.0f};
    case TransformFunction::Rotate:
    {
        // Positive angles turn clockwise on a y-down screen, as in CSS.
        const float c = std::cos(a[0] * kDegToRad);
        const float s = std::sin(a[0] * kDegToRad);
        return {c, s, -s, c, 0.0f, 0.0f};
    }
    case TransformFunction::Skew:
        return {1.0f, std::tan(a[1] * kDegToRad), std::tan(a[0] * kDegToRad), 1.0f, 0.0f, 0.0f};
    case TransformFunction::Matrix:
        return {a[0], a[1], a[2], a[3], a[4] * scale, a[5] * scale};
    }
    return {};
}

} // namespace

bool ComputeElementTransform(const VisualStyle& vs, float x, float y, float w, float h, float scale,
                             Affine2D& out)
{
    const TransformValue& tv = vs.Transform;
    if (tv.IsNone())
        return false;

    Affine2D m{};
    for (uint8_t i = 0; i < tv.Count; ++i)
        m = m * OpMatrix(tv.Ops[i], w, h, scale);

    const TransformOriginValue& o = vs.TransformOrigin;
    const float ox = (x + (o.XIsPercent ? o.X * 0.01f * w : o.X)) * scale;
    const float oy = (y + (o.YIsPercent ? o.Y * 0.01f * h : o.Y)) * scale;
    out = Affine2D::Translation(ox, oy) * m * Affine2D::Translation(-ox, -oy);
    return true;
}

} // namespace GameEngine::UI
