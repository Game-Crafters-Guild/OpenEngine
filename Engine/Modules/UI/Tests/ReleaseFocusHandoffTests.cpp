// A click handler that claims focus for another element during the release
// dispatch keeps that focus. The release edge's default focus assignment must
// not hand it back to the control that was pressed.
//
// The editor's top-toolbar Universal Search button is the production shape:
// its click handler opens the search dialog, which focuses its query field.

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>

#include <string>

using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <button id="opener"/>
  <button id="field"/>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 40px; }
#opener { width: 120px; height: 40px; }
#field { width: 120px; height: 40px; }
)";

} // namespace

TEST(ReleaseFocusHandoff, ClickHandlerFocusSurvivesTheReleaseEdge)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    GameEngine::UIElement* opener = fx.Element("opener");
    ASSERT_NE(opener, nullptr);
    opener->RegisterEventHandler(GameEngine::kEventButtonClick, [&fx](GameEngine::UIEvent&) {
        fx.Manager().SetFocusById("field");
    });

    ASSERT_TRUE(fx.FocusViaClick("opener"));
    EXPECT_EQ(fx.Manager().GetFocusedElementId(), "field")
        << "the release edge handed focus back to the pressed button";
}

// A drag held by a non-focusable capturer (a splitter, a scrollbar thumb) ends without moving
// focus, wherever the pointer is: the release does not focus the field it ends over, even when
// the pointer drifted after the last frame and the release hit-tests again.
TEST(ReleaseFocusHandoff, ANonFocusableCaptureReleasedOverAFieldFocusesNothing)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, R"(<uielement id="root">
  <uielement id="thumb"/>
  <textfield id="field"/>
</uielement>)", R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 40px; }
#thumb { width: 120px; height: 20px; }
#field { width: 120px; height: 40px; }
)");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    GameEngine::UIElement* thumb = fx.Element("thumb");
    GameEngine::UIElement* field = fx.Element("field");
    ASSERT_NE(thumb, nullptr);
    ASSERT_NE(field, nullptr);
    thumb->RegisterEventHandler(GameEngine::kEventMouseDown, [thumb](GameEngine::UIEvent& e) { e.Capture(thumb); });

    fx.Manager().OnMouseMove(thumb->GetLayoutX() + 5.0f, thumb->GetLayoutY() + 5.0f);
    fx.StepFrame();
    fx.Manager().OnMouseButton(0, true);
    fx.StepFrame();
    ASSERT_TRUE(fx.Manager().IsMouseCaptured());
    const float x = field->GetLayoutX() + 5.0f;
    const float y = field->GetLayoutY() + 5.0f;
    fx.Manager().OnMouseMove(x, y);
    fx.StepFrame();
    fx.Manager().OnMouseMove(x + 1.0f, y);
    fx.Manager().OnMouseButton(0, false);
    fx.StepFrame();

    EXPECT_FALSE(field->IsFocusTargetForId(fx.Manager().GetFocusedElementId()))
        << "the release of the thumb's drag focused the field under the pointer";
}
