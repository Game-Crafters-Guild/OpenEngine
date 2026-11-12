// The mark-up pills as the editor ships them, read from the staged sheets: the pill's unseen dot
// and the Mark-ups panel row's dot are one look, a filled dot in the same color, and
// UIUtil::SetShown hides a pill and its dot over their own `display` rules.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/StyleUtil.h"
#include "UI/UIElement.h"
#include "UI/UIStyle.h"

#include "Core/ApplicationPaths.h"

#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using GameEngine::ApplicationPaths;
using GameEngine::DisplayMode;
using GameEngine::UIElement;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UIUtil::SetShown;

namespace
{

std::string ReadShippedCss(const std::string& relative)
{
    std::ifstream in(ApplicationPaths::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="pill" class="markup-label-unread"/>
  <uielement id="row" class="markups-update-dot"/>
</uielement>)";

constexpr char kGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 200px; height: 100px; }
)";

constexpr char kPillXml[] = R"(<uielement id="root">
  <uielement id="anchor" class="markup-label-anchor">
    <uielement id="pill" class="markup-label">
      <uielement id="unread" class="markup-label-unread"/>
    </uielement>
  </uielement>
</uielement>)";

constexpr char kPillGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 100px; }
)";

DisplayMode DisplayOf(const IsolatedUIFixture& fx, const std::string& id)
{
    return fx.Style(id)->Layout.DisplayMode;
}

} // namespace

TEST(ShippedMarkupUnseenDotTests, ThePillsUnseenDotIsTheRowsDotFilled)
{
    std::string css;
    for (const char* sheet : {"theme/tokens.css", "controls/Markups/MarkupLabelOverlay.css", "panels/MarkupsPanel.css"})
    {
        const std::string text = ReadShippedCss(sheet);
        ASSERT_FALSE(text.empty()) << "shipped stylesheet not staged next to the test exe: " << sheet;
        css += text;
        css += '\n';
    }
    css += kGeometryCss;

    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kXml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }
    ASSERT_NE(fx.Style("pill"), nullptr);
    ASSERT_NE(fx.Style("row"), nullptr);
    EXPECT_EQ(fx.Style("pill")->Visual.BackgroundColor >> 24, 0xFFu) << "the pill's dot is filled";
    EXPECT_EQ(fx.Style("pill")->Visual.BackgroundColor, fx.Style("row")->Visual.BackgroundColor);
}

TEST(ShippedMarkupUnseenDotTests, SetShownHidesAndShowsThePillOverItsOwnDisplayRule)
{
    std::string css;
    for (const char* sheet : {"theme/tokens.css", "theme/core.css", "controls/Markups/MarkupLabelOverlay.css"})
    {
        const std::string text = ReadShippedCss(sheet);
        ASSERT_FALSE(text.empty()) << "shipped stylesheet not staged next to the test exe: " << sheet;
        css += text;
        css += '\n';
    }
    css += kPillGeometryCss;

    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kPillXml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }
    UIElement* anchor = fx.Element("anchor");
    UIElement* unread = fx.Element("unread");
    ASSERT_NE(anchor, nullptr);
    ASSERT_NE(unread, nullptr);
    ASSERT_EQ(DisplayOf(fx, "anchor"), DisplayMode::Flex) << "MarkupLabelOverlay.css gives the anchor display: flex";

    // The overlay hides the dot inside a shown pill, so the test reads the dot before it hides the anchor.
    SetShown(*unread, false);
    fx.Settle();
    EXPECT_EQ(DisplayOf(fx, "unread"), DisplayMode::None);

    SetShown(*anchor, false);
    fx.Settle();
    EXPECT_EQ(DisplayOf(fx, "anchor"), DisplayMode::None);

    SetShown(*anchor, true);
    SetShown(*unread, true);
    fx.Settle();
    EXPECT_EQ(DisplayOf(fx, "anchor"), DisplayMode::Flex);
    EXPECT_NE(DisplayOf(fx, "unread"), DisplayMode::None);
}
