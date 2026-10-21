// The Scene View tool strip's badge count, laid out over the REAL editor stylesheets: at every
// count it can show (one digit, two digits and the capped "99+") the count stays inside the strip
// that holds it, clear of the strip's border, in the floating strip in both orientations and in
// the inline mirror in the toolbar, lifted above its button so it covers less of the icon, with its
// text lifted to the circle's optical centre. The count
// is absolutely placed on its badge button, so no structural check sees it leave the strip; only a
// measurement does.

#include <gtest/gtest.h>

#include "SceneView/SceneViewToolStrip.h"
#include "SceneView/SceneViewToolStripRegistry.h"

#include "UI/Controls/Label.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIElement.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"
#include "UIRgTestHarness.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using GameEngine::Label;
using GameEngine::Stylesheet;
using GameEngine::UIElement;
using GameEngine::UIManager;
using GameEngine::Editor::RegisteredToolStrip;
using GameEngine::Editor::RegisteredToolStripActions;
using GameEngine::Editor::SceneViewToolStripEntry;
using GameEngine::Editor::SceneViewToolStripRegistry;
using GameEngine::Editor::ToolStripPlacement;

namespace
{

constexpr uint32_t kViewportW = 600;
constexpr uint32_t kViewportH = 400;
constexpr int kSettleFrames = 3;
constexpr float kFrameSeconds = 1.0f / 60.0f;
constexpr const char* kEntryId = "notes";

// The floating strip's border (.scene-tool-overlay) and the room the count keeps between itself
// and the inside of that border, so it stays off the strip's rounded edge.
constexpr float kStripBorderPx = 1.0f;
constexpr float kClearanceInsideBorderPx = 2.0f;
// The count's resting size (.scene-tool-badge): a one-digit count is a 16 px circle.
constexpr float kBadgeHeightPx = 16.0f;
// How far above the circle's centre the count's text box is centred. The label centres its line
// box in its content box, which leaves the digits half a pixel low in the circle; a whole-pixel
// lift puts them half a pixel high, the side that reads as centred, wherever the strip sits on
// the pixel grid (a half-pixel lift rounds either way with the strip's sub-pixel position).
constexpr float kTextLiftPx = 1.0f;
// How far the count is lifted above its badge button's top edge (.scene-tool-badge): 3 px where the
// strip has room above the button, 2 px in the horizontal strip, whose buttons sit on its padding.
constexpr float kBadgeLiftPx = 3.0f;
constexpr float kHorizontalBadgeLiftPx = 2.0f;

// The theme sheets the strips sit on, in theme.css import order, then the control and panel sheets
// that style the strip's buttons and the strip itself.
constexpr const char* kSheets[] = {
    "Assets/UI/theme/tokens.css",
    "Assets/UI/theme/core.css",
    "Assets/UI/controls/Button.css",
    "Assets/UI/panels/SceneViewPanel.css",
};

std::string ReadEditorFile(const std::string& relativePath)
{
    const std::filesystem::path path = std::filesystem::path(GE_EDITOR_SOURCE_DIR) / relativePath;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

enum class StripShape
{
    FloatingVertical,
    FloatingHorizontal,
    Inline
};

// A real UIManager over the editor's sheets holding one Scene View strip with a built-in button
// and a registered entry that has a badge, the way SceneViewPanel builds either strip.
struct BadgeStripFixture
{
    std::unique_ptr<GameEngine::Rendering::IDevice> Device;
    std::unique_ptr<UIManager> Ui;
    SceneViewToolStripRegistry Registry;
    RegisteredToolStrip Kept;
    std::size_t Count = 0;
    UIElement* Container = nullptr; // what the count must stay inside
    Label* Badge = nullptr;

    // False only without a Vulkan device; an editor sheet that does not read or parse fails the test.
    bool Build(StripShape shape)
    {
        Device = MakeHeadlessDevice();
        if (!Device)
            return false;
        Ui = std::make_unique<UIManager>(Device.get());
        Ui->SetLayoutSizeOverride(kViewportW, kViewportH);
        for (const char* sheetPath : kSheets)
        {
            const std::string css = ReadEditorFile(sheetPath);
            EXPECT_FALSE(css.empty()) << "stylesheet did not read: " << sheetPath;
            Stylesheet sheet{};
            EXPECT_TRUE(GameEngine::UIParsing::CSSParser::ParseStylesFromString(css, sheet))
                << "stylesheet did not parse: " << sheetPath;
            sheet.SourceName = sheetPath;
            Ui->AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));
        }

        SceneViewToolStripEntry entry;
        entry.Id = kEntryId;
        entry.BadgeCount = [this]() { return Count; };
        entry.OnBadgeButton = [](UIElement&) {};
        Registry.Register(entry);

        auto root = std::make_unique<UIElement>();
        auto strip = std::make_unique<UIElement>();
        auto builtIn = std::make_unique<UIElement>();
        builtIn->AddClass("scene-tool-btn");
        strip->AddChild(std::move(builtIn));
        UIElement* stripPtr = strip.get();
        const ToolStripPlacement placement =
            shape == StripShape::Inline ? ToolStripPlacement::Inline : ToolStripPlacement::Floating;
        if (shape == StripShape::Inline)
        {
            auto toolbar = std::make_unique<UIElement>();
            toolbar->AddClass("sceneview-toolbar");
            strip->AddClass("sceneview-toolbar-buttons");
            strip->AddClass("inline-tool-buttons");
            Container = toolbar.get();
            toolbar->AddChild(std::move(strip));
            root->AddChild(std::move(toolbar));
        }
        else
        {
            strip->AddClass("scene-tool-overlay");
            if (shape == StripShape::FloatingHorizontal)
                strip->AddClass("horizontal");
            Container = stripPtr;
            root->AddChild(std::move(strip));
        }
        Kept.Populate(*stripPtr, placement, Registry, RegisteredToolStripActions{});
        Badge = dynamic_cast<Label*>(stripPtr->FindById(GameEngine::HashStringId(GameEngine::Editor::RegisteredToolBadgeCountId(placement, kEntryId))));
        Ui->SetRoot(std::move(root));
        return true;
    }

    void Show(std::size_t count)
    {
        Count = count;
        Kept.Refresh(nullptr);
        for (int i = 0; i < kSettleFrames; ++i)
            Ui->Update(kFrameSeconds, /*interactive=*/true);
    }
};

// The count's rectangle lies inside the container's border box less its border and the clearance.
void ExpectInside(const UIElement& badge, const UIElement& container, float inset, const std::string& what)
{
    const float left = container.GetLayoutX() + inset;
    const float top = container.GetLayoutY() + inset;
    const float right = container.GetLayoutX() + container.GetLayoutWidth() - inset;
    const float bottom = container.GetLayoutY() + container.GetLayoutHeight() - inset;
    EXPECT_GE(badge.GetLayoutX(), left) << what << ": the count leaves the strip on the left";
    EXPECT_GE(badge.GetLayoutY(), top) << what << ": the count leaves the strip at the top";
    EXPECT_LE(badge.GetLayoutX() + badge.GetLayoutWidth(), right) << what << ": the count leaves the strip on the right";
    EXPECT_LE(badge.GetLayoutY() + badge.GetLayoutHeight(), bottom) << what << ": the count leaves the strip at the bottom";
}

void ExpectEveryCountInside(StripShape shape, float inset, float lift, const char* shapeName)
{
    BadgeStripFixture fixture;
    if (!fixture.Build(shape))
        GTEST_SKIP() << "no Vulkan device";
    ASSERT_NE(fixture.Badge, nullptr);

    float oneDigitWidth = 0.0f;
    for (const std::size_t count : {std::size_t{1}, std::size_t{12}, std::size_t{140}})
    {
        fixture.Show(count);
        const std::string what = std::string(shapeName) + ", count \"" + fixture.Badge->GetText() + "\"";
        ASSERT_FALSE(fixture.Badge->HasClass("hidden")) << what;
        ASSERT_GT(fixture.Container->GetLayoutWidth(), 0.0f) << what << ": the strip did not lay out; this test is vacuous";
        EXPECT_FLOAT_EQ(fixture.Badge->GetLayoutHeight(), kBadgeHeightPx) << what;
        ExpectInside(*fixture.Badge, *fixture.Container, inset, what);
        ASSERT_NE(fixture.Badge->GetParent(), nullptr) << what;
        EXPECT_FLOAT_EQ(fixture.Badge->GetParent()->GetLayoutY() - fixture.Badge->GetLayoutY(), lift)
            << what << ": the count is not lifted above its button";
        const GameEngine::Box4& padding = fixture.Badge->GetResolvedStyle().Layout.Padding;
        const float textBoxCentre = padding.Top + (kBadgeHeightPx - padding.Top - padding.Bottom) * 0.5f;
        EXPECT_FLOAT_EQ(kBadgeHeightPx * 0.5f - textBoxCentre, kTextLiftPx)
            << what << ": the count's text is not lifted to the circle's optical centre";
        if (count == 1)
            oneDigitWidth = fixture.Badge->GetLayoutWidth();
        else
            EXPECT_GT(fixture.Badge->GetLayoutWidth(), oneDigitWidth)
                << what << ": the count did not widen with its digits, so the widest count is not measured";
    }
}

} // namespace

TEST(SceneViewToolStripBadgeLayout, EveryCountStaysInsideTheVerticalFloatingStrip)
{
    ExpectEveryCountInside(StripShape::FloatingVertical, kStripBorderPx + kClearanceInsideBorderPx, kBadgeLiftPx, "vertical strip");
}

TEST(SceneViewToolStripBadgeLayout, EveryCountStaysInsideTheHorizontalFloatingStrip)
{
    ExpectEveryCountInside(
        StripShape::FloatingHorizontal, kStripBorderPx + kClearanceInsideBorderPx, kHorizontalBadgeLiftPx, "horizontal strip");
}

// The inline mirror has no strip background of its own: the toolbar row is what the count stays in.
TEST(SceneViewToolStripBadgeLayout, EveryCountStaysInsideTheToolbarRow)
{
    ExpectEveryCountInside(StripShape::Inline, kClearanceInsideBorderPx, kBadgeLiftPx, "inline toolbar");
}
