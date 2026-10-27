#pragma once

#include "Components/Spline/SplineFence.h"
#include "Inspectors/SplineRecipe/SplineRecipePreviewCanvas.h"
#include "Mathematics/Vector2.h"
#include "SplineLayout/FenceLayout.h"
#include "UI/UIElement.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
class Label;
namespace SplineRecipes
{
struct FenceMeshBounds;
}
} // namespace GameEngine

namespace GameEngine::EditorUI
{

/// The fence's preview strip: the recipe laid out on the reference run by SplineLayout::BuildFenceLayout,
/// the function the fence's rebuild calls, and drawn along the run. Its layout is
/// UI/inspectors/SplineRecipePreview/SplineFencePreview.uxml. It takes no pointer input.
class SplineFencePreview final : public UIElement
{
  public:
    /// A strip with no plate: the layout's factory makes the strip this way. Use Create.
    SplineFencePreview();

    /// A strip built from its layout; a bare strip that draws nothing when the layout did not load.
    /// The strip takes no pointer input: its stylesheet sets pointer-events to none.
    static std::unique_ptr<SplineFencePreview> Create();

    /// Lays `recipe` out again when it, `measured` (RecipeControllers::FencePartBounds) or `unplacedGates`
    /// changes. A part that `measured` does not list draws dashed at a stand-in size, with a note. A gate
    /// at a (point, span) that `unplacedGates` lists draws as a dashed outline labeled "Gate: not placed".
    void SetFence(const Components::SplineFence& recipe, std::span<const SplineRecipes::FenceMeshBounds> measured,
                  std::span<const std::pair<uint32, uint32>> unplacedGates);

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, float x, float y, float w,
                              float h) override;

  private:
    // The bounds of one role's pool in slot order, and whether the rebuild measured each.
    struct PoolPieces
    {
        std::vector<SplineLayout::FencePieceBounds> Bounds;
        std::vector<bool> Measured;
    };

    // The pools the strip resolves: Post, Span, Gate, Crest and Cap.
    static constexpr std::size_t kPoolCount = 5;

    // What a shape draws as; the value indexes the strip's colors.
    enum class Role : uint8_t
    {
        Post,
        Span,
        Gate,
        Crest,
    };

    // The most corners a part's outline has after the ground cuts its box.
    static constexpr std::size_t kMaxOutlineCorners = 6;

    // One part as the strip draws it, in run meters (along, height): its side box, and the outline it
    // fills, which is the part of the box above the ground line.
    struct Shape
    {
        Mathematics::Vector2 Corners[4];
        Mathematics::Vector2 Outline[kMaxOutlineCorners];
        uint32 OutlineCount = 0;
        // A Stepped span: the strip leaves out its part below the ground line.
        bool CutAtGround = false;
        // Part of the box is below the ground line: the strip draws that part as an outline only.
        bool Buried = false;
        Role PartRole = Role::Span;
        bool Measured = true;
        // False for a gate whose override places nothing on the spline.
        bool Placed = true;
    };

    // A post line: where a station stands and the top of the parts at it, in run meters.
    struct PostLine
    {
        float32 Along = 0.0f;
        float32 Base = 0.0f;
        float32 Top = 0.0f;
    };

    // A span's name above it: the ordinal the override rows use, over the span's box from Left to
    // Right, in run meters.
    struct SpanLabel
    {
        float32 Left = 0.0f;
        float32 Right = 0.0f;
        uint32 Ordinal = 0;
        bool IsGate = false;
        bool Placed = true;
    };

    // The left and right edge of a drawn label, in physical pixels.
    struct LabelExtent
    {
        float32 Left = 0.0f;
        float32 Right = 0.0f;
    };

    bool SameParts(const Components::SplineFence& recipe, std::span<const SplineRecipes::FenceMeshBounds> measured) const;
    void ResolveParts(const Components::SplineFence& recipe, std::span<const SplineRecipes::FenceMeshBounds> measured);
    void FitRun();
    void Relayout();
    void AddShape(const SplineLayout::TilePose& pose, const SplineLayout::FencePieceBounds& bounds, float32 lengthScale,
                  Role role, bool measured);
    float32 TopOfPartsBetween(float32 left, float32 right, float32 fallback) const;
    void UpdateLegend();
    void EmitShapes(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ResolvedStyle& style) const;
    void EmitPartLine(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const Shape& shape,
                      const Mathematics::Vector2& from, const Mathematics::Vector2& to, float32 thickness,
                      uint32_t color, uint32_t buriedColor) const;
    void EmitSpanLengthBar(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ResolvedStyle& style,
                           float32 marksLeft) const;
    bool EmitSpanLabel(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ResolvedStyle& style,
                       const SpanLabel& label, std::string_view text, uint32_t argb, const LabelExtent& bounds);
    void EmitSpanLabels(UI::PrimitiveEmitContext& ctx, const RecipePreviewFrame& frame, const ResolvedStyle& style,
                        const LabelExtent& bounds);

    Components::SplineFence m_Recipe{};
    bool m_HasRecipe = false;
    std::vector<std::pair<uint32, uint32>> m_UnplacedGates;
    PoolPieces m_Pools[kPoolCount];
    std::vector<SplineLayout::CenterSample> m_Center;
    std::vector<float32> m_RunBoundaries;
    std::vector<Shape> m_Shapes;
    std::vector<PostLine> m_PostLines;
    std::vector<SpanLabel> m_SpanLabels;
    // The x extent of each span label the last draw placed, in physical pixels.
    std::vector<LabelExtent> m_DrawnLabels;
    ReferenceRun m_Run;
    // The Span bar's text, and the laid length alone for a bar with no room for the text.
    std::string m_SpanLengthText;
    std::string m_SpanLengthShortText;
    // The extent of the run and its parts, in run meters.
    float32 m_AlongMin = 0.0f;
    float32 m_AlongMax = 0.0f;
    float32 m_HeightMin = 0.0f;
    float32 m_HeightMax = 0.0f;

    Label* m_Caption = nullptr;
    UIElement* m_Plot = nullptr;
    UIElement* m_Note = nullptr;
    UIElement* m_PostEntry = nullptr;
    UIElement* m_SpanEntry = nullptr;
    UIElement* m_GateEntry = nullptr;
    UIElement* m_CrestEntry = nullptr;
    // The line under the plot that gives the laid length of each measured gate the layout scaled.
    Label* m_GateLength = nullptr;
};

} // namespace GameEngine::EditorUI
