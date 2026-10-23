#include "SmartShape2D/ShapeGeometry.h"

#include "SmartShape2D/SmartShape.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Triangulation.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <numbers>

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

void AppendVertex(Mesh& mesh, Vector2 position, Vector2 uv, float depth)
{
    Vertex vertex{};
    vertex.Position[0] = position.x;
    vertex.Position[1] = position.y;
    vertex.Position[2] = depth;
    vertex.Normal[2] = -1.0f;
    vertex.Tangent[0] = 1.0f;
    vertex.Tangent[3] = -1.0f;
    vertex.TexCoords[0] = uv.x;
    vertex.TexCoords[1] = uv.y;
    mesh.Vertices.push_back(vertex);
}

void AppendQuad(Mesh& mesh, Vector2 a, Vector2 b, Vector2 c, Vector2 d,
                float startU, float endU, float depth, bool flip)
{
    const auto base = static_cast<uint32>(mesh.Vertices.size());
    const float top = flip ? 1.0f : 0.0f;
    const float bottom = 1.0f - top;
    AppendVertex(mesh, a, {startU, top}, depth);
    AppendVertex(mesh, b, {endU, top}, depth);
    AppendVertex(mesh, c, {endU, bottom}, depth);
    AppendVertex(mesh, d, {startU, bottom}, depth);
    const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross < 0.0f)
        mesh.Indices.insert(mesh.Indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    else
        mesh.Indices.insert(mesh.Indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
}

ShapePart PartForNormal(Vector2 normal)
{
    if (std::abs(normal.y) >= std::abs(normal.x))
        return normal.y >= 0.0f ? ShapePart::Top : ShapePart::Bottom;
    return normal.x >= 0.0f ? ShapePart::Right : ShapePart::Left;
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

void ComputeBounds(Mesh& mesh)
{
    if (mesh.Vertices.empty())
        return;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        mesh.MinBounds[axis] = mesh.Vertices.front().Position[axis];
        mesh.MaxBounds[axis] = mesh.MinBounds[axis];
        for (const auto& vertex : mesh.Vertices)
        {
            mesh.MinBounds[axis] = std::min(mesh.MinBounds[axis], vertex.Position[axis]);
            mesh.MaxBounds[axis] = std::max(mesh.MaxBounds[axis], vertex.Position[axis]);
        }
    }
}

void BuildFill(ShapeGeometry& result, const Components::SmartShape& settings, float area)
{
    auto& mesh = result.Parts[static_cast<size_t>(ShapePart::Fill)];
    const float angle = settings.FillRotation * std::numbers::pi_v<float> / 180.0f;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    for (const auto& point : result.Outline)
    {
        const Vector2 rotated{cosine * point.x - sine * point.y, sine * point.x + cosine * point.y};
        const Vector2 uv{rotated.x / settings.FillScale.x + settings.FillOffset.x,
                         -rotated.y / settings.FillScale.y + settings.FillOffset.y};
        AppendVertex(mesh, point, uv, 0.0f);
    }
    std::vector<uint32> polygon(result.Outline.size());
    std::iota(polygon.begin(), polygon.end(), 0u);
    if (area < 0.0f)
        std::reverse(polygon.begin(), polygon.end());
    Mathematics::EarClip(result.Outline, std::move(polygon), mesh.Indices);
    for (size_t i = 0; i < mesh.Indices.size(); i += 3)
        std::swap(mesh.Indices[i + 1], mesh.Indices[i + 2]);
}

bool MatchesNormal(Vector2 normal, const Components::SmartShapeEdge& rule)
{
    if (rule.NormalSweep == 0.0f || std::abs(rule.NormalSweep) >= 360.0f)
        return true;
    const float angle = std::atan2(normal.y, normal.x) * 180.0f / std::numbers::pi_v<float>;
    const float start = rule.NormalSweep < 0.0f ? rule.NormalStart + rule.NormalSweep : rule.NormalStart;
    float distance = std::fmod(angle - start, 360.0f);
    if (distance < 0.0f)
        distance += 360.0f;
    return distance <= std::abs(rule.NormalSweep);
}

void BuildEdges(ShapeGeometry& result, const Components::SmartShape& settings,
                const std::vector<float>& widths, const std::vector<uint32>& segments,
                std::span<const Components::SmartShapeEdge> overrides, bool closed, float area)
{
    const auto& points = result.Outline;
    const size_t count = points.size();
    const size_t edges = closed ? count : count - 1;
    const float side = closed && area > 0.0f ? -1.0f : 1.0f;
    std::vector<Vector2> normals(edges);
    std::vector<float> lengths(edges);
    float totalLength = 0.0f;
    for (size_t i = 0; i < edges; ++i)
    {
        const Vector2 delta = points[(i + 1) % count] - points[i];
        lengths[i] = delta.Length();
        normals[i] = Vector2{-delta.y, delta.x} * (side / lengths[i]);
        totalLength += lengths[i];
    }
    const float tileCount = std::max(1.0f, std::round(totalLength / settings.TileLength));
    const float tileLength = settings.StretchToFit ? totalLength / tileCount : settings.TileLength;
    std::vector<Vector2> offsets(count);
    for (size_t i = 0; i < count; ++i)
    {
        const Vector2 next = normals[std::min(i, edges - 1)];
        const Vector2 previous = normals[i == 0 ? (closed ? edges - 1 : 0) : i - 1];
        const Vector2 sum = previous + next;
        const float length = sum.Length();
        if (length <= kMinimumLength)
            offsets[i] = next;
        else
        {
            const Vector2 bisector = sum / length;
            const float denominator = std::max(kMinimumLength, Vector2::Dot(bisector, next));
            offsets[i] = bisector * std::min(settings.MiterLimit, 1.0f / denominator);
        }
        offsets[i] = offsets[i] * (settings.EdgeWidth * widths[i]);
    }
    float distance = 0.0f;
    for (size_t i = 0; i < edges; ++i)
    {
        const size_t next = (i + 1) % count;
        size_t part = static_cast<size_t>(PartForNormal(normals[i]));
        bool flip = settings.FlipEdges;
        bool visible = true;
        for (const bool normalRule : {false, true})
        {
            bool matched = false;
            for (size_t index = 0; index < overrides.size(); ++index)
            {
                const auto& edge = overrides[index];
                if (edge.UseNormalRange != normalRule ||
                    (normalRule ? !MatchesNormal(normals[i], edge) : edge.PointIndex != segments[i]))
                    continue;
                if (!edge.Material.IsNull())
                    part = static_cast<size_t>(ShapePart::Count) + index;
                flip = flip != edge.Flip;
                visible = edge.Visible;
                matched = true;
                break;
            }
            if (matched)
                break;
        }
        if (!visible)
        {
            distance += lengths[i];
            continue;
        }
        auto& mesh = result.Parts[part];
        const float outer = 0.5f + settings.EdgeOffset;
        const float inner = -0.5f + settings.EdgeOffset;
        AppendQuad(mesh, points[i] + offsets[i] * outer, points[next] + offsets[next] * outer,
                   points[next] + offsets[next] * inner, points[i] + offsets[i] * inner,
                   distance / tileLength, (distance + lengths[i]) / tileLength, settings.EdgeDepth, flip);
        distance += lengths[i];
    }
    // Corner sprites are optional overlays, using the same unit square convention as caps.
    for (size_t i = closed ? 0 : 1; i < (closed ? count : count - 1); ++i)
    {
        const auto previous = normals[(i + edges - 1) % edges];
        const auto next = normals[i % edges];
        const float turn = previous.x * next.y - previous.y * next.x;
        if (std::abs(turn) < 0.01f)
            continue;
        const bool outer = turn * side < 0.0f;
        const auto& material = outer ? settings.OuterCornerMaterial : settings.InnerCornerMaterial;
        if (material.IsNull())
            continue;
        const float halfWidth = settings.EdgeWidth * widths[i] * 0.5f;
        const Vector2 center = points[i] + offsets[i] * settings.EdgeOffset;
        auto& mesh = result.Parts[static_cast<size_t>(outer ? ShapePart::OuterCorner : ShapePart::InnerCorner)];
        AppendQuad(mesh, center + Vector2{-halfWidth, halfWidth}, center + Vector2{halfWidth, halfWidth},
                   center + Vector2{halfWidth, -halfWidth}, center + Vector2{-halfWidth, -halfWidth},
                   0.0f, 1.0f, settings.EdgeDepth * 2.0f, settings.FlipEdges);
    }
    if (closed)
        return;
    for (const bool start : {true, false})
    {
        if ((start ? settings.StartCapMaterial : settings.EndCapMaterial).IsNull())
            continue;
        const size_t point = start ? 0 : count - 1;
        const Vector2 normal = normals[start ? 0 : edges - 1];
        const Vector2 direction = (start ? points[0] - points[1] : points[count - 1] - points[count - 2]).Normalize();
        const float width = settings.EdgeWidth * widths[point];
        const Vector2 center = points[point] + normal * (settings.EdgeOffset * width);
        auto& mesh = result.Parts[static_cast<size_t>(start ? ShapePart::StartCap : ShapePart::EndCap)];
        AppendQuad(mesh, center + normal * (width * 0.5f), center + direction * width + normal * (width * 0.5f),
                   center + direction * width - normal * (width * 0.5f), center - normal * (width * 0.5f),
                   0.0f, 1.0f, settings.EdgeDepth, settings.FlipEdges);
    }
}
} // namespace

ShapeGeometry BuildShape(const Spline::SplineData& spline, const Components::SmartShape& settings,
                         std::span<const Components::SmartShapeEdge> overrides)
{
    ShapeGeometry result;
    result.Parts.resize(static_cast<size_t>(ShapePart::Count) + overrides.size());
    result.Materials = {settings.FillMaterial, settings.TopMaterial, settings.RightMaterial, settings.BottomMaterial,
                        settings.LeftMaterial, settings.InnerCornerMaterial, settings.OuterCornerMaterial,
                        settings.StartCapMaterial, settings.EndCapMaterial};
    for (const auto& edge : overrides)
        result.Materials.push_back(edge.Material);
    if (!spline.IsValid())
    {
        result.Error = "Add at least two spline points.";
        return result;
    }
    if (!IsFinite(settings.FillScale) || !IsFinite(settings.FillOffset) ||
        settings.FillScale.x <= kMinimumLength || settings.FillScale.y <= kMinimumLength ||
        !std::isfinite(settings.FillRotation) || !std::isfinite(settings.EdgeWidth) || settings.EdgeWidth <= 0.0f ||
        !std::isfinite(settings.TileLength) || settings.TileLength <= kMinimumLength ||
        !std::isfinite(settings.EdgeOffset) || !std::isfinite(settings.EdgeDepth) ||
        !std::isfinite(settings.MiterLimit) || settings.MiterLimit < 1.0f ||
        !std::isfinite(settings.CollisionDepth) || settings.CollisionDepth <= 0.0f ||
        settings.CurveSubdivisions < 1 || settings.CurveSubdivisions > 64)
    {
        result.Error = "Use finite dimensions, positive fill scales, width and tile length, a miter limit of at least 1, and 1–64 curve subdivisions.";
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
            !std::isfinite(edge.WidthScale) || edge.WidthScale <= 0.0f)
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
        BuildFill(result, settings, area);
    if (settings.EdgesEnabled)
        BuildEdges(result, settings, widths, sampleSegments, overrides, closed, area);
    for (auto& mesh : result.Parts)
        ComputeBounds(mesh);
    return result;
}
} // namespace GameEngine::SmartShape2D
