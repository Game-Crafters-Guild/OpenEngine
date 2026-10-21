// The record-then-apply contract of the Windows web view, against a fake target: what reaches the
// native view, and in which order, before and after it exists. The native view's own geometry is
// a live check; nothing here creates a window.

#include "WebViewState.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using GameEngine::Platform::IWebViewTarget;
using GameEngine::Platform::WebViewBounds;
using GameEngine::Platform::WebViewColor;
using GameEngine::Platform::WebViewState;

namespace
{

class RecordingTarget : public IWebViewTarget
{
public:
    std::vector<std::string> Calls;

    void ApplyBounds(const WebViewBounds& b) override
    {
        Calls.push_back("bounds " + std::to_string(b.Left) + "," + std::to_string(b.Top) + "," +
                        std::to_string(b.Right) + "," + std::to_string(b.Bottom));
    }
    void ApplyVisible(bool visible) override { Calls.push_back(visible ? "show" : "hide"); }
    void ApplyBackgroundColor(const WebViewColor& c) override
    {
        Calls.push_back("color " + std::to_string(c.R) + "," + std::to_string(c.G) + "," + std::to_string(c.B));
    }
    bool NavigateToURL(const std::string& url) override
    {
        Calls.push_back("url " + url);
        return true;
    }
    bool NavigateToHTML(const std::string& html) override
    {
        Calls.push_back("html " + html);
        return true;
    }
};

using Calls = std::vector<std::string>;

} // namespace

TEST(WebViewState, AppliesRecordedStateInOrderOnAttach)
{
    WebViewState state;
    RecordingTarget target;
    EXPECT_TRUE(state.LoadURL("https://example.com/"));
    state.SetVisible(true);
    state.SetPosition(10, 20);
    state.Resize(300, 200);
    state.SetBackgroundColor({39, 39, 39});

    state.Attach(target);

    EXPECT_EQ(target.Calls, (Calls{"color 39,39,39", "bounds 10,20,310,220", "show", "url https://example.com/"}));
}

TEST(WebViewState, LastPageBeforeAttachWins)
{
    WebViewState urlLast;
    RecordingTarget urlTarget;
    urlLast.LoadHTML("<p>placeholder</p>");
    urlLast.LoadURL("https://example.com/");
    urlLast.Attach(urlTarget);
    EXPECT_EQ(urlTarget.Calls, (Calls{"url https://example.com/"}));

    WebViewState htmlLast;
    RecordingTarget htmlTarget;
    htmlLast.LoadURL("https://example.com/");
    htmlLast.LoadHTML("<p>placeholder</p>");
    htmlLast.Attach(htmlTarget);
    EXPECT_EQ(htmlTarget.Calls, (Calls{"html <p>placeholder</p>"}));
}

TEST(WebViewState, VisibilityBeforeAttachIsTheLastRequested)
{
    WebViewState state;
    RecordingTarget target;
    state.SetVisible(false);
    state.SetVisible(true);
    EXPECT_TRUE(state.IsVisible());

    state.Attach(target);

    EXPECT_EQ(target.Calls, (Calls{"show"}));
}

// The panel sends Resize then SetPosition every frame: the pair reaches the view as one rect at
// the new origin, and a repeat of the same pair reaches it not at all.
TEST(WebViewState, ResizeKeepsOriginAndUnchangedRectIsNotSent)
{
    WebViewState state;
    RecordingTarget target;
    state.Attach(target);
    state.Resize(300, 200);
    state.SetPosition(10, 20);
    target.Calls.clear();

    state.Resize(300, 200);
    state.SetPosition(10, 20);
    EXPECT_TRUE(target.Calls.empty());

    state.Resize(400, 200);
    EXPECT_EQ(target.Calls, (Calls{"bounds 10,20,410,220"}));
}

TEST(WebViewState, UnchangedVisibilityIsNotSent)
{
    WebViewState state;
    RecordingTarget target;
    state.Attach(target);
    state.SetVisible(true);
    state.SetVisible(true);
    state.SetVisible(false);
    state.SetVisible(false);

    EXPECT_EQ(target.Calls, (Calls{"show", "hide"}));
}

TEST(WebViewState, DetachedTargetReceivesNothing)
{
    WebViewState state;
    RecordingTarget target;
    state.Attach(target);
    state.Detach();

    state.Resize(300, 200);
    state.SetVisible(true);
    state.SetBackgroundColor({1, 2, 3});
    state.LoadURL("https://example.com/");

    EXPECT_TRUE(target.Calls.empty());
}
