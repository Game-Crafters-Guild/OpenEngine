#include "SmartShape2D/ShapeEdges.h"

#include "SmartShape2D/EdgeQuad.h"
#include "SmartShape2D/ShapeMesh.h"
#include "SmartShape2D/SmartShape.h"
#include "Mathematics/Geometry.h"
#include "Spline/SplineData.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

namespace GameEngine::SmartShape2D
{
using Mathematics::Vector2;
namespace
{
constexpr float kMinimumLength = 0.00001f;

ShapePart PartForNormal(Vector2 normal)
{
    if (std::abs(normal.y) >= std::abs(normal.x))
        return normal.y >= 0.0f ? ShapePart::Top : ShapePart::Bottom;
    return normal.x >= 0.0f ? ShapePart::Right : ShapePart::Left;
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

void BuildEdgeLayer(ShapeGeometry& result, const Components::SmartShape& settings,
                const std::vector<float>& widths, const std::vector<uint32>& segments,
                std::span<const Components::SmartShapeEdge> overrides,
                std::span<const Components::SmartShapeTexture> textures, bool closed, float area,
                const std::vector<const Components::SmartShapeEdge*>& rules, const std::vector<bool>& active)
{
    const auto& points = result.Outline;
    const size_t count = points.size();
    const size_t edges = closed ? count : count - 1;
    const bool reverse = (closed ? area < 0.0f : true) != settings.FlipEdges;
    const float side = reverse ? 1.0f : -1.0f;
    constexpr size_t kParts = static_cast<size_t>(ShapePart::Count);
    std::mt19937 random(settings.TextureSeed);
    std::vector<EdgeQuad> quads;
    std::vector<size_t> choices;
    size_t closingTaperPart = 0;
    Vector2 closingTaperSize{};
    // Rules own contiguous runs. End tapers and UV fitting restart at a rule boundary
    // or hidden gap, independently of the texture variant selected for each quad.
    std::vector<uint32> groups(edges);
    uint32 group = 0;
    for (size_t index = 1; index < edges; ++index)
    {
        if (rules[index] != rules[index - 1] || active[index] != active[index - 1])
            ++group;
        groups[index] = group;
    }
    if (closed && rules.front() == rules.back() && active.front() == active.back())
    {
        for (size_t index = edges; index > 0 && groups[index - 1] == group; --index)
            groups[index - 1] = 0;
    }
    size_t firstEdge = 0;
    if (closed)
    {
        for (size_t index = 0; index < edges; ++index)
        {
            const size_t previous = (index + edges - 1) % edges;
            if (active[index] && (!active[previous] || rules[index] != rules[previous]))
            {
                firstEdge = index;
                break;
            }
        }
    }
    for (size_t step = 0; step < edges; ++step)
    {
        const size_t index = (firstEdge + step) % edges;
        const size_t next = (index + 1) % count;
        const auto delta = points[next] - points[index];
        const auto normal = Vector2{-delta.y, delta.x} * (side / delta.Length());
        size_t part = static_cast<size_t>(PartForNormal(normal));
        const auto* rule = rules[index];
        if (rule && !rule->Material.IsNull())
            part = kParts + static_cast<size_t>(rule - overrides.data());
        const bool runStart = (!closed && index == 0) || rules[index] != rules[(index + edges - 1) % edges] || !active[(index + edges - 1) % edges];
        const bool runEnd = (!closed && index + 1 == edges) || rules[index] != rules[(index + 1) % edges] || !active[(index + 1) % edges];
        if (runStart && !quads.empty())
            quads.back().IgnoreWeldNext = true;
        if (!active[index] || (rule && !rule->Visible))
        {
            if (!quads.empty())
                quads.back().IgnoreWeldNext = true;
            continue;
        }
        const bool ownTextureSettings = rule && rule->OverrideTextureSettings;
        const bool randomize = ownTextureSettings ? rule->RandomizeTextures : settings.RandomizeTextures;
        const bool useCorners = ownTextureSettings ? rule->UseCornerTextures : settings.UseCornerTextures;
        const bool useTapers = ownTextureSettings ? rule->UseTaperTextures : settings.UseTaperTextures;
        choices.clear();
        for (const bool specific : {true, false})
        {
            for (size_t candidate = 0; candidate < textures.size(); ++candidate)
            {
                if (textures[candidate].TextureSet == (rule ? rule->TextureSet : 0) &&
                    textures[candidate].PointIndex == (specific ? static_cast<int32>(segments[index]) : -1))
                    choices.push_back(candidate);
            }
            if (!choices.empty())
                break;
        }
        std::sort(choices.begin(), choices.end(), [&textures](size_t left, size_t right)
                  { return textures[left].Index < textures[right].Index; });
        const Components::SmartShapeTexture* texture = nullptr;
        size_t base = 0;
        if (!choices.empty())
        {
            const size_t selection = randomize ? random() : (rule ? rule->TextureIndex : 0);
            const size_t chosen = choices[selection % choices.size()];
            texture = &textures[chosen];
            base = kParts + overrides.size() + chosen * kParts;
            if (!rule || rule->Material.IsNull())
                part = base + static_cast<size_t>(PartForNormal(normal));
        }
        const float width = settings.EdgeWidth * (texture ? texture->WidthScale : 1.0f) *
                            (rule && rule->UseNormalRange ? rule->WidthScale : 1.0f);
        const float offset = settings.EdgeOffset + (rule ? rule->Offset : 0.0f);
        EdgeQuad quad;
        quad.Part = part;
        quad.Group = groups[index];
        quad.TileLength = texture ? texture->TileLength : settings.TileLength;
        quad.Flip = rule && rule->Flip;
        quad.Stretch = ownTextureSettings ? rule->StretchToFit : settings.StretchToFit;
        quad.Depth = settings.EdgeDepth + (rule ? rule->DepthOffset : 0.0f);
        quad.IgnoreWeldNext = rule && !rule->Weld;
        if (rule && !rule->Weld && !quads.empty())
            quads.back().IgnoreWeldNext = true;
        quad.A = points[index] + normal * (width * widths[index] * (0.5f + offset * 0.5f));
        quad.B = points[index] + normal * (width * widths[index] * (-0.5f + offset * 0.5f));
        quad.C = points[next] + normal * (width * widths[index] * (-0.5f + offset * 0.5f));
        quad.D = points[next] + normal * (width * widths[index] * (0.5f + offset * 0.5f));
        bool hasCorner = false;
        bool sharp = false;
        if (!runStart)
        {
            const size_t previous = (index + count - 1) % count;
            const auto incoming = (points[index] - points[previous]).Normalize();
            const auto outgoing = delta.Normalize();
            const float turn = std::atan2(incoming.x * outgoing.y - incoming.y * outgoing.x,
                                         Vector2::Dot(incoming, outgoing)) * 180.0f / std::numbers::pi_v<float>;
            sharp = std::abs(turn) > 90.0f;
            const bool inner = (turn < 0.0f) != reverse;
            const auto cornerKind = inner ? ShapePart::InnerCorner : ShapePart::OuterCorner;
            const size_t cornerPart = base + static_cast<size_t>(cornerKind);
            if (useCorners && std::abs(turn) >= 80.0f && std::abs(turn) <= 100.0f && !result.Materials[cornerPart].IsNull())
            {
                auto corner = MakeEdgeCorner(points[previous], points[index], points[next],
                    width * widths[index], width * widths[previous], width * offset, settings.FlipEdges,
                    inner ? QuadCorner::Inner : QuadCorner::Outer);
                corner.Part = cornerPart;
                corner.Group = quad.Group;
                corner.Flip = quad.Flip;
                corner.Depth = quad.Depth;
                corner.IgnoreWeldNext = quad.IgnoreWeldNext;
                quads.push_back(corner);
                hasCorner = true;
            }
        }
        const bool sharpTaper = useTapers && (settings.TaperSharpCorners || (rule && rule->TaperSharpCorners)) && sharp && !hasCorner;
        if (sharpTaper && closed && step == 0)
        {
            closingTaperPart = base + static_cast<size_t>(ShapePart::SharpEndTaper);
            closingTaperSize = texture ? texture->EndTaperSize : settings.EndTaperSize;
        }
        if (sharpTaper && !quads.empty())
        {
            const size_t taperPart = base + static_cast<size_t>(ShapePart::SharpEndTaper);
            if (!result.Materials[taperPart].IsNull())
            {
                EdgeQuad taper;
                if (SplitEdgeTaper(quads.back(), taper, texture ? texture->EndTaperSize : settings.EndTaperSize, taperPart, true))
                    quads.push_back(taper);
            }
            quads.back().IgnoreWeldNext = true;
        }
        if (useTapers && (runStart || sharpTaper))
        {
            const size_t taperPart = base + static_cast<size_t>(sharpTaper ? ShapePart::SharpStartTaper : ShapePart::StartCap);
            if (!result.Materials[taperPart].IsNull())
            {
                EdgeQuad taper;
                if (SplitEdgeTaper(quad, taper, texture ? texture->StartTaperSize : settings.StartTaperSize, taperPart, false))
                    quads.push_back(taper);
            }
        }
        quads.push_back(quad);
        if (useTapers && runEnd)
        {
            const size_t taperPart = base + static_cast<size_t>(ShapePart::EndCap);
            if (!result.Materials[taperPart].IsNull())
            {
                EdgeQuad taper;
                if (SplitEdgeTaper(quads.back(), taper, texture ? texture->EndTaperSize : settings.EndTaperSize, taperPart, true))
                    quads.push_back(taper);
            }
        }
        if (runEnd && !quads.empty())
            quads.back().IgnoreWeldNext = true;
    }
    if (closingTaperPart != 0 && !quads.empty())
    {
        if (!result.Materials[closingTaperPart].IsNull())
        {
            EdgeQuad taper;
            if (SplitEdgeTaper(quads.back(), taper, closingTaperSize, closingTaperPart, true))
                quads.push_back(taper);
        }
        quads.back().IgnoreWeldNext = true;
    }
    if (settings.WeldEdges)
    {
        size_t index = 0;
        while (index + 1 < quads.size())
        {
            WeldEdgeQuads(quads[index], quads[index + 1]);
            const auto& quad = quads[index];
            if (Mathematics::SegmentsIntersect(quad.A, quad.D, quad.B, quad.C) ||
                Mathematics::SegmentsIntersect(quad.A, quad.B, quad.D, quad.C))
            {
                quads.erase(quads.begin() + static_cast<std::ptrdiff_t>(index));
                if (index > 0)
                    --index;
            }
            else
                ++index;
        }
        if (closed && quads.size() > 1)
            WeldEdgeQuads(quads.back(), quads.front());
    }
    // Join a material run crossing the closed outline's index seam before fitting its UVs.
    if (closed && quads.size() > 1 && result.Materials[quads.front().Part] == result.Materials[quads.back().Part] &&
        quads.front().Flip == quads.back().Flip && quads.front().Group == quads.back().Group && !quads.front().Taper && !quads.back().Taper &&
        quads.front().Corner == QuadCorner::None && quads.back().Corner == QuadCorner::None)
    {
        const auto boundary = std::find_if(quads.begin(), quads.end(), [&first = quads.front(), &result](const EdgeQuad& quad)
        {
            return result.Materials[quad.Part] != result.Materials[first.Part] || quad.Group != first.Group ||
                   quad.Flip != first.Flip || quad.Stretch != first.Stretch || quad.TileLength != first.TileLength ||
                   quad.Depth != first.Depth || quad.Taper || quad.Corner != QuadCorner::None;
        });
        if (boundary != quads.end())
            std::rotate(quads.begin(), boundary, quads.end());
    }
    // Stretch/crop is resolved per consecutive material run, as in the source mesh builder.
    for (size_t begin = 0; begin < quads.size();)
    {
        size_t end = begin + 1;
        const auto& first = quads[begin];
        while (end < quads.size() && result.Materials[quads[end].Part] == result.Materials[first.Part] &&
               quads[end].Group == first.Group && quads[end].Flip == first.Flip && quads[end].Stretch == first.Stretch &&
               quads[end].TileLength == first.TileLength && quads[end].Depth == first.Depth)
            ++end;
        float length = 0.0f;
        for (size_t index = begin; index < end; ++index)
            length += quads[index].Length();
        const float repeats = std::round(length / first.TileLength);
        const float scale = first.Stretch && length > kMinimumLength ? repeats / length : 1.0f / first.TileLength;
        float distance = 0.0f;
        for (size_t index = begin; index < end; ++index)
        {
            const auto& quad = quads[index];
            const bool sprite = quad.Corner != QuadCorner::None;
            AppendShapeQuad(result.Parts[quad.Part], quad.A, quad.D, quad.C, quad.B,
                sprite ? 0.0f : distance * scale, sprite ? 1.0f : (distance + quad.Length()) * scale,
                quad.Depth, quad.Flip);
            distance += quad.Length();
        }
        begin = end;
    }
}

} // namespace

void BuildShapeEdges(ShapeGeometry& result, const Components::SmartShape& settings,
                const std::vector<float>& widths, const std::vector<uint32>& segments,
                std::span<const Components::SmartShapeEdge> overrides,
                std::span<const Components::SmartShapeTexture> textures, const Spline::SplineData& spline, float area)
{
    const bool closed = spline.IsEffectivelyClosed();
    const size_t count = result.Outline.size();
    const size_t edges = closed ? count : count - 1;
    const float side = ((closed ? area < 0.0f : true) != settings.FlipEdges) ? 1.0f : -1.0f;
    std::vector<Vector2> normals(edges);
    std::vector<const Components::SmartShapeEdge*> pointRules(edges);
    std::vector<bool> defaultEdges(edges, true);
    for (size_t index = 0; index < edges; ++index)
    {
        // Material ranges select the authored edge, not each tessellated tangent.
        const auto& start = spline.Points[segments[index]].Position;
        const auto& end = spline.Points[(segments[index] + 1) % spline.Points.size()].Position;
        const Vector2 delta{end.x - start.x, end.y - start.y};
        normals[index] = Vector2{-delta.y, delta.x} * side;
        for (const auto& rule : overrides)
        {
            if (!rule.UseNormalRange && rule.PointIndex == segments[index])
            {
                pointRules[index] = &rule;
                break;
            }
        }
    }
    // Every matching normal material owns a separate layer. Point overrides replace
    // every layer on their segment; invisible normal materials only hide their own layer.
    std::vector<const Components::SmartShapeEdge*> layerRules(edges);
    std::vector<bool> active(edges);
    for (const auto& rule : overrides)
    {
        if (!rule.UseNormalRange)
            continue;
        std::fill(layerRules.begin(), layerRules.end(), &rule);
        bool any = false;
        for (size_t index = 0; index < edges; ++index)
        {
            const bool matches = MatchesNormal(normals[index], rule);
            defaultEdges[index] = defaultEdges[index] && !matches;
            active[index] = matches && !pointRules[index];
            any |= active[index];
        }
        if (any)
            BuildEdgeLayer(result, settings, widths, segments, overrides, textures, closed, area, layerRules, active);
    }
    for (size_t index = 0; index < edges; ++index)
        active[index] = defaultEdges[index] || pointRules[index];
    BuildEdgeLayer(result, settings, widths, segments, overrides, textures, closed, area, pointRules, active);
}
}
