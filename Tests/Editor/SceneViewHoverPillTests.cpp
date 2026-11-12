// The hover pill of a Scene View pane: its place beside the pointer and at the bottom center, the 4 px it keeps from
// each label and each pane edge, the 1 px move that places it again, one pill per pane slot, and the pill hidden
// while the pointer is not in its pane or the measure tool is on.

#include <gtest/gtest.h>

#include "SceneView/SceneViewController.h"
#include "SceneView/SceneViewport/SceneViewHoverPill.h"
#include "SceneView/SceneViewport/SceneViewport.h"
#include "SceneView/SceneViewport/SceneViewportBinding.h"
#include "SceneView/SceneViewport/SceneViewportHost.h"
#include "SceneView/SceneViewport/SceneViewportMeasureSettings.h"
#include "Types/StringId.h"

#include "InspectorLayoutFixture.h"
#include "SceneViewportLeafStubs.h"

#include "UI/Internal/LayoutAccess.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIStyle.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace
{
using GameEngine::SceneViewHoverPill;
using GameEngine::SceneViewport;
using GameEngine::UIElement;
using GameEngine::UILayoutAccess;
using GameEngine::Mathematics::Vector2;
namespace Style = GameEngine::Style;

constexpr float kPaneWidth = 400.0f;
constexpr float kPaneHeight = 200.0f;
constexpr float kPillWidth = 80.0f;
constexpr float kPillHeight = 28.0f;
// The gap below the pill at the bottom center (SceneViewHoverPill.cpp).
constexpr float kBottomInsetPx = 14.0f;
// The smallest gap between the pill and a label or an edge of the pane (SceneViewHoverPill.cpp).
constexpr float kClearancePx = 4.0f;
constexpr float kLayoutTolerancePx = 0.01f;

const Vector2 kPaneSize(kPaneWidth, kPaneHeight);
const Vector2 kPillSize(kPillWidth, kPillHeight);
// The pane label at the bottom left of the pane: x 12 to 132, y 166 to 186.
const GameEngine::Mathematics::Rect kPaneLabel{12.0f, 166.0f, 120.0f, 20.0f};

GameEngine::SceneViewHoverPillLabels PaneLabelOnly()
{
    GameEngine::SceneViewHoverPillLabels labels;
    labels.PaneLabel = kPaneLabel;
    return labels;
}

GameEngine::SceneViewHoverPillLabels PaneAndHostLabels(const GameEngine::Mathematics::Rect& hostLabel)
{
    GameEngine::SceneViewHoverPillLabels labels = PaneLabelOnly();
    labels.HostLabel = hostLabel;
    return labels;
}

float StyleValue(SceneViewHoverPill& pill, GameEngine::StyleProp<GameEngine::StyleLength> property)
{
    const auto value = pill.Style().Get(property);
    return value ? value->Value : -1.0f;
}

float Opacity(SceneViewHoverPill& pill)
{
    return pill.Style().Get(Style::Opacity).value_or(-1.0f);
}

/// A controller for a binding: zeroed storage of the controller's size and alignment, never constructed. Its inline
/// reads give no active tool; its overlay text is the stub text (SceneViewportLeafStubs.h).
GameEngine::SceneViewController* ZeroedController()
{
    alignas(GameEngine::SceneViewController) static std::byte s_Storage[sizeof(GameEngine::SceneViewController)] = {};
    return reinterpret_cast<GameEngine::SceneViewController*>(s_Storage);
}

GameEngine::SceneViewportBinding BindingTo(GameEngine::SceneViewController* controller)
{
    GameEngine::SceneViewportBinding binding;
    binding.Controller = controller;
    return binding;
}

/// The pill of a "front" pane, shown beside a pointer.
SceneViewHoverPill* ShownPill(SceneViewport& pane)
{
    auto* pill = dynamic_cast<SceneViewHoverPill*>(pane.FindById(GameEngine::HashStringId("SceneViewHoverPillFront")));
    if (pill)
        pill->Show("Sphere", true, Vector2(10.0f, 10.0f), {});
    return pill;
}

/// A laid-out pane of 400 x 200 px with a laid-out pill of 80 x 28 px.
class SceneViewHoverPillTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto pill = std::make_unique<SceneViewHoverPill>();
        Pill = pill.get();
        Pane.AddChild(std::move(pill));
        UILayoutAccess::SetLastLayoutRect(Pane, 0.0f, 0.0f, kPaneWidth, kPaneHeight);
        UILayoutAccess::SetLastLayoutRect(*Pill, 0.0f, 0.0f, kPillWidth, kPillHeight);
    }

    UIElement Pane;
    SceneViewHoverPill* Pill = nullptr;
};

TEST(SceneViewHoverPillPlacementTests, ThePillSitsBelowAndRightOfThePointer)
{
    const Vector2 topLeft = SceneViewHoverPill::Place(true, Vector2(50.0f, 30.0f), kPaneSize, kPillSize, PaneLabelOnly());
    EXPECT_FLOAT_EQ(topLeft.x, 64.0f);
    EXPECT_FLOAT_EQ(topLeft.y, 48.0f);
}

TEST(SceneViewHoverPillPlacementTests, InTheLabelRowAwayFromEveryLabelThePillStaysBelowThePointer)
{
    // The pill below spans x 214 to 294 and y 168 to 196: right of the pane label, left of the host label.
    const Vector2 pointer(200.0f, 150.0f);
    const Vector2 topLeft = SceneViewHoverPill::Place(true, pointer, kPaneSize, kPillSize,
                                                      PaneAndHostLabels({300.0f, 160.0f, 88.0f, 26.0f}));
    EXPECT_FLOAT_EQ(topLeft.x, 214.0f);
    EXPECT_FLOAT_EQ(topLeft.y, pointer.y + 18.0f);
}

TEST(SceneViewHoverPillPlacementTests, WhenThePillBelowMeetsThePaneLabelItGoesAboveThePointer)
{
    const Vector2 pointer(60.0f, 150.0f);
    const Vector2 topLeft = SceneViewHoverPill::Place(true, pointer, kPaneSize, kPillSize, PaneLabelOnly());
    EXPECT_FLOAT_EQ(topLeft.x, 74.0f);
    EXPECT_FLOAT_EQ(topLeft.y, pointer.y - 18.0f - kPillHeight);
}

TEST(SceneViewHoverPillPlacementTests, WhenThePillAboveThePointerMeetsTheHostLabelItMovesAboveThatLabel)
{
    // Below leaves the pane; above the pointer (y 144 to 172) meets the host label at y 160.
    const GameEngine::Mathematics::Rect hostLabel{300.0f, 160.0f, 88.0f, 26.0f};
    const Vector2 topLeft =
        SceneViewHoverPill::Place(true, Vector2(300.0f, 190.0f), kPaneSize, kPillSize, PaneAndHostLabels(hostLabel));
    EXPECT_FLOAT_EQ(topLeft.x, 314.0f);
    EXPECT_FLOAT_EQ(topLeft.y, hostLabel.Y - kClearancePx - kPillHeight);
}

TEST(SceneViewHoverPillPlacementTests, AtTheBottomCenterAwayFromEveryLabelThePillStaysInTheLabelRow)
{
    // The pill spans x 160 to 240: right of the pane label (x 12 to 132), so it does not move up.
    const Vector2 topLeft = SceneViewHoverPill::Place(false, Vector2(), kPaneSize, kPillSize, PaneLabelOnly());
    EXPECT_FLOAT_EQ(topLeft.x, (kPaneWidth - kPillWidth) * 0.5f);
    EXPECT_FLOAT_EQ(topLeft.y, kPaneHeight - kBottomInsetPx - kPillHeight);
}

TEST(SceneViewHoverPillPlacementTests, AtTheBottomCenterThePillMovesAboveALabelItMeets)
{
    const GameEngine::Mathematics::Rect hostLabel{150.0f, 160.0f, 100.0f, 24.0f};
    const Vector2 topLeft =
        SceneViewHoverPill::Place(false, Vector2(), kPaneSize, kPillSize, PaneAndHostLabels(hostLabel));
    EXPECT_FLOAT_EQ(topLeft.x, (kPaneWidth - kPillWidth) * 0.5f);
    EXPECT_FLOAT_EQ(topLeft.y, hostLabel.Y - kClearancePx - kPillHeight);
}

TEST(SceneViewHoverPillPlacementTests, APillCloserThanTheGapToALabelMovesSoTheGapIsAMinimum)
{
    // Below the pointer the pill spans y 135 to 163: 3 px above the pane label at y 166.
    const Vector2 pointer(60.0f, 117.0f);
    const Vector2 topLeft = SceneViewHoverPill::Place(true, pointer, kPaneSize, kPillSize, PaneLabelOnly());
    EXPECT_FLOAT_EQ(topLeft.y, pointer.y - 18.0f - kPillHeight);

    // One pixel higher, the gap is exactly 4 px: the pill stays below the pointer.
    const Vector2 higher(60.0f, 116.0f);
    EXPECT_FLOAT_EQ(SceneViewHoverPill::Place(true, higher, kPaneSize, kPillSize, PaneLabelOnly()).y, higher.y + 18.0f);
}

TEST(SceneViewHoverPillPlacementTests, InTheBottomRightCornerThePillKeepsTheGapFromThePaneEdges)
{
    const Vector2 pointer(395.0f, 195.0f);
    const Vector2 topLeft = SceneViewHoverPill::Place(true, pointer, kPaneSize, kPillSize, {});
    EXPECT_FLOAT_EQ(topLeft.x, kPaneWidth - kPillWidth - kClearancePx);
    EXPECT_FLOAT_EQ(topLeft.y, pointer.y - 18.0f - kPillHeight);

    // In the top-left corner the pill below the pointer keeps the gap from the left edge.
    EXPECT_FLOAT_EQ(SceneViewHoverPill::Place(true, Vector2(-20.0f, 2.0f), kPaneSize, kPillSize, {}).x, kClearancePx);
}

TEST(SceneViewHoverPillPlacementTests, APillWiderThanThePaneStartsAtTheLeftGap)
{
    const Vector2 widePill(kPaneWidth + 40.0f, kPillHeight);
    EXPECT_FLOAT_EQ(SceneViewHoverPill::Place(false, Vector2(), kPaneSize, widePill, {}).x, kClearancePx);
    EXPECT_FLOAT_EQ(SceneViewHoverPill::Place(true, Vector2(200.0f, 50.0f), kPaneSize, widePill, {}).x, kClearancePx);
}

TEST_F(SceneViewHoverPillTest, ThePillMovesOnlyWhenThePointerMovesByOnePixelOrMore)
{
    Pill->Show("Sphere", true, Vector2(50.0f, 30.0f), PaneLabelOnly());
    EXPECT_FLOAT_EQ(Opacity(*Pill), 1.0f);
    EXPECT_FLOAT_EQ(StyleValue(*Pill, Style::PositionLeft), 64.0f);

    Pill->Show("Sphere", true, Vector2(50.6f, 30.6f), PaneLabelOnly());
    EXPECT_FLOAT_EQ(StyleValue(*Pill, Style::PositionLeft), 64.0f);
    EXPECT_FLOAT_EQ(StyleValue(*Pill, Style::PositionTop), 48.0f);

    Pill->Show("Sphere", true, Vector2(52.0f, 30.0f), PaneLabelOnly());
    EXPECT_FLOAT_EQ(StyleValue(*Pill, Style::PositionLeft), 66.0f);
}

TEST_F(SceneViewHoverPillTest, ThePillMovesWhenALabelBoxMoves)
{
    const Vector2 pointer(60.0f, 150.0f);
    Pill->Show("Sphere", true, pointer, {});
    EXPECT_FLOAT_EQ(StyleValue(*Pill, Style::PositionTop), pointer.y + 18.0f);

    Pill->Show("Sphere", true, pointer, PaneLabelOnly());
    EXPECT_FLOAT_EQ(StyleValue(*Pill, Style::PositionTop), pointer.y - 18.0f - kPillHeight);
}

// Laid out over the editor sheets and SceneViewport.css, so a stylesheet edge that stretches the pill shows here.
TEST(SceneViewHoverPillLayoutTests, WithoutThePointerPlacementThePillSitsAtTheBottomCenter)
{
    auto pane = std::make_unique<UIElement>();
    pane->Style()
        .Set(Style::Width, GameEngine::StyleLength::Px(kPaneWidth))
        .Set(Style::Height, GameEngine::StyleLength::Px(kPaneHeight));
    auto pillOwned = std::make_unique<SceneViewHoverPill>();
    pillOwned->AddClass("scene-view-hover-pill");
    SceneViewHoverPill* pill = pillOwned.get();
    UIElement* paneElement = pane.get();
    pane->AddChild(std::move(pillOwned));

    InspectorLayoutTesting::InspectorLayoutFixture fixture;
    fixture.ExtraSheets = {"Assets/UI/controls/SceneViewport/SceneViewport.css"};
    if (!fixture.Build(std::move(pane)))
        GTEST_SKIP() << fixture.Diagnostic;
    // The first Show sets the text; the next frames place the pill at its laid-out width.
    for (int frame = 0; frame < 3; ++frame)
    {
        pill->Show("Sphere", false, Vector2(50.0f, 30.0f), {});
        fixture.Ui->Update(1.0f / 60.0f, /*interactive=*/true);
    }

    const float paneBottom = paneElement->GetLayoutY() + paneElement->GetLayoutHeight();
    const float pillLeft = pill->GetLayoutX() - paneElement->GetLayoutX();
    ASSERT_GT(pill->GetLayoutWidth(), 0.0f) << "the pill did not lay out; this test is vacuous";
    EXPECT_NEAR(paneBottom - (pill->GetLayoutY() + pill->GetLayoutHeight()), kBottomInsetPx, kLayoutTolerancePx);
    EXPECT_GT(pill->GetLayoutY() - paneElement->GetLayoutY(), kPaneHeight * 0.5f) << "the pill stretches up the pane";
    EXPECT_NEAR(pillLeft, std::round((kPaneWidth - pill->GetLayoutWidth()) * 0.5f), 1.0f);

    pill->Hide();
    EXPECT_FLOAT_EQ(Opacity(*pill), 0.0f);
}

TEST(SceneViewHoverPillPaneTests, EverySlotBuildsItsOwnPill)
{
    constexpr std::string_view kSlots[] = {"perspective", "top", "front", "side"};
    constexpr std::string_view kIds[] = {"SceneViewHoverPill", "SceneViewHoverPillTop", "SceneViewHoverPillFront",
                                         "SceneViewHoverPillSide"};
    for (size_t index = 0; index < std::size(kSlots); ++index)
    {
        SceneViewport pane;
        pane.SetSlot(std::string(kSlots[index]));
        EXPECT_NE(dynamic_cast<SceneViewHoverPill*>(pane.FindById(GameEngine::HashStringId(kIds[index]))), nullptr)
            << kSlots[index];
    }
}

/// A bound pane whose controller has overlay text, so the pane shows a pill unless a hide rule applies.
class SceneViewHoverPillBoundPaneTest : public ::testing::Test
{
protected:
    void SetUp() override { GameEngine::Editor::TestStubs::OverlayHoverText() = "Junction"; }
    void TearDown() override { GameEngine::Editor::TestStubs::OverlayHoverText().clear(); }
};

TEST_F(SceneViewHoverPillBoundPaneTest, WithoutThePointerThePaneHidesItsPill)
{
    SceneViewport pane;
    pane.SetSlot("front");
    pane.Bind(BindingTo(ZeroedController()));
    SceneViewHoverPill* pill = ShownPill(pane);
    ASSERT_NE(pill, nullptr);
    ASSERT_FLOAT_EQ(Opacity(*pill), 1.0f);

    pane.UpdateHoverPill();
    EXPECT_FLOAT_EQ(Opacity(*pill), 0.0f);
}

TEST_F(SceneViewHoverPillBoundPaneTest, WithThePointerInsideThePaneHidesItsPillWhileTheMeasureToolIsOn)
{
    SceneViewport pane;
    pane.SetSlot("front");
    UILayoutAccess::SetLastLayoutRect(pane, 0.0f, 0.0f, kPaneWidth, kPaneHeight);
    GameEngine::UIEvent move;
    move.Id = GameEngine::kEventMouseMove;
    move.X = 50.0f;
    move.Y = 30.0f;
    // The move goes in before the bind: a bound pane passes a move to its controller.
    pane.DispatchEvent(move);
    ASSERT_TRUE(pane.IsPointerInside());
    pane.Bind(BindingTo(ZeroedController()));
    GameEngine::SceneViewportMeasureSettings measure;
    measure.Enabled = true;
    pane.SetMeasureSettings(measure);
    SceneViewHoverPill* pill = ShownPill(pane);
    ASSERT_NE(pill, nullptr);
    ASSERT_FLOAT_EQ(Opacity(*pill), 1.0f);

    pane.UpdateHoverPill();
    EXPECT_FLOAT_EQ(Opacity(*pill), 0.0f);
}

/// A host that shows `Label` as its own label; its other actions do nothing.
class HostWithLabel final : public GameEngine::SceneViewportHost
{
public:
    void ActivateViewport(SceneViewport&) override {}
    void HoverViewport(SceneViewport&, bool) override {}
    bool IsPanHoldDown() const override { return false; }
    void MarkPanHoldDragged() override {}
    bool HandleViewportKey(SceneViewport&, GameEngine::UIEvent&, bool) override { return false; }
    bool ShowViewportContextMenu(const Vector2&) override { return false; }
    void ExitMeasureTool(GameEngine::SceneViewController&) override {}
    bool IsSplitView() const override { return true; }
    const GameEngine::UIElement* GetHostLabel() const override { return Label; }
    void NotifyRotationGizmoOrbit(SceneViewport&) override {}
    void NotifyRotationGizmoAxisSnap(SceneViewport&, int) override {}

    GameEngine::UIElement* Label = nullptr;
};

TEST_F(SceneViewHoverPillBoundPaneTest, ThePaneMovesItsPillAboveTheHostLabelOverThePane)
{
    // The pane sits at (100, 50) in the window; the host label's box is in window pixels.
    constexpr float kPaneX = 100.0f;
    constexpr float kPaneY = 50.0f;
    const GameEngine::Mathematics::Rect hostLabelInPane{300.0f, 160.0f, 88.0f, 26.0f};
    SceneViewport pane;
    pane.SetSlot("front");
    UILayoutAccess::SetLastLayoutRect(pane, kPaneX, kPaneY, kPaneWidth, kPaneHeight);
    HostWithLabel host;
    auto label = std::make_unique<GameEngine::UIElement>();
    host.Label = label.get();
    pane.AddChild(std::move(label));
    UILayoutAccess::SetLastLayoutRect(*host.Label, kPaneX + hostLabelInPane.X, kPaneY + hostLabelInPane.Y,
                                      hostLabelInPane.Width, hostLabelInPane.Height);
    auto* pill = dynamic_cast<SceneViewHoverPill*>(pane.FindById(GameEngine::HashStringId("SceneViewHoverPillFront")));
    ASSERT_NE(pill, nullptr);
    UILayoutAccess::SetLastLayoutRect(*pill, kPaneX, kPaneY, kPillWidth, kPillHeight);
    // The pointer goes in before the bind: a bound pane passes a move to its controller.
    GameEngine::UIEvent move;
    move.Id = GameEngine::kEventMouseMove;
    move.X = kPaneX + 300.0f;
    move.Y = kPaneY + 190.0f;
    pane.DispatchEvent(move);
    ASSERT_TRUE(pane.IsPointerInside());
    GameEngine::SceneViewportBinding binding = BindingTo(ZeroedController());
    binding.Host = &host;
    pane.Bind(binding);

    // Below the pointer leaves the pane; above it (y 144 to 172 in the pane) meets the host label.
    pane.UpdateHoverPill();
    EXPECT_FLOAT_EQ(Opacity(*pill), 1.0f);
    EXPECT_FLOAT_EQ(StyleValue(*pill, Style::PositionLeft), 314.0f);
    EXPECT_FLOAT_EQ(StyleValue(*pill, Style::PositionTop), hostLabelInPane.Y - kClearancePx - kPillHeight);
}

} // namespace
