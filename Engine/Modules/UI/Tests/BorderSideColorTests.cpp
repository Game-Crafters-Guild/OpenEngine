// Each side of a solid border paints in its own color (#7004). The box's own
// Rect carries the ring of the sides that share the most common color, one Rect
// paints each side in another color, and one Triangle gives each square corner
// between two colors back to the side drawn first, on the corner's diagonal.

#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "Memory/AllocationCountScope.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using GameEngine::UI::PackFromARGB;
using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;
using GameEngine::UITesting::PixelAt;
using GameEngine::UITesting::RenderUiToBytes;
using GameEngine::UITesting::Rgb;
using namespace GameEngine;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="grey"/>
  <uielement id="bar"/>
  <uielement id="mitre"/>
  <uielement id="dots"/>
  <uielement id="dashes"/>
</uielement>)";

// #mitre: green right and bottom sides (the ring), a red top and a blue left.
// Its 10px sides make each corner square 10x10, so a sample 3.5px off the
// diagonal is clear of its anti-aliased edge.
constexpr char kCss[] = R"(
#root   { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
#grey   { width: 100px; height: 40px; margin: 10px; border: 1px solid #c0c0c0; background-color: #202020; }
#bar    { width: 100px; height: 40px; margin: 10px; border-width: 0 0 0 3px; border-top-color: #ff0000;
          border-right-color: #00ff00; border-bottom-color: #0000ff; border-left-color: #e08a2a;
          background-color: #202020; }
#mitre  { width: 100px; height: 80px; margin: 10px; border-width: 10px; border-color: #00ff00;
          border-top-color: #ff0000; border-left-color: #0000ff; background-color: #000000; }
#dots   { width: 80px; height: 40px; margin: 10px; border-style: dotted; border-width: 2px;
          border-color: #ff0000; border-left-color: #0000ff; }
#dashes { width: 80px; height: 40px; margin: 10px; border-style: dashed; border-width: 0 0 0 2px;
          border-color: #ff0000; border-left-color: #0000ff; }
)";

bool BuildOrSkip(IsolatedUIFixture& fx, const std::string& xml, const std::string& css, std::string& why)
{
    const bool built = fx.Build(1.0f, xml, css);
    if (!fx.DeviceAvailable())
    {
        why = "No Vulkan device available";
        return false;
    }
    EXPECT_TRUE(built) << fx.Diagnostic();
    return built;
}

void ExpectNear(const Rgb& p, uint32_t argb, const std::string& what)
{
    constexpr int kTolerance = 2;
    const int r = static_cast<int>((argb >> 16) & 0xFF);
    const int g = static_cast<int>((argb >> 8) & 0xFF);
    const int b = static_cast<int>(argb & 0xFF);
    EXPECT_TRUE(std::abs(p.R - r) <= kTolerance && std::abs(p.G - g) <= kTolerance && std::abs(p.B - b) <= kTolerance)
        << what << ": got " << p.R << "," << p.G << "," << p.B << " want " << r << "," << g << "," << b;
}

Rgb PixelIn(const std::vector<uint8_t>& px, const PhysicalRect& box, float dx, float dy)
{
    return PixelAt(px, static_cast<uint32_t>(box.X + dx), static_cast<uint32_t>(box.Y + dy));
}

} // namespace

// A box whose painted sides share one color stays the one Rect it always was,
// whatever the colors of its sides without a width are.
TEST(BorderSideColor, OneColorBorderIsOneRect)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, kXml, kCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const std::vector<UIPrimitive> grey = fx.Primitives("grey");
    ASSERT_EQ(grey.size(), 1u) << "border: 1px solid is one primitive";
    EXPECT_EQ(UI::GetMode(grey[0].ModeAndFlags), PrimitiveMode::Rect);
    EXPECT_EQ(grey[0].BorderColor, PackFromARGB(0xFFC0C0C0u));
    for (float width : grey[0].BorderWidths)
        EXPECT_FLOAT_EQ(width, 1.0f);

    const std::vector<UIPrimitive> bar = fx.Primitives("bar");
    ASSERT_EQ(bar.size(), 1u) << "only the left side has a width, so only its color paints";
    EXPECT_EQ(UI::GetMode(bar[0].ModeAndFlags), PrimitiveMode::Rect);
    EXPECT_EQ(bar[0].BorderColor, PackFromARGB(0xFFE08A2Au));
    EXPECT_FLOAT_EQ(bar[0].BorderWidths[0], 3.0f) << "L of BorderWidths[L,T,R,B]";
    EXPECT_FLOAT_EQ(bar[0].BorderWidths[1], 0.0f);
    EXPECT_FLOAT_EQ(bar[0].BorderWidths[2], 0.0f);
    EXPECT_FLOAT_EQ(bar[0].BorderWidths[3], 0.0f);
}

// Each side paints its own color, and a square corner between two colors is
// split on the line from the outer corner to the inner corner, as in CSS.
TEST(BorderSideColor, EachSidePaintsItsOwnColorAndSquareCornersSplitOnTheDiagonal)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, kXml, kCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty()) << "render/readback produced no pixels";
    const PhysicalRect b = fx.BorderBox("mitre");
    ASSERT_FLOAT_EQ(b.W, 100.0f);
    ASSERT_FLOAT_EQ(b.H, 80.0f);

    constexpr uint32_t kRed = 0xFFFF0000u;
    constexpr uint32_t kGreen = 0xFF00FF00u;
    constexpr uint32_t kBlue = 0xFF0000FFu;
    ExpectNear(PixelIn(px, b, 50, 5), kRed, "top side");
    ExpectNear(PixelIn(px, b, 95, 40), kGreen, "right side");
    ExpectNear(PixelIn(px, b, 50, 75), kGreen, "bottom side");
    ExpectNear(PixelIn(px, b, 5, 40), kBlue, "left side");
    ExpectNear(PixelIn(px, b, 7, 2), kRed, "top-left corner, the top's half");
    ExpectNear(PixelIn(px, b, 2, 7), kBlue, "top-left corner, the left's half");
    ExpectNear(PixelIn(px, b, 92, 2), kRed, "top-right corner, the top's half");
    ExpectNear(PixelIn(px, b, 97, 7), kGreen, "top-right corner, the right's half");
    ExpectNear(PixelIn(px, b, 2, 72), kBlue, "bottom-left corner, the left's half");
    ExpectNear(PixelIn(px, b, 7, 77), kGreen, "bottom-left corner, the bottom's half");
    ExpectNear(PixelIn(px, b, 50, 40), 0xFF000000u, "the fill");

    // The cost: the box with the green ring, a Rect for the top and one for the
    // left, and a Triangle for each corner between two colors.
    EXPECT_EQ(fx.Primitives("mitre", PrimitiveMode::Rect).size(), 3u);
    EXPECT_EQ(fx.Primitives("mitre", PrimitiveMode::Triangle).size(), 3u);
}

// Each dot takes the color of its side. Dashes run only along sides that have
// a width, in that side's color.
TEST(BorderSideColor, DotsAndDashesTakeTheColorOfTheirSide)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, kXml, kCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const PhysicalRect dotsBox = fx.BorderBox("dots");
    const std::vector<UIPrimitive> dots = fx.Primitives("dots", PrimitiveMode::Rect);
    ASSERT_GT(dots.size(), 4u);
    int leftDots = 0;
    for (size_t dotIndex = 1; dotIndex < dots.size(); ++dotIndex) // [0] is the box itself
    {
        const bool onLeft = dots[dotIndex].X + dots[dotIndex].W * 0.5f < dotsBox.X + 4.0f;
        leftDots += onLeft ? 1 : 0;
        EXPECT_EQ(dots[dotIndex].FillColor, PackFromARGB(onLeft ? 0xFF0000FFu : 0xFFFF0000u))
            << "dot at " << dots[dotIndex].X << "," << dots[dotIndex].Y;
    }
    EXPECT_GT(leftDots, 0);

    const PhysicalRect dashesBox = fx.BorderBox("dashes");
    const std::vector<UIPrimitive> dashes = fx.Primitives("dashes", PrimitiveMode::Line);
    ASSERT_FALSE(dashes.empty());
    for (const UIPrimitive& dash : dashes)
    {
        EXPECT_LT(dash.X + dash.W * 0.5f, dashesBox.X + 4.0f) << "a dash on a side without a width, at "
                                                               << dash.X << "," << dash.Y;
        EXPECT_EQ(dash.FillColor, PackFromARGB(0xFF0000FFu));
    }
}

// The game's main menu: an entry with only a 3px left side, transparent until
// focus colors it ember.
TEST(BorderSideColor, FocusedMenuEntryPaintsItsEmberBar)
{
    constexpr char kEntryXml[] = R"(<uielement id="root">
  <button id="entry" class="menu-entry"><label text="Skirmish"/></button>
</uielement>)";
    constexpr char kEntryCss[] = R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
button.menu-entry { width: 200px; height: 40px; border-radius: 0; border-width: 0 0 0 3px;
                    border-color: rgba(224, 138, 42, 0); background-color: #0e1218; }
button.menu-entry:focus { border-left-color: #e08a2a; }
)";
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, kEntryXml, kEntryCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    fx.FocusViaTab();
    fx.PumpSeconds(0.5f); // past any border-color transition of the button's own sheet

    const std::vector<UIPrimitive> rects = fx.Primitives("entry", PrimitiveMode::Rect);
    ASSERT_EQ(rects.size(), 1u);
    EXPECT_EQ(rects[0].BorderColor, PackFromARGB(0xFFE08A2Au));
    EXPECT_FLOAT_EQ(rects[0].BorderWidths[0], 3.0f);

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty());
    const PhysicalRect b = fx.BorderBox("entry");
    ExpectNear(PixelIn(px, b, 1, b.H * 0.5f), 0xFFE08A2Au, "the focused entry's left bar");
}

// A :focus rule's border color paints from the first frame: focus set before
// any frame runs, the first style pass resolves the focused color (#6088).
TEST(BorderSideColor, FocusRuleBorderColorPaintsFromTheFirstFrame)
{
    constexpr char kEntryXml[] = R"(<uielement id="root">
  <button id="entry" class="menu-entry"><label text="Skirmish"/></button>
</uielement>)";
    constexpr char kEntryCss[] = R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
button.menu-entry { width: 200px; height: 40px; border-radius: 0; border-width: 1px;
                    border-color: #3a3a3a; background-color: #0e1218; }
button.menu-entry:focus { border-color: #e08a2a; }
)";
    IsolatedUIFixture fx;
    const bool loaded = fx.Load(1.0f, kEntryXml, kEntryCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(loaded) << fx.Diagnostic();
    fx.Manager().SetFocusById("entry");
    fx.Settle();

    const std::vector<UIPrimitive> rects = fx.Primitives("entry", PrimitiveMode::Rect);
    ASSERT_EQ(rects.size(), 1u);
    EXPECT_EQ(rects[0].BorderColor, PackFromARGB(0xFFE08A2Au));
}

// The Button sheet fades border-color over 0.15 s, so focus takes an entry from
// one border color to two, and blur takes it back. The other color paints during
// the whole fade, and after the fade out the box is one Rect again.
TEST(BorderSideColor, FocusFadeAddsTheSideInAnotherColorAndBlurRemovesIt)
{
    constexpr char kEntryXml[] = R"(<uielement id="root">
  <button id="square" class="entry"><label text="Skirmish"/></button>
  <button id="rounded" class="entry round"><label text="Campaign"/></button>
</uielement>)";
    constexpr char kEntryCss[] = R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
button.entry { width: 200px; height: 40px; margin: 10px; border-radius: 0; border: 1px solid #3a3a3a;
               border-left: 3px solid rgba(224, 138, 42, 0); background-color: #0e1218;
               transition: border-color 0.15s; }
button.entry.round { border-radius: 4px; border-color: rgba(196, 164, 92, 0.16);
                     border-left-color: rgba(224, 138, 42, 0); }
button.entry:focus { border-left-color: #e08a2a; }
)";
    IsolatedUIFixture fx;
    const bool loaded = fx.Load(1.0f, kEntryXml, kEntryCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(loaded) << fx.Diagnostic();
    fx.Settle();
    ASSERT_EQ(fx.Primitives("square", PrimitiveMode::Rect).size(), 1u) << "at rest the left side is transparent";

    const PhysicalRect square = fx.BorderBox("square");
    const PhysicalRect rounded = fx.BorderBox("rounded");
    const uint32_t ember = PackFromARGB(0xFFE08A2Au);

    // Focus after the first frames. Part way through the fade the left side is a
    // second Rect with part of its alpha, and each left corner has a Triangle.
    fx.Manager().SetFocusById("square");
    fx.PumpSeconds(0.016f);
    fx.PumpSeconds(0.04f);
    std::vector<UIPrimitive> rects = fx.Primitives("square", PrimitiveMode::Rect);
    ASSERT_EQ(rects.size(), 2u) << "mid fade in: the box and the left side";
    const uint32_t alpha = rects[1].BorderColor >> 24;
    EXPECT_TRUE(alpha > 0u && alpha < 255u) << "mid fade in the left side's alpha is " << alpha;
    EXPECT_EQ(fx.Primitives("square", PrimitiveMode::Triangle).size(), 2u) << "the two left corners";
    std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty());
    const Rgb fading = PixelIn(px, square, 1, square.H * 0.5f);
    EXPECT_TRUE(fading.R > 0x0e + 20 && fading.R < 0xe0 - 20) << "mid fade in the left side's red is " << fading.R;

    fx.PumpSeconds(0.5f);
    rects = fx.Primitives("square", PrimitiveMode::Rect);
    ASSERT_EQ(rects.size(), 2u);
    EXPECT_EQ(rects[1].BorderColor, ember);
    px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ExpectNear(PixelIn(px, square, 1, square.H * 0.5f), 0xFFE08A2Au, "the focused entry's left side");
    ExpectNear(PixelIn(px, square, square.W * 0.5f, 0), 0xFF3A3A3Au, "the focused entry's top side");

    // Focus moves on. The first entry keeps its left side through the fade out
    // and is then one Rect; the rounded entry gets its side and no Triangle.
    fx.Manager().SetFocusById("rounded");
    fx.PumpSeconds(0.016f);
    fx.PumpSeconds(0.04f);
    EXPECT_EQ(fx.Primitives("square", PrimitiveMode::Rect).size(), 2u) << "mid fade out the left side paints";
    fx.PumpSeconds(0.5f);
    EXPECT_EQ(fx.Primitives("square", PrimitiveMode::Rect).size(), 1u) << "after the fade out";
    EXPECT_EQ(fx.Primitives("square", PrimitiveMode::Triangle).size(), 0u) << "after the fade out";
    rects = fx.Primitives("rounded", PrimitiveMode::Rect);
    ASSERT_EQ(rects.size(), 2u);
    EXPECT_EQ(rects[1].BorderColor, ember);
    EXPECT_EQ(fx.Primitives("rounded", PrimitiveMode::Triangle).size(), 0u) << "a rounded corner has no Triangle";
    px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ExpectNear(PixelIn(px, square, 1, square.H * 0.5f), 0xFF0E1218u, "the blurred entry's left side shows the fill");
    ExpectNear(PixelIn(px, rounded, 1, rounded.H * 0.5f), 0xFFE08A2Au, "the focused rounded entry's left side");
}

// The game's card: a 1px gold border with a 3px left side that is transparent
// at rest and ember when selected, on a 4px radius. The transparent side paints
// nothing, the ember side paints, and the rounded join warns once.
TEST(BorderSideColor, SelectedCardOnASmallRadiusPaintsItsEmberSide)
{
    constexpr char kCardXml[] = R"(<uielement id="root">
  <uielement id="card7004" class="card selected"/>
  <uielement id="card7004b" class="card selected"/>
  <uielement id="resting7004" class="card"/>
</uielement>)";
    constexpr char kCardCss[] = R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
.card { width: 200px; height: 40px; margin: 10px; border: 1px solid #c9a043; border-radius: 4px;
        border-left: 3px solid rgba(224, 138, 42, 0); background-color: #102030; }
.card.selected { border-left: 3px solid #e08a2a; }
)";
    Logger::Log::Initialize({});
    auto sink = std::make_unique<Logger::CallbackSink>();
    auto* sinkPtr = sink.get();
    auto messages = std::make_shared<std::vector<std::string>>();
    const std::uint64_t callbackId = sinkPtr->RegisterCallback(
        [messages](const Logger::LogMessage& msg) { messages->emplace_back(msg.Message.c_str()); });
    Logger::Log::AddSink(std::move(sink));

    IsolatedUIFixture fx;
    std::string why;
    const bool built = BuildOrSkip(fx, kCardXml, kCardCss, why);
    if (built)
        fx.PumpSeconds(0.2f);
    Logger::Log::Flush();
    sinkPtr->UnregisterCallback(callbackId);
    if (!built)
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty());
    const PhysicalRect b = fx.BorderBox("card7004");
    ExpectNear(PixelIn(px, b, 1, b.H * 0.5f), 0xFFE08A2Au, "the selected card's left side");
    ExpectNear(PixelIn(px, b, b.W * 0.5f, 0), 0xFFC9A043u, "the selected card's top side");
    ExpectNear(PixelIn(px, b, b.W - 1, b.H * 0.5f), 0xFFC9A043u, "the selected card's right side");

    EXPECT_EQ(fx.Primitives("card7004", PrimitiveMode::Rect).size(), 2u)
        << "the box with the gold ring, and one Rect for the ember side";

    const PhysicalRect resting = fx.BorderBox("resting7004");
    ExpectNear(PixelIn(px, resting, 1, resting.H * 0.5f), 0xFF102030u, "the resting card's transparent left side");
    ExpectNear(PixelIn(px, resting, resting.W * 0.5f, 0), 0xFFC9A043u, "the resting card's top side");
    EXPECT_EQ(fx.Primitives("resting7004").size(), 1u) << "a transparent side adds no primitive";
    int warnings = 0;
    for (const std::string& message : *messages)
        warnings += (message.find("#card7004") != std::string::npos &&
                     message.find("rounded corner") != std::string::npos) ? 1 : 0;
    EXPECT_EQ(warnings, 1) << "one warning for the two cards of one rule, however many frames paint them";
    int restingWarnings = 0;
    for (const std::string& message : *messages)
        restingWarnings += message.find("#resting7004") != std::string::npos ? 1 : 0;
    EXPECT_EQ(restingWarnings, 0) << "a transparent side at a rounded corner logs no warning";
}

namespace
{

// The class names are longer than a short-string buffer, so a string built from
// them allocates.
constexpr char kFadeCardXml[] = R"(<uielement id="root">
  <button id="card" class="rounded-two-color-menu-card currently-selected-menu-card"/>
</uielement>)";

// A 1px gold border with a 3px left side in `leftColor` on a 4px radius. Focus
// fades the fill for 2 s.
std::string FadeCardCss(const std::string& leftColor)
{
    return R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
button.rounded-two-color-menu-card { width: 200px; height: 40px; margin: 10px; border: 1px solid #c9a043;
    border-left: 3px solid )" + leftColor + R"(; border-radius: 4px; background-color: #102030;
    transition: background-color 2s; }
button.rounded-two-color-menu-card:focus { background-color: #f0f0f0; }
)";
}

// Settles the card, focuses it, and counts the allocations of four frames of its
// fill fade. Each of these frames paints the card again, because its fill changes.
uint64_t AllocationsOverAFillFade(IsolatedUIFixture& fx)
{
    fx.Settle();
    fx.Manager().SetFocusById("card");
    for (int frame = 0; frame < 3; ++frame)
        fx.StepFrame();
    Logger::Log::Flush();
    const std::vector<UIPrimitive> before = fx.Primitives("card", PrimitiveMode::Rect);
    uint64_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (int frame = 0; frame < 4; ++frame)
            fx.StepFrame();
        allocations = probe.Count();
    }
    const std::vector<UIPrimitive> after = fx.Primitives("card", PrimitiveMode::Rect);
    EXPECT_FALSE(before.empty() || after.empty());
    if (!before.empty() && !after.empty())
        EXPECT_NE(before.front().FillColor, after.front().FillColor) << "the fade paints the card on each frame";
    return allocations;
}

} // namespace

// The rounded-corner warning reads the style when it resolves. A paint pass builds
// no warning key, so a rounded corner between two colors paints with the
// allocations of the same card in one color.
TEST(BorderSideColor, RoundedJoinBetweenTwoColorsPaintsWithTheAllocationsOfOneColor)
{
    uint64_t twoColors = 0;
    {
        IsolatedUIFixture fx;
        const bool loaded = fx.Load(1.0f, kFadeCardXml, FadeCardCss("#e08a2a"));
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(loaded) << fx.Diagnostic();
        twoColors = AllocationsOverAFillFade(fx);
        EXPECT_EQ(fx.Primitives("card", PrimitiveMode::Rect).size(), 2u) << "the gold ring and the ember side";
    }
    uint64_t oneColor = 0;
    {
        IsolatedUIFixture fx;
        ASSERT_TRUE(fx.Load(1.0f, kFadeCardXml, FadeCardCss("#c9a043"))) << fx.Diagnostic();
        oneColor = AllocationsOverAFillFade(fx);
        EXPECT_EQ(fx.Primitives("card", PrimitiveMode::Rect).size(), 1u) << "one gold ring";
    }
    EXPECT_EQ(twoColors, oneColor) << "a paint pass of the rounded join allocates nothing more";
}
