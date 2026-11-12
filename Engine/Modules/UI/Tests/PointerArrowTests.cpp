// The pointer arrow of the hover tooltip and of PopoverArrow: it points at the center of its
// source on each side, stays on the straight part of the bubble's edge when the bubble is
// clamped, and takes its fill and border from the bubble's style. The arrow of a tooltip
// anchored to a panel sits on the panel's edge, lined up with the hovered tile.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"

#include "Mathematics/Vector2.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

using namespace GameEngine;
using Mathematics::Vector2;

namespace
{

// A 400 x 300 window with one tooltip source in its middle. The bubble has a fill, a
// border and rounded corners of its own, all from the stylesheet.
constexpr char kCenteredXml[] = R"(<uielement id="root"><uielement id="source"/></uielement>)";
constexpr char kCenteredCss[] = R"(
#root { width: 400px; height: 300px; }
#source { position: absolute; left: 175px; top: 135px; width: 50px; height: 30px; }
.tooltip { background-color: #203040; border-width: 2px; border-color: #c08020; border-radius: 6px; padding: 4px 8px; }
)";

// The same window with the source at its left edge: a bubble below it is clamped to the window.
constexpr char kEdgeCss[] = R"(
#root { width: 400px; height: 300px; }
#source { position: absolute; left: 0px; top: 135px; width: 12px; height: 30px; }
.tooltip { background-color: #203040; border-width: 2px; border-color: #c08020; border-radius: 6px; padding: 4px 8px; }
)";

constexpr char kText[] = "Build Barracks";
constexpr float kBubbleRadius = 6.0f;
constexpr float kBubbleBorder = 2.0f;

// Hovers the source past the hover delay, then pumps frames for the bubble to measure and place.
void HoverSource(UITesting::IsolatedUIFixture& fixture, UIElement::TooltipPlacement placement)
{
    UIElement* source = fixture.Element("source");
    source->SetTooltip(kText);
    source->SetTooltipPlacement(placement);
    fixture.Manager().OnMouseMove(source->GetLayoutX() + 5.0f, source->GetLayoutY() + 5.0f);
    for (int frame = 0; frame < 4; ++frame)
        fixture.PumpSeconds(0.3f);
}

std::vector<UI::UIPrimitive> ArrowPrimitives(const UIManager& manager)
{
    std::vector<UI::UIPrimitive> out;
    for (uint16_t i = 0;; ++i)
    {
        const UI::UIPrimitive* primitive = manager.PeekTooltipArrowForTesting(i);
        if (!primitive)
            break;
        out.push_back(*primitive);
    }
    return out;
}

// The three vertices of a triangle primitive (MakeTriangle's storage).
std::array<Vector2, 3> Vertices(const UI::UIPrimitive& triangle)
{
    return {Vector2(triangle.UvRect[0], triangle.UvRect[1]), Vector2(triangle.UvRect[2], triangle.UvRect[3]),
            Vector2(triangle.BorderWidths[0], triangle.BorderWidths[1])};
}

// The vertex nearest to `point`: the tip of an arrow that points at it.
Vector2 NearestVertex(const UI::UIPrimitive& triangle, Vector2 point)
{
    const auto vertices = Vertices(triangle);
    return *std::min_element(vertices.begin(), vertices.end(), [point](Vector2 a, Vector2 b) {
        return (a - point).Length() < (b - point).Length();
    });
}

struct Box
{
    float X = 0.0f;
    float Y = 0.0f;
    float W = 0.0f;
    float H = 0.0f;
};

Box LayoutBox(const UIElement& element, float scale)
{
    return {element.GetLayoutX() * scale, element.GetLayoutY() * scale, element.GetLayoutWidth() * scale,
            element.GetLayoutHeight() * scale};
}

// A 200 x 120 panel holding 20 x 30 tiles in three rows. The top row has a tile at each end of
// the panel and two between them; the column at x 90 has a tile in each row.
constexpr char kGridXml[] = R"(<uielement id="root"><uielement id="panel">
<uielement id="row0-col0"/><uielement id="row0-col1"/><uielement id="row0-col2"/><uielement id="row0-col3"/>
<uielement id="row1-col1"/><uielement id="row2-col1"/></uielement></uielement>)";
constexpr char kGridCss[] = R"(
#root { width: 400px; height: 300px; }
#panel { position: absolute; left: 100px; top: 150px; width: 200px; height: 120px; }
#row0-col0 { position: absolute; left: 0px; top: 0px; width: 20px; height: 30px; }
#row0-col1 { position: absolute; left: 90px; top: 0px; width: 20px; height: 30px; }
#row0-col2 { position: absolute; left: 140px; top: 0px; width: 20px; height: 30px; }
#row0-col3 { position: absolute; left: 180px; top: 0px; width: 20px; height: 30px; }
#row1-col1 { position: absolute; left: 90px; top: 45px; width: 20px; height: 30px; }
#row2-col1 { position: absolute; left: 90px; top: 90px; width: 20px; height: 30px; }
.tooltip { background-color: #203040; border-width: 2px; border-color: #c08020; border-radius: 6px; padding: 4px 8px; }
)";
constexpr const char* kGridTiles[] = {"row0-col0", "row0-col1", "row0-col2", "row0-col3", "row1-col1", "row2-col1"};

// A 60 x 240 panel holding 20 x 30 tiles in two columns: one in each column at y 100, and one
// in the right column at y 160.
constexpr char kColumnXml[] = R"(<uielement id="root"><uielement id="panel">
<uielement id="left-row1"/><uielement id="right-row1"/><uielement id="right-row2"/></uielement></uielement>)";
constexpr char kColumnCss[] = R"(
#root { width: 400px; height: 300px; }
#panel { position: absolute; left: 170px; top: 30px; width: 60px; height: 240px; }
#left-row1 { position: absolute; left: 0px; top: 100px; width: 20px; height: 30px; }
#right-row1 { position: absolute; left: 40px; top: 100px; width: 20px; height: 30px; }
#right-row2 { position: absolute; left: 40px; top: 160px; width: 20px; height: 30px; }
.tooltip { background-color: #203040; border-width: 2px; border-color: #c08020; border-radius: 6px; padding: 4px 8px; }
)";
constexpr const char* kColumnTiles[] = {"left-row1", "right-row1", "right-row2"};

// Gives each tile the same tooltip, on `placement`, anchored to the panel.
void AnchorTilesToThePanel(UITesting::IsolatedUIFixture& fixture, std::span<const char* const> tiles,
                           UIElement::TooltipPlacement placement)
{
    UIElement* panel = fixture.Element("panel");
    for (const char* id : tiles)
    {
        UIElement* tile = fixture.Element(id);
        tile->SetTooltip(kText);
        tile->SetTooltipPlacement(placement);
        tile->SetTooltipAnchor(panel);
    }
}

struct HoveredArrow
{
    Box Bubble;
    Box Tile;
    Vector2 Tip;
    std::vector<UI::UIPrimitive> Triangles;
};

// Moves the pointer onto the tile and pumps past the hover delay. The tip is the vertex of the
// outer triangle that lies furthest along `pointing`. False when no arrow is drawn.
bool HoverTile(UITesting::IsolatedUIFixture& fixture, const char* tileId, Vector2 pointing, HoveredArrow& out)
{
    const UIElement* tile = fixture.Element(tileId);
    fixture.Manager().OnMouseMove(tile->GetLayoutX() + 5.0f, tile->GetLayoutY() + 5.0f);
    for (int frame = 0; frame < 4; ++frame)
        fixture.PumpSeconds(0.3f);
    out.Triangles = ArrowPrimitives(fixture.Manager());
    if (out.Triangles.empty())
        return false;
    const auto vertices = Vertices(out.Triangles.front());
    out.Tip = *std::max_element(vertices.begin(), vertices.end(), [pointing](Vector2 a, Vector2 b) {
        return Vector2::Dot(a, pointing) < Vector2::Dot(b, pointing);
    });
    out.Bubble = LayoutBox(*fixture.Element("ui-tooltip"), 1.0f);
    out.Tile = LayoutBox(*tile, 1.0f);
    return true;
}

float CenterX(const Box& box)
{
    return box.X + box.W * 0.5f;
}

float CenterY(const Box& box)
{
    return box.Y + box.H * 0.5f;
}

} // namespace

// Each side: the tip sits just off the source, level with its center, and the base sits on
// the bubble's facing edge.
TEST(PointerArrowTests, TheTooltipArrowPointsAtTheSourceCenterOnEachSide)
{
    using Placement = UIElement::TooltipPlacement;
    for (const Placement placement : {Placement::Right, Placement::Left, Placement::Above, Placement::Below})
    {
        SCOPED_TRACE(static_cast<int>(placement));
        UITesting::IsolatedUIFixture fixture;
        const bool built = fixture.Build(1.0f, kCenteredXml, kCenteredCss);
        if (!fixture.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built);
        HoverSource(fixture, placement);

        const auto arrow = ArrowPrimitives(fixture.Manager());
        ASSERT_FALSE(arrow.empty()) << "no arrow emitted";
        const Box source = LayoutBox(*fixture.Element("source"), 1.0f);
        const Box bubble = LayoutBox(*fixture.Element("ui-tooltip"), 1.0f);
        const Vector2 center(source.X + source.W * 0.5f, source.Y + source.H * 0.5f);
        const Vector2 tip = NearestVertex(arrow.front(), center);

        const bool horizontal = placement == Placement::Right || placement == Placement::Left;
        if (horizontal)
        {
            EXPECT_NEAR(tip.y, center.y, 0.5f) << "the tip is level with the source's center";
            const float sourceEdge = placement == Placement::Right ? source.X + source.W : source.X;
            const float bubbleEdge = placement == Placement::Right ? bubble.X : bubble.X + bubble.W;
            EXPECT_GT(std::abs(tip.x - center.x), source.W * 0.5f) << "the tip is outside the source";
            EXPECT_LE(std::abs(tip.x - sourceEdge), 10.0f) << "the tip is near the source's edge";
            EXPECT_LT(std::abs(tip.x - sourceEdge), std::abs(bubbleEdge - sourceEdge)) << "the tip is off the bubble";
        }
        else
        {
            EXPECT_NEAR(tip.x, center.x, 0.5f) << "the tip is level with the source's center";
            const float sourceEdge = placement == Placement::Below ? source.Y + source.H : source.Y;
            const float bubbleEdge = placement == Placement::Below ? bubble.Y : bubble.Y + bubble.H;
            EXPECT_GT(std::abs(tip.y - center.y), source.H * 0.5f) << "the tip is outside the source";
            EXPECT_LE(std::abs(tip.y - sourceEdge), 10.0f) << "the tip is near the source's edge";
            EXPECT_LT(std::abs(tip.y - sourceEdge), std::abs(bubbleEdge - sourceEdge)) << "the tip is off the bubble";
        }
    }
}

// The source is at the window's left edge, so the bubble below it is pushed right of the
// source's center. The arrow follows the source as far as it can and stops where the
// bubble's rounded corner starts: its base stays on the straight part of the top edge.
TEST(PointerArrowTests, TheTooltipArrowStaysOnTheBubblesEdgeWhenTheBubbleIsClamped)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kCenteredXml, kEdgeCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built);
    HoverSource(fixture, UIElement::TooltipPlacement::Below);

    const auto arrow = ArrowPrimitives(fixture.Manager());
    ASSERT_FALSE(arrow.empty()) << "no arrow emitted";
    const Box source = LayoutBox(*fixture.Element("source"), 1.0f);
    const Box bubble = LayoutBox(*fixture.Element("ui-tooltip"), 1.0f);
    ASSERT_GT(bubble.X, source.X + source.W * 0.5f - kBubbleRadius) << "the bubble must be clamped past the source";

    for (const UI::UIPrimitive& triangle : arrow)
    {
        for (const Vector2 vertex : Vertices(triangle))
        {
            EXPECT_GE(vertex.x, bubble.X + kBubbleRadius - 0.01f) << "a vertex is on the rounded corner";
            EXPECT_LE(vertex.x, bubble.X + bubble.W - kBubbleRadius + 0.01f) << "a vertex is on the rounded corner";
            EXPECT_LE(vertex.y, bubble.Y + kBubbleBorder + 1.01f) << "a vertex is inside the bubble's content";
        }
    }
}

// The arrow is drawn in the bubble's paint: a border triangle in the bubble's border color,
// then a fill triangle in its background color. Physical px at a content scale of 1.5.
TEST(PointerArrowTests, TheTooltipArrowTakesTheBubblesFillAndBorder)
{
    constexpr float kScale = 1.5f;
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(kScale, kCenteredXml, kCenteredCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built);
    HoverSource(fixture, UIElement::TooltipPlacement::Below);

    const auto bubblePaint = fixture.Primitives("ui-tooltip", UI::PrimitiveMode::Rect);
    ASSERT_FALSE(bubblePaint.empty()) << "the bubble painted nothing";
    const auto arrow = ArrowPrimitives(fixture.Manager());
    ASSERT_EQ(arrow.size(), 2u) << "a bordered bubble's arrow is a border triangle and a fill triangle";
    EXPECT_EQ(arrow[0].FillColor, bubblePaint.front().BorderColor) << "the border triangle takes the bubble's border";
    EXPECT_EQ(arrow[1].FillColor, bubblePaint.front().FillColor) << "the fill triangle takes the bubble's background";

    const Box source = LayoutBox(*fixture.Element("source"), kScale);
    const Vector2 center(source.X + source.W * 0.5f, source.Y + source.H * 0.5f);
    EXPECT_NEAR(NearestVertex(arrow[0], center).x, center.x, 0.5f) << "the tip is in physical px";
}

// The bubble's 2 px border goes around the arrow's fill and does not make it smaller: the fill reaches as far
// past the bubble's edge, and is as wide on that edge, as the fill of an arrow with no border.
TEST(PointerArrowTests, TheTooltipArrowsFillKeepsItsSizeWhenTheBorderIsWide)
{
    constexpr float kFillHeight = 8.0f;
    constexpr float kFillBase = 9.0f;
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kCenteredXml, kCenteredCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built);
    HoverSource(fixture, UIElement::TooltipPlacement::Below);

    const auto arrow = ArrowPrimitives(fixture.Manager());
    ASSERT_EQ(arrow.size(), 2u) << "a bordered bubble's arrow is a border triangle and a fill triangle";
    const Box bubble = LayoutBox(*fixture.Element("ui-tooltip"), 1.0f);
    auto fill = Vertices(arrow[1]);
    std::sort(fill.begin(), fill.end(), [](Vector2 a, Vector2 b) { return a.y < b.y; });
    const Vector2 apex = fill[0];
    const float baseY = fill[1].y;
    ASSERT_NEAR(fill[2].y, baseY, 0.01f) << "the fill's base is level";
    EXPECT_NEAR(bubble.Y - apex.y, kFillHeight, 0.25f) << "the fill's apex is as far past the bubble's edge";
    const float widthAtEdge = std::abs(fill[2].x - fill[1].x) * (bubble.Y - apex.y) / (baseY - apex.y);
    EXPECT_NEAR(widthAtEdge, kFillBase, 0.3f) << "the fill is as wide on the bubble's edge";
}

// PopoverArrow points the way its `side` attribute names and takes the fill and border of the
// bubble it is a child of.
TEST(PointerArrowTests, APopoverArrowPointsBySideInItsBubblesPaint)
{
    constexpr char kXml[] = R"(<uielement id="root"><uielement id="panel"><PopoverArrow id="arrow" side="down"/></uielement></uielement>)";
    constexpr char kCss[] = R"(
#root { width: 400px; height: 300px; }
#panel { position: absolute; left: 100px; top: 40px; width: 200px; height: 120px; background-color: #203040; border-width: 2px; border-color: #c08020; border-radius: 6px; }
#arrow { position: absolute; left: 90px; bottom: -12px; width: 20px; height: 12px; }
)";
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kXml, kCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built);

    const auto panelPaint = fixture.Primitives("panel", UI::PrimitiveMode::Rect);
    ASSERT_FALSE(panelPaint.empty()) << "the panel painted nothing";
    const auto arrow = fixture.Primitives("arrow", UI::PrimitiveMode::Triangle);
    ASSERT_EQ(arrow.size(), 2u) << "a bordered bubble's arrow is a border triangle and a fill triangle";
    EXPECT_EQ(arrow[0].FillColor, panelPaint.front().BorderColor) << "the border triangle takes the panel's border";
    EXPECT_EQ(arrow[1].FillColor, panelPaint.front().FillColor) << "the fill triangle takes the panel's background";

    const Box panel = LayoutBox(*fixture.Element("panel"), 1.0f);
    const Box box = LayoutBox(*fixture.Element("arrow"), 1.0f);
    const auto vertices = Vertices(arrow[0]);
    const Vector2 tip = *std::max_element(vertices.begin(), vertices.end(),
                                          [](Vector2 a, Vector2 b) { return a.y < b.y; });
    EXPECT_NEAR(tip.x, box.X + box.W * 0.5f, 0.5f) << "the tip is at the middle of the arrow's box";
    EXPECT_NEAR(tip.y, box.Y + box.H, 0.5f) << "the tip points down, at the far edge of the arrow's box";
    EXPECT_GT(tip.y, panel.Y + panel.H) << "the tip is below the panel";
}

// The bubble sits above the panel. The arrow sits on the panel's top edge at the hovered tile's
// center x, the same for the tile in the top row and for the tile in the bottom row.
TEST(PointerArrowTests, AnAnchoredTooltipsArrowLinesUpWithTheTileInEveryRow)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kGridXml, kGridCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built);
    AnchorTilesToThePanel(fixture, kGridTiles, UIElement::TooltipPlacement::Above);
    const Box panel = LayoutBox(*fixture.Element("panel"), 1.0f);
    const Vector2 down(0.0f, 1.0f);

    HoveredArrow top;
    ASSERT_TRUE(HoverTile(fixture, "row0-col1", down, top)) << "no arrow for the top row's tile";
    HoveredArrow bottom;
    ASSERT_TRUE(HoverTile(fixture, "row2-col1", down, bottom)) << "no arrow for the bottom row's tile";

    for (const HoveredArrow* hovered : {&top, &bottom})
    {
        EXPECT_LE(hovered->Bubble.Y + hovered->Bubble.H, panel.Y) << "the bubble covers the panel";
        EXPECT_NEAR(hovered->Tip.x, CenterX(hovered->Tile), 0.5f) << "the arrow is not at the tile's center x";
        EXPECT_GT(hovered->Tip.y, hovered->Bubble.Y + hovered->Bubble.H) << "the arrow does not point down";
        EXPECT_LE(hovered->Tip.y, panel.Y) << "the arrow reaches into the panel";
    }
    EXPECT_FLOAT_EQ(bottom.Tip.x, top.Tip.x) << "the rows of one column have different arrows";
}

// The bubble is as wide as the panel, so it stays in one place for every tile. The arrow moves
// to the center x of each tile the pointer goes to.
TEST(PointerArrowTests, AnAnchoredTooltipsArrowMovesBetweenColumnsWhileTheBubbleStays)
{
    std::string css = kGridCss;
    css += ".tooltip { width: 200px; }";
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kGridXml, css);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built);
    AnchorTilesToThePanel(fixture, kGridTiles, UIElement::TooltipPlacement::Above);
    const Box panel = LayoutBox(*fixture.Element("panel"), 1.0f);
    const Vector2 down(0.0f, 1.0f);

    HoveredArrow first;
    ASSERT_TRUE(HoverTile(fixture, "row0-col1", down, first));
    HoveredArrow second;
    ASSERT_TRUE(HoverTile(fixture, "row0-col2", down, second));

    ASSERT_FLOAT_EQ(first.Bubble.W, panel.W) << "the bubble must be as wide as the panel";
    EXPECT_FLOAT_EQ(second.Bubble.X, first.Bubble.X) << "the bubble moved";
    EXPECT_FLOAT_EQ(second.Bubble.Y, first.Bubble.Y) << "the bubble moved";
    EXPECT_NEAR(first.Tip.x, CenterX(first.Tile), 0.5f);
    EXPECT_NEAR(second.Tip.x, CenterX(second.Tile), 0.5f) << "the arrow did not move to the new column";
}

// The tiles at the two ends of the panel have their centers past the straight part of the
// bubble's edge. The arrow stops where the bubble's rounded corner starts, the same distance
// from each end, and still points down.
TEST(PointerArrowTests, AnAnchoredTooltipsArrowStopsAtTheBubblesStraightEdgeAtBothEnds)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kGridXml, kGridCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built);
    AnchorTilesToThePanel(fixture, kGridTiles, UIElement::TooltipPlacement::Above);
    const Box panel = LayoutBox(*fixture.Element("panel"), 1.0f);
    const Vector2 down(0.0f, 1.0f);

    HoveredArrow leftEnd;
    ASSERT_TRUE(HoverTile(fixture, "row0-col0", down, leftEnd));
    HoveredArrow rightEnd;
    ASSERT_TRUE(HoverTile(fixture, "row0-col3", down, rightEnd));

    // The bubble is as wide as its text, a fraction of a pixel short of a whole pixel.
    ASSERT_NEAR(leftEnd.Bubble.X, panel.X, 1.0f) << "the bubble must be held at the panel's left end";
    ASSERT_NEAR(rightEnd.Bubble.X + rightEnd.Bubble.W, panel.X + panel.W, 1.0f)
        << "the bubble must be held at the panel's right end";
    EXPECT_GT(leftEnd.Tip.x, CenterX(leftEnd.Tile)) << "the arrow is on the rounded corner";
    EXPECT_LT(rightEnd.Tip.x, CenterX(rightEnd.Tile)) << "the arrow is on the rounded corner";
    EXPECT_NEAR(leftEnd.Tip.x - leftEnd.Bubble.X, rightEnd.Bubble.X + rightEnd.Bubble.W - rightEnd.Tip.x, 0.5f)
        << "the arrow stops at a different distance from each end";
    for (const HoveredArrow* hovered : {&leftEnd, &rightEnd})
    {
        EXPECT_GT(hovered->Tip.y, hovered->Bubble.Y + hovered->Bubble.H) << "the arrow does not point down";
        for (const UI::UIPrimitive& triangle : hovered->Triangles)
        {
            for (const Vector2 vertex : Vertices(triangle))
            {
                EXPECT_GE(vertex.x, hovered->Bubble.X + kBubbleRadius - 0.01f) << "a vertex is on the rounded corner";
                EXPECT_LE(vertex.x, hovered->Bubble.X + hovered->Bubble.W - kBubbleRadius + 0.01f)
                    << "a vertex is on the rounded corner";
            }
        }
    }
}

// The bubble sits right of the panel, then left of it. The arrow sits on the panel's facing edge
// at the hovered tile's center y: the same for both tiles of a row, and it moves to the next row.
TEST(PointerArrowTests, AnAnchoredTooltipsArrowOnTheRightOrLeftLinesUpWithTheTileInEveryColumn)
{
    using Placement = UIElement::TooltipPlacement;
    for (const Placement placement : {Placement::Right, Placement::Left})
    {
        SCOPED_TRACE(placement == Placement::Right ? "Right" : "Left");
        UITesting::IsolatedUIFixture fixture;
        const bool built = fixture.Build(1.0f, kColumnXml, kColumnCss);
        if (!fixture.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built);
        AnchorTilesToThePanel(fixture, kColumnTiles, placement);
        const Box panel = LayoutBox(*fixture.Element("panel"), 1.0f);
        const Vector2 pointing(placement == Placement::Right ? -1.0f : 1.0f, 0.0f);

        HoveredArrow leftColumn;
        ASSERT_TRUE(HoverTile(fixture, "left-row1", pointing, leftColumn));
        HoveredArrow rightColumn;
        ASSERT_TRUE(HoverTile(fixture, "right-row1", pointing, rightColumn));
        HoveredArrow nextRow;
        ASSERT_TRUE(HoverTile(fixture, "right-row2", pointing, nextRow));

        for (const HoveredArrow* hovered : {&leftColumn, &rightColumn, &nextRow})
        {
            EXPECT_NEAR(hovered->Tip.y, CenterY(hovered->Tile), 0.5f) << "the arrow is not at the tile's center y";
            if (placement == Placement::Right)
            {
                EXPECT_GE(hovered->Bubble.X, panel.X + panel.W) << "the bubble covers the panel";
                EXPECT_GE(hovered->Tip.x, panel.X + panel.W) << "the arrow reaches into the panel";
                EXPECT_LT(hovered->Tip.x, hovered->Bubble.X) << "the arrow does not point left";
            }
            else
            {
                EXPECT_LE(hovered->Bubble.X + hovered->Bubble.W, panel.X) << "the bubble covers the panel";
                EXPECT_LE(hovered->Tip.x, panel.X) << "the arrow reaches into the panel";
                EXPECT_GT(hovered->Tip.x, hovered->Bubble.X + hovered->Bubble.W) << "the arrow does not point right";
            }
        }
        EXPECT_FLOAT_EQ(rightColumn.Tip.y, leftColumn.Tip.y) << "the columns of one row have different arrows";
        EXPECT_GT(nextRow.Tip.y, rightColumn.Tip.y) << "the arrow did not move to the next row";
    }
}
