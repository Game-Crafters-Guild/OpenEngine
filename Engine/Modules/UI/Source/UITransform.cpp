#include "UI/UITransform.h"

#include "Mathematics/MatrixOps.h"
#include "UI/ResolvedStyle.h"

#include <numbers>

namespace GameEngine::UI
{

namespace
{

constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180.0f;

float ResolveLength(const StyleLength& length, float percentBasis)
{
    return length.IsPercent() ? length.Value * 0.01f * percentBasis : length.Value;
}

Mathematics::Matrix3x3 OpMatrix(const TransformOp& op, float w, float h, float unitsPerCssPx)
{
    const float* a = op.Args;
    switch (op.Function)
    {
    case TransformFunction::Translate:
        return Mathematics::MakeTranslation2D(Mathematics::Vector2(ResolveLength(op.TranslateX, w) * unitsPerCssPx,
                                                                   ResolveLength(op.TranslateY, h) * unitsPerCssPx));
    case TransformFunction::Scale:
        return Mathematics::MakeScale2D(Mathematics::Vector2(a[0], a[1]));
    case TransformFunction::Rotate:
        return Mathematics::MakeRotation2D(a[0] * kRadiansPerDegree);
    case TransformFunction::Skew:
        return Mathematics::MakeSkew2D(a[0] * kRadiansPerDegree, a[1] * kRadiansPerDegree);
    case TransformFunction::Matrix:
        return Mathematics::MakeAffine2D(a[0], a[1], a[2], a[3], a[4] * unitsPerCssPx, a[5] * unitsPerCssPx);
    }
    return Mathematics::Matrix3x3::Identity();
}

} // namespace

bool ComputeElementTransform(const VisualStyle& vs, const Mathematics::Rect& borderBox, float unitsPerCssPx,
                             Mathematics::Matrix3x3& out)
{
    const TransformValue& tv = vs.Transform;
    if (tv.IsNone())
        return false;

    Mathematics::Matrix3x3 m = Mathematics::Matrix3x3::Identity();
    for (const TransformOp& op : tv.Ops())
        m = m * OpMatrix(op, borderBox.Width, borderBox.Height, unitsPerCssPx);

    const Mathematics::Vector2 origin(
        (borderBox.X + ResolveLength(vs.TransformOrigin.X, borderBox.Width)) * unitsPerCssPx,
        (borderBox.Y + ResolveLength(vs.TransformOrigin.Y, borderBox.Height)) * unitsPerCssPx);
    out = Mathematics::MakeTranslation2D(origin) * m * Mathematics::MakeTranslation2D(Mathematics::Vector2(-origin.x, -origin.y));
    return true;
}

} // namespace GameEngine::UI
