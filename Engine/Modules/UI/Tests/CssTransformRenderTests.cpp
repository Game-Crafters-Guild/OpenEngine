// CSS `transform` at paint and hit-test time, read back as pixels.
//
// Every probe is a flat opaque colour on a black root, sampled as 5x5-uniform
// patches well away from any edge, and every pin is chosen so that it reads
// differently with the transform ignored: a point inside the transformed
// footprint but outside the layout rect, or the other way round.
//
//   #bar     200x40 red at (300,280), rotate(90deg): a 40x200 bar about the
//            same centre (400,300).
//   #slide   100x100 green at (50,450), translate(50%): paints x 100..200.
//   #disc    200x200 circle (overflow hidden, radius 50%) at (550,50) holding
//            #map, 300x300 blue rotated 45deg: an untransformed ancestor's
//            round clip cutting a rotated child (the minimap case).
//   #rotclip 200x200 at (50,200), rotate(45deg) with overflow hidden, holding
//            #fill, 400x400 yellow: the clip rotates with its owner, so the
//            fill shows as a diamond about (150,300).

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"

#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UI/UITargetSpace.h"

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::RenderUiToBytes;
using GameEngine::UITesting::Rgb;
using GameEngine::UITesting::UniformPatch;
using namespace GameEngine;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="bar"/>
  <uielement id="slide"/>
  <uielement id="disc"><uielement id="map"/></uielement>
  <uielement id="rotclip"><uielement id="fill"/></uielement>
</uielement>)";

constexpr char kCss[] = R"(
#root    { width: 800px; height: 600px; background-color: #000000; }
#bar     { position: absolute; left: 300px; top: 280px; width: 200px; height: 40px;
           background-color: #ff0000; transform: rotate(90deg); }
#slide   { position: absolute; left: 50px; top: 450px; width: 100px; height: 100px;
           background-color: #00ff00; transform: translate(50%); }
#disc    { position: absolute; left: 550px; top: 50px; width: 200px; height: 200px;
           overflow: hidden; border-radius: 50%; }
#map     { position: absolute; left: -50px; top: -50px; width: 300px; height: 300px;
           background-color: #0000ff; transform: rotate(45deg); }
#rotclip { position: absolute; left: 50px; top: 200px; width: 200px; height: 200px;
           overflow: hidden; transform: rotate(45deg); }
#fill    { position: absolute; left: -100px; top: -100px; width: 400px; height: 400px;
           background-color: #ffff00; }
)";

struct Colour
{
    int R, G, B;
};
constexpr Colour kBlack{0, 0, 0};
constexpr Colour kRed{255, 0, 0};
constexpr Colour kGreen{0, 255, 0};
constexpr Colour kBlue{0, 0, 255};
constexpr Colour kYellow{255, 255, 0};

bool BuildOrSkip(IsolatedUIFixture& fx, std::string& why)
{
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
    {
        why = "No Vulkan device available";
        return false;
    }
    EXPECT_TRUE(built) << fx.Diagnostic();
    return built;
}

void ExpectAt(const std::vector<uint8_t>& px, uint32_t x, uint32_t y, Colour c, const char* what)
{
    Rgb p{};
    std::string why;
    ASSERT_TRUE(UniformPatch(px, x, y, p, why)) << what << ": " << why;
    EXPECT_EQ(p.R, c.R) << what << " at (" << x << "," << y << ")";
    EXPECT_EQ(p.G, c.G) << what << " at (" << x << "," << y << ")";
    EXPECT_EQ(p.B, c.B) << what << " at (" << x << "," << y << ")";
}

// The element under the pointer, through the real pointer path.
UIElement* HoverAt(IsolatedUIFixture& fx, float x, float y)
{
    fx.Manager().OnMouseMove(x, y);
    fx.StepFrame();
    return fx.Manager().GetHoveredElement();
}

std::vector<uint8_t> Render(IsolatedUIFixture& fx)
{
    return RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
}

#define BUILD_OR_SKIP(fx)                 \
    IsolatedUIFixture fx;                 \
    {                                     \
        std::string why_;                 \
        if (!BuildOrSkip(fx, why_))       \
        {                                 \
            if (!why_.empty())            \
                GTEST_SKIP() << why_;     \
            return;                       \
        }                                 \
    }

} // namespace

TEST(CssTransformRender, RotationMovesThePaintNotTheLayout)
{
    BUILD_OR_SKIP(fx);
    const auto bar = fx.BorderBox("bar");
    EXPECT_FLOAT_EQ(bar.X, 300.0f) << "layout ignores the transform";
    EXPECT_FLOAT_EQ(bar.W, 200.0f);

    const std::vector<uint8_t> px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 400, 300, kRed, "centre stays put");
    ExpectAt(px, 400, 220, kRed, "rotated footprint, outside the layout rect");
    ExpectAt(px, 330, 300, kBlack, "layout rect, outside the rotated footprint");
}

TEST(CssTransformRender, TranslatePercentageIsOfTheBorderBox)
{
    BUILD_OR_SKIP(fx);
    const std::vector<uint8_t> px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 175, 500, kGreen, "moved half its own width right");
    ExpectAt(px, 75, 500, kBlack, "vacated left half");
}

// The minimap: an untransformed round clip cuts a rotated child on screen.
TEST(CssTransformRender, AnUntransformedAncestorClipCutsARotatedChild)
{
    BUILD_OR_SKIP(fx);
    const std::vector<uint8_t> px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 650, 150, kBlue, "centre of the disc");
    ExpectAt(px, 650, 238, kBlue, "inside the circle near its rim");
    ExpectAt(px, 735, 235, kBlack, "inside the box, outside the circle");
}

// The clip a rotated element establishes rotates with it.
TEST(CssTransformRender, ARotatedElementsOwnClipRotatesWithIt)
{
    BUILD_OR_SKIP(fx);
    const std::vector<uint8_t> px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 150, 300, kYellow, "centre");
    ExpectAt(px, 150, 170, kYellow, "diamond's top point, above the layout box");
    ExpectAt(px, 235, 385, kBlack, "layout box corner, outside the diamond");
}

TEST(CssTransformRender, HitTestingFollowsThePaint)
{
    BUILD_OR_SKIP(fx);
    EXPECT_EQ(HoverAt(fx, 400, 220), fx.Element("bar")) << "rotated footprint hits";
    EXPECT_NE(HoverAt(fx, 330, 300), fx.Element("bar")) << "vacated layout rect misses";
    EXPECT_EQ(HoverAt(fx, 150, 170), fx.Element("fill")) << "inside the rotated clip";
    EXPECT_NE(HoverAt(fx, 235, 385), fx.Element("fill")) << "outside the rotated clip";
    EXPECT_EQ(HoverAt(fx, 650, 150), fx.Element("map")) << "inside the disc";
    EXPECT_NE(HoverAt(fx, 760, 150), fx.Element("map")) << "outside the disc's box";
}

// A changed value rewrites one transform slot in the drain: the slot stays
// the element's, and its children follow without being re-emitted.
TEST(CssTransformRender, AnOverrideRewritesTheSlotAndChildrenFollow)
{
    BUILD_OR_SKIP(fx);
    UIElement* rotclip = fx.Element("rotclip");
    UIElement* bar = fx.Element("bar");
    const uint32_t slot = rotclip->m_TransformSlotIdx;
    ASSERT_NE(slot, UI::kNoTransform);

    rotclip->Overrides().Set(Style::Transform, TransformValue{}.Rotate(0.0f));
    bar->Overrides().Set(Style::Transform, TransformValue{}.Rotate(0.0f));
    fx.Settle();
    EXPECT_EQ(rotclip->m_TransformSlotIdx, slot) << "same slot, rewritten in place";

    const std::vector<uint8_t> px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 150, 170, kBlack, "unrotated clip no longer reaches above the box");
    ExpectAt(px, 235, 385, kYellow, "the box corner is inside again");
    ExpectAt(px, 330, 300, kRed, "bar back along its layout rect");
    ExpectAt(px, 400, 220, kBlack, "and out of its rotated footprint");
}

// Switching to or from `none` changes which slot the subtree names, which the
// drain cannot restamp; it escalates, and the result is the same either way.
TEST(CssTransformRender, SwitchingToAndFromNoneRepaints)
{
    BUILD_OR_SKIP(fx);
    UIElement* slide = fx.Element("slide");
    slide->Overrides().Set(Style::Transform, TransformValue{});
    fx.Settle();
    EXPECT_EQ(slide->m_TransformSlotIdx, UI::kNoTransform) << "slot released";
    std::vector<uint8_t> px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 75, 500, kGreen, "back at its layout position");
    ExpectAt(px, 175, 500, kBlack, "and not translated");

    slide->Overrides().Set(Style::Transform, TransformValue{}.Translate(-40.0f, 0.0f, true, false));
    fx.Settle();
    EXPECT_NE(slide->m_TransformSlotIdx, UI::kNoTransform);
    px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 65, 500, kGreen, "translated left by 40%");
    ExpectAt(px, 135, 500, kBlack, "vacated right side");
}

// Untransformed primitives and clips name the identity slot, so a UI that
// never uses a transform renders exactly as before.
TEST(CssTransformRender, UntransformedPrimitivesNameTheIdentitySlot)
{
    BUILD_OR_SKIP(fx);
    for (const UI::UIPrimitive& p : fx.Primitives("root"))
        EXPECT_EQ(p.TransformIndex, UI::kNoTransform);
    for (const UI::UIPrimitive& p : fx.Primitives("fill"))
        EXPECT_EQ(p.TransformIndex, fx.Element("rotclip")->m_TransformSlotIdx)
            << "a child paints in its transformed ancestor's space";
    const UI::UITransformSlot* t = fx.Manager().PeekTransformSlotForTesting(fx.Element("map")->m_TransformSlotIdx);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->ParentIndex, UI::kNoTransform) << "the disc has no transform to chain to";
}

