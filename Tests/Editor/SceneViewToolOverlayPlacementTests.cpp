// The Scene View's floating tool strip over the real editor stylesheets: centered on the viewport's
// left edge while it clears the FPS label, moved up to clear it, pinned under the viewport's top
// when it cannot fit, and kept where the user dragged it.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "UI/Controls/Widgets/SceneView/SceneViewToolOverlay.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <memory>

using GameEngine::SceneViewToolOverlay;
using GameEngine::StyleLength;
using GameEngine::UIElement;
namespace Style = GameEngine::Style;

namespace
{

// A Scene View 742 physical px tall: a maximized 1440 px display at UI scale 2.
constexpr float kViewportPhysicalHeight = 742.0f;
constexpr uint32_t kViewportWidth = 900;
// Shorter than a strip of eight tools.
constexpr uint32_t kShortViewportHeight = 200;
// The inset of the viewport overlays: the strip's left, the FPS label's left and bottom.
constexpr float kViewportInsetPx = 14.0f;
constexpr float kPositionTolerancePx = 0.5f;
// The default project's strip; the tests add one and two registered tools.
constexpr int kDefaultToolCount = 8;
constexpr float kUiScales[] = {1.0f, 1.5f, 2.0f};
constexpr float kOwnerUiScale = 2.0f;
constexpr float kDraggedLeftPx = 120.0f;
constexpr float kDraggedTopPx = 200.0f;
constexpr int kSettleFrames = 3;
constexpr float kFrameSeconds = 1.0f / 60.0f;
constexpr const char* kNoDevice = "no Vulkan device";

// One Scene View viewport holding the tool strip and the FPS label at its default bottom-left
// place, the way SceneViewPanel.uxml and SceneViewPanel declare them.
struct ToolStripScene
{
    InspectorLayoutTesting::InspectorLayoutFixture Fixture;
    UIElement* Viewport = nullptr;
    SceneViewToolOverlay* Strip = nullptr;
    UIElement* FpsLabel = nullptr;

    bool Build(uint32_t viewportHeight, int toolCount, bool fpsLabelVisible)
    {
        Fixture.ViewportW = kViewportWidth;
        Fixture.ViewportH = viewportHeight;
        Fixture.ExtraSheets = {"Assets/UI/theme/scene-view.css", "Assets/UI/theme/views.css",
                               GE_UI_MODULE_SOURCE_DIR "/Assets/UI/controls/Button.css",
                               "Assets/UI/panels/SceneViewPanel.css"};

        auto panel = std::make_unique<UIElement>();
        panel->AddClass("sceneview-panel");
        auto viewport = std::make_unique<UIElement>();
        viewport->AddClass("scene-viewport");
        auto strip = std::make_unique<SceneViewToolOverlay>();
        strip->AddClass("scene-tool-overlay");
        for (int i = 0; i < toolCount; ++i)
        {
            auto button = std::make_unique<UIElement>();
            button->AddClass("scene-tool-btn");
            strip->AddChild(std::move(button));
        }
        auto fpsLabel = std::make_unique<UIElement>();
        fpsLabel->SetId("SceneViewFpsLabel");
        fpsLabel->AddClass("fps-counter");
        fpsLabel->Style()
            .Set(Style::PositionLeft, StyleLength::Px(kViewportInsetPx))
            .Set(Style::PositionBottom, StyleLength::Px(kViewportInsetPx));
        if (!fpsLabelVisible)
            fpsLabel->Style().Set(Style::Opacity, 0.0f);

        Viewport = viewport.get();
        Strip = strip.get();
        FpsLabel = fpsLabel.get();
        viewport->AddChild(std::move(strip));
        viewport->AddChild(std::move(fpsLabel));
        panel->AddChild(std::move(viewport));
        return Fixture.Build(std::move(panel));
    }

    void Settle()
    {
        for (int i = 0; i < kSettleFrames; ++i)
            Fixture.Ui->Update(kFrameSeconds, /*interactive=*/true);
    }

    float StripTop() const { return Strip->GetLayoutY() - Viewport->GetLayoutY(); }
    float StripBottom() const { return StripTop() + Strip->GetLayoutHeight(); }
    float LabelTop() const { return FpsLabel->GetLayoutY() - Viewport->GetLayoutY(); }
};

uint32_t ViewportHeightAt(float uiScale)
{
    return static_cast<uint32_t>(kViewportPhysicalHeight / uiScale);
}

// Centered while the centered strip ends one inset above the FPS label; else ending one inset
// above the label while it fits under the top inset; else pinned at the top inset.
void ExpectClearOfTheFpsLabel(int toolCount)
{
    for (const float uiScale : kUiScales)
    {
        SCOPED_TRACE(testing::Message() << "UI scale " << uiScale << ", " << toolCount << " tools");
        ToolStripScene scene;
        if (!scene.Build(ViewportHeightAt(uiScale), toolCount, /*fpsLabelVisible=*/true))
        {
            if (scene.Fixture.Diagnostic == kNoDevice)
                GTEST_SKIP() << kNoDevice;
            FAIL() << scene.Fixture.Diagnostic;
        }

        const float viewportHeight = scene.Viewport->GetLayoutHeight();
        const float height = scene.Strip->GetLayoutHeight();
        ASSERT_FLOAT_EQ(viewportHeight, static_cast<float>(ViewportHeightAt(uiScale)))
            << "the viewport did not fill the panel; this test is vacuous";
        ASSERT_GT(height, 0.0f) << "the strip did not lay out; this test is vacuous";
        ASSERT_FLOAT_EQ(scene.LabelTop() + scene.FpsLabel->GetLayoutHeight(), viewportHeight - kViewportInsetPx)
            << "the FPS label left its bottom-left place; this test is vacuous";
        EXPECT_FLOAT_EQ(scene.Strip->GetLayoutX() - scene.Viewport->GetLayoutX(), kViewportInsetPx)
            << "the strip left the viewport's left inset";

        const float lowestBottom = scene.LabelTop() - kViewportInsetPx;
        const float gapAbove = scene.StripTop();
        const float gapBelow = viewportHeight - scene.StripBottom();
        if ((viewportHeight + height) * 0.5f <= lowestBottom)
            EXPECT_NEAR(gapAbove, gapBelow, kPositionTolerancePx) << "the strip is not centered on the viewport's height";
        else if (height <= lowestBottom - kViewportInsetPx)
            EXPECT_NEAR(scene.StripBottom(), lowestBottom, kPositionTolerancePx)
                << "the strip does not end one inset above the FPS label";
        else
            EXPECT_NEAR(gapAbove, kViewportInsetPx, kPositionTolerancePx) << "the strip does not pin at the top inset";
    }
}

} // namespace

TEST(SceneViewToolOverlayPlacement, ClearOfTheFpsLabelWithEightTools)
{
    ExpectClearOfTheFpsLabel(kDefaultToolCount);
}

TEST(SceneViewToolOverlayPlacement, ClearOfTheFpsLabelWithNineTools)
{
    ExpectClearOfTheFpsLabel(kDefaultToolCount + 1);
}

TEST(SceneViewToolOverlayPlacement, ClearOfTheFpsLabelWithTenTools)
{
    ExpectClearOfTheFpsLabel(kDefaultToolCount + 2);
}

TEST(SceneViewToolOverlayPlacement, PinsAtTheTopInsetInAViewportShorterThanTheStrip)
{
    ToolStripScene scene;
    if (!scene.Build(kShortViewportHeight, kDefaultToolCount, /*fpsLabelVisible=*/true))
    {
        if (scene.Fixture.Diagnostic == kNoDevice)
            GTEST_SKIP() << kNoDevice;
        FAIL() << scene.Fixture.Diagnostic;
    }
    ASSERT_GT(scene.Strip->GetLayoutHeight(), scene.Viewport->GetLayoutHeight())
        << "the strip fits the viewport; this test is vacuous";
    EXPECT_NEAR(scene.StripTop(), kViewportInsetPx, kPositionTolerancePx)
        << "the strip's first tools leave the viewport at the top";
}

TEST(SceneViewToolOverlayPlacement, CenteredWhenTheFpsLabelIsHidden)
{
    ToolStripScene scene;
    if (!scene.Build(ViewportHeightAt(kOwnerUiScale), kDefaultToolCount + 1, /*fpsLabelVisible=*/false))
    {
        if (scene.Fixture.Diagnostic == kNoDevice)
            GTEST_SKIP() << kNoDevice;
        FAIL() << scene.Fixture.Diagnostic;
    }
    const float viewportHeight = scene.Viewport->GetLayoutHeight();
    ASSERT_GT(scene.StripBottom(), scene.LabelTop() - kViewportInsetPx)
        << "the centered strip clears the label's place; this test is vacuous";
    EXPECT_NEAR(scene.StripTop(), viewportHeight - scene.StripBottom(), kPositionTolerancePx)
        << "the strip avoids a hidden FPS label";
}

TEST(SceneViewToolOverlayPlacement, KeepsTheDraggedPlace)
{
    ToolStripScene scene;
    if (!scene.Build(ViewportHeightAt(kOwnerUiScale), kDefaultToolCount + 2, /*fpsLabelVisible=*/true))
    {
        if (scene.Fixture.Diagnostic == kNoDevice)
            GTEST_SKIP() << kNoDevice;
        FAIL() << scene.Fixture.Diagnostic;
    }
    // The drag in SceneViewPanel sets left and top together.
    scene.Strip->Style()
        .Set(Style::PositionLeft, StyleLength::Px(kDraggedLeftPx))
        .Set(Style::PositionTop, StyleLength::Px(kDraggedTopPx));
    scene.Settle();
    EXPECT_FLOAT_EQ(scene.Strip->GetLayoutX() - scene.Viewport->GetLayoutX(), kDraggedLeftPx);
    EXPECT_FLOAT_EQ(scene.StripTop(), kDraggedTopPx) << "the strip left the place the user dragged it to";
}
