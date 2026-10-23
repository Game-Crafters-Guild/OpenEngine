#include "SmartShape2D/ShapeGeometry.h"

#include "SmartShape2D/SmartShape.h"
#include "SmartShape2D/ShapeEdges.h"
#include "SmartShape2D/ShapeMesh.h"
#include "SmartShape2D/ShapeOffset.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Triangulation.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <numbers>
#include <tuple>

namespace GameEngine::SmartShape2D
{
namespace
{
using Mathematics::Vector2;
constexpr float kMinimumLength = 0.00001f;
constexpr size_t kMaximumSamples = 4096;

bool IsFinite(Vector2 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool HasCrossings(const std::vector<Vector2>& points, bool closed)
{
    const size_t edges = closed ? points.size() : points.size() - 1;
    for (size_t i = 0; i < edges; ++i)
    {
        for (size_t j = i + 2; j < edges; ++j)
        {
            if (closed && i == 0 && j + 1 == edges)
                continue;
            if (Mathematics::SegmentsIntersect(points[i], points[(i + 1) % points.size()],
                                               points[j], points[(j + 1) % points.size()]))
                return true;
        }
    }
    return false;
}

void BuildFill(ShapeGeometry& result, const Components::SmartShape& settings, const Mathematics::Matrix4x4& world)
{
    auto& mesh = result.Parts[static_cast<size_t>(ShapePart::Fill)];
    const float parentAngle = settings.FillAbsoluteRotation ? 0.0f : std::atan2(world.Data()[1], world.Data()[0]);
    const float angle = -settings.FillRotation * std::numbers::pi_v<float> / 180.0f - parentAngle;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const auto fill = OffsetShapePolygon(result.Outline, settings.FillMeshOffset);
    for (const auto& point : fill)
    {
        auto positioned = world.TransformPoint({point.x, point.y, 0.0f});
        if (!settings.FillAbsolutePosition)
            positioned = positioned - world.TransformPoint({});
        const Vector2 rotated{cosine * positioned.x - sine * positioned.y, sine * positioned.x + cosine * positioned.y};
        const Vector2 uv{(rotated.x - settings.FillOffset.x) / settings.FillScale.x,
                         -(rotated.y - settings.FillOffset.y) / settings.FillScale.y};
        AppendShapeVertex(mesh, point, uv, settings.FillDepth);
    }
    std::vector<uint32> polygon(fill.size());
    std::iota(polygon.begin(), polygon.end(), 0u);
    if (Mathematics::PolygonDoubledSignedArea(fill) < 0.0f)
        std::reverse(polygon.begin(), polygon.end());
    Mathematics::EarClip(fill, std::move(polygon), mesh.Indices);
    for (size_t i = 0; i < mesh.Indices.size(); i += 3)
        std::swap(mesh.Indices[i + 1], mesh.Indices[i + 2]);
}

} // namespace

ShapeGeometry BuildShape(const Spline::SplineData& spline, const Components::SmartShape& settings,
                         std::span<const Components::SmartShapeEdge> overrides,
                         std::span<const Components::SmartShapeTexture> textures,
                         const Mathematics::Matrix4x4& world)
{
    ShapeGeometry result;
    result.Parts.resize(static_cast<size_t>(ShapePart::Count) + overrides.size());
    result.Materials = {settings.FillMaterial, settings.TopMaterial, settings.RightMaterial, settings.BottomMaterial,
                        settings.LeftMaterial, settings.InnerCornerMaterial, settings.OuterCornerMaterial,
                        settings.StartCapMaterial, settings.EndCapMaterial, {}, {}};
    for (size_t part = 0; part < static_cast<size_t>(ShapePart::Count); ++part)
        result.Kinds.push_back(static_cast<ShapePart>(part));
    for (const auto& edge : overrides)
    {
        result.Materials.push_back(edge.Material);
        result.Kinds.push_back(ShapePart::Top);
    }
    std::vector<std::tuple<uint32, int32, uint32>> textureKeys;
    textureKeys.reserve(textures.size());
    for (const auto& texture : textures)
        textureKeys.emplace_back(texture.TextureSet, texture.PointIndex, texture.Index);
    std::sort(textureKeys.begin(), textureKeys.end());
    if (std::adjacent_find(textureKeys.begin(), textureKeys.end()) != textureKeys.end())
    {
        result.Error = "Give each texture entry a unique Index within its Texture Set and Point Index.";
        return result;
    }
    for (const auto& texture : textures)
    {
        if (!std::isfinite(texture.TileLength) || texture.TileLength <= kMinimumLength ||
            !std::isfinite(texture.WidthScale) || texture.WidthScale <= 0.0f ||
            !IsFinite(texture.StartTaperSize) || texture.StartTaperSize.x <= 0.0f || texture.StartTaperSize.y <= 0.0f ||
            !IsFinite(texture.EndTaperSize) || texture.EndTaperSize.x <= 0.0f || texture.EndTaperSize.y <= 0.0f)
        {
            result.Error = "Texture entries need positive finite width, tile length and taper lengths.";
            return result;
        }
        result.Materials.insert(result.Materials.end(), {{}, texture.Material, texture.Material, texture.Material,
            texture.Material, texture.InnerCornerMaterial, texture.OuterCornerMaterial, texture.StartTaperMaterial,
            texture.EndTaperMaterial, texture.SharpStartTaperMaterial, texture.SharpEndTaperMaterial});
        for (size_t part = 0; part < static_cast<size_t>(ShapePart::Count); ++part)
            result.Kinds.push_back(static_cast<ShapePart>(part));
    }
    result.Parts.resize(result.Materials.size());
    if (!spline.IsValid())
    {
        result.Error = "Add at least two spline points.";
        return result;
    }
    if (!IsFinite(settings.FillScale) || !IsFinite(settings.FillOffset) ||
        settings.FillScale.x <= kMinimumLength || settings.FillScale.y <= kMinimumLength ||
        !std::isfinite(settings.FillRotation) || !std::isfinite(settings.FillDepth) || !std::isfinite(settings.FillMeshOffset) || !std::isfinite(settings.EdgeWidth) || settings.EdgeWidth <= 0.0f ||
        !std::isfinite(settings.TileLength) || settings.TileLength <= kMinimumLength ||
        !std::isfinite(settings.EdgeOffset) || !std::isfinite(settings.EdgeDepth) ||
        !IsFinite(settings.StartTaperSize) || settings.StartTaperSize.x <= 0.0f || settings.StartTaperSize.y <= 0.0f ||
        !IsFinite(settings.EndTaperSize) || settings.EndTaperSize.x <= 0.0f || settings.EndTaperSize.y <= 0.0f ||
        !std::isfinite(settings.CollisionDepth) || settings.CollisionDepth <= 0.0f ||
        settings.CurveSubdivisions < 1 || settings.CurveSubdivisions > 64)
    {
        result.Error = "Use finite dimensions, positive fill scales, width and tile length, 1–64 curve subdivisions.";
        return result;
    }
    for (const auto& point : spline.Points)
    {
        if (!IsFinite({point.Position.x, point.Position.y}) || !std::isfinite(point.Position.z) ||
            std::abs(point.Position.z) > kMinimumLength || !std::isfinite(point.Radius) || point.Radius <= 0.0f ||
            !IsFinite({point.TangentIn.x, point.TangentIn.y}) || !IsFinite({point.TangentOut.x, point.TangentOut.y}) ||
            !std::isfinite(point.TangentIn.z) || !std::isfinite(point.TangentOut.z) ||
            std::abs(point.TangentIn.z) > kMinimumLength || std::abs(point.TangentOut.z) > kMinimumLength)
        {
            result.Error = "Keep spline points and handles in local XY (Z = 0), with finite coordinates and positive point radii.";
            return result;
        }
    }
    std::vector<float> widthScales(spline.Points.size(), 1.0f);
    std::vector<bool> overridden(spline.Points.size(), false);
    for (const auto& edge : overrides)
    {
        if ((!edge.UseNormalRange && (edge.PointIndex >= spline.Points.size() || overridden[edge.PointIndex])) ||
            !std::isfinite(edge.NormalStart) || !std::isfinite(edge.NormalSweep) ||
            !std::isfinite(edge.WidthScale) || edge.WidthScale <= 0.0f ||
            !std::isfinite(edge.Offset) || !std::isfinite(edge.DepthOffset))
        {
            result.Error = "Use one edge override per existing point, with a positive finite width scale.";
            return result;
        }
        if (!edge.UseNormalRange)
        {
            overridden[edge.PointIndex] = true;
            widthScales[edge.PointIndex] = edge.WidthScale;
        }
    }
    const bool closed = spline.IsEffectivelyClosed();
    const uint32 segments = spline.GetSegmentCount();
    const uint32 subdivisions = spline.Type == Spline::SplineType::Linear ? 1u : settings.CurveSubdivisions;
    if (segments > (kMaximumSamples - 1) / subdivisions)
    {
        result.Error = "Reduce curve subdivisions or split the shape: the sampled outline exceeds 4096 points.";
        return result;
    }
    const uint32 samples = segments * subdivisions;
    std::vector<float> widths;
    std::vector<uint32> sampleSegments;
    for (uint32 i = 0; i < samples + (closed ? 0u : 1u); ++i)
    {
        const float parameter = static_cast<float>(i) / static_cast<float>(samples);
        const auto position = Spline::EvaluatePosition(spline, parameter);
        const Vector2 point{position.x, position.y};
        if (!IsFinite(point))
        {
            result.Error = "The curve produced non-finite coordinates; reduce point and handle magnitudes.";
            result.Outline.clear();
            return result;
        }
        if (!result.Outline.empty() && (point - result.Outline.back()).Length() <= kMinimumLength)
            continue;
        result.Outline.push_back(point);
        const uint32 segment = std::min(i / subdivisions, segments - 1);
        const float fraction = i == samples ? 1.0f : static_cast<float>(i % subdivisions) / static_cast<float>(subdivisions);
        const size_t next = (segment + 1) % spline.Points.size();
        widths.push_back(Math::Lerp(spline.Points[segment].Radius * widthScales[segment],
                                  spline.Points[next].Radius * widthScales[next], fraction));
        sampleSegments.push_back(segment);
    }
    if (closed && result.Outline.size() > 1 && (result.Outline.front() - result.Outline.back()).Length() <= kMinimumLength)
    {
        result.Outline.pop_back();
        widths.pop_back();
        sampleSegments.pop_back();
    }
    if (result.Outline.size() < (closed ? 3u : 2u) || HasCrossings(result.Outline, closed))
    {
        result.Error = "Separate overlapping points and remove outline crossings.";
        return result;
    }
    const float area = closed ? Mathematics::PolygonDoubledSignedArea(result.Outline) * 0.5f : 0.0f;
    if (closed && std::abs(area) <= kMinimumLength)
    {
        result.Error = "Move the points so the closed shape encloses an area.";
        return result;
    }
    if (closed && settings.FillEnabled)
        BuildFill(result, settings, world);
    if (settings.EdgesEnabled)
        BuildShapeEdges(result, settings, widths, sampleSegments, overrides, textures, spline, area);
    for (auto& mesh : result.Parts)
    {
        ComputeShapeBounds(mesh);
        GenerateMeshTangents(mesh, true);
    }
    return result;
}
} // namespace GameEngine::SmartShape2D
