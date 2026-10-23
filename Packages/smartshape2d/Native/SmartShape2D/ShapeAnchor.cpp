#include "SmartShape2D/ShapeAnchor.h"

#include "Mathematics/MatrixOps.h"
#include "Mathematics/Interpolation.h"
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
    constexpr float kMinimumDirectionLengthSquared = 0.00000001f;
    const uint32 segments = spline.GetSegmentCount();
    if (!spline.IsValid() || segments == 0 || !std::isfinite(anchor.PointOffset) || !std::isfinite(anchor.Rotation))
        return {};
    const float segment = static_cast<float>(anchor.PointIndex % segments);
    const float offset = Math::Clamp01(anchor.PointOffset);
    const float count = static_cast<float>(segments);
    const auto position = parent.TransformPoint(Spline::EvaluatePosition(spline, (segment + offset) / count));
    const auto before = parent.TransformPoint(Spline::EvaluatePosition(spline, (segment + std::max(0.0f, offset - 0.1f)) / count));
    const auto after = parent.TransformPoint(Spline::EvaluatePosition(spline, (segment + std::min(1.0f, offset + 0.1f)) / count));
    auto direction = (after - before).Normalize();
    const auto origin = parent.TransformPoint({});
    const auto parentX = parent.TransformPoint({1.0f, 0.0f, 0.0f}) - origin;
    if (direction.LengthSquared() <= kMinimumDirectionLengthSquared)
        direction = parentX.Normalize();
    const auto parentY = parent.TransformPoint({0.0f, 1.0f, 0.0f}) - origin;
    auto normal = Mathematics::Vector3::Cross(parentX, parentY).Normalize();
    const auto parentZ = parent.TransformPoint({0.0f, 0.0f, 1.0f}) - origin;
    if (Mathematics::Vector3::Dot(normal, parentZ) < 0.0f)
        normal = normal * -1.0f;
    const auto perpendicular = Mathematics::Vector3::Cross(normal, direction).Normalize();
    const float angle = anchor.Rotation * std::numbers::pi_v<float> / 180.0f;
    const auto right = direction * std::cos(angle) + perpendicular * std::sin(angle);
    const auto up = perpendicular * std::cos(angle) - direction * std::sin(angle);
    Mathematics::Matrix4x4 basis;
    basis[0] = {right.x, right.y, right.z, 0.0f};
    basis[1] = {up.x, up.y, up.z, 0.0f};
    basis[2] = {normal.x, normal.y, normal.z, 0.0f};
    basis[3] = {position.x, position.y, position.z, 1.0f};
    Mathematics::Vector3 scale{1.0f, 1.0f, 1.0f};
    if (anchor.UseShapeScale)
    {
        scale = {parentX.Length(), parentY.Length(), parentZ.Length()};
    }
    const auto inverse = Mathematics::Inverse(parent);
    if (!std::all_of(inverse.Data(), inverse.Data() + 16, [](float value) { return std::isfinite(value); }))
        return {};
    return inverse * basis * Mathematics::MakeScale(scale);
}
}
