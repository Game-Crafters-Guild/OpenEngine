// The transient notice: a confirmation that shows on Show and hides on its own once its duration
// has passed after the last Show. The host drives the clock through Tick, so the tests pass the
// time in and need no UIManager and no frame loop.

#include <gtest/gtest.h>

#include "UI/Controls/Label.h"
#include "UI/Controls/TransientNotice.h"
#include "UI/UIElement.h"

#include <chrono>
#include <memory>
#include <string>

using GameEngine::Label;
using GameEngine::UIElement;
using GameEngine::EditorUI::TransientNotice;

namespace
{

using Clock = std::chrono::steady_clock;
constexpr const char* kShownClass = "transient-notice--shown";

Label* FindText(UIElement& root)
{
    for (const std::unique_ptr<UIElement>& child : root.GetChildren())
    {
        if (child->HasClass("transient-notice-text"))
            return dynamic_cast<Label*>(child.get());
        if (Label* found = FindText(*child))
            return found;
    }
    return nullptr;
}

} // namespace

TEST(TransientNotice, ShowPutsTheTextOnTheNoticeAndShowsIt)
{
    TransientNotice notice;
    ASSERT_NE(FindText(notice), nullptr) << "the layout did not build the notice's text";
    EXPECT_FALSE(notice.HasClass(kShownClass));

    notice.Show("Copied 1 line", Clock::now());

    EXPECT_TRUE(notice.HasClass(kShownClass));
    EXPECT_EQ(FindText(notice)->GetText(), "Copied 1 line");
}

TEST(TransientNotice, HidesOnceItsDurationHasPassed)
{
    TransientNotice notice;
    const Clock::time_point start = Clock::now();
    notice.Show("Copied", start);

    notice.Tick(start + TransientNotice::kShownDuration - std::chrono::milliseconds(1));
    EXPECT_TRUE(notice.HasClass(kShownClass));

    notice.Tick(start + TransientNotice::kShownDuration);
    EXPECT_FALSE(notice.HasClass(kShownClass));
}

TEST(TransientNotice, ASecondShowReplacesTheTextAndRestartsTheDuration)
{
    TransientNotice notice;
    const Clock::time_point start = Clock::now();
    notice.Show("Copied 1 line", start);
    const Clock::time_point second = start + TransientNotice::kShownDuration / 2;
    notice.Show("Copied 3 lines", second);

    notice.Tick(start + TransientNotice::kShownDuration);
    EXPECT_TRUE(notice.HasClass(kShownClass)) << "the first Show's deadline hid a notice shown again since";
    EXPECT_EQ(FindText(notice)->GetText(), "Copied 3 lines");

    notice.Tick(second + TransientNotice::kShownDuration);
    EXPECT_FALSE(notice.HasClass(kShownClass));
}
