#include "Inspectors/SplineFence/SplineFencePreview.h"

#include "Components/Spline/SplinePoolSelection.h"
#include "Core/CpuProfiler.h"
#include "Inspectors/SplineRecipe/SplineRecipePreviewCanvas.h"
#include "Logger/Logger.h"
#include "Mathematics/Interpolation.h"
#include "SplineLayout/PieceBasis.h"
#include "SplineRecipes/FenceRecipeLayout.h"
#include "Types/FormatNumber.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Label.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIPrimitive.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace GameEngine::EditorUI
{
namespace
{

using Components::SplineFencePartKind;
using Mathematics::Vector2;
using Mathematics::Vector3;
using SplineLayout::FencePieceBounds;

constexpr const char* kLayoutAssetPath = "UI/inspectors/SplineRecipePreview/SplineFencePreview.uxml";
constexpr std::array<std::string_view, 8> kRequiredIds = {
    "spline-fence-preview-caption",      "spline-fence-preview-note",         "spline-fence-preview-plot",
    "spline-fence-preview-legend-post",  "spline-fence-preview-legend-span",  "spline-fence-preview-legend-gate",
    "spline-fence-preview-legend-crest", "spline-fence-preview-gate-length"};

// The pools the strip resolves, in the order of SplineFencePreview::m_Pools.
constexpr std::array<SplineFencePartKind, 5> kPoolKinds = {SplineFencePartKind::Post, SplineFencePartKind::Span,
                                                           SplineFencePartKind::Gate, SplineFencePartKind::Crest,
                                                           SplineFencePartKind::Cap};
constexpr std::size_t kPostPool = 0;
constexpr std::size_t kSpanPool = 1;
constexpr std::size_t kGatePool = 2;
constexpr std::size_t kCrestPool = 3;
constexpr std::size_t kCapPool = 4;

// The stand-in size of a part the scene has not measured yet, in meters: a span or a gate as long
// as Span Length and 1 m tall, a 1.2 m post, and a crest or cap piece that sits on a span's top.
constexpr float32 kStandInSpanHeight = 1.0f;
constexpr float32 kStandInThickness = 0.1f;
constexpr float32 kStandInPostHeight = 1.2f;
constexpr float32 kStandInPostWidth = 0.2f;
constexpr float32 kStandInCrestLength = 0.5f;
constexpr float32 kStandInCrestHeight = 0.2f;
constexpr float32 kStandInCapLength = 0.2f;

// The ground the strip shows below the lowest part, so a part that stands on the surface still has
// ground under it.
constexpr float32 kGroundDepthMeters = 0.25f;
constexpr float32 kDashLengthPx = 4.0f;
constexpr float32 kDashGapPx = 3.0f;
constexpr float32 kDashThicknessPx = 1.0f;
constexpr float32 kLabelGapPx = 2.0f;
// The space between the Span Length bar and its text, and between two span labels.
constexpr float32 kBarTextGapPx = 6.0f;
constexpr float32 kSpanLabelSpacingPx = 4.0f;
constexpr float32 kSeamThicknessPx = 1.0f;
constexpr float32 kGateLineThicknessPx = 1.0f;
constexpr float32 kBarTickPx = 3.0f;
// A plot never gets flatter than this, so a long flat run keeps room for its labels.
constexpr float32 kMaxPlotAspect = 40.0f;
// A part counts as below the ground when a corner is deeper than this, in meters.
constexpr float32 kBuriedToleranceMeters = 0.001f;
// A line shorter than this, in pixels, is left out.
constexpr float32 kMinLinePx = 0.5f;

const StringId kPostVar = HashStringId("--spline-preview-post");
const StringId kSpanVar = HashStringId("--spline-preview-span");
const StringId kGateVar = HashStringId("--spline-preview-gate");
const StringId kGateFillVar = HashStringId("--spline-preview-gate-fill");
const StringId kSeamVar = HashStringId("--spline-preview-seam");
const StringId kCrestVar = HashStringId("--spline-preview-crest");
const StringId kStationVar = HashStringId("--spline-preview-station");
const StringId kLabelVar = HashStringId("--spline-preview-label");
const StringId kBuriedVar = HashStringId("--spline-preview-buried");

FencePieceBounds BoxOf(float32 length, float32 height, float32 thickness)
{
    FencePieceBounds bounds;
    bounds.Center = Vector3(0.0f, 0.5f * height, 0.0f);
    bounds.HalfExtents = Vector3(0.5f * thickness, 0.5f * height, 0.5f * length);
    return bounds;
}

FencePieceBounds StandInBounds(SplineFencePartKind kind, float32 spanLength)
{
    switch (kind)
    {
    case SplineFencePartKind::Post:
        return BoxOf(kStandInPostWidth, kStandInPostHeight, kStandInPostWidth);
    case SplineFencePartKind::Crest:
        return BoxOf(kStandInCrestLength, kStandInCrestHeight, kStandInThickness);
    case SplineFencePartKind::Cap:
        return BoxOf(kStandInCapLength, kStandInCrestHeight, kStandInThickness);
    default:
        return BoxOf(spanLength, kStandInSpanHeight, kStandInThickness);
    }
}

const SplineRecipes::FenceMeshBounds* FindMeasured(std::span<const SplineRecipes::FenceMeshBounds> measured,
                                                    const Components::ModelRef& mesh)
{
    for (const SplineRecipes::FenceMeshBounds& entry : measured)
    {
        if (entry.Mesh == mesh)
            return &entry;
    }
    return nullptr;
}

bool SameBounds(const FencePieceBounds& a, const FencePieceBounds& b)
{
    return a.Center.x == b.Center.x && a.Center.y == b.Center.y && a.Center.z == b.Center.z &&
           a.HalfExtents.x == b.HalfExtents.x && a.HalfExtents.y == b.HalfExtents.y &&
           a.HalfExtents.z == b.HalfExtents.z;
}

// The bounds slot `slot` of `kind` draws with: the measured box, or the stand-in.
FencePieceBounds ResolvedBounds(const SplineRecipes::FencePool& pool, uint32 slot, SplineFencePartKind kind,
                                float32 spanLength, std::span<const SplineRecipes::FenceMeshBounds> measured,
                                bool& outMeasured)
{
    const SplineRecipes::FenceMeshBounds* found = FindMeasured(measured, pool.Meshes[slot]);
    outMeasured = found != nullptr;
    return found ? found->Bounds : StandInBounds(kind, spanLength);
}

void SetShown(UIElement* element, bool shown)
{
    if (!element)
        return;
    if (shown)
        element->RemoveClass("hidden");
    else
        element->AddClass("hidden");
}

// The along length of `bounds` along its piece axis, in meters.
float32 LengthOf(const FencePieceBounds& bounds)
{
    return 2.0f * (bounds.Axis() == SplineLayout::PieceAxis::X ? bounds.HalfExtents.x : bounds.HalfExtents.z);
}

// Cuts the convex polygon `points` to the side where height >= slope * along + offset, in place.
// The result is convex and has at most one corner more than the input, so `points` holds count + 1.
uint32 ClipAboveLine(Vector2* points, uint32 count, float32 slope, float32 offset)
{
    Vector2 kept[8];
    uint32 keptCount = 0;
    for (uint32 corner = 0; corner < count; ++corner)
    {
        const Vector2& from = points[corner];
        const Vector2& to = points[(corner + 1) % count];
        const float32 fromSide = from.y - (slope * from.x + offset);
        const float32 toSide = to.y - (slope * to.x + offset);
        if (fromSide >= 0.0f)
            kept[keptCount++] = from;
        if ((fromSide >= 0.0f) != (toSide >= 0.0f))
        {
            const float32 t = fromSide / (fromSide - toSide);
            kept[keptCount++] = Vector2(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
        }
    }
    std::copy(kept, kept + keptCount, points);
    return keptCount;
}

// The fractions [Start, End] along the segment from `from` to `to` (run meters) where it lies on or
// above the ground of `run`. The interval is empty, Start >= End, for a segment wholly below it.
struct GroundInterval
{
    float32 Start = 0.0f;
    float32 End = 1.0f;
};

// The line height = Slope * along + Offset, in run meters.
struct GroundLine
{
    float32 Slope = 0.0f;
    float32 Offset = 0.0f;
};

GroundInterval AboveGroundInterval(const ReferenceRun& run, const Vector2& from, const Vector2& to)
{
    // The ground is the higher of the flat line and the slope line: above it is above both.
    const std::array<GroundLine, 2> lines = {GroundLine{0.0f, 0.0f},
                                             GroundLine{kReferenceRunGrade, -kReferenceRunGrade * run.FlatLength()}};
    GroundInterval interval;
    for (const GroundLine& line : lines)
    {
        const float32 fromSide = from.y - (line.Slope * from.x + line.Offset);
        const float32 toSide = to.y - (line.Slope * to.x + line.Offset);
        if (fromSide >= 0.0f && toSide >= 0.0f)
            continue;
        if (fromSide < 0.0f && toSide < 0.0f)
            return {1.0f, 0.0f};
        const float32 t = fromSide / (fromSide - toSide);
        if (fromSide < 0.0f)
            interval.Start = std::max(interval.Start, t);
        else
            interval.End = std::min(interval.End, t);
    }
    return interval;
}

// Draws the fractions [start, end] of the segment from `from` to `to` (run meters).
void EmitSegmentPart(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const Vector2& from,
                     const Vector2& to, float32 start, float32 end, float32 thickness, uint32_t color)
{
    const Vector2 a(Math::Lerp(from.x, to.x, start), Math::Lerp(from.y, to.y, start));
    const Vector2 b(Math::Lerp(from.x, to.x, end), Math::Lerp(from.y, to.y, end));
    const Vector2 pixelA(frame.X(a.x), frame.Y(a.y));
    const Vector2 pixelB(frame.X(b.x), frame.Y(b.y));
    if (end <= start || (pixelB - pixelA).Length() < kMinLinePx * ctx.ContentScale)
        return;
    ctx.Emit(UI::MakeLine(pixelA.x, pixelA.y, pixelB.x, pixelB.y, thickness, color));
}

void EmitAboveGround(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ReferenceRun& run,
                     const Vector2& from, const Vector2& to, float32 thickness, uint32_t color)
{
    const GroundInterval above = AboveGroundInterval(run, from, to);
    EmitSegmentPart(ctx, frame, from, to, above.Start, above.End, thickness, color);
}

void EmitBelowGround(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ReferenceRun& run,
                     const Vector2& from, const Vector2& to, float32 thickness, uint32_t color)
{
    const GroundInterval above = AboveGroundInterval(run, from, to);
    if (above.End <= above.Start)
    {
        EmitSegmentPart(ctx, frame, from, to, 0.0f, 1.0f, thickness, color);
        return;
    }
    EmitSegmentPart(ctx, frame, from, to, 0.0f, above.Start, thickness, color);
    EmitSegmentPart(ctx, frame, from, to, above.End, 1.0f, thickness, color);
}

// Fills a convex polygon with a fan from each corner, so every inner edge has a triangle on both
// sides and the anti-aliased edges leave no light seam inside it.
void EmitConvexFill(UI::PrimitiveEmitContext& ctx, const Vector2* corners, uint32 count, uint32_t fill)
{
    for (uint32 apex = 0; apex < count; ++apex)
    {
        for (uint32 step = 1; step + 1 < count; ++step)
        {
            const Vector2& a = corners[apex];
            const Vector2& b = corners[(apex + step) % count];
            const Vector2& c = corners[(apex + step + 1) % count];
            ctx.Emit(UI::MakeTriangle(a.x, a.y, b.x, b.y, c.x, c.y, fill));
        }
    }
}

// Fills the quad with the four triangles of its two diagonals. Each diagonal lies inside the other
// pair, so the anti-aliased edges along a diagonal do not leave a light seam across the box.
void EmitQuad(UI::PrimitiveEmitContext& ctx, const Vector2* corners, uint32_t fill)
{
    for (std::size_t skipped = 0; skipped < 4; ++skipped)
    {
        const Vector2& a = corners[(skipped + 1) % 4];
        const Vector2& b = corners[(skipped + 2) % 4];
        const Vector2& c = corners[(skipped + 3) % 4];
        ctx.Emit(UI::MakeTriangle(a.x, a.y, b.x, b.y, c.x, c.y, fill));
    }
}

void EmitDashedLine(UI::PrimitiveEmitContext& ctx, const Vector2& from, const Vector2& to, uint32_t color)
{
    const float32 contentScale = ctx.ContentScale;
    const Vector2 delta = to - from;
    const float32 length = delta.Length();
    const float32 dash = kDashLengthPx * contentScale;
    const float32 step = (kDashLengthPx + kDashGapPx) * contentScale;
    if (length <= 0.0f || step <= 0.0f)
        return;
    for (float32 start = 0.0f; start < length; start += step)
    {
        const Vector2 dashStart = from + delta * (start / length);
        const Vector2 dashEnd = from + delta * (std::min(start + dash, length) / length);
        ctx.Emit(UI::MakeLine(dashStart.x, dashStart.y, dashEnd.x, dashEnd.y, kDashThicknessPx * contentScale, color));
    }
}

} // namespace

SplineFencePreview::SplineFencePreview()
{
    PrepareRecipePreviewStrip(*this);
    m_Center = ReferenceRunCenterline(m_Run);
    // One run from point 0 to point 1, so a span's ordinal is the one the override rows name.
    m_RunBoundaries = {0.0f, static_cast<float32>(m_Center.size() - 1u)};
}

std::unique_ptr<SplineFencePreview> SplineFencePreview::Create()
{
    std::unique_ptr<UIElement> root = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!root)
        return std::make_unique<SplineFencePreview>();
    if (!dynamic_cast<SplineFencePreview*>(root.get()))
    {
        Logger::Log::Error("SplineFencePreview: {} has no SplineFencePreview root, so the fence has no preview.",
                           kLayoutAssetPath);
        return std::make_unique<SplineFencePreview>();
    }
    std::unique_ptr<SplineFencePreview> strip(static_cast<SplineFencePreview*>(root.release()));
    strip->m_Caption = dynamic_cast<Label*>(strip->FindById("spline-fence-preview-caption"_sid));
    strip->m_Note = strip->FindById("spline-fence-preview-note"_sid);
    strip->m_Plot = strip->FindById("spline-fence-preview-plot"_sid);
    strip->m_PostEntry = strip->FindById("spline-fence-preview-legend-post"_sid);
    strip->m_SpanEntry = strip->FindById("spline-fence-preview-legend-span"_sid);
    strip->m_GateEntry = strip->FindById("spline-fence-preview-legend-gate"_sid);
    strip->m_CrestEntry = strip->FindById("spline-fence-preview-legend-crest"_sid);
    strip->m_GateLength = dynamic_cast<Label*>(strip->FindById("spline-fence-preview-gate-length"_sid));
    if (strip->m_Caption)
        strip->m_Caption->SetText(ReferenceRunCaption(strip->m_Run));
    return strip;
}

void SplineFencePreview::SetFence(const Components::SplineFence& recipe,
                                  std::span<const SplineRecipes::FenceMeshBounds> measured,
                                  std::span<const std::pair<uint32, uint32>> unplacedGates)
{
    const Components::SplineFence sanitized = SplineLayout::SanitizeFenceRecipe(recipe);
    const bool sameGates =
        std::equal(unplacedGates.begin(), unplacedGates.end(), m_UnplacedGates.begin(), m_UnplacedGates.end());
    if (m_HasRecipe && sanitized == m_Recipe && sameGates && SameParts(sanitized, measured))
        return;
    GE_CPU_PROFILE_SCOPE("SplineFencePreview.Relayout");
    m_Recipe = sanitized;
    m_HasRecipe = true;
    m_UnplacedGates.assign(unplacedGates.begin(), unplacedGates.end());
    ResolveParts(m_Recipe, measured);
    Relayout();
    UpdateLegend();
    MarkDirty(VisualDirty);
}

bool SplineFencePreview::SameParts(const Components::SplineFence& recipe,
                                   std::span<const SplineRecipes::FenceMeshBounds> measured) const
{
    for (std::size_t p = 0; p < kPoolKinds.size(); ++p)
    {
        const SplineRecipes::FencePool pool = SplineRecipes::FencePoolOf(recipe, kPoolKinds[p]);
        const PoolPieces& drawn = m_Pools[p];
        if (pool.Count != drawn.Bounds.size())
            return false;
        for (uint32 slot = 0; slot < pool.Count; ++slot)
        {
            bool isMeasured = false;
            const FencePieceBounds bounds =
                ResolvedBounds(pool, slot, kPoolKinds[p], recipe.PostPitch, measured, isMeasured);
            if (isMeasured != drawn.Measured[slot] || !SameBounds(bounds, drawn.Bounds[slot]))
                return false;
        }
    }
    return true;
}

void SplineFencePreview::ResolveParts(const Components::SplineFence& recipe,
                                      std::span<const SplineRecipes::FenceMeshBounds> measured)
{
    static_assert(kPoolKinds.size() == kPoolCount, "one pool kind per drawn pool");
    for (std::size_t p = 0; p < kPoolKinds.size(); ++p)
    {
        const SplineRecipes::FencePool pool = SplineRecipes::FencePoolOf(recipe, kPoolKinds[p]);
        PoolPieces& pieces = m_Pools[p];
        pieces.Bounds.resize(pool.Count);
        pieces.Measured.resize(pool.Count);
        for (uint32 slot = 0; slot < pool.Count; ++slot)
        {
            bool isMeasured = false;
            pieces.Bounds[slot] = ResolvedBounds(pool, slot, kPoolKinds[p], recipe.PostPitch, measured, isMeasured);
            pieces.Measured[slot] = isMeasured;
        }
    }
}

// Grows the run to four of the longest span or gate part when that part is longer than 12 m.
void SplineFencePreview::FitRun()
{
    float32 longestPart = 0.0f;
    std::string_view partName = "span";
    for (const std::size_t pool : {kSpanPool, kGatePool})
    {
        for (const FencePieceBounds& bounds : m_Pools[pool].Bounds)
        {
            if (LengthOf(bounds) <= longestPart)
                continue;
            longestPart = LengthOf(bounds);
            partName = pool == kGatePool ? "gate" : "span";
        }
    }
    const ReferenceRun run = ReferenceRunFor(longestPart, partName);
    if (run == m_Run)
        return;
    m_Run = run;
    m_Center = ReferenceRunCenterline(m_Run);
    m_RunBoundaries = {0.0f, static_cast<float32>(m_Center.size() - 1u)};
    if (m_Caption)
        m_Caption->SetText(ReferenceRunCaption(m_Run));
}

void SplineFencePreview::Relayout()
{
    FitRun();
    SplineRecipes::FenceRecipePieces pieces;
    pieces.PostFootprint = m_Pools[kPostPool].Bounds.empty() ? nullptr : &m_Pools[kPostPool].Bounds.front();
    pieces.Spans = m_Pools[kSpanPool].Bounds;
    pieces.Gates = m_Pools[kGatePool].Bounds;
    pieces.Crests = m_Pools[kCrestPool].Bounds;
    pieces.Caps = m_Pools[kCapPool].Bounds;
    SplineLayout::FenceLayoutParams params = SplineRecipes::FenceLayoutParamsOf(m_Recipe, pieces);
    params.RunBoundaries = m_RunBoundaries;
    const SplineLayout::FenceLayoutResult built = SplineLayout::BuildFenceLayout(m_Center, params);

    m_Shapes.clear();
    m_PostLines.clear();
    m_SpanLabels.clear();
    std::string gateLength;
    const PoolPieces& posts = m_Pools[kPostPool];
    for (const SplineLayout::FenceStation& station : built.Stations)
    {
        m_PostLines.push_back({station.Pose.Base.z, station.Pose.Base.y, station.Pose.Base.y});
        if (posts.Bounds.empty())
            continue;
        const uint32 pick = Components::SplinePoolSelect(m_Recipe.Seed, Components::SplinePoolRole::Post, station.Index,
                                                         static_cast<uint32>(posts.Bounds.size()));
        AddShape(station.Pose, posts.Bounds[pick], 1.0f, Role::Post, posts.Measured[pick]);
    }
    for (const SplineLayout::FenceSpan& span : built.Spans)
    {
        const PoolPieces& pool = span.IsGate ? m_Pools[kGatePool] : m_Pools[kSpanPool];
        if (span.PoolSlot >= pool.Bounds.size())
            continue;
        AddShape(span.Pose, pool.Bounds[span.PoolSlot], span.LengthScale, span.IsGate ? Role::Gate : Role::Span,
                 pool.Measured[span.PoolSlot]);
        Shape& shape = m_Shapes.back();
        const std::pair<uint32, uint32> address{span.Run, span.OrdinalInRun};
        shape.Placed = !span.IsGate ||
                       std::find(m_UnplacedGates.begin(), m_UnplacedGates.end(), address) == m_UnplacedGates.end();
        const auto [left, right] = std::minmax({shape.Corners[0].x, shape.Corners[1].x, shape.Corners[2].x,
                                                shape.Corners[3].x});
        m_SpanLabels.push_back({left, right, span.OrdinalInRun, span.IsGate, shape.Placed});
        if (span.IsGate && shape.Placed && shape.Measured && span.LengthScale != 1.0f)
        {
            const float32 meshLength = LengthOf(pool.Bounds[span.PoolSlot]);
            gateLength += (gateLength.empty() ? "" : "; ") + std::string("Gate: Span ") +
                          std::to_string(span.OrdinalInRun) + ", " +
                          RecipePreviewLaidLengthText(meshLength * span.LengthScale, meshLength, true);
        }
    }
    if (m_GateLength)
    {
        m_GateLength->SetText(gateLength);
        SetShown(m_GateLength, !gateLength.empty());
    }
    for (const SplineLayout::FenceCrest& crest : built.Crests)
    {
        const PoolPieces& pool = crest.IsCap ? m_Pools[kCapPool] : m_Pools[kCrestPool];
        if (crest.PoolSlot < pool.Bounds.size())
            AddShape(crest.Pose, pool.Bounds[crest.PoolSlot], crest.LengthScale, Role::Crest, pool.Measured[crest.PoolSlot]);
    }

    // The plot holds the run and every part, also a post that stands out past an end of the run.
    m_AlongMin = 0.0f;
    m_AlongMax = m_Run.Length;
    m_HeightMin = 0.0f;
    m_HeightMax = m_Run.GroundHeight(m_Run.Length);
    for (const Shape& shape : m_Shapes)
    {
        for (const Vector2& corner : shape.Corners)
        {
            m_AlongMin = std::min(m_AlongMin, corner.x);
            m_AlongMax = std::max(m_AlongMax, corner.x);
            m_HeightMin = std::min(m_HeightMin, corner.y);
            m_HeightMax = std::max(m_HeightMax, corner.y);
        }
    }
    m_HeightMin -= kGroundDepthMeters;
    for (PostLine& line : m_PostLines)
        line.Top = TopOfPartsBetween(line.Along, line.Along, line.Base + kStandInPostHeight);

    // The bar under the first span gives the length laid there, against the length of the first
    // span's measured mesh, or against Span Length.
    m_SpanLengthText.clear();
    m_SpanLengthShortText.clear();
    if (m_PostLines.size() >= 2u)
    {
        const float32 laidLength = m_PostLines[1].Along - m_PostLines[0].Along;
        float32 authored = m_Recipe.PostPitch;
        bool authoredIsMeshLength = false;
        if (!built.Spans.empty())
        {
            const SplineLayout::FenceSpan& first = built.Spans.front();
            const PoolPieces& pool = first.IsGate ? m_Pools[kGatePool] : m_Pools[kSpanPool];
            if (first.PoolSlot < pool.Bounds.size() && pool.Measured[first.PoolSlot])
            {
                authored = LengthOf(pool.Bounds[first.PoolSlot]);
                authoredIsMeshLength = true;
            }
        }
        m_SpanLengthText = RecipePreviewLaidLengthText(laidLength, authored, authoredIsMeshLength);
        m_SpanLengthShortText = FormatMeters(laidLength, kRecipePreviewLaidLengthDecimals);
    }

    // The plot's shape follows the run's box, so both axes keep one scale; the stylesheet caps its height.
    if (m_Plot)
    {
        const float32 alongSpan = m_AlongMax - m_AlongMin;
        const float32 aspect = alongSpan / std::max(m_HeightMax - m_HeightMin, alongSpan / kMaxPlotAspect);
        m_Plot->Overrides().Set(Style::AspectRatio, aspect);
    }
}

// The top of the parts that stand between `left` and `right` meters along, or `fallback` when no
// part does.
float32 SplineFencePreview::TopOfPartsBetween(float32 left, float32 right, float32 fallback) const
{
    bool found = false;
    float32 top = -std::numeric_limits<float32>::max();
    for (const Shape& shape : m_Shapes)
    {
        float32 shapeLeft = shape.Corners[0].x;
        float32 shapeRight = shape.Corners[0].x;
        float32 shapeTop = shape.Corners[0].y;
        for (const Vector2& corner : shape.Corners)
        {
            shapeLeft = std::min(shapeLeft, corner.x);
            shapeRight = std::max(shapeRight, corner.x);
            shapeTop = std::max(shapeTop, corner.y);
        }
        if (right < shapeLeft || left > shapeRight)
            continue;
        found = true;
        top = std::max(top, shapeTop);
    }
    return found ? top : fallback;
}

void SplineFencePreview::AddShape(const SplineLayout::TilePose& pose, const FencePieceBounds& bounds,
                                  float32 lengthScale, Role role, bool measured)
{
    // The piece's local box laid on the pose as the emitted transform lays it (PieceEntity.h), seen
    // from the side: the along and up extents through the box's center across the run.
    const SplineLayout::PieceAxis axis = bounds.Axis();
    const SplineLayout::PieceBasis basis = SplineLayout::MakePieceBasis(pose, axis);
    const SplineLayout::PieceAxisScale scale = SplineLayout::MakePieceAxisScale(axis, lengthScale);
    const float32 halfAlong = axis == SplineLayout::PieceAxis::X ? bounds.HalfExtents.x : bounds.HalfExtents.z;
    Shape shape;
    shape.PartRole = role;
    shape.Measured = measured;
    // Corners in order around the box: bottom back, bottom front, top front, top back.
    for (std::size_t corner = 0; corner < 4; ++corner)
    {
        const float32 alongSign = corner == 1 || corner == 2 ? 1.0f : -1.0f;
        const float32 upSign = corner >= 2 ? 1.0f : -1.0f;
        Vector3 local = bounds.Center;
        local.y += upSign * bounds.HalfExtents.y;
        if (axis == SplineLayout::PieceAxis::X)
            local.x += alongSign * halfAlong;
        else
            local.z += alongSign * halfAlong;
        const Vector3 world = pose.Position + basis.LocalX * (scale.X * local.x) + basis.LocalY * local.y +
                              basis.LocalZ * (scale.Z * local.z);
        shape.Corners[corner] = Vector2(world.z, world.y);
    }
    std::copy(std::begin(shape.Corners), std::end(shape.Corners), shape.Outline);
    shape.OutlineCount = 4;
    // A Stepped span sinks its low end into the slope: the strip shows only the part above the ground.
    // Any other part below the ground shows as an outline under the fill of its part above the ground.
    shape.CutAtGround =
        m_Recipe.SpanGrade == Components::SplineSpanGrade::Stepped && (role == Role::Span || role == Role::Gate);
    shape.Buried = !shape.CutAtGround &&
                   std::any_of(std::begin(shape.Corners), std::end(shape.Corners), [this](const Vector2& corner)
                               { return corner.y < m_Run.GroundHeight(corner.x) - kBuriedToleranceMeters; });
    if (shape.CutAtGround || shape.Buried)
    {
        shape.OutlineCount = ClipAboveLine(shape.Outline, shape.OutlineCount, 0.0f, 0.0f);
        shape.OutlineCount = ClipAboveLine(shape.Outline, shape.OutlineCount, kReferenceRunGrade,
                                           -kReferenceRunGrade * m_Run.FlatLength());
    }
    m_Shapes.push_back(shape);
}

void SplineFencePreview::UpdateLegend()
{
    const bool anySpan = std::any_of(m_Shapes.begin(), m_Shapes.end(),
                                     [](const Shape& shape) { return shape.PartRole == Role::Span; });
    const bool anyGate = std::any_of(m_Shapes.begin(), m_Shapes.end(),
                                     [](const Shape& shape) { return shape.PartRole == Role::Gate; });
    const bool anyCrest = std::any_of(m_Shapes.begin(), m_Shapes.end(),
                                      [](const Shape& shape) { return shape.PartRole == Role::Crest; });
    const bool anyStandIn =
        std::any_of(m_Shapes.begin(), m_Shapes.end(), [](const Shape& shape) { return !shape.Measured; });
    SetShown(m_PostEntry, !m_Pools[kPostPool].Bounds.empty());
    SetShown(m_SpanEntry, anySpan);
    SetShown(m_GateEntry, anyGate);
    SetShown(m_CrestEntry, anyCrest);
    SetShown(m_Note, anyStandIn);
}

void SplineFencePreview::EmitShapes(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame,
                                    const ResolvedStyle& style) const
{
    const float32 contentScale = ctx.ContentScale;
    const uint32_t fills[] = {RecipePreviewColor(style, kPostVar), RecipePreviewColor(style, kSpanVar),
                              RecipePreviewColor(style, kGateFillVar), RecipePreviewColor(style, kCrestVar)};
    const uint32_t gateLine = RecipePreviewColor(style, kGateVar);
    const uint32_t seam = RecipePreviewColor(style, kSeamVar);
    const uint32_t buried = RecipePreviewColor(style, kBuriedVar);
    const uint32_t dashColor = UI::PackFromARGB(style.GetCustomColor(kLabelVar).value_or(style.Visual.Color));
    const float32 thickness = kGateLineThicknessPx * contentScale;
    for (const Shape& shape : m_Shapes)
    {
        if (shape.CutAtGround && shape.OutlineCount < 3)
            continue;
        Vector2 outline[kMaxOutlineCorners];
        for (uint32 corner = 0; corner < shape.OutlineCount; ++corner)
            outline[corner] = Vector2(frame.X(shape.Outline[corner].x), frame.Y(shape.Outline[corner].y));
        // The box's corners in run meters; a box cut at the ground has its bottom corners raised onto it.
        Vector2 box[4];
        for (std::size_t corner = 0; corner < 4; ++corner)
        {
            const Vector2& point = shape.Corners[corner];
            box[corner] = shape.CutAtGround ? Vector2(point.x, std::max(point.y, m_Run.GroundHeight(point.x))) : point;
        }
        // The dashed outline of a gate that places nothing or of a stand-in follows the whole box.
        Vector2 dashed[kMaxOutlineCorners];
        const uint32 dashedCount = shape.CutAtGround ? shape.OutlineCount : 4u;
        for (uint32 corner = 0; corner < dashedCount; ++corner)
        {
            const Vector2& point = shape.CutAtGround ? shape.Outline[corner] : shape.Corners[corner];
            dashed[corner] = Vector2(frame.X(point.x), frame.Y(point.y));
        }
        if (!shape.Placed)
        {
            // A gate that places nothing on the spline: its outline only, dashed.
            for (uint32 corner = 0; corner < dashedCount; ++corner)
                EmitDashedLine(ctx, dashed[corner], dashed[(corner + 1) % dashedCount], gateLine);
            continue;
        }
        const uint32_t fill = fills[static_cast<std::size_t>(shape.PartRole)];
        if (shape.OutlineCount == 4)
            EmitQuad(ctx, outline, fill);
        else if (shape.OutlineCount >= 3)
            EmitConvexFill(ctx, outline, shape.OutlineCount, fill);

        if (shape.PartRole == Role::Span || shape.PartRole == Role::Gate)
        {
            // The seam at the span's back end parts it from the span before it; below the ground the
            // box's outline takes its place.
            EmitAboveGround(ctx, frame, m_Run, box[3], box[0], kSeamThicknessPx * contentScale, seam);
        }
        if (shape.PartRole == Role::Gate)
        {
            // The gate's outline, the stile where its two leaves meet, and a brace on each leaf.
            if (shape.Buried)
            {
                for (std::size_t corner = 0; corner < 4; ++corner)
                    EmitPartLine(ctx, frame, shape, box[corner], box[(corner + 1) % 4], thickness, gateLine, buried);
            }
            else
            {
                for (uint32 corner = 0; corner < shape.OutlineCount; ++corner)
                {
                    const Vector2& from = outline[corner];
                    const Vector2& to = outline[(corner + 1) % shape.OutlineCount];
                    ctx.Emit(UI::MakeLine(from.x, from.y, to.x, to.y, thickness, gateLine));
                }
            }
            const Vector2 topMiddle = (box[2] + box[3]) * 0.5f;
            const Vector2 bottomMiddle = (box[0] + box[1]) * 0.5f;
            EmitPartLine(ctx, frame, shape, bottomMiddle, topMiddle, thickness, gateLine, buried);
            EmitPartLine(ctx, frame, shape, box[0], topMiddle, thickness, gateLine, buried);
            EmitPartLine(ctx, frame, shape, box[1], topMiddle, thickness, gateLine, buried);
        }
        else if (shape.Buried)
        {
            for (std::size_t corner = 0; corner < 4; ++corner)
                EmitBelowGround(ctx, frame, m_Run, box[corner], box[(corner + 1) % 4], thickness, buried);
        }
        if (shape.Measured)
            continue;
        for (uint32 corner = 0; corner < dashedCount; ++corner)
            EmitDashedLine(ctx, dashed[corner], dashed[(corner + 1) % dashedCount], dashColor);
    }
}

// Draws the line from `from` to `to` (run meters) of `shape` in `color`; the part below the ground of
// a buried shape draws in `buriedColor`.
void SplineFencePreview::EmitPartLine(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const Shape& shape,
                                      const Vector2& from, const Vector2& to, float32 thickness, uint32_t color,
                                      uint32_t buriedColor) const
{
    if (!shape.Buried)
    {
        EmitSegmentPart(ctx, frame, from, to, 0.0f, 1.0f, thickness, color);
        return;
    }
    EmitAboveGround(ctx, frame, m_Run, from, to, thickness, color);
    EmitBelowGround(ctx, frame, m_Run, from, to, thickness, buriedColor);
}

// Draws the bar from the first post to the second under the floor, with its ticks and its text at
// its right. The text ends left of `marksLeft`, the marks' left edge: it is the laid length alone
// when the full text has no room there, and it is left out when neither has room.
void SplineFencePreview::EmitSpanLengthBar(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame,
                                           const ResolvedStyle& style, float32 marksLeft) const
{
    if (m_SpanLengthText.empty())
        return;
    const float32 contentScale = ctx.ContentScale;
    const uint32_t argb = style.GetCustomColor(kLabelVar).value_or(style.Visual.Color);
    const uint32_t color = UI::PackFromARGB(argb);
    const float32 top = frame.Y(m_HeightMin) + kLabelGapPx * contentScale;
    const float32 barY = top + 0.5f * RecipePreviewLineHeight(ctx, style);
    const float32 left = frame.X(m_PostLines[0].Along);
    const float32 right = frame.X(m_PostLines[1].Along);
    const float32 tick = kBarTickPx * contentScale;
    const float32 thickness = kDashThicknessPx * contentScale;
    ctx.Emit(UI::MakeLine(left, barY, right, barY, thickness, color));
    ctx.Emit(UI::MakeLine(left, barY - tick, left, barY + tick, thickness, color));
    ctx.Emit(UI::MakeLine(right, barY - tick, right, barY + tick, thickness, color));
    const float32 textLeft = right + kBarTextGapPx * contentScale;
    for (const std::string& text : {m_SpanLengthText, m_SpanLengthShortText})
    {
        const float32 width = RecipePreviewTextWidth(ctx, style, text);
        if (textLeft + width + kSpanLabelSpacingPx * contentScale > marksLeft)
            continue;
        float32 previousRight = right;
        EmitRecipePreviewLabel(ctx, style, text, argb, textLeft + 0.5f * width, top, previousRight);
        return;
    }
}

void SplineFencePreview::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, float, float,
                                              float, float)
{
    // Parallel-drain thread contract (UIElement.h): the labels shape text on the UI thread.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }
    if (!m_Plot || !m_HasRecipe)
        return;
    GE_CPU_PROFILE_SCOPE("SplineFencePreview.Draw");

    // The plot's rect holds the run; its margins hold the labels and the scale bar.
    const float32 contentScale = ctx.ContentScale;
    const float32 plotX = m_Plot->GetLayoutX() * contentScale;
    const float32 plotY = m_Plot->GetLayoutY() * contentScale;
    const float32 plotWidth = m_Plot->GetLayoutWidth() * contentScale;
    const float32 plotHeight = m_Plot->GetLayoutHeight() * contentScale;
    if (plotWidth <= 0.0f || plotHeight <= 0.0f)
        return;
    const RecipePreviewFrame frame =
        FitRecipePreviewFrame(plotX, plotY, plotWidth, plotHeight, m_AlongMin, m_AlongMax, m_HeightMin, m_HeightMax);

    EmitReferenceRunGround(ctx, frame, m_Run, style, m_HeightMin);
    EmitShapes(ctx, frame, style);

    // A dashed line at each post runs through the parts that stand there.
    const uint32_t stationColor = RecipePreviewColor(style, kStationVar);
    for (const PostLine& line : m_PostLines)
    {
        EmitDashedLine(ctx, Vector2(frame.X(line.Along), frame.Y(line.Base)),
                       Vector2(frame.X(line.Along), frame.Y(line.Top)), stationColor);
    }

    EmitReferenceRunSurface(ctx, frame, m_Run, style);
    // The scale bar stands in the plot's left margin, left of the leftmost part.
    EmitRecipePreviewScaleBar(ctx, frame, style, frame.X(m_AlongMin) - RecipePreviewScaleBarWidth(ctx, frame, style),
                              m_Run.GroundHeight(0.0f));

    // The marks keep their place under the ground; the Span bar's text yields to them.
    const float32 barRight =
        m_PostLines.size() >= 2u ? frame.X(m_PostLines[1].Along) : -std::numeric_limits<float32>::max();
    const float32 marksLeft = EmitReferenceRunMarks(ctx, frame, m_Run, style, m_HeightMin, barRight);
    EmitSpanLengthBar(ctx, frame, style, marksLeft);

    // The labels stay inside the strip.
    const LabelExtent stripBounds{GetLayoutX() * contentScale, (GetLayoutX() + GetLayoutWidth()) * contentScale};
    EmitSpanLabels(ctx, frame, style, stripBounds);
}

// Draws `text` centered over the box of `label`, above every part under the text, when it overlaps no
// label drawn before it. Near an end of `bounds` the text moves inside them, as far as it still covers
// the box's center. Returns whether it drew.
bool SplineFencePreview::EmitSpanLabel(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame,
                                       const ResolvedStyle& style, const SpanLabel& label, std::string_view text,
                                       uint32_t argb, const LabelExtent& bounds)
{
    const float32 width = RecipePreviewTextWidth(ctx, style, text);
    if (width > bounds.Right - bounds.Left)
        return false;
    const float32 boxCenterX = 0.5f * (frame.X(label.Left) + frame.X(label.Right));
    const float32 centerX = std::clamp(boxCenterX, bounds.Left + 0.5f * width, bounds.Right - 0.5f * width);
    if (std::abs(centerX - boxCenterX) > 0.5f * width)
        return false;
    const float32 gap = kSpanLabelSpacingPx * ctx.ContentScale;
    const LabelExtent extent{centerX - 0.5f * width - gap, centerX + 0.5f * width + gap};
    const bool overlaps = std::any_of(m_DrawnLabels.begin(), m_DrawnLabels.end(), [&extent](const LabelExtent& drawn) {
        return extent.Left < drawn.Right && drawn.Left < extent.Right;
    });
    if (overlaps)
        return false;
    const float32 textLeft = frame.Along(centerX - 0.5f * width);
    const float32 textRight = frame.Along(centerX + 0.5f * width);
    const float32 ground = std::max(m_Run.GroundHeight(textLeft), m_Run.GroundHeight(textRight));
    const float32 partsTop = std::max(ground, TopOfPartsBetween(textLeft, textRight, ground));
    const float32 top = frame.Y(partsTop) - RecipePreviewLineHeight(ctx, style) - kLabelGapPx * ctx.ContentScale;
    float32 previousRight = -std::numeric_limits<float32>::max();
    if (!EmitRecipePreviewLabel(ctx, style, text, argb, centerX, top, previousRight))
        return false;
    m_DrawnLabels.push_back(extent);
    return true;
}

// Draws the gate labels first, then the span labels, so a gate keeps its name among many short spans.
// Each label takes the longest of its forms that overlaps no label drawn before it, or is left out.
void SplineFencePreview::EmitSpanLabels(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame,
                                        const ResolvedStyle& style, const LabelExtent& bounds)
{
    const uint32_t labelArgb = style.GetCustomColor(kLabelVar).value_or(style.Visual.Color);
    const uint32_t gateArgb = style.GetCustomColor(kGateVar).value_or(labelArgb);
    m_DrawnLabels.clear();
    for (const bool gates : {true, false})
    {
        for (const SpanLabel& label : m_SpanLabels)
        {
            if (label.IsGate != gates)
                continue;
            const uint32_t argb = gates ? gateArgb : labelArgb;
            const std::string span = "Span " + std::to_string(label.Ordinal);
            if (!label.Placed)
            {
                if (!EmitSpanLabel(ctx, frame, style, label, "Gate: not placed", argb, bounds))
                    EmitSpanLabel(ctx, frame, style, label, "Not placed", argb, bounds);
                continue;
            }
            if (!gates)
            {
                EmitSpanLabel(ctx, frame, style, label, span, argb, bounds);
                continue;
            }
            if (!EmitSpanLabel(ctx, frame, style, label, "Gate: " + span, argb, bounds))
                EmitSpanLabel(ctx, frame, style, label, span, argb, bounds);
        }
    }
}

} // namespace GameEngine::EditorUI

namespace RegisterSplineFencePreview
{
static auto s_RegisterSplineFencePreview =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::EditorUI::SplineFencePreview>(
        "SplineFencePreview", []() { return std::make_unique<GameEngine::EditorUI::SplineFencePreview>(); });
}
