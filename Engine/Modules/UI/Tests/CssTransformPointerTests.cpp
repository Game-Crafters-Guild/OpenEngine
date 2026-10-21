// CSS `transform` and the pointer. A UIEvent carries the pointer in its
// current target's layout space, re-expressed at every bubble step, so a
// control under a transform compares it with its own layout rect as it would
// anywhere else; window-level popups (tooltips, dropdown popups) anchor to the
// element where it is drawn. The paint and the hit test are
// CssTransformRenderTests.cpp, the window-rect helpers
// CssTransformWindowRectTests.cpp.

#include <gtest/gtest.h>

#include <string>

#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"

#include "Mathematics/Vector2.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Scrollbar.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/Interaction/TooltipOverlay.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UITargetSpace.h"

using GameEngine::UITesting::IsolatedUIFixture;
using namespace GameEngine;

namespace
{

constexpr float kTolerance = 1e-3f;

#define BUILD_OR_SKIP(fx, xml, css)                         \
    IsolatedUIFixture fx;                                   \
    {                                                       \
        const bool built_ = fx.Build(1.0f, xml, css);       \
        if (!fx.DeviceAvailable())                          \
            GTEST_SKIP() << "No Vulkan device available";   \
        ASSERT_TRUE(built_) << fx.Diagnostic();             \
    }

void MoveTo(IsolatedUIFixture& fx, float x, float y)
{
    fx.Manager().OnMouseMove(x, y);
    fx.StepFrame();
}

void Press(IsolatedUIFixture& fx, float x, float y)
{
    MoveTo(fx, x, y);
    fx.Manager().OnMouseButton(0, true);
    fx.StepFrame();
}

void Release(IsolatedUIFixture& fx)
{
    fx.Manager().OnMouseButton(0, false);
    fx.StepFrame();
}

} // namespace

// ---------------------------------------------------------------------------
// Events, through the real pointer path.
// ---------------------------------------------------------------------------

// #outer turns a quarter about its centre (200, 150); #inner, inside it, is
// scaled 2x about its top-left corner (120, 120). The inner layout point
// (130, 125) is (140, 130) in #outer's space and (220, 90) in the window.
TEST(CssTransformPointer, EachBubbleStepSeesThePointerInItsOwnLayoutSpace)
{
    constexpr char kXml[] = R"(<uielement id="root"><uielement id="outer"><uielement id="inner"/></uielement></uielement>)";
    constexpr char kCss[] = R"(
#root  { width: 800px; height: 600px; }
#outer { position: absolute; left: 100px; top: 100px; width: 200px; height: 100px; transform: rotate(90deg); }
#inner { position: absolute; left: 20px; top: 20px; width: 40px; height: 20px;
         background-color: #ffffff; transform: scale(2); transform-origin: 0 0; }
)";
    BUILD_OR_SKIP(fx, kXml, kCss);

    struct Seen
    {
        float X = -1.0f;
        float Y = -1.0f;
    };
    Seen inner;
    Seen outer;
    Seen root;
    fx.Element("inner")->RegisterEventHandler(kEventMouseDown, [&inner](UIEvent& e) { inner = {e.X, e.Y}; });
    fx.Element("outer")->RegisterEventHandler(kEventMouseDown, [&outer](UIEvent& e) { outer = {e.X, e.Y}; });
    fx.Element("root")->RegisterEventHandler(kEventMouseDown, [&root](UIEvent& e) { root = {e.X, e.Y}; });

    Press(fx, 220.0f, 90.0f);
    Release(fx);

    EXPECT_NEAR(inner.X, 130.0f, kTolerance);
    EXPECT_NEAR(inner.Y, 125.0f, kTolerance);
    EXPECT_NEAR(outer.X, 140.0f, kTolerance);
    EXPECT_NEAR(outer.Y, 130.0f, kTolerance);
    EXPECT_NEAR(root.X, 220.0f, kTolerance) << "outside every transform the pointer is the window point";
    EXPECT_NEAR(root.Y, 90.0f, kTolerance);
}

// Beside X and Y in each target's space, an event carries the window point
// once for the whole dispatch: a handler on a scaled element reads both.
TEST(CssTransformPointer, TheWindowPointIsTheSameAtEveryBubbleStep)
{
    constexpr char kXml[] = R"(<uielement id="root"><uielement id="zoom"><uielement id="leaf"/></uielement></uielement>)";
    constexpr char kCss[] = R"(
#root { width: 800px; height: 600px; }
#zoom { position: absolute; left: 100px; top: 100px; width: 200px; height: 100px;
        transform: scale(2); transform-origin: 0 0; }
#leaf { position: absolute; left: 0px; top: 0px; width: 200px; height: 100px; background-color: #ffffff; }
)";
    BUILD_OR_SKIP(fx, kXml, kCss);
    struct Seen
    {
        Mathematics::Vector2 Local{-1.0f, -1.0f};
        Mathematics::Vector2 Window{-1.0f, -1.0f};
    };
    Seen leaf;
    Seen root;
    const auto record = [](Seen& seen) {
        return [&seen](UIEvent& e) { seen = {Mathematics::Vector2(e.X, e.Y), Mathematics::Vector2(e.WindowX, e.WindowY)}; };
    };
    fx.Element("leaf")->RegisterEventHandler(kEventMouseDown, record(leaf));
    fx.Element("root")->RegisterEventHandler(kEventMouseDown, record(root));

    // The window point (300, 180) is the leaf's layout point (200, 140).
    Press(fx, 300.0f, 180.0f);
    Release(fx);

    EXPECT_NEAR(leaf.Local.x, 200.0f, kTolerance);
    EXPECT_NEAR(leaf.Local.y, 140.0f, kTolerance);
    EXPECT_NEAR(leaf.Window.x, 300.0f, kTolerance) << "the window point is not re-expressed";
    EXPECT_NEAR(leaf.Window.y, 180.0f, kTolerance);
    EXPECT_NEAR(root.Window.x, 300.0f, kTolerance);
    EXPECT_NEAR(root.Window.y, 180.0f, kTolerance);
}

// Three identical sliders: one plain, one scaled 2x about its top-left
// corner, one turned a quarter about its centre. Pressing and dragging where
// a fraction of the track is drawn gives each the value the plain one gets at
// the same fraction.
TEST(CssTransformPointer, ASliderReadsThePointerWhereItIsDrawn)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <slider id="plain"/>
  <uielement id="zoom"><slider id="scaled"/></uielement>
  <uielement id="turn"><slider id="turned"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#root   { width: 800px; height: 600px; }
#plain  { position: absolute; left: 100px; top: 400px; width: 200px; height: 20px; }
#zoom   { position: absolute; left: 100px; top: 100px; width: 200px; height: 20px;
          transform: scale(2); transform-origin: 0 0; }
#turn   { position: absolute; left: 450px; top: 200px; width: 200px; height: 20px; transform: rotate(90deg); }
#scaled, #turned { position: absolute; left: 0px; top: 0px; width: 200px; height: 20px; }
)";
    BUILD_OR_SKIP(fx, kXml, kCss);
    auto* plain = dynamic_cast<Slider*>(fx.Element("plain"));
    auto* scaled = dynamic_cast<Slider*>(fx.Element("scaled"));
    auto* turned = dynamic_cast<Slider*>(fx.Element("turned"));
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(scaled, nullptr);
    ASSERT_NE(turned, nullptr);

    // Fraction f of the 200px track: layout x = left + 200 f, at mid height.
    const auto plainAt = [](float f) { return Mathematics::Vector2(100.0f + 200.0f * f, 410.0f); };
    const auto scaledAt = [](float f) { return Mathematics::Vector2(100.0f + 400.0f * f, 120.0f); };
    // The turn about (550, 210) takes the layout point (450 + 200 f, 210) to
    // (550, 110 + 200 f).
    const auto turnedAt = [](float f) { return Mathematics::Vector2(550.0f, 110.0f + 200.0f * f); };

    const auto drag = [&fx](Mathematics::Vector2 from, Mathematics::Vector2 to)
    {
        Press(fx, from.x, from.y);
        MoveTo(fx, to.x, to.y);
    };

    drag(plainAt(0.3f), plainAt(0.7f));
    const float plainValue = plain->GetValue();
    Release(fx);
    drag(scaledAt(0.3f), scaledAt(0.7f));
    const float scaledValue = scaled->GetValue();
    Release(fx);
    drag(turnedAt(0.3f), turnedAt(0.7f));
    const float turnedValue = turned->GetValue();
    Release(fx);

    ASSERT_GT(plainValue, 0.5f) << "the reference drag moved the plain slider";
    EXPECT_NEAR(scaledValue, plainValue, kTolerance);
    EXPECT_NEAR(turnedValue, plainValue, kTolerance);
}

// A click puts the caret where it lands in the drawn text, scaled or not.
TEST(CssTransformPointer, AScaledTextInputPutsTheCaretUnderThePointer)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <textinput id="plain"/>
  <uielement id="zoom"><textinput id="scaled"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#root   { width: 800px; height: 600px; }
#plain  { position: absolute; left: 100px; top: 300px; width: 200px; height: 24px; font-size: 14px; }
#zoom   { position: absolute; left: 100px; top: 100px; width: 200px; height: 24px;
          transform: scale(2); transform-origin: 0 0; }
#scaled { position: absolute; left: 0px; top: 0px; width: 200px; height: 24px; font-size: 14px; }
)";
    BUILD_OR_SKIP(fx, kXml, kCss);
    auto* plain = dynamic_cast<TextInput*>(fx.Element("plain"));
    auto* scaled = dynamic_cast<TextInput*>(fx.Element("scaled"));
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(scaled, nullptr);
    plain->SetValue("abcdefghijklmnop");
    scaled->SetValue("abcdefghijklmnop");
    fx.Settle();

    // 45 layout px into the text box.
    Press(fx, 145.0f, 312.0f);
    Release(fx);
    const int plainCaret = plain->GetCaretIndex();
    Press(fx, 100.0f + 2.0f * 45.0f, 100.0f + 2.0f * 12.0f);
    Release(fx);

    ASSERT_GT(plainCaret, 0) << "the reference click landed inside the text";
    EXPECT_EQ(scaled->GetCaretIndex(), plainCaret);
}

// Dragging along a scroll bar under a quarter turn scrolls by the distance the
// pointer moved along the drawn bar, as the same drag does on a plain one.
TEST(CssTransformPointer, AScrollbarUnderARotationDragsAlongItsDrawnAxis)
{
    constexpr char kXml[] = R"(<uielement id="root">
  <scrollview id="plain"/>
  <uielement id="turn"><scrollview id="turned"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#root   { width: 800px; height: 600px; }
#plain  { position: absolute; left: 50px; top: 50px; width: 150px; height: 150px; }
#turn   { position: absolute; left: 450px; top: 350px; width: 150px; height: 150px; transform: rotate(90deg); }
#turned { position: absolute; left: 0px; top: 0px; width: 150px; height: 150px; }
.scroll-row { display: flex; flex-direction: row; flex-grow: 1; min-width: 0; min-height: 0; }
.scroll-viewport { display: flex; flex-direction: column; flex-grow: 1; min-width: 0; min-height: 0; overflow: hidden; }
.scroll-content { display: flex; flex-direction: column; min-width: 0; }
.tall { width: 100px; height: 1500px; min-height: 1500px; flex-shrink: 0; }
)";
    BUILD_OR_SKIP(fx, kXml, kCss);
    auto* plain = dynamic_cast<ScrollView*>(fx.Element("plain"));
    auto* turned = dynamic_cast<ScrollView*>(fx.Element("turned"));
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(turned, nullptr);
    for (ScrollView* view : {plain, turned})
    {
        auto tall = std::make_unique<UIElement>();
        tall->AddClass("tall");
        view->AddContent(std::move(tall));
    }
    fx.Settle();
    auto* plainBar = dynamic_cast<Scrollbar*>(plain->GetVerticalScrollbar());
    auto* turnedBar = dynamic_cast<Scrollbar*>(turned->GetVerticalScrollbar());
    ASSERT_NE(plainBar, nullptr);
    ASSERT_NE(turnedBar, nullptr);
    ASSERT_FALSE(plainBar->IsHidden());
    ASSERT_GT(plainBar->GetLayoutHeight(), 0.0f);

    // A quarter of the way down the bar to three quarters, in each bar's
    // layout space.
    const auto along = [](const Scrollbar& bar, float f)
    {
        return Mathematics::Vector2(bar.GetLayoutX() + bar.GetLayoutWidth() * 0.5f,
                                    bar.GetLayoutY() + bar.GetLayoutHeight() * f);
    };
    // The turn about (525, 425) takes the layout point (x, y) to (950 - y, x - 100).
    const auto toWindow = [](Mathematics::Vector2 p) { return Mathematics::Vector2(950.0f - p.y, p.x - 100.0f); };

    Press(fx, along(*plainBar, 0.25f).x, along(*plainBar, 0.25f).y);
    MoveTo(fx, along(*plainBar, 0.75f).x, along(*plainBar, 0.75f).y);
    Release(fx);
    const Mathematics::Vector2 from = toWindow(along(*turnedBar, 0.25f));
    const Mathematics::Vector2 to = toWindow(along(*turnedBar, 0.75f));
    Press(fx, from.x, from.y);
    MoveTo(fx, to.x, to.y);
    Release(fx);

    ASSERT_GT(plain->GetScrollY(), 0.5f) << "the reference drag scrolled the plain view";
    EXPECT_NEAR(turned->GetScrollY(), plain->GetScrollY(), 0.5f);
}

// ---------------------------------------------------------------------------
// Window-level popups anchor to the drawn element.
// ---------------------------------------------------------------------------

// Two sources share the layout rect (300, 450) 100x20; one is scaled 2x about
// its centre (350, 460), so it is drawn from y 440 to 480, 10 px below its
// layout bottom. A tooltip below it sits exactly those 10 px lower than the
// same tooltip below the plain source.
TEST(CssTransformPointer, ATooltipBelowAScaledSourceClearsTheDrawnBox)
{
    constexpr char kXml[] = R"(<uielement id="root"><uielement id="plain"/><uielement id="scaled"/></uielement>)";
    constexpr char kCss[] = R"(
#root   { width: 800px; height: 600px; }
#plain, #scaled { position: absolute; left: 300px; top: 450px; width: 100px; height: 20px; }
#scaled { transform: scale(2); }
)";
    BUILD_OR_SKIP(fx, kXml, kCss);
    UIElement* root = fx.Manager().GetRootElement();
    UI::Interaction::TooltipOverlay tip;
    tip.SetHoverDelay(0.0f);
    float time = 0.0f;
    const auto placeBelow = [&](UIElement* source)
    {
        source->SetTooltip("Tip");
        source->SetTooltipPlacement(UIElement::TooltipPlacement::Below);
        for (int i = 0; i < 4; ++i)
        {
            time += 0.1f;
            tip.Update(root, source, 350.0f, 460.0f, time);
            fx.Settle();
        }
        UIElement* wrapper = root->FindById(HashStringId("ui-tooltip-wrapper"));
        return wrapper ? Mathematics::Vector2(wrapper->GetLayoutX() + wrapper->GetLayoutWidth() * 0.5f,
                                              wrapper->GetLayoutY())
                       : Mathematics::Vector2(-1.0f, -1.0f);
    };

    const Mathematics::Vector2 plain = placeBelow(fx.Element("plain"));
    const Mathematics::Vector2 scaled = placeBelow(fx.Element("scaled"));
    ASSERT_GT(plain.y, 470.0f) << "the reference bubble sits below its source";
    EXPECT_FLOAT_EQ(scaled.y - plain.y, 10.0f) << "the bubble moves down by the drawn growth below the source";
    EXPECT_FLOAT_EQ(scaled.x, plain.x) << "a scale about the centre leaves the horizontal anchor";
}

// A dropdown scaled 2x about its top-left corner (400, 40) has its header
// drawn at (400, 40)-(700, 88). Its popup, an overlay, paints in the header's
// transform: three 20 px rows laid out under the header at 1x are drawn at 2x,
// x 400-700 and y 88-208, as a popup inside a scaled element is on the web.
TEST(CssTransformPointer, ADropdownPopupIsDrawnInItsHeadersTransform)
{
    constexpr char kXml[] = R"(<uielement id="root"><uielement id="zoom"><dropdown id="dd"/></uielement></uielement>)";
    constexpr char kCss[] = R"(
#root  { width: 800px; height: 600px; background-color: #000000; }
#zoom  { position: absolute; left: 400px; top: 40px; width: 150px; height: 24px;
         transform: scale(2); transform-origin: 0 0; }
#dd    { position: absolute; left: 0px; top: 0px; width: 150px; height: 24px; background-color: #ff0000; }
.dropdown-items { display: none; position: absolute; top: 100%; left: 0px; width: 100%;
                  flex-direction: column; background-color: #00ff00; }
.dropdown-items.open { display: flex; }
.dropdown-item { height: 20px; flex-shrink: 0; }
)";
    BUILD_OR_SKIP(fx, kXml, kCss);
    auto* dd = dynamic_cast<Dropdown*>(fx.Element("dd"));
    ASSERT_NE(dd, nullptr);
    dd->SetOptionsFromString("One,Two,Three");
    fx.Settle();
    dd->OpenMenuUi();
    fx.Settle();

    UIElement* popup = dd->GetItemsContainer();
    ASSERT_NE(popup, nullptr);
    ASSERT_TRUE(dd->IsMenuOpen());
    ASSERT_FLOAT_EQ(popup->GetLayoutHeight(), 60.0f);
    const Mathematics::Rect drawn = popup->GetWindowRect();
    EXPECT_NEAR(drawn.X, 400.0f, kTolerance);
    EXPECT_NEAR(drawn.Y, 88.0f, kTolerance);
    EXPECT_NEAR(drawn.Width, 300.0f, kTolerance);
    EXPECT_NEAR(drawn.Height, 120.0f, kTolerance);

    const std::vector<uint8_t> px = UITesting::RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty());
    const auto expectAt = [&px](uint32_t x, uint32_t y, int r, int g, int b, const char* what)
    {
        UITesting::Rgb p{};
        std::string why;
        ASSERT_TRUE(UITesting::UniformPatch(px, x, y, p, why)) << what << ": " << why;
        EXPECT_EQ(p.R, r) << what;
        EXPECT_EQ(p.G, g) << what;
        EXPECT_EQ(p.B, b) << what;
    };
    expectAt(650, 60, 255, 0, 0, "the header, drawn at 2x");
    expectAt(650, 190, 0, 255, 0, "the popup past its 1x width and height: drawn at 2x");
    expectAt(650, 220, 0, 0, 0, "below the 2x popup");
}
