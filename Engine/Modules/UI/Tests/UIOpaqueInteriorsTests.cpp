// UIOpaqueInteriors: which draw positions the UI pre-pass paints, as which
// rects, when the list rebuilds, and that the pre-pass changes no pixel.
#include "UIOpaqueInteriors.h"

#include "IsolatedUIFixture.h"
#include "UI/UIPrimitive.h"
#include "UIManagerSdfRingAccess.h"
#include "UIPixelReadback.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace GameEngine;
using GameEngine::UI::UIClipRect;
using GameEngine::UI::UIOpaqueInterior;
using GameEngine::UI::UIOpaqueInteriors;
using GameEngine::UI::UIPrimitive;

namespace
{

constexpr uint32_t kOpaqueGray = 0xFF272727u;

UIPrimitive OpaqueRect(float x, float y, float w, float h)
{
    UIPrimitive prim{};
    prim.X = x;
    prim.Y = y;
    prim.W = w;
    prim.H = h;
    prim.FillColor = kOpaqueGray;
    prim.ModeAndFlags = static_cast<uint32_t>(UI::PrimitiveMode::Rect) |
                        (static_cast<uint32_t>(UI::kNoClip) << UI::kPrimClipIndexShift);
    return prim;
}

struct Scene
{
    std::vector<UIPrimitive> Primitives;
    std::vector<UIClipRect> Clips;
    std::vector<uint32_t> DrawOrder;

    UIOpaqueInteriors::Sources Sources(uint64_t version = 1) const
    {
        return {Primitives, Clips, DrawOrder, version, version, version};
    }
};

// Each primitive drawn once, in slot order.
Scene SceneOf(std::vector<UIPrimitive> primitives, std::vector<UIClipRect> clips = {})
{
    Scene scene{std::move(primitives), std::move(clips), {}};
    for (uint32_t slot = 0; slot < scene.Primitives.size(); ++slot)
        scene.DrawOrder.push_back(slot);
    return scene;
}

void ExpectInterior(const UIOpaqueInterior& entry, uint32_t position, float x, float y, float w, float h)
{
    EXPECT_EQ(entry.DrawPosition, position);
    EXPECT_FLOAT_EQ(entry.Bounds.X, x) << "position " << position;
    EXPECT_FLOAT_EQ(entry.Bounds.Y, y) << "position " << position;
    EXPECT_FLOAT_EQ(entry.Bounds.Width, w) << "position " << position;
    EXPECT_FLOAT_EQ(entry.Bounds.Height, h) << "position " << position;
    EXPECT_EQ(entry.FillColor, kOpaqueGray);
}

} // namespace

TEST(UIOpaqueInteriors, NestedPanelsListTheirInsetInteriorsFrontToBack)
{
    // A window, a rounded bordered panel on it, and a child cut by a clip.
    UIPrimitive window = OpaqueRect(0.0f, 0.0f, 200.0f, 100.0f);
    UIPrimitive panel = OpaqueRect(10.0f, 10.0f, 100.0f, 80.0f);
    for (int corner = 0; corner < 4; ++corner)
    {
        panel.Radii[corner] = 4.0f;
        panel.RadiiY[corner] = 4.0f;
        panel.BorderWidths[corner] = 2.0f;
    }
    UIPrimitive child = OpaqueRect(20.0f, 20.0f, 50.0f, 40.0f);
    SetClip(child, 0);
    UIClipRect clip{};
    clip.Rect[2] = 60.0f;
    clip.Rect[3] = 50.0f;

    const Scene scene = SceneOf({window, panel, child}, {clip});
    UIOpaqueInteriors interiors;
    ASSERT_TRUE(interiors.Refresh(scene.Sources()));
    ASSERT_EQ(interiors.Interiors().size(), 3u);
    // Inset 1 px of anti-aliasing; the panel also by its border and radius;
    // the child also cut by its clip, inset 1 px.
    ExpectInterior(interiors.Interiors()[0], 2, 21.0f, 21.0f, 38.0f, 28.0f);
    ExpectInterior(interiors.Interiors()[1], 1, 17.0f, 17.0f, 86.0f, 66.0f);
    ExpectInterior(interiors.Interiors()[2], 0, 1.0f, 1.0f, 198.0f, 98.0f);
}

TEST(UIOpaqueInteriors, PrimitivesTheShaderDoesNotPaintOpaqueAreNotListed)
{
    std::vector<UIPrimitive> rejected;
    UIPrimitive translucent = OpaqueRect(0.0f, 0.0f, 50.0f, 50.0f);
    translucent.FillColor = 0xFE272727u;
    rejected.push_back(translucent);
    UIPrimitive pill = OpaqueRect(0.0f, 0.0f, 20.0f, 10.0f);
    for (int corner = 0; corner < 4; ++corner)
    {
        pill.Radii[corner] = 100.0f;
        pill.RadiiY[corner] = 100.0f;
    }
    rejected.push_back(pill);
    UIPrimitive textured = OpaqueRect(0.0f, 0.0f, 50.0f, 50.0f);
    textured.ModeAndFlags = (textured.ModeAndFlags & ~UI::kPrimModeMask) |
                            static_cast<uint32_t>(UI::PrimitiveMode::Textured);
    rejected.push_back(textured);
    UIPrimitive gradient = OpaqueRect(0.0f, 0.0f, 50.0f, 50.0f);
    gradient.ModeAndFlags |= static_cast<uint32_t>(UI::GradientMode::Vertical) << UI::kPrimGradientShift;
    rejected.push_back(gradient);
    UIPrimitive faded = OpaqueRect(0.0f, 0.0f, 50.0f, 50.0f);
    faded.Opacity = 0.5f;
    rejected.push_back(faded);
    UIPrimitive transformed = OpaqueRect(0.0f, 0.0f, 50.0f, 50.0f);
    transformed.TransformIndex = 1;
    rejected.push_back(transformed);
    UIPrimitive caret = OpaqueRect(0.0f, 0.0f, 50.0f, 50.0f);
    caret.ModeAndFlags |= UI::kPrimCaretBit;
    rejected.push_back(caret);
    UIPrimitive insetShadow = OpaqueRect(0.0f, 0.0f, 50.0f, 50.0f);
    insetShadow.ModeAndFlags |= UI::kPrimInsetShadowBit;
    rejected.push_back(insetShadow);
    UIPrimitive underRotatedClip = OpaqueRect(0.0f, 0.0f, 50.0f, 50.0f);
    SetClip(underRotatedClip, 0);
    rejected.push_back(underRotatedClip);
    UIClipRect rotatedClip{};
    rotatedClip.Rect[2] = 100.0f;
    rotatedClip.Rect[3] = 100.0f;
    rotatedClip.TransformIndex = 1;

    for (size_t index = 0; index < rejected.size(); ++index)
    {
        const Scene scene = SceneOf({rejected[index]}, {rotatedClip});
        UIOpaqueInteriors interiors;
        interiors.Refresh(scene.Sources());
        EXPECT_TRUE(interiors.Interiors().empty()) << "case " << index;
    }
}

TEST(UIOpaqueInteriors, AShadowedRectListsItsElementRectNotItsShadowReach)
{
    // The rect carries 10 px of shadow reach as UvRect padding.
    UIPrimitive shadowed = OpaqueRect(0.0f, 0.0f, 120.0f, 70.0f);
    shadowed.ShadowSoftness = 4.0f;
    shadowed.ShadowColor = 0x99000000u;
    for (float& pad : shadowed.UvRect)
        pad = 10.0f;

    const Scene scene = SceneOf({shadowed});
    UIOpaqueInteriors interiors;
    interiors.Refresh(scene.Sources());
    ASSERT_EQ(interiors.Interiors().size(), 1u);
    ExpectInterior(interiors.Interiors()[0], 0, 11.0f, 11.0f, 98.0f, 48.0f);
}

TEST(UIOpaqueInteriors, RebuildsOnlyWhenASourceVersionMoves)
{
    Scene scene = SceneOf({OpaqueRect(0.0f, 0.0f, 50.0f, 50.0f)});
    UIOpaqueInteriors interiors;
    ASSERT_TRUE(interiors.Refresh(scene.Sources(1)));
    const uint64_t built = interiors.Version();

    scene.Primitives[0].FillColor = 0x80272727u;
    EXPECT_FALSE(interiors.Refresh(scene.Sources(1))) << "no version moved: the list stays";
    EXPECT_EQ(interiors.Version(), built);
    EXPECT_EQ(interiors.Interiors().size(), 1u);

    UIOpaqueInteriors::Sources moved = scene.Sources(1);
    moved.PrimitiveVersion = 2;
    EXPECT_TRUE(interiors.Refresh(moved));
    EXPECT_NE(interiors.Version(), built);
    EXPECT_TRUE(interiors.Interiors().empty());
}

TEST(UIOpaqueInteriors, MoreDrawPositionsThanDepthLevelsListNothing)
{
    Scene scene = SceneOf({OpaqueRect(0.0f, 0.0f, 50.0f, 50.0f)});
    scene.DrawOrder.assign(UIOpaqueInteriors::kMaxDrawPositions + 1, 0u);
    UIOpaqueInteriors interiors;
    interiors.Refresh(scene.Sources());
    EXPECT_TRUE(interiors.Interiors().empty());
}

namespace
{

constexpr const char* kPrePassXml = R"(<uielement id='root'>
  <uielement id='dock'>
    <uielement id='panel'><uielement id='inner' /><label id='caption'>Plate label Hxg</label><uielement id='veil' /></uielement>
    <uielement id='clipper'><uielement id='clipped' /></uielement>
    <uielement id='faded' />
    <uielement id='shadowed' />
  </uielement>
</uielement>)";

constexpr const char* kPrePassCss = R"(
#root { display: flex; width: 800px; height: 600px; background-color: #1c1d1d; }
#dock { display: flex; flex-direction: row; flex-wrap: wrap; width: 780px; height: 580px; margin: 10px; background-color: #1c1d1d; }
#panel { width: 300px; height: 200px; margin: 7.5px; padding: 10px; background-color: #272727; border: 2px solid #3a3a3a; border-radius: 6px; }
#inner { width: 200px; height: 120px; background-color: #2f4f3f; }
#caption { color: #d0d0d0; font-size: 13px; }
#veil { width: 100px; height: 50px; background-color: rgba(255, 255, 255, 0.3); }
#clipper { width: 200px; height: 150px; margin: 10px; overflow: hidden; border-radius: 12px; background-color: #303a44; }
#clipped { width: 300px; height: 300px; background-color: #446688; }
#faded { width: 120px; height: 120px; margin: 10px; opacity: 0.5; background-color: #aa3311; }
#shadowed { width: 140px; height: 90px; margin: 20px; background-color: #335577; box-shadow: 4px 6px 8px rgba(0, 0, 0, 0.6); }
)";

size_t CountDifferingPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t& first)
{
    constexpr size_t kPixelBytes = 8; // RGBA16F
    size_t differing = 0;
    first = a.size();
    for (size_t offset = 0; offset + kPixelBytes <= a.size(); offset += kPixelBytes)
    {
        if (std::memcmp(a.data() + offset, b.data() + offset, kPixelBytes) == 0)
            continue;
        if (differing++ == 0)
            first = offset / kPixelBytes;
    }
    return differing;
}

// Renders the scene with and without the pre-pass and expects equal bytes.
void ExpectPrePassFrameMatchesPaintersOrder(float contentScale, UI::UITargetSpace space)
{
    SCOPED_TRACE(testing::Message() << "content scale " << contentScale << ", target kind "
                                    << static_cast<int>(space.GetKind()));
    UITesting::IsolatedUIFixture fx;
    const bool built = fx.Build(contentScale, kPrePassXml, kPrePassCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const Rendering::ViewReadbackResult withPrePass = UITesting::RenderUiToReadback(fx.Manager(), space);
    ASSERT_FALSE(withPrePass.pixels.empty());
    ASSERT_GE(UIManagerSdfRingAccess::OpaqueInteriorCount(fx.Manager()), 5u)
        << "the window, dock, panel, inner panel and clipped child are opaque";

    UIManagerSdfRingAccess::DisableOpaquePrePass(fx.Manager());
    const Rendering::ViewReadbackResult paintersOrder = UITesting::RenderUiToReadback(fx.Manager(), space);
    ASSERT_EQ(paintersOrder.pixels.size(), withPrePass.pixels.size());

    size_t first = 0;
    const size_t differing = CountDifferingPixels(withPrePass.pixels, paintersOrder.pixels, first);
    EXPECT_EQ(differing, 0u) << "first at x " << first % UITesting::kReadbackW << " y "
                             << first / UITesting::kReadbackW;
}

} // namespace

// A fractional content scale puts text between device pixels; plate edges snap to them.
TEST(UIOpaqueInteriors, PrePassFrameIsByteIdenticalToThePaintersOrderFrame)
{
    for (const float contentScale : {1.0f, 1.25f, 1.5f, 2.0f})
    {
        ExpectPrePassFrameMatchesPaintersOrder(contentScale, UI::UITargetSpace::LinearSdr());
        ExpectPrePassFrameMatchesPaintersOrder(contentScale, UI::UITargetSpace::EncodedSrgb());
    }
}

TEST(UIOpaqueInteriors, TheMainDrawDoesNotShadeAnOpaqueInterior)
{
    UITesting::IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kPrePassXml, kPrePassCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const UI::UITargetSpace space = UI::UITargetSpace::LinearSdr();
    const Rendering::ViewReadbackResult painted = UITesting::RenderUiToReadback(fx.Manager(), space);
    ASSERT_FALSE(painted.pixels.empty());
    ASSERT_GE(UIManagerSdfRingAccess::OpaqueInteriorCount(fx.Manager()), 5u);

    // Only the pre-pass writes the interiors: given another fill, the frame
    // shows it wherever an interior is topmost, because the strict depth test
    // rejects each primitive's main-draw fragments over its own interior.
    constexpr uint32_t kMagenta = 0xFFFF00FFu;
    UIManagerSdfRingAccess::PaintOpaqueInteriors(fx.Manager(), kMagenta);
    const Rendering::ViewReadbackResult marked = UITesting::RenderUiToReadback(fx.Manager(), space);
    ASSERT_EQ(marked.pixels.size(), painted.pixels.size());

    size_t first = 0;
    const size_t differing = CountDifferingPixels(marked.pixels, painted.pixels, first);
    // The root's 10 px ring around the dock alone is about 27,000 pixels.
    EXPECT_GE(differing, 10000u);
}

TEST(UIOpaqueInteriors, APassWithNoDepthAttachmentDrawsWithNoDepthState)
{
    UITesting::IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kPrePassXml, kPrePassCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const UI::UITargetSpace space = UI::UITargetSpace::LinearSdr();
    ASSERT_FALSE(UITesting::RenderUiToReadback(fx.Manager(), space).pixels.empty());
    EXPECT_EQ(UIManagerSdfRingAccess::OverPrePassPipelineCount(fx.Manager()), 1u)
        << "the frame with interiors draws over the pre-pass depth";

    UIManagerSdfRingAccess::DisableOpaquePrePass(fx.Manager());
    ASSERT_FALSE(UITesting::RenderUiToReadback(fx.Manager(), space).pixels.empty());
    const Rendering::DepthStencilState& depth =
        UIManagerSdfRingAccess::MainDrawDesc(fx.Manager()).depthStencilState;
    EXPECT_FALSE(depth.depthTestEnable);
    EXPECT_FALSE(depth.depthWriteEnable);
}

TEST(UIOpaqueInteriors, AManagerWithThePrePassOffDrawsWithNoDepthTest)
{
    UITesting::IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kPrePassXml, kPrePassCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    fx.Manager().SetOpaquePrePassEnabled(false);
    ASSERT_FALSE(UITesting::RenderUiToReadback(fx.Manager(), UI::UITargetSpace::LinearSdr()).pixels.empty());
    EXPECT_EQ(UIManagerSdfRingAccess::OverPrePassPipelineCount(fx.Manager()), 0u);
}
