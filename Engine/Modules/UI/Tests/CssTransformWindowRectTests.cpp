// UIElement::GetWindowRect and WindowToLayout: an element's layout rect and
// layout-space points carried to and from the window through every CSS
// transform that applies to it. Standalone elements, no device.

#include <gtest/gtest.h>

#include <memory>

#include "Mathematics/Rect.h"
#include "Mathematics/Vector2.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIElement.h"

using namespace GameEngine;

namespace
{

constexpr float kTolerance = 1e-3f;

// A standalone element at a layout rect, with `transform` set on its resolved
// style the way the cascade would.
std::unique_ptr<UIElement> MakeBox(float x, float y, float w, float h, const TransformValue& transform = {})
{
    auto el = std::make_unique<UIElement>();
    UILayoutAccess::SetLastLayoutRect(*el, x, y, w, h);
    el->GetMutableResolvedStyle().Visual.Transform = transform;
    return el;
}

void ExpectRect(const Mathematics::Rect& r, float x, float y, float w, float h, const char* what)
{
    EXPECT_NEAR(r.X, x, kTolerance) << what;
    EXPECT_NEAR(r.Y, y, kTolerance) << what;
    EXPECT_NEAR(r.Width, w, kTolerance) << what;
    EXPECT_NEAR(r.Height, h, kTolerance) << what;
}

} // namespace

// A 100x40 box at (100, 100) turned a quarter about its centre (150, 120) is
// drawn as a 40x100 box about the same centre; its child, scaled 2x about its
// own top-left corner, is drawn twice its size and turned with its parent.
TEST(CssTransformWindowRect, TheWindowRectBoundsTheDrawnBox)
{
    auto parentOwned = MakeBox(100, 100, 100, 40, TransformValue{}.Rotate(90.0f));
    UIElement* parent = parentOwned.get();
    auto childOwned = MakeBox(110, 110, 20, 10, TransformValue{}.Scale(2.0f, 2.0f));
    childOwned->GetMutableResolvedStyle().Visual.TransformOrigin = {StyleLength::Px(0.0f), StyleLength::Px(0.0f)};
    UIElement* child = childOwned.get();
    parent->AddChild(std::move(childOwned));

    ExpectRect(parent->GetWindowRect(), 130, 70, 40, 100, "the parent's own transform applies");
    // The child's drawn box in the parent's space is (110, 110)-(150, 130);
    // the quarter turn about (150, 120) takes (x, y) to (270 - y, x - 30).
    ExpectRect(child->GetWindowRect(), 140, 80, 20, 40, "both transforms apply, innermost first");

    auto plain = MakeBox(5, 6, 7, 8);
    ExpectRect(plain->GetWindowRect(), 5, 6, 7, 8, "no transform: the layout rect");
}

TEST(CssTransformWindowRect, WindowToLayoutUndoesEveryTransform)
{
    auto parentOwned = MakeBox(100, 100, 100, 40, TransformValue{}.Rotate(90.0f));
    UIElement* parent = parentOwned.get();
    auto childOwned = MakeBox(110, 110, 20, 10, TransformValue{}.Scale(2.0f, 2.0f));
    childOwned->GetMutableResolvedStyle().Visual.TransformOrigin = {StyleLength::Px(0.0f), StyleLength::Px(0.0f)};
    UIElement* child = childOwned.get();
    parent->AddChild(std::move(childOwned));

    // The child's layout point (115, 112) is (120, 114) in the parent's space
    // and (156, 90) in the window.
    Mathematics::Vector2 local;
    ASSERT_TRUE(child->WindowToLayout(Mathematics::Vector2(156.0f, 90.0f), local));
    EXPECT_NEAR(local.x, 115.0f, kTolerance);
    EXPECT_NEAR(local.y, 112.0f, kTolerance);
    ASSERT_TRUE(parent->WindowToLayout(Mathematics::Vector2(156.0f, 90.0f), local));
    EXPECT_NEAR(local.x, 120.0f, kTolerance);
    EXPECT_NEAR(local.y, 114.0f, kTolerance);

    // A transform that collapses the box (scale(0)) draws nothing, so no
    // window point lies on it.
    parent->GetMutableResolvedStyle().Visual.Transform = TransformValue{}.Scale(0.0f, 1.0f);
    EXPECT_FALSE(child->WindowToLayout(Mathematics::Vector2(156.0f, 90.0f), local));
}

// An overlay (a dropdown's popup, a tooltip) escapes its ancestors' clips but
// paints in their transform: a popup under a header scaled 2x about (100, 100)
// is drawn at 2x under it, as on the web.
TEST(CssTransformWindowRect, AnOverlayPaintsInItsOwnersTransform)
{
    auto parentOwned = MakeBox(100, 100, 100, 40, TransformValue{}.Scale(2.0f, 2.0f));
    parentOwned->GetMutableResolvedStyle().Visual.TransformOrigin = {StyleLength::Px(0.0f), StyleLength::Px(0.0f)};
    UIElement* parent = parentOwned.get();
    auto overlayOwned = MakeBox(100, 140, 100, 60);
    overlayOwned->SetOverlayLayer(OverlayLayer::Dropdown);
    UIElement* overlay = overlayOwned.get();
    auto rowOwned = MakeBox(100, 150, 100, 20);
    UIElement* row = rowOwned.get();
    overlay->AddChild(std::move(rowOwned));
    parent->AddChild(std::move(overlayOwned));

    ExpectRect(overlay->GetWindowRect(), 100, 180, 200, 120, "the overlay itself");
    ExpectRect(row->GetWindowRect(), 100, 200, 200, 40, "and what is inside it");
    Mathematics::Vector2 local;
    ASSERT_TRUE(row->WindowToLayout(Mathematics::Vector2(120.0f, 220.0f), local));
    EXPECT_NEAR(local.x, 110.0f, kTolerance);
    EXPECT_NEAR(local.y, 160.0f, kTolerance);
}

