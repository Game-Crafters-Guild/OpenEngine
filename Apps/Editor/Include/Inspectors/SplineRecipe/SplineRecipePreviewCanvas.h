#pragma once

#include "SplineLayout/TileLayout.h"
#include "Types/StringId.h"
#include "Types/Types.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class UIElement;
struct ResolvedStyle;
namespace UI
{
struct PrimitiveEmitContext;
}
} // namespace GameEngine

namespace GameEngine::EditorUI
{

// What the spline recipe preview strips share: the reference run, the mapping from run meters to
// strip pixels, and the ground and scale they draw under a recipe. The strips draw with the real
// layout functions; nothing here lays a recipe out.

// The reference run of the fence and placement strips: along +Z, flat for its first two thirds, then
// a 1 in 4 rise. Its height at the start is 0 m.
inline constexpr float32 kReferenceRunLength = 12.0f;
inline constexpr float32 kReferenceRunFlatShare = 2.0f / 3.0f;
inline constexpr float32 kReferenceRunGrade = 0.25f;
// A part longer than the 12 m run makes the run this many of the part long.
inline constexpr uint32 kReferenceRunGrownPartCount = 4;

// The run a strip lays its recipe out on: 12 m, or four of the longest part when that part is longer.
struct ReferenceRun
{
    float32 Length = kReferenceRunLength;
    // How many part lengths the run grew to, 0 for the 12 m run, and that part's role, such as "span".
    uint32 GrownPartCount = 0;
    std::string_view GrownPartName;

    float32 FlatLength() const { return Length * kReferenceRunFlatShare; }
    // The ground height at `along` meters.
    float32 GroundHeight(float32 along) const { return std::max(along - FlatLength(), 0.0f) * kReferenceRunGrade; }
    bool operator==(const ReferenceRun&) const = default;
};

// Maps run meters to strip pixels at one scale on both axes, so a slope and a post show at their
// true angle. Pixels are physical; along grows right and height grows up.
struct RecipePreviewFrame
{
    float32 OriginX = 0.0f;
    float32 OriginY = 0.0f;
    float32 PixelsPerMeter = 0.0f;
    float32 AlongMin = 0.0f;
    float32 HeightMin = 0.0f;

    float32 X(float32 along) const { return OriginX + (along - AlongMin) * PixelsPerMeter; }
    float32 Y(float32 height) const { return OriginY - (height - HeightMin) * PixelsPerMeter; }
    // The run meters at the pixel column `x`.
    float32 Along(float32 x) const { return AlongMin + (x - OriginX) / PixelsPerMeter; }
};

// Makes `strip` a preview strip styled by SplineRecipePreview.css. A strip's constructor calls it.
void PrepareRecipePreviewStrip(UIElement& strip);

// The frame for the run box [alongMin, alongMax] x [heightMin, heightMax] in the physical pixel rect
// (x, y, width, height), on its floor and centered across it. The box takes the largest scale at
// which it fits on both axes.
RecipePreviewFrame FitRecipePreviewFrame(float32 x, float32 y, float32 width, float32 height, float32 alongMin,
                                         float32 alongMax, float32 heightMin, float32 heightMax);

// The run for a recipe whose longest part is `longestPart` meters long; `partName` names that part's
// role, such as "span", and must outlive the run.
ReferenceRun ReferenceRunFor(float32 longestPart, std::string_view partName);

// The strip's caption for `run`, for example "Reference run: 12 m, flat, then a 1 in 4 slope" or
// "Reference run: 60 m (4 span lengths), flat, then a 1 in 4 slope".
std::string ReferenceRunCaption(const ReferenceRun& run);

// The decimals of a laid length, as the Span Length field gives them: "4.04 m".
inline constexpr int kRecipePreviewLaidLengthDecimals = 2;

// The text of a length bar: the length `laid` that the layout laid, and the length `authored` it
// was stretched or shortened from when the two differ, for example "4.04 m laid (stretched from 3.6)".
// `authoredIsMeshLength` is true when `authored` is a mesh's length, false when it is a field's value.
std::string RecipePreviewLaidLengthText(float32 laid, float32 authored, bool authoredIsMeshLength);

// The run as a centerline on its ground, with the ground's normal at each sample.
std::vector<SplineLayout::CenterSample> ReferenceRunCenterline(const ReferenceRun& run);

// The color of the custom property `name` of `style`, packed for a primitive; transparent when unset.
uint32_t RecipePreviewColor(const ResolvedStyle& style, StringId name);

// Fills the ground of `run` down to `floorHeight`.
void EmitReferenceRunGround(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ReferenceRun& run,
                            const ResolvedStyle& style, float32 floorHeight);

// Marks, under the floor, where the slope of `run` starts and where the run ends. A mark that would
// start left of `previousRight` is left out. Returns the left edge of the leftmost mark it drew, or
// the largest float when it drew none.
float32 EmitReferenceRunMarks(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ReferenceRun& run,
                              const ResolvedStyle& style, float32 floorHeight, float32 previousRight);

// Draws the surface line of `run`. A strip draws it over the recipe, so a part that reaches below its
// pivot shows where it meets the ground.
void EmitReferenceRunSurface(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ReferenceRun& run,
                             const ResolvedStyle& style);

// The height of one line of the plate's labels, in physical pixels.
float32 RecipePreviewLineHeight(const UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style);

// The width the scale bar of `frame` and its label take, in physical pixels.
float32 RecipePreviewScaleBarWidth(const UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame,
                                   const ResolvedStyle& style);

// Draws the scale bar with its label, its line at `x`, standing on `height`. The bar is the shortest
// of 1, 2, 5 and 10 m that is at least 40 px tall in `frame`, or 10 m.
void EmitRecipePreviewScaleBar(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame,
                               const ResolvedStyle& style, float32 x, float32 height);

// The width of `text` in the plate's labels, in physical pixels.
float32 RecipePreviewTextWidth(const UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, std::string_view text);

// Draws `text` centered on `centerX` with its top at `top`, when it starts right of `previousRight`.
// Returns whether it drew, and sets `previousRight` to its right edge when it did.
bool EmitRecipePreviewLabel(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, std::string_view text,
                            uint32_t argb, float32 centerX, float32 top, float32& previousRight);

} // namespace GameEngine::EditorUI
