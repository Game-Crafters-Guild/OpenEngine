#include "SmartShape2D/ShapeOffset.h"

#include "clipper2/clipper.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SmartShape2D
{
namespace
{
std::vector<Mathematics::Vector2> Inflate(std::span<const Mathematics::Vector2> outline,
    float distance, Clipper2Lib::EndType end)
{
    constexpr double kCoordinateLimit = static_cast<double>(Clipper2Lib::MAX_COORD) / 4000000.0;
    if (!std::isfinite(distance) || std::abs(distance) > kCoordinateLimit)
        return {};
    for (const auto& point : outline)
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            std::abs(point.x) > kCoordinateLimit || std::abs(point.y) > kCoordinateLimit)
            return {};
    }
    Clipper2Lib::PathD path;
    path.reserve(outline.size());
    for (const auto& point : outline)
        path.emplace_back(point.x, point.y);
    const auto contours = Clipper2Lib::InflatePaths({path}, distance, Clipper2Lib::JoinType::Miter, end, 2.0, 6);
    std::vector<Mathematics::Vector2> result;
    if (!contours.empty())
    {
        result.reserve(contours.front().size());
        for (const auto& point : contours.front())
            result.push_back({static_cast<float>(point.x), static_cast<float>(point.y)});
    }
    return result;
}
}

std::vector<Mathematics::Vector2> OffsetShapePolygon(std::span<const Mathematics::Vector2> outline, float distance)
{
    if (distance == 0.0f)
        return {outline.begin(), outline.end()};
    return Inflate(outline, distance, Clipper2Lib::EndType::Polygon);
}

std::vector<Mathematics::Vector2> OffsetShapePath(std::span<const Mathematics::Vector2> outline, float distance)
{
    using Mathematics::Vector2;
    if (outline.size() < 2 || distance == 0.0f)
        return {outline.begin(), outline.end()};
    std::vector<Vector2> result;
    result.reserve(outline.size() * 2);
    auto direction = outline[1] - outline[0];
    auto normal = Vector2{-direction.y, direction.x}.Normalize();
    result.push_back(outline.front() + normal * distance);
    for (size_t index = 1; index + 1 < outline.size(); ++index)
    {
        const auto nextDirection = outline[index + 1] - outline[index];
        const auto nextNormal = Vector2{-nextDirection.y, nextDirection.x}.Normalize();
        const auto bisector = (normal + nextNormal).Normalize();
        const float denominator = std::max(Vector2::Dot(bisector, normal), 0.000001f);
        const bool cap = distance * Vector2::Dot(normal, nextDirection) < 0.0f && denominator < 0.5f;
        if (cap)
        {
            const auto top = outline[index] + bisector * distance;
            const Vector2 perpendicular{-bisector.y, bisector.x};
            const auto rhs = top - (outline[index] + normal * distance);
            const float determinant = -direction.x * perpendicular.y + perpendicular.x * direction.y;
            const float parameter = (direction.x * rhs.y - direction.y * rhs.x) / determinant;
            result.push_back(top + perpendicular * parameter);
            result.push_back(top - perpendicular * parameter);
        }
        else
            result.push_back(outline[index] + bisector * (distance / denominator));
        direction = nextDirection;
        normal = nextNormal;
    }
    result.push_back(outline.back() + normal * distance);
    return result;
}

std::vector<Mathematics::Vector2> BufferShapePath(std::span<const Mathematics::Vector2> outline, float radius)
{
    return Inflate(outline, radius, Clipper2Lib::EndType::Butt);
}
}
