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

// Switching to or from `none` re-points the element's subtree at the slot it
// paints in now, in the drain: the rest of the UI is not regenerated, and the
// result is the same as a full regeneration would give.
TEST(CssTransformRender, SwitchingToAndFromNoneIsAPerElementUpdate)
{
    BUILD_OR_SKIP(fx);
    UIElement* slide = fx.Element("slide");
    UIElement* disc = fx.Element("disc");
    UIManager& ui = fx.Manager();
    ui.SetUpdateProfilingEnabled(true);

    static constexpr size_t kFrames = 4;
    const auto settleProfiled = [&fx]()
    {
        for (size_t i = 0; i < kFrames; ++i)
            fx.StepFrame();
    };
    const auto expectNoFullRegen = [&ui](const char* what)
    {
        const auto& history = ui.GetUpdateProfilingHistory();
        ASSERT_GE(history.size(), kFrames);
        uint32_t drained = 0;
        for (size_t i = history.size() - kFrames; i < history.size(); ++i)
        {
            EXPECT_EQ(history[i].GenAllPrimitivesRegenCause, 0u) << what << ": a full regeneration was asked for";
            drained += history[i].DrainItems;
        }
        EXPECT_GT(drained, 0u) << what << ": the drain did the update";
    };

    slide->Overrides().Set(Style::Transform, TransformValue{});
    settleProfiled();
    expectNoFullRegen("translate to none");
    EXPECT_EQ(slide->m_TransformSlotIdx, UI::kNoTransform) << "slot released";
    std::vector<uint8_t> px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 75, 500, kGreen, "back at its layout position");
    ExpectAt(px, 175, 500, kBlack, "and not translated");

    slide->Overrides().Set(Style::Transform,
                           TransformValue{}.Translate(StyleLength::Percent(-40.0f), StyleLength::Px(0.0f)));
    // A container: its own clip and its transformed child move to the new space.
    disc->Overrides().Set(Style::Transform, TransformValue{}.Translate(StyleLength::Px(-100.0f), StyleLength::Px(0.0f)));
    settleProfiled();
    expectNoFullRegen("none to translate");
    EXPECT_NE(slide->m_TransformSlotIdx, UI::kNoTransform);
    ASSERT_NE(disc->m_TransformSlotIdx, UI::kNoTransform);
    const UI::UITransformSlot* map = ui.PeekTransformSlotForTesting(fx.Element("map")->m_TransformSlotIdx);
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(map->ParentIndex, disc->m_TransformSlotIdx) << "the child chains to its new transformed parent";
    px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 65, 500, kGreen, "translated left by 40%");
    ExpectAt(px, 135, 500, kBlack, "vacated right side");
    ExpectAt(px, 550, 150, kBlue, "the disc's centre moved left with it");
    ExpectAt(px, 740, 150, kBlack, "and its clip with it");
}

// Untransformed primitives and clips name the identity slot, so the vertex
// stage skips both chain walks for a UI that uses no transform.
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


// The deepest clip-chain level the shaders evaluate (kMaxClipChainDepth,
// UI/ui_sdf_common.glsl) still cuts in its own space. Four nested clips, each
// owned by a transformed element so none is folded into another on the CPU;
// the outermost, untransformed, keeps x 100..200, and the leaf inside them is
// translated 50px right, so its own space and the outer clip's differ.
TEST(CssTransformRender, TheDeepestClipLevelCutsInItsOwnSpace)
{
    constexpr char kDeepXml[] = R"(<uielement id="root">
  <uielement id="c1"><uielement id="c2"><uielement id="c3"><uielement id="c4">
    <uielement id="leaf"/>
  </uielement></uielement></uielement></uielement>
</uielement>)";
    constexpr char kDeepCss[] = R"(
#root { width: 800px; height: 600px; background-color: #000000; }
#c1   { position: absolute; left: 100px; top: 100px; width: 100px; height: 100px; overflow: hidden; }
#c2, #c3, #c4 { position: absolute; left: -100px; top: -100px; width: 900px; height: 900px;
                overflow: hidden; transform: translate(0px); }
#leaf { position: absolute; left: 250px; top: 300px; width: 200px; height: 100px;
        background-color: #00ff00; transform: translate(50px); }
)";
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kDeepXml, kDeepCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    // c2-c4 each sit 100px up and left of their parent, so c4 is at
    // (-200, -200) and the leaf's layout box spans x 50 .. 250, drawn at 100 .. 300.
    const auto leaf = fx.BorderBox("leaf");
    ASSERT_FLOAT_EQ(leaf.X, 50.0f);

    const std::vector<uint8_t> px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 120, 150, kGreen, "inside the outer clip, drawn");
    ExpectAt(px, 230, 150, kBlack, "drawn, but past the outer clip");
}

namespace
{

// `levels` nested containers, each translated 5 px right about its own top-left
// corner, around a 50x50 red leaf laid out at (100, 100).
void BuildNestedTranslates(IsolatedUIFixture& fx, int levels, bool& built)
{
    std::string xml = R"(<uielement id="root">)";
    for (int i = 0; i < levels; ++i)
        xml += "<uielement class=\"level\">";
    xml += R"(<uielement id="leaf"/>)";
    for (int i = 0; i < levels; ++i)
        xml += "</uielement>";
    xml += "</uielement>";
    constexpr char kNestedCss[] = R"(
#root  { width: 800px; height: 600px; background-color: #000000; }
.level { position: absolute; left: 0px; top: 0px; width: 400px; height: 300px;
         transform: translate(5px, 0px); transform-origin: 0 0; }
#leaf  { position: absolute; left: 100px; top: 100px; width: 50px; height: 50px; background-color: #ff0000; }
)";
    built = fx.Build(1.0f, xml, kNestedCss);
}

} // namespace

// At the deepest chain the UI composes, every level applies, in the paint, the
// hit test and the window rect alike: the leaf is drawn at x 260..310.
TEST(CssTransformRender, ATransformChainAtTheDepthLimitComposesWhole)
{
    IsolatedUIFixture fx;
    bool built = false;
    BuildNestedTranslates(fx, UI::kMaxTransformChainDepth, built);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    EXPECT_NEAR(fx.Element("leaf")->GetWindowRect().X, 100.0f + 5.0f * UI::kMaxTransformChainDepth, 0.01f);
    EXPECT_EQ(HoverAt(fx, 305, 125), fx.Element("leaf"));
    EXPECT_NE(HoverAt(fx, 255, 125), fx.Element("leaf"));
    const std::vector<uint8_t> px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 305, 125, kRed, "drawn where it is hit");
    ExpectAt(px, 255, 125, kBlack, "not drawn short of the last level");
}

// One level past the limit is ignored by the paint, the hit test and the window
// rect alike, so the leaf is drawn and hit at x 260..310, not at 265..315.
TEST(CssTransformRender, ATransformPastTheDepthLimitIsIgnoredAlike)
{
    IsolatedUIFixture fx;
    bool built = false;
    BuildNestedTranslates(fx, UI::kMaxTransformChainDepth + 1, built);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    EXPECT_NEAR(fx.Element("leaf")->GetWindowRect().X, 100.0f + 5.0f * UI::kMaxTransformChainDepth, 0.01f);
    EXPECT_EQ(HoverAt(fx, 305, 125), fx.Element("leaf"));
    EXPECT_NE(HoverAt(fx, 312, 125), fx.Element("leaf")) << "the ignored level does not move the hit";
    const std::vector<uint8_t> px = Render(fx);
    ASSERT_FALSE(px.empty());
    ExpectAt(px, 305, 125, kRed, "drawn where it is hit");
    ExpectAt(px, 314, 125, kBlack, "the ignored level does not move the paint");
}

// An edge that a transform lands on a pixel boundary is crisp, whatever the
// scale: the antialiasing ramp spans one screen pixel across the edge, also
// across the stretched axis of a non-uniform scale. Each box is blue on black
// and scaled about its top-left corner, so its drawn edges sit on whole pixels.
TEST(CssTransformRender, AnEdgeOnAPixelBoundaryIsCrispUnderAnyScale)
{
    constexpr char kEdgeXml[] = R"(<uielement id="root">
  <uielement id="stretched"/><uielement id="uniform"/><uielement id="triple"/>
</uielement>)";
    constexpr char kEdgeCss[] = R"(
#root { width: 800px; height: 600px; background-color: #000000; }
#stretched, #uniform, #triple { position: absolute; background-color: #0000ff; transform-origin: 0 0; }
#stretched { left: 100px; top: 100px; width: 100px; height: 60px; transform: scale(1.5, 0.75); }
#uniform   { left: 100px; top: 300px; width: 100px; height: 60px; transform: scale(1.5); }
#triple    { left: 400px; top: 300px; width: 40px;  height: 20px; transform: scale(3); }
)";
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kEdgeXml, kEdgeCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    const std::vector<uint8_t> px = Render(fx);
    ASSERT_FALSE(px.empty());
    const auto blueAt = [&px](uint32_t x, uint32_t y) { return static_cast<int>(px[(y * 800u + x) * 4u + 2u]); };
    struct Edge
    {
        const char* What;
        uint32_t Outside;
        uint32_t Inside;
        uint32_t Row;
    };
    // stretched: drawn x 100..250; uniform: x 100..250; triple: x 400..520.
    const Edge edges[] = {{"scale(1.5, 0.75) left", 99, 100, 120},  {"scale(1.5, 0.75) right", 250, 249, 120},
                          {"scale(1.5) left", 99, 100, 340},        {"scale(1.5) right", 250, 249, 340},
                          {"scale(3) left", 399, 400, 330},         {"scale(3) right", 520, 519, 330}};
    for (const Edge& e : edges)
    {
        EXPECT_EQ(blueAt(e.Outside, e.Row), 0) << e.What << ": the pixel outside the edge";
        EXPECT_EQ(blueAt(e.Inside, e.Row), 255) << e.What << ": the pixel inside the edge";
    }
}
