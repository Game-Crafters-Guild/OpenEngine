#include "Inspectors/SplineRecipe/SplineRecipePreviewCanvas.h"

#include "Mathematics/Vector3.h"
#include "Rendering/Text/FontAtlas.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"

#include "Types/FormatNumber.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace GameEngine::EditorUI
{
namespace
{

constexpr const char* kStyleAssetPath = "UI/inspectors/SplineRecipePreview/SplineRecipePreview.css";

// A run length, a mark and the scale bar read to a tenth of a meter: "12 m", "41.3 m".
constexpr int kRunLengthDecimals = 1;

// One centerline sample every 10 cm: the layout measures the run's length on this polyline.
constexpr float32 kReferenceRunSampleStep = 0.1f;

// The smallest run box a frame fits, so a degenerate box cannot divide by zero.
constexpr float32 kMinimumSpanMeters = 1e-3f;
constexpr float32 kGroundLineThicknessPx = 1.5f;
constexpr float32 kScaleBarThicknessPx = 1.0f;
constexpr float32 kScaleBarTickPx = 3.0f;
// The lengths the scale bar takes, in meters, and the height it reaches at least, in logical pixels.
constexpr float32 kScaleBarMeters[] = {1.0f, 2.0f, 5.0f, 10.0f};
constexpr float32 kScaleBarMinimumPx = 40.0f;
constexpr float32 kLabelGapPx = 4.0f;
constexpr float32 kLabelLineHeightRatio = 1.3f;

const StringId kGroundVar = HashStringId("--spline-preview-ground");
const StringId kGroundLineVar = HashStringId("--spline-preview-ground-line");
const StringId kLabelVar = HashStringId("--spline-preview-label");

// The stylesheet sets the size; SplineRecipePreview.css keeps it at the editor's 12 px floor.
float32 LabelFontSize(const ResolvedStyle& style)
{
    return style.Visual.FontSize;
}

uint32_t LabelArgb(const ResolvedStyle& style)
{
    return style.GetCustomColor(kLabelVar).value_or(style.Visual.Color);
}

// Fills the band between the ground line and the floor from `fromAlong` to `toAlong`, where the
// ground is straight.
void EmitGroundBand(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ReferenceRun& run,
                    float32 fromAlong, float32 toAlong, float32 floorHeight, uint32_t fill)
{
    const float32 x0 = frame.X(fromAlong);
    const float32 x1 = frame.X(toAlong);
    const float32 top0 = frame.Y(run.GroundHeight(fromAlong));
    const float32 top1 = frame.Y(run.GroundHeight(toAlong));
    const float32 bottom = frame.Y(floorHeight);
    ctx.Emit(UI::MakeTriangle(x0, top0, x1, top1, x0, bottom, fill));
    ctx.Emit(UI::MakeTriangle(x1, top1, x1, bottom, x0, bottom, fill));
}

// The shortest of kScaleBarMeters that stands kScaleBarMinimumPx tall in `frame`, or the longest.
float32 ScaleBarMeters(const RecipePreviewFrame& frame, float32 contentScale)
{
    for (const float32 meters : kScaleBarMeters)
    {
        if (meters * frame.PixelsPerMeter >= kScaleBarMinimumPx * contentScale)
            return meters;
    }
    return std::end(kScaleBarMeters)[-1];
}

} // namespace

void PrepareRecipePreviewStrip(UIElement& strip)
{
    strip.RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
}

RecipePreviewFrame FitRecipePreviewFrame(float32 x, float32 y, float32 width, float32 height, float32 alongMin,
                                         float32 alongMax, float32 heightMin, float32 heightMax)
{
    const float32 alongSpan = std::max(alongMax - alongMin, kMinimumSpanMeters);
    const float32 heightSpan = std::max(heightMax - heightMin, kMinimumSpanMeters);
    RecipePreviewFrame frame;
    frame.PixelsPerMeter = std::min(width / alongSpan, height / heightSpan);
    frame.OriginX = x + 0.5f * (width - alongSpan * frame.PixelsPerMeter);
    frame.OriginY = y + height;
    frame.AlongMin = alongMin;
    frame.HeightMin = heightMin;
    return frame;
}

ReferenceRun ReferenceRunFor(float32 longestPart, std::string_view partName)
{
    ReferenceRun run;
    if (longestPart <= kReferenceRunLength)
        return run;
    run.Length = longestPart * static_cast<float32>(kReferenceRunGrownPartCount);
    run.GrownPartCount = kReferenceRunGrownPartCount;
    run.GrownPartName = partName;
    return run;
}

std::string ReferenceRunCaption(const ReferenceRun& run)
{
    std::string extent = FormatMeters(run.Length, kRunLengthDecimals);
    if (run.GrownPartCount > 0)
        extent += " (" + std::to_string(run.GrownPartCount) + " " + std::string(run.GrownPartName) + " lengths)";
    return "Reference run: " + extent + ", flat, then a 1 in 4 slope";
}

std::string RecipePreviewLaidLengthText(float32 laid, float32 authored, bool authoredIsMeshLength)
{
    // A field's value prints as typed; a mesh's length is measured, so it prints rounded. The mesh
    // form is short, so the text fits beside the bar more often.
    const std::string laidText = FormatMeters(laid, kRecipePreviewLaidLengthDecimals);
    const std::string authoredText = FormatMeters(authored, kRecipePreviewLaidLengthDecimals);
    const std::string_view change = laid > authored ? "stretched" : "shortened";
    if (authoredIsMeshLength)
    {
        if (laidText == authoredText)
            return laidText + ", the mesh's length";
        return laidText + " laid (" + authoredText + " mesh, " + std::string(change) + ")";
    }
    if (laidText == authoredText)
        return FormatFloat(authored) + " m";
    return laidText + " laid (" + std::string(change) + " from " + FormatFloat(authored) + ")";
}

std::vector<SplineLayout::CenterSample> ReferenceRunCenterline(const ReferenceRun& run)
{
    const Mathematics::Vector3 flatNormal(0.0f, 1.0f, 0.0f);
    const float32 slopeLength = std::sqrt(1.0f + kReferenceRunGrade * kReferenceRunGrade);
    const Mathematics::Vector3 slopeNormal(0.0f, 1.0f / slopeLength, -kReferenceRunGrade / slopeLength);
    const uint32 sampleCount = static_cast<uint32>(std::lround(run.Length / kReferenceRunSampleStep)) + 1u;
    std::vector<SplineLayout::CenterSample> center;
    center.reserve(sampleCount);
    for (uint32 sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        const float32 along = run.Length * static_cast<float32>(sampleIndex) / static_cast<float32>(sampleCount - 1u);
        SplineLayout::CenterSample sample;
        sample.Pos = Mathematics::Vector3(0.0f, run.GroundHeight(along), along);
        sample.Normal = along > run.FlatLength() ? slopeNormal : flatNormal;
        center.push_back(sample);
    }
    return center;
}

uint32_t RecipePreviewColor(const ResolvedStyle& style, StringId name)
{
    return UI::PackFromARGB(style.GetCustomColor(name).value_or(0u));
}

void EmitReferenceRunGround(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ReferenceRun& run,
                            const ResolvedStyle& style, float32 floorHeight)
{
    const uint32_t fill = RecipePreviewColor(style, kGroundVar);
    EmitGroundBand(ctx, frame, run, 0.0f, run.FlatLength(), floorHeight, fill);
    EmitGroundBand(ctx, frame, run, run.FlatLength(), run.Length, floorHeight, fill);
}

float32 EmitReferenceRunMarks(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ReferenceRun& run,
                              const ResolvedStyle& style, float32 floorHeight, float32 previousRight)
{
    // The marks sit under the floor line, centered on the place they name.
    const float32 top = frame.Y(floorHeight) + kLabelGapPx * 0.5f * ctx.ContentScale;
    const uint32_t argb = LabelArgb(style);
    float32 leftmost = std::numeric_limits<float32>::max();
    for (const float32 along : {run.FlatLength(), run.Length})
    {
        const std::string text = FormatMeters(along, kRunLengthDecimals);
        const float32 halfWidth = 0.5f * RecipePreviewTextWidth(ctx, style, text);
        const float32 centerX = std::min(frame.X(along), frame.X(run.Length) - halfWidth);
        if (EmitRecipePreviewLabel(ctx, style, text, argb, centerX, top, previousRight))
            leftmost = std::min(leftmost, centerX - halfWidth);
    }
    return leftmost;
}

void EmitReferenceRunSurface(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ReferenceRun& run,
                             const ResolvedStyle& style)
{
    const uint32_t line = RecipePreviewColor(style, kGroundLineVar);
    const float32 thickness = kGroundLineThicknessPx * ctx.ContentScale;
    const float32 flat = run.FlatLength();
    ctx.Emit(UI::MakeLine(frame.X(0.0f), frame.Y(0.0f), frame.X(flat), frame.Y(0.0f), thickness, line));
    ctx.Emit(UI::MakeLine(frame.X(flat), frame.Y(0.0f), frame.X(run.Length), frame.Y(run.GroundHeight(run.Length)),
                          thickness, line));
}

float32 RecipePreviewLineHeight(const UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style)
{
    return LabelFontSize(style) * kLabelLineHeightRatio * ctx.ContentScale;
}

float32 RecipePreviewScaleBarWidth(const UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame,
                                   const ResolvedStyle& style)
{
    const std::string text = FormatMeters(ScaleBarMeters(frame, ctx.ContentScale), kRunLengthDecimals);
    return (kScaleBarTickPx + 2.0f * kLabelGapPx) * ctx.ContentScale + RecipePreviewTextWidth(ctx, style, text);
}

void EmitRecipePreviewScaleBar(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame,
                               const ResolvedStyle& style, float32 x, float32 height)
{
    const uint32_t argb = LabelArgb(style);
    const uint32_t color = UI::PackFromARGB(argb);
    const float32 contentScale = ctx.ContentScale;
    const float32 meters = ScaleBarMeters(frame, contentScale);
    const float32 bottom = frame.Y(height);
    const float32 top = frame.Y(height + meters);
    const float32 thickness = kScaleBarThicknessPx * contentScale;
    ctx.Emit(UI::MakeLine(x, bottom, x, top, thickness, color));
    ctx.Emit(UI::MakeLine(x, top, x + kScaleBarTickPx * contentScale, top, thickness, color));
    ctx.Emit(UI::MakeLine(x, bottom, x + kScaleBarTickPx * contentScale, bottom, thickness, color));
    if (!ctx.FontAtlas)
        return;
    const float32 fontSize = LabelFontSize(style);
    const float32 lineHeight = RecipePreviewLineHeight(ctx, style);
    ctx.EmitText(FormatMeters(meters, kRunLengthDecimals), x + (kScaleBarTickPx + kLabelGapPx) * contentScale,
                 0.5f * (top + bottom) - 0.5f * lineHeight, fontSize, argb, ctx.FontAtlas);
}

float32 RecipePreviewTextWidth(const UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, std::string_view text)
{
    return ctx.FontAtlas ? ctx.FontAtlas->MeasureUtf8(text, std::max(1.0f, LabelFontSize(style) * ctx.ContentScale)).width
                         : 0.0f;
}

bool EmitRecipePreviewLabel(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, std::string_view text,
                            uint32_t argb, float32 centerX, float32 top, float32& previousRight)
{
    if (!ctx.FontAtlas)
        return false;
    const float32 fontSize = LabelFontSize(style);
    const float32 width = RecipePreviewTextWidth(ctx, style, text);
    const float32 left = centerX - 0.5f * width;
    if (left < previousRight + kLabelGapPx * ctx.ContentScale)
        return false;
    ctx.EmitText(text, left, top, fontSize, argb, ctx.FontAtlas);
    previousRight = left + width;
    return true;
}

} // namespace GameEngine::EditorUI
