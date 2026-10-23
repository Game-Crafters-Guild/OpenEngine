// The hover tooltip stays inside the window. A source that asks for a side
// (TooltipPlacement Right, Below) at the window's right or bottom edge gets the
// bubble on the opposite side when it fits there, and a bubble that fits on
// neither side is kept inside the window. A pinned tooltip shows with the
// pointer elsewhere and hides when the pin ends; it is hidden while its source
// is hidden or its dock tab is swapped out, and a destroyed source ends the pin
// so hover tooltips show again.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "IsolatedUIFixture.h"

#include "UI/Controls/Mount.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

using namespace GameEngine;

namespace
{

// A 400 x 300 window with one tooltip source in its bottom-right corner.
constexpr char kXml[] = R"(<uielement id="root"><uielement id="source"/></uielement>)";
constexpr char kCss[] = R"(
#root { width: 400px; height: 300px; }
#source { position: absolute; left: 340px; top: 260px; width: 50px; height: 30px; }
)";

// Fits to the left of and above the source, not to its right or below it.
constexpr char kShortText[] = "Move";
// Wider than the room on either side of the source.
constexpr char kWideText[] = "A tooltip wider than the room between its source and the window edge";

struct Placed
{
    float SourceX = 0.0f;
    float SourceY = 0.0f;
    float X = 0.0f;
    float Y = 0.0f;
    float W = 0.0f;
    float H = 0.0f;
};

// Hovers the source past the hover delay. False when the fixture cannot run
// (`skipped`, the caller skips) or no bubble was placed.
bool HoverSource(UIElement::TooltipPlacement placement, const char* text, Placed& placed, bool& skipped)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kXml, kCss);
    skipped = !fixture.DeviceAvailable();
    if (skipped || !built)
        return false;

    UIElement* source = fixture.Element("source");
    source->SetTooltip(text);
    source->SetTooltipPlacement(placement);
    fixture.Manager().OnMouseMove(source->GetLayoutX() + 5.0f, source->GetLayoutY() + 5.0f);
    // Past the 0.5 s hover delay, then frames for the bubble to measure and place.
    for (int frame = 0; frame < 4; ++frame)
        fixture.PumpSeconds(0.3f);
    const UIElement* bubble = fixture.Element("ui-tooltip-wrapper");
    if (!bubble || bubble->GetLayoutWidth() <= 0.0f)
        return false;
    placed = {source->GetLayoutX(), source->GetLayoutY(), bubble->GetLayoutX(), bubble->GetLayoutY(),
              bubble->GetLayoutWidth(), bubble->GetLayoutHeight()};
    return true;
}

// True when the tooltip bubble is placed in the window; a hidden bubble is parked off it.
bool BubbleShown(const UITesting::IsolatedUIFixture& fixture)
{
    const UIElement* bubble = fixture.Element("ui-tooltip-wrapper");
    return bubble && bubble->GetLayoutX() >= 0.0f && bubble->GetLayoutY() >= 0.0f;
}

// Frames for a pinned bubble to measure and place, shorter than the hover delay.
void PumpPinFrames(UITesting::IsolatedUIFixture& fixture)
{
    for (int frame = 0; frame < 3; ++frame)
        fixture.PumpSeconds(0.05f);
}

// A dock-style Mount in the window's top-left corner showing one of two 120 x 80 tab panels;
// the first holds a tooltip source. The test owns the panels, as a dockspace does.
struct MountedTabs
{
    Mount* Host = nullptr;
    std::unique_ptr<UIElement> First;
    std::unique_ptr<UIElement> Second;
    UIElement* Source = nullptr;
};

std::unique_ptr<UIElement> MakeTabPanel()
{
    auto panel = std::make_unique<UIElement>();
    panel->Overrides().Set(Style::Width, StyleLength::Px(120.0f)).Set(Style::Height, StyleLength::Px(80.0f));
    return panel;
}

MountedTabs MountTabs(UITesting::IsolatedUIFixture& fixture)
{
    MountedTabs tabs;
    auto host = std::make_unique<Mount>();
    UI::Layout::SetAbsolutePosition(*host, Mathematics::Rect{0.0f, 0.0f, 120.0f, 80.0f});
    tabs.Host = host.get();
    fixture.Manager().GetRootElement()->AddChild(std::move(host));
    tabs.First = MakeTabPanel();
    tabs.Second = MakeTabPanel();
    auto source = std::make_unique<UIElement>();
    source->Overrides().Set(Style::Width, StyleLength::Px(60.0f)).Set(Style::Height, StyleLength::Px(20.0f));
    source->SetTooltip(kShortText);
    tabs.Source = source.get();
    tabs.First->AddChild(std::move(source));
    tabs.Host->SetTarget(tabs.First.get());
    return tabs;
}

void ExpectInsideTheWindow(const Placed& bubble)
{
    EXPECT_GE(bubble.X, 0.0f);
    EXPECT_GE(bubble.Y, 0.0f);
    EXPECT_LE(bubble.X + bubble.W, 400.0f) << "right edge";
    EXPECT_LE(bubble.Y + bubble.H, 300.0f) << "bottom edge";
}

} // namespace

TEST(TooltipPlacementTests, ARightTooltipAtTheWindowsRightEdgeFlipsLeft)
{
    Placed bubble;
    bool skipped = false;
    const bool shown = HoverSource(UIElement::TooltipPlacement::Right, kShortText, bubble, skipped);
    if (skipped)
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(shown) << "no tooltip shown";
    ExpectInsideTheWindow(bubble);
    EXPECT_LE(bubble.X + bubble.W, bubble.SourceX) << "the bubble sits left of its source";
}

TEST(TooltipPlacementTests, ABelowTooltipAtTheWindowsBottomEdgeFlipsAbove)
{
    Placed bubble;
    bool skipped = false;
    const bool shown = HoverSource(UIElement::TooltipPlacement::Below, kShortText, bubble, skipped);
    if (skipped)
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(shown) << "no tooltip shown";
    ExpectInsideTheWindow(bubble);
    EXPECT_LE(bubble.Y + bubble.H, bubble.SourceY) << "the bubble sits above its source";
}

// Neither side has room for the bubble: it is clamped inside the window instead.
TEST(TooltipPlacementTests, ARightTooltipThatFitsOnNeitherSideStaysInsideTheWindow)
{
    Placed bubble;
    bool skipped = false;
    const bool shown = HoverSource(UIElement::TooltipPlacement::Right, kWideText, bubble, skipped);
    if (skipped)
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(shown) << "no tooltip shown";
    EXPECT_GT(bubble.W, bubble.SourceX - 16.0f - 6.0f) << "the text must be too wide for the left side";
    ExpectInsideTheWindow(bubble);
}

// The pointer is away from the source the whole time: the pin alone shows the bubble, at once,
// with the source's text, and the unpin hides it.
TEST(TooltipPlacementTests, APinnedTooltipShowsWithoutAHoverUntilItIsUnpinned)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kXml, kCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built);

    UIElement* source = fixture.Element("source");
    source->SetTooltip(kShortText);
    fixture.Manager().OnMouseMove(5.0f, 5.0f);
    fixture.Manager().PinTooltip(*source);
    // Shorter than the hover delay: the frames the bubble needs to measure and place.
    for (int frame = 0; frame < 3; ++frame)
        fixture.PumpSeconds(0.05f);

    const UIElement* bubble = fixture.Element("ui-tooltip-wrapper");
    ASSERT_NE(bubble, nullptr) << "no tooltip shown";
    EXPECT_GE(bubble->GetLayoutX(), 0.0f) << "the bubble is parked off the window";
    EXPECT_GE(bubble->GetLayoutY(), 0.0f) << "the bubble is parked off the window";
    const UIElement* text = fixture.Element("ui-tooltip-text");
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->GetTextContent(), kShortText);

    fixture.Manager().UnpinTooltip(*source);
    fixture.PumpSeconds(0.05f);
    EXPECT_LT(bubble->GetLayoutX(), 0.0f) << "the bubble outlived the pin";
}

// A hidden source shows no bubble, and the bubble comes back when the source shows again.
TEST(TooltipPlacementTests, APinnedTooltipIsHiddenWhileItsSourceIsHidden)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kXml, kCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built);

    UIElement* source = fixture.Element("source");
    source->SetTooltip(kShortText);
    fixture.Manager().OnMouseMove(5.0f, 5.0f);
    fixture.Manager().PinTooltip(*source);
    PumpPinFrames(fixture);
    ASSERT_TRUE(BubbleShown(fixture)) << "the pin shows no bubble; the test is vacuous";

    UI::Layout::SetElementHidden(*source, true);
    PumpPinFrames(fixture);
    EXPECT_FALSE(BubbleShown(fixture)) << "the bubble stays on screen for a hidden source";

    UI::Layout::SetElementHidden(*source, false);
    PumpPinFrames(fixture);
    EXPECT_TRUE(BubbleShown(fixture)) << "the bubble did not come back when the source showed again";
}

// A dock tab swapped out of its Mount leaves the tree without a structure change: the bubble
// hides with it and comes back when the tab is active again.
TEST(TooltipPlacementTests, APinnedTooltipIsHiddenWhileItsDockTabIsSwappedOut)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kXml, kCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built);

    MountedTabs tabs = MountTabs(fixture);
    fixture.Manager().OnMouseMove(390.0f, 5.0f);
    fixture.Manager().PinTooltip(*tabs.Source);
    PumpPinFrames(fixture);
    ASSERT_TRUE(BubbleShown(fixture)) << "the pin shows no bubble; the test is vacuous";

    tabs.Host->SwapTargetForActivation(tabs.Second.get());
    PumpPinFrames(fixture);
    EXPECT_FALSE(BubbleShown(fixture)) << "the bubble stays on screen over the tab that replaced its source's";

    tabs.Host->SwapTargetForActivation(tabs.First.get());
    PumpPinFrames(fixture);
    EXPECT_TRUE(BubbleShown(fixture)) << "the bubble did not come back with its tab";
}

// The source is destroyed while its tab is swapped out, so no structure change reports it:
// the pin ends without reading the freed element, and hover tooltips work again.
TEST(TooltipPlacementTests, APinnedTooltipEndsWhenItsSourceIsDestroyed)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, kXml, kCss);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built);

    MountedTabs tabs = MountTabs(fixture);
    fixture.Manager().OnMouseMove(390.0f, 5.0f);
    fixture.Manager().PinTooltip(*tabs.Source);
    PumpPinFrames(fixture);
    ASSERT_TRUE(BubbleShown(fixture)) << "the pin shows no bubble; the test is vacuous";

    tabs.Host->SwapTargetForActivation(tabs.Second.get());
    tabs.First.reset();
    PumpPinFrames(fixture);
    EXPECT_FALSE(BubbleShown(fixture)) << "the bubble outlived its source";

    UIElement* hovered = fixture.Element("source");
    hovered->SetTooltip(kWideText);
    fixture.Manager().OnMouseMove(hovered->GetLayoutX() + 5.0f, hovered->GetLayoutY() + 5.0f);
    // Past the 0.5 s hover delay, then frames for the bubble to measure and place.
    for (int frame = 0; frame < 4; ++frame)
        fixture.PumpSeconds(0.3f);
    EXPECT_TRUE(BubbleShown(fixture)) << "the ended pin still holds the tooltip: a hover shows no bubble";
    const UIElement* text = fixture.Element("ui-tooltip-text");
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->GetTextContent(), kWideText);
}
