// One Scene View pane: its place in the live list while it is bound, the slot ids of the children its layout
// builds, its overlays bound to its controller, the hover actions it sends to its host, the pointer inside its
// bounds, the end of its camera drag, the host label over it, and no activation while another pane drags.

#include <gtest/gtest.h>

#include "SceneView/SceneViewController.h"
#include "SceneView/SceneViewport/SceneViewport.h"
#include "SceneView/SceneViewport/SceneViewportBinding.h"
#include "SceneView/SceneViewport/SceneViewportHost.h"
#include "SceneView/SceneViewport/SceneViewportMeasureSettings.h"
#include "SceneView/SceneViewport/SceneViewportRegistry.h"
#include "Types/StringId.h"
#include "UI/Controls/Widgets/SceneView/SceneViewMeasureOverlay.h"
#include "UI/Controls/Widgets/SceneView/SceneViewRulerOverlay.h"
#include "UI/Controls/Widgets/ViewportRotationGizmo.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIEvents.h"

#include "SceneViewportLeafStubs.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using GameEngine::SceneViewController;
using GameEngine::SceneViewport;
using GameEngine::SceneViewportBinding;
using GameEngine::SceneViewportHost;
using GameEngine::SceneViewportRegistry;
using GameEngine::UIEvent;

bool IsLive(const SceneViewport& pane)
{
    const auto live = SceneViewportRegistry::GetLive();
    return std::find(live.begin(), live.end(), &pane) != live.end();
}

/// Three controllers for bindings: zeroed storage of the controller's size and alignment, never constructed. The
/// stub overlays store them; the pane reads only their inline members, which read as no tool and no mode.
SceneViewController* ZeroedController(std::size_t index = 0)
{
    alignas(SceneViewController) static std::byte s_Storage[3][sizeof(SceneViewController)] = {};
    return reinterpret_cast<SceneViewController*>(s_Storage[index]);
}

SceneViewportBinding BindingTo(SceneViewController* controller)
{
    SceneViewportBinding binding;
    binding.Controller = controller;
    return binding;
}

// The slots in the order of their slot index, and the id suffixes each slot gives the pane's children.
constexpr std::string_view kSlots[] = {"perspective", "top", "front", "side"};
constexpr std::string_view kIdSuffixes[] = {"", "Top", "Front", "Side"};
constexpr std::string_view kLabelIdSuffixes[] = {"Perspective", "Top", "Front", "Side"};

std::string SlotId(std::string_view baseId, std::size_t slotIndex)
{
    return std::string(baseId) + std::string(kIdSuffixes[slotIndex]);
}

struct HoverCall
{
    SceneViewport* Pane = nullptr;
    bool Inside = false;
};

/// Records the activations and the hover actions and shows `Label` as its own label; the other actions are not
/// part of these tests.
class RecordingHost final : public SceneViewportHost
{
public:
    void ActivateViewport(SceneViewport& pane) override { Activated.push_back(&pane); }
    void HoverViewport(SceneViewport& pane, bool inside) override { HoverCalls.push_back({&pane, inside}); }
    bool IsPanHoldDown() const override { return false; }
    void MarkPanHoldDragged() override {}
    bool HandleViewportKey(SceneViewport&, UIEvent&, bool) override { return false; }
    bool ShowViewportContextMenu(const GameEngine::Mathematics::Vector2&) override { return false; }
    void ExitMeasureTool(SceneViewController&) override {}
    bool IsSplitView() const override { return false; }
    const GameEngine::UIElement* GetHostLabel() const override { return Label; }
    void NotifyRotationGizmoOrbit(SceneViewport&) override {}
    void NotifyRotationGizmoAxisSnap(SceneViewport&, int) override {}

    std::vector<SceneViewport*> Activated;
    std::vector<HoverCall> HoverCalls;
    const GameEngine::UIElement* Label = nullptr;
};

/// Adds a label of `box` (window pixels) to `parent` and returns it.
GameEngine::UIElement& AddLabel(GameEngine::UIElement& parent, const GameEngine::Mathematics::Rect& box)
{
    auto label = std::make_unique<GameEngine::UIElement>();
    GameEngine::UIElement& added = *label;
    parent.AddChild(std::move(label));
    GameEngine::UILayoutAccess::SetLastLayoutRect(added, box.X, box.Y, box.Width, box.Height);
    return added;
}

SceneViewportBinding HostBinding(RecordingHost& host, std::size_t controllerIndex)
{
    SceneViewportBinding binding = BindingTo(ZeroedController(controllerIndex));
    binding.Host = &host;
    return binding;
}

UIEvent MouseMoveAt(float x, float y)
{
    UIEvent move;
    move.Id = GameEngine::kEventMouseMove;
    move.X = x;
    move.Y = y;
    return move;
}

bool HasChildWithId(SceneViewport& pane, std::string_view id)
{
    return pane.FindById(GameEngine::HashStringId(id)) != nullptr;
}

TEST(SceneViewportTests, APaneIsLiveWhileBoundAndLeavesTheListWhenDestroyed)
{
    auto pane = std::make_unique<SceneViewport>();
    const SceneViewport* address = pane.get();
    EXPECT_FALSE(IsLive(*pane));

    SceneViewportBinding binding;
    binding.Controller = ZeroedController();
    pane->Bind(binding);
    EXPECT_TRUE(IsLive(*pane));
    pane->Bind(binding);
    EXPECT_EQ(std::count(SceneViewportRegistry::GetLive().begin(), SceneViewportRegistry::GetLive().end(), address), 1);

    pane.reset();
    const auto live = SceneViewportRegistry::GetLive();
    EXPECT_EQ(std::find(live.begin(), live.end(), address), live.end());
}

TEST(SceneViewportTests, APaneUnboundFromItsControllerLeavesTheList)
{
    SceneViewport pane;
    SceneViewportBinding binding;
    binding.Controller = ZeroedController();
    pane.Bind(binding);
    ASSERT_TRUE(IsLive(pane));

    pane.Bind(SceneViewportBinding{});
    EXPECT_FALSE(IsLive(pane));
}

TEST(SceneViewportTests, TheSlotIdsArePresentAfterSetSlotAndUnchangedByASecondSetSlot)
{
    SceneViewport pane;
    pane.SetSlot("top");
    const size_t childCount = pane.GetChildren().size();
    ASSERT_GT(childCount, 0u);
    EXPECT_TRUE(HasChildWithId(pane, "SceneViewViewportLabelTop"));
    EXPECT_TRUE(HasChildWithId(pane, "SceneViewRulerOverlayTop"));
    EXPECT_TRUE(HasChildWithId(pane, "SceneViewMeasureOverlayTop"));
    EXPECT_TRUE(HasChildWithId(pane, "ViewportRotationGizmoTop"));
    EXPECT_FALSE(HasChildWithId(pane, "SceneViewRulerOverlay"));

    // A layout hot reload sets the slot attribute again on the live pane.
    pane.SetSlot("top");
    EXPECT_EQ(pane.GetChildren().size(), childCount);
    EXPECT_TRUE(HasChildWithId(pane, "SceneViewRulerOverlayTop"));
    EXPECT_FALSE(HasChildWithId(pane, "SceneViewRulerOverlayTopTop"));
}

TEST(SceneViewportTests, EverySlotGivesEachChildItsSlotId)
{
    constexpr std::string_view kBaseIds[] = {"SceneViewRulerOverlay", "SceneViewMeasureOverlay", "ViewportRotationGizmo",
                                             "SceneViewHoverPill", "SceneViewHdrTestPatternOverlay"};
    for (std::size_t index = 0; index < std::size(kSlots); ++index)
    {
        SceneViewport pane;
        pane.SetSlot(std::string(kSlots[index]));
        EXPECT_TRUE(HasChildWithId(pane, "SceneViewViewportLabel" + std::string(kLabelIdSuffixes[index])))
            << kSlots[index];
        for (std::string_view baseId : kBaseIds)
            EXPECT_TRUE(HasChildWithId(pane, SlotId(baseId, index))) << SlotId(baseId, index);
    }
}

/// The three overlays of a pane with a slot, found by their slot ids.
struct PaneOverlays
{
    const GameEngine::ViewportRotationGizmo* Gizmo = nullptr;
    const GameEngine::SceneViewRulerOverlay* Ruler = nullptr;
    const GameEngine::SceneViewMeasureOverlay* Measure = nullptr;
};

PaneOverlays FindOverlays(SceneViewport& pane, std::size_t slotIndex)
{
    PaneOverlays overlays;
    overlays.Gizmo = dynamic_cast<const GameEngine::ViewportRotationGizmo*>(
        pane.FindById(GameEngine::HashStringId(SlotId("ViewportRotationGizmo", slotIndex))));
    overlays.Ruler = dynamic_cast<const GameEngine::SceneViewRulerOverlay*>(
        pane.FindById(GameEngine::HashStringId(SlotId("SceneViewRulerOverlay", slotIndex))));
    overlays.Measure = dynamic_cast<const GameEngine::SceneViewMeasureOverlay*>(
        pane.FindById(GameEngine::HashStringId(SlotId("SceneViewMeasureOverlay", slotIndex))));
    return overlays;
}

/// Each overlay has ticked and read `controller` on its last tick.
void ExpectOverlaysTickedWith(const PaneOverlays& overlays, const SceneViewController* controller)
{
    const GameEngine::UIElement* elements[] = {overlays.Gizmo, overlays.Ruler, overlays.Measure};
    for (const GameEngine::UIElement* overlay : elements)
    {
        const GameEngine::Editor::TestStubs::OverlayRecord* record =
            GameEngine::Editor::TestStubs::FindRecord(overlay);
        ASSERT_NE(record, nullptr) << "the overlay did not tick";
        EXPECT_GT(record->Ticks, 0);
        EXPECT_EQ(record->Controller, controller);
    }
}

TEST(SceneViewportTests, AfterBindEachOverlayOfEverySlotTicksWithTheBoundControllerAndNoPanel)
{
    for (std::size_t index = 0; index < std::size(kSlots); ++index)
    {
        GameEngine::Editor::TestStubs::ClearRecords();
        SceneViewport pane;
        pane.SetSlot(std::string(kSlots[index]));
        const PaneOverlays overlays = FindOverlays(pane, index);
        ASSERT_NE(overlays.Gizmo, nullptr) << kSlots[index];
        ASSERT_NE(overlays.Ruler, nullptr) << kSlots[index];
        ASSERT_NE(overlays.Measure, nullptr) << kSlots[index];

        pane.Bind(BindingTo(ZeroedController(0)));
        pane.TickOverlays();
        ExpectOverlaysTickedWith(overlays, ZeroedController(0));
        const GameEngine::Editor::TestStubs::OverlayRecord* gizmo =
            GameEngine::Editor::TestStubs::FindRecord(overlays.Gizmo);
        ASSERT_NE(gizmo, nullptr);
        EXPECT_EQ(gizmo->Pane, &pane) << kSlots[index];
        EXPECT_EQ(gizmo->ViewportIndex, index) << kSlots[index];
        EXPECT_EQ(pane.GetRotationGizmo(), overlays.Gizmo) << kSlots[index];
        pane.Bind(SceneViewportBinding{});
    }
    GameEngine::Editor::TestStubs::ClearRecords();
}

TEST(SceneViewportTests, APaneReboundToAnotherControllerTicksItsOverlaysWithTheNewController)
{
    GameEngine::Editor::TestStubs::ClearRecords();
    SceneViewport pane;
    pane.SetSlot("front");
    const PaneOverlays overlays = FindOverlays(pane, 2);
    ASSERT_NE(overlays.Gizmo, nullptr);
    ASSERT_NE(overlays.Ruler, nullptr);
    ASSERT_NE(overlays.Measure, nullptr);
    pane.Bind(BindingTo(ZeroedController(0)));
    pane.TickOverlays();

    pane.Bind(BindingTo(ZeroedController(1)));
    pane.TickOverlays();
    ExpectOverlaysTickedWith(overlays, ZeroedController(1));
    pane.Bind(SceneViewportBinding{});
    GameEngine::Editor::TestStubs::ClearRecords();
}

TEST(SceneViewportTests, APointerMoveInsideThePaneSetsTheRulerCursorInPanePixels)
{
    GameEngine::Editor::TestStubs::ClearRecords();
    SceneViewport pane;
    pane.SetSlot("top");
    const PaneOverlays overlays = FindOverlays(pane, 1);
    ASSERT_NE(overlays.Ruler, nullptr);
    GameEngine::UILayoutAccess::SetLastLayoutRect(pane, 100.0f, 50.0f, 200.0f, 100.0f);
    pane.Bind(BindingTo(ZeroedController(0)));
    // With the measure tool on, the move ends before the controller's tool context, which zeroed storage lacks.
    GameEngine::SceneViewportMeasureSettings measure;
    measure.Enabled = true;
    pane.SetMeasureSettings(measure);

    UIEvent move = MouseMoveAt(150.0f, 80.0f);
    pane.DispatchEvent(move);
    const GameEngine::Editor::TestStubs::OverlayRecord* ruler =
        GameEngine::Editor::TestStubs::FindRecord(overlays.Ruler);
    ASSERT_NE(ruler, nullptr) << "the move did not reach the ruler";
    EXPECT_TRUE(ruler->CursorInside);
    EXPECT_FLOAT_EQ(ruler->Cursor.x, 50.0f);
    EXPECT_FLOAT_EQ(ruler->Cursor.y, 30.0f);
    pane.Bind(SceneViewportBinding{});
    GameEngine::Editor::TestStubs::ClearRecords();
}

TEST(SceneViewportTests, TheMeasureSettingsReachThePaneMeasureOverlay)
{
    GameEngine::Editor::TestStubs::ClearRecords();
    SceneViewport pane;
    pane.SetSlot("side");
    const PaneOverlays overlays = FindOverlays(pane, 3);
    ASSERT_NE(overlays.Measure, nullptr);

    GameEngine::SceneViewportMeasureSettings settings;
    settings.Enabled = true;
    settings.Units = GameEngine::SceneViewMeasureOverlay::UnitSystem::Imperial;
    settings.TwoDMode = GameEngine::SceneViewMeasureOverlay::TwoDMode::Points;
    pane.SetMeasureSettings(settings);
    const GameEngine::Editor::TestStubs::OverlayRecord* measure =
        GameEngine::Editor::TestStubs::FindRecord(overlays.Measure);
    ASSERT_NE(measure, nullptr) << "the settings did not reach the measure overlay";
    EXPECT_TRUE(measure->MeasureEnabled);
    EXPECT_EQ(measure->Units, GameEngine::SceneViewMeasureOverlay::UnitSystem::Imperial);
    EXPECT_EQ(measure->TwoDMode, GameEngine::SceneViewMeasureOverlay::TwoDMode::Points);
    GameEngine::Editor::TestStubs::ClearRecords();
}

TEST(SceneViewportTests, ThePerspectiveSlotKeepsTheBaseIds)
{
    SceneViewport pane;
    pane.SetSlot("perspective");
    EXPECT_TRUE(HasChildWithId(pane, "SceneViewViewportLabelPerspective"));
    EXPECT_TRUE(HasChildWithId(pane, "SceneViewRulerOverlay"));
    EXPECT_TRUE(HasChildWithId(pane, "SceneViewMeasureOverlay"));
}

TEST(SceneViewportTests, ThePointerEnteringAndLeavingAPaneHoversItInItsHost)
{
    RecordingHost host;
    SceneViewport pane;
    SceneViewportBinding binding;
    binding.Host = &host;
    pane.Bind(binding);

    UIEvent enter;
    enter.Id = GameEngine::kEventMouseEnter;
    pane.DispatchEvent(enter);
    UIEvent leave;
    leave.Id = GameEngine::kEventMouseLeave;
    pane.DispatchEvent(leave);

    ASSERT_EQ(host.HoverCalls.size(), 2u);
    EXPECT_EQ(host.HoverCalls[0].Pane, &pane);
    EXPECT_TRUE(host.HoverCalls[0].Inside);
    EXPECT_EQ(host.HoverCalls[1].Pane, &pane);
    EXPECT_FALSE(host.HoverCalls[1].Inside);
}

TEST(SceneViewportTests, AMoveInsideThePaneBoundsIsInsideAndAMoveOutsideIsNot)
{
    SceneViewport pane;
    GameEngine::UILayoutAccess::SetLastLayoutRect(pane, 100.0f, 50.0f, 200.0f, 100.0f);

    UIEvent inside = MouseMoveAt(150.0f, 80.0f);
    pane.DispatchEvent(inside);
    EXPECT_TRUE(pane.IsPointerInside());
    EXPECT_FLOAT_EQ(pane.GetPointerLocalPosition().x, 50.0f);
    EXPECT_FLOAT_EQ(pane.GetPointerLocalPosition().y, 30.0f);

    // A captured move outside the pane keeps the last position inside.
    UIEvent outside = MouseMoveAt(400.0f, 80.0f);
    pane.DispatchEvent(outside);
    EXPECT_FALSE(pane.IsPointerInside());
    EXPECT_FLOAT_EQ(pane.GetPointerLocalPosition().x, 50.0f);
}

TEST(SceneViewportTests, AReleaseOrACancelOfTheDragButtonEndsTheCameraDrag)
{
    SceneViewport pane;
    GameEngine::SceneViewCameraGesture& gesture = pane.GetCameraGesture();
    const GameEngine::Mathematics::Vector2 pressPosition(10.0f, 10.0f);

    gesture.BeginPress(GameEngine::Input::kMouseButton_Left, pressPosition, false, false, false, false);
    UIEvent release;
    release.Id = GameEngine::kEventMouseUp;
    release.Button = GameEngine::Input::kMouseButton_Left;
    pane.DispatchEvent(release);
    EXPECT_FALSE(gesture.Dragging);

    // The UI manager cancels a press when it disables the capturing element, with no release.
    gesture.BeginPress(GameEngine::Input::kMouseButton_Right, pressPosition, false, false, false, false);
    ASSERT_TRUE(gesture.IsInteracting());
    UIEvent cancel;
    cancel.Id = GameEngine::kEventMouseCancel;
    cancel.Button = GameEngine::Input::kMouseButton_Right;
    pane.DispatchEvent(cancel);
    EXPECT_FALSE(gesture.Dragging);
    EXPECT_FALSE(gesture.IsInteracting());
}

TEST(SceneViewportTests, TheHostLabelBoxIsTheShownLabelOverThisPaneAndEmptyOtherwise)
{
    RecordingHost host;
    SceneViewport pane;
    SceneViewport otherPane;
    pane.Bind(HostBinding(host, 0));
    otherPane.Bind(HostBinding(host, 1));
    const GameEngine::Mathematics::Rect box{120.0f, 210.0f, 100.0f, 28.0f};
    const GameEngine::UIElement& labelOverPane = AddLabel(pane, box);
    const GameEngine::UIElement& labelOverOtherPane = AddLabel(otherPane, box);

    host.Label = &labelOverPane;
    const GameEngine::Mathematics::Rect shown = pane.GetHostLabelBox();
    EXPECT_FLOAT_EQ(shown.X, box.X);
    EXPECT_FLOAT_EQ(shown.Y, box.Y);
    EXPECT_FLOAT_EQ(shown.Width, box.Width);
    EXPECT_FLOAT_EQ(shown.Height, box.Height);
    EXPECT_TRUE(otherPane.GetHostLabelBox().IsEmpty()) << "the label is over another pane";

    host.Label = &labelOverOtherPane;
    EXPECT_TRUE(pane.GetHostLabelBox().IsEmpty()) << "the label is over another pane";

    // A hidden label keeps its layout box; the host then shows no label.
    host.Label = nullptr;
    EXPECT_TRUE(pane.GetHostLabelBox().IsEmpty()) << "the host label is hidden";
    EXPECT_TRUE(otherPane.GetHostLabelBox().IsEmpty()) << "the host label is hidden";
}

TEST(SceneViewportTests, APressWhileAnotherPaneDragsDoesNotActivateThePaneUntilTheDragEnds)
{
    RecordingHost host;
    SceneViewport dragging;
    SceneViewport pressed;
    dragging.Bind(HostBinding(host, 0));
    pressed.Bind(HostBinding(host, 1));
    GameEngine::SceneViewCameraGesture& gesture = dragging.GetCameraGesture();
    gesture.BeginPress(GameEngine::Input::kMouseButton_Right, GameEngine::Mathematics::Vector2(10.0f, 10.0f), false,
                       false, false, false);
    ASSERT_TRUE(gesture.Dragging);

    // The rotation gizmo's press calls Activate: the dragging pane keeps the active slot.
    pressed.Activate();
    EXPECT_TRUE(host.Activated.empty());
    // The dragging pane itself still activates.
    dragging.Activate();
    ASSERT_EQ(host.Activated.size(), 1u);
    EXPECT_EQ(host.Activated[0], &dragging);

    gesture.EndPress(GameEngine::Input::kMouseButton_Right);
    ASSERT_FALSE(gesture.Dragging);
    pressed.Activate();
    ASSERT_EQ(host.Activated.size(), 2u);
    EXPECT_EQ(host.Activated[1], &pressed);
}

TEST(SceneViewportTests, TheHostDragsWhileAPaneOfThatHostDragsAndNotForAnotherHost)
{
    RecordingHost host;
    RecordingHost otherHost;
    SceneViewport dragging;
    SceneViewport idle;
    SceneViewport otherHostPane;
    dragging.Bind(HostBinding(host, 0));
    idle.Bind(HostBinding(host, 1));
    otherHostPane.Bind(HostBinding(otherHost, 2));
    EXPECT_FALSE(SceneViewport::IsHostDragging(host, nullptr));

    GameEngine::SceneViewCameraGesture& gesture = dragging.GetCameraGesture();
    gesture.BeginPress(GameEngine::Input::kMouseButton_Right, GameEngine::Mathematics::Vector2(10.0f, 10.0f), false,
                       false, false, false);
    ASSERT_TRUE(gesture.Dragging);

    // The quad toggle asks with no pane to skip; a pane asks with itself.
    EXPECT_TRUE(SceneViewport::IsHostDragging(host, nullptr));
    EXPECT_TRUE(SceneViewport::IsHostDragging(host, &idle));
    EXPECT_FALSE(SceneViewport::IsHostDragging(host, &dragging)) << "the pane skips its own drag";
    EXPECT_FALSE(SceneViewport::IsHostDragging(otherHost, nullptr)) << "a drag in another host";

    gesture.EndPress(GameEngine::Input::kMouseButton_Right);
    EXPECT_FALSE(SceneViewport::IsHostDragging(host, nullptr));
}

} // namespace
