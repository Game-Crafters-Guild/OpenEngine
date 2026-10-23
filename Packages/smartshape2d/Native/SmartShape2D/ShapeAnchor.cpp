#include "SmartShape2D/ShapeAnchor.h"

#include "Mathematics/MatrixOps.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace GameEngine::SmartShape2D
{
Mathematics::Matrix4x4 EvaluateAnchor(const Spline::SplineData& spline,
    const Components::SmartShapeAnchor& anchor, const Mathematics::Matrix4x4& parent)
{
    const uint32 segments = spline.GetSegmentCount();
    if (!spline.IsValid() || segments == 0 || !std::isfinite(anchor.PointOffset) || !std::isfinite(anchor.Rotation))
        return {};
    const float segment = static_cast<float>(anchor.PointIndex % segments);
    const float offset = std::clamp(anchor.PointOffset, 0.0f, 1.0f);
    const float count = static_cast<float>(segments);
    const auto position = parent.TransformPoint(Spline::EvaluatePosition(spline, (segment + offset) / count));
    const auto before = parent.TransformPoint(Spline::EvaluatePosition(spline, (segment + std::max(0.0f, offset - 0.1f)) / count));
    const auto after = parent.TransformPoint(Spline::EvaluatePosition(spline, (segment + std::min(1.0f, offset + 0.1f)) / count));
    const auto direction = after - before;
    const float angle = std::atan2(direction.y, direction.x) + anchor.Rotation * std::numbers::pi_v<float> / 180.0f;
    Mathematics::Vector3 scale{1.0f, 1.0f, 1.0f};
    if (anchor.UseShapeScale)
    {
        const auto origin = parent.TransformPoint({});
        scale = {(parent.TransformPoint({1.0f, 0.0f, 0.0f}) - origin).Length(),
                 (parent.TransformPoint({0.0f, 1.0f, 0.0f}) - origin).Length(),
                 (parent.TransformPoint({0.0f, 0.0f, 1.0f}) - origin).Length()};
    }
    const auto inverse = Mathematics::Inverse(parent);
    if (!std::all_of(inverse.Data(), inverse.Data() + 16, [](float value) { return std::isfinite(value); }))
        return {};
    return inverse * Mathematics::Matrix4x4::Translation(position) *
        Mathematics::MakeRotationZ(angle) * Mathematics::MakeScale(scale);
}
}
