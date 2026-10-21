// A percentage min-width on a flex child resolves against its containing block
// on every layout, not only on the first one (#3765).
//
// The editor's inspector section header gives its title `flex: 0 1 auto` and
// `min-width: 37%` with short measured text, so the title's width is the
// percentage. Narrowing the inspector by dragging its dock edge changes the
// width of the header row without touching the title's own style; the title
// must then be 37% of the new row width, the same number a tree built at the
// narrow width gives. A percentage min-height follows a shortened owner the
// same way.

#include "IsolatedUIFixture.h"

#include <gtest/gtest.h>

using GameEngine::UIElement;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr float kExactPx = 0.01f;

// The panel stands in for the inspector's dock area and the row for a section
// header: the row's width comes from the panel through stretch, never from its
// own style, which is how a dock drag reaches it.
constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="panel">
    <uielement id="row">
      <label id="title">Light</label>
      <uielement id="badge"/>
    </uielement>
  </uielement>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
#panel { display: flex; flex-direction: column; width: 700px; }
#panel.narrow { width: 400px; }
#row { display: flex; flex-direction: row; height: 20px; }
#title { font-family: Roboto; font-size: 13px; flex: 0 1 auto; width: auto; min-width: 37%; white-space: nowrap; overflow: hidden; }
#badge { width: 40px; height: 16px; flex-shrink: 0; }
)";

// The same tree built at the narrow width: the answer the narrowed tree must match.
constexpr char kNarrowXml[] = R"(<uielement id="root">
  <uielement id="panel" class="narrow">
    <uielement id="row">
      <label id="title">Light</label>
      <uielement id="badge"/>
    </uielement>
  </uielement>
</uielement>)";

// A column whose height comes from its owner through flex-grow. The wrapper
// matters: a percentage min-height on an item whose own container's height is
// set directly resolves against the grandparent on the first layout (upstream
// yoga #1903), which would hide the shrink.
constexpr char kColumnXml[] = R"(<uielement id="root">
  <uielement id="outer">
    <uielement id="panel">
      <label id="title">Light</label>
      <uielement id="badge"/>
    </uielement>
  </uielement>
</uielement>)";

constexpr char kColumnCss[] = R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 800px; height: 600px; }
#outer { display: flex; flex-direction: column; width: 200px; height: 300px; }
#outer.short { height: 150px; }
#panel { display: flex; flex-direction: column; flex-grow: 1; }
#title { font-family: Roboto; font-size: 13px; flex: 0 1 auto; height: auto; min-height: 37%; white-space: nowrap; overflow: hidden; }
#badge { width: 40px; height: 16px; flex-shrink: 0; }
)";

constexpr float kTallPanelPx = 300.0f;
constexpr float kShortPanelPx = 150.0f;

constexpr float kWidePanelPx = 700.0f;
constexpr float kNarrowPanelPx = 400.0f;
constexpr float kMinFraction = 0.37f;

} // namespace

TEST(PercentMinWidthShrink, FlexChildMinWidthFollowsANarrowedOwner)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIElement* panel = fx.Element("panel");
    const UIElement* title = fx.Element("title");
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(title, nullptr);
    ASSERT_NEAR(fx.Element("row")->GetLayoutWidth(), kWidePanelPx, kExactPx);
    ASSERT_NEAR(title->GetLayoutWidth(), kWidePanelPx * kMinFraction, kExactPx)
        << "the short title should sit at its min-width on the wide panel";

    panel->AddClass("narrow");
    fx.Settle();

    ASSERT_NEAR(fx.Element("row")->GetLayoutWidth(), kNarrowPanelPx, kExactPx);
    EXPECT_NEAR(title->GetLayoutWidth(), kNarrowPanelPx * kMinFraction, kExactPx)
        << "the title kept a min-width resolved against the wide panel";
    EXPECT_NEAR(fx.Element("badge")->GetLayoutX(), kNarrowPanelPx * kMinFraction, kExactPx);

    IsolatedUIFixture fresh;
    ASSERT_TRUE(fresh.Build(1.0f, kNarrowXml, kCss)) << fresh.Diagnostic();
    EXPECT_NEAR(title->GetLayoutWidth(), fresh.Element("title")->GetLayoutWidth(), kExactPx);
    EXPECT_NEAR(fx.Element("badge")->GetLayoutX(), fresh.Element("badge")->GetLayoutX(), kExactPx);
}

TEST(PercentMinWidthShrink, FlexChildMinHeightFollowsAShortenedOwner)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kColumnXml, kColumnCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIElement* outer = fx.Element("outer");
    const UIElement* panel = fx.Element("panel");
    const UIElement* title = fx.Element("title");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(title, nullptr);
    ASSERT_NEAR(title->GetLayoutHeight(), kTallPanelPx * kMinFraction, kExactPx)
        << "the one-line title should sit at its min-height in the tall panel";

    outer->AddClass("short");
    fx.Settle();

    ASSERT_NEAR(panel->GetLayoutHeight(), kShortPanelPx, kExactPx);
    EXPECT_NEAR(title->GetLayoutHeight(), kShortPanelPx * kMinFraction, kExactPx)
        << "the title kept a min-height resolved against the tall panel";
    EXPECT_NEAR(fx.Element("badge")->GetLayoutY() - panel->GetLayoutY(), kShortPanelPx * kMinFraction,
                kExactPx);
}
