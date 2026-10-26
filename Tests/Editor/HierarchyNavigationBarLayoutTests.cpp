// The Hierarchy's two bars: the panel search and the item size bar below the tree. They are laid
// out by the editor theme and the bar's own stylesheet, and four things about them are easy to
// lose without any structural check noticing: the size bar stays at the bottom wherever the search
// sits; a bottom search sits above the size bar; both ride above the tree's
// horizontal scrollbar while one is on screen; and the search reads as a field at rest. This file
// lays the REAL bar and the REAL panel search it places out over the REAL editor stylesheets and
// measures them, and checks that a fresh profile places the search at the top and that a saved
// bottom placement keeps it at the bottom.
#include <gtest/gtest.h>

#include "Editor/Hierarchy/HierarchyNavigationBar.h"
#include "Editor/Settings/SettingsStore.h"
#include "UI/Controls/Scrollbar.h"
#include "UI/EditorSearchBars.h"
#include "UI/PanelSearchBar.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"
#include "UIRgTestHarness.h"

#include "TestTempDir.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using GameEngine::EditorSearchBars;
using GameEngine::Stylesheet;
using GameEngine::UIElement;
using GameEngine::UIManager;
using GameEngine::Editor::HierarchyNavigationBar;

namespace
{

constexpr uint32_t kViewportW = 400;
constexpr uint32_t kViewportH = 300;
constexpr int kSettleFrames = 3;
constexpr float kFrameSeconds = 1.0f / 60.0f;
constexpr float kLayoutTolerancePx = 0.01f;

// The size bar's resting inset from the panel edge, its left edge on the tree's first icon
// column, the 16px rows glyph that leads it and the 8px gap after the glyph
// (HierarchyNavigationBar.css), and the width every item size slider shares (ItemSizeSlider.css).
constexpr float kBarInsetPx = 8.0f;
constexpr float kBarLeftPx = 23.0f;
constexpr float kSizeIconPx = 16.0f;
constexpr float kSizeIconGapPx = 8.0f;
constexpr float kSliderWidthPx = 102.0f;
// The panel search: 24px tall, 6px from the panel edge at the top, and 6px above the 22px size
// bar at the bottom (theme/views.css).
constexpr float kSearchHeightPx = 24.0f;
constexpr float kSearchTopInsetPx = 6.0f;
constexpr float kBarHeightPx = 22.0f;
constexpr float kSearchAboveBarPx = 6.0f;
// The lift above the tree's horizontal scrollbar: the bar sits 4px above the panel edge and is
// Scrollbar::kDefaultThicknessPx tall, and the size bar clears it by 3px. Derived from the bar's
// real thickness, so a thickness change the stylesheet's literal misses turns this test red.
constexpr float kScrollbarInsetPx = 4.0f;
constexpr float kBarClearanceAboveScrollbarPx = 3.0f;
constexpr float kBarAboveScrollbarPx =
    kScrollbarInsetPx + GameEngine::Scrollbar::kDefaultThicknessPx + kBarClearanceAboveScrollbarPx;

// The theme sheets the bars sit on, in theme.css import order, then the two control sheets the
// size bar and its slider request for their own subtrees, which cascade after the theme.
constexpr const char* kSheets[] = {
    "Assets/UI/theme/tokens.css",
    "Assets/UI/theme/core.css",
    "Assets/UI/theme/views.css",
    "Assets/UI/theme/widgets.css",
    "Assets/UI/theme/slider.css",
    "Assets/UI/controls/ItemSizeSlider/ItemSizeSlider.css",
    "Assets/UI/panels/HierarchyNavigationBar/HierarchyNavigationBar.css",
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

struct HierarchyParts
{
    UIElement* Panel = nullptr;
    HierarchyNavigationBar* Bar = nullptr;
    UIElement* SizeIcon = nullptr;
    UIElement* Slider = nullptr;
    UIElement* Search = nullptr;
};

// A real UIManager over the editor's own stylesheets holding a Hierarchy-shaped panel with the
// real panel search (BuildPanelSearchBar) and the real size bar in it, as HierarchyPanel builds
// them, so a measurement here is the editor's layout and look and not the test's own CSS.
struct NavigationBarFixture
{
    std::unique_ptr<GameEngine::Rendering::IDevice> Device;
    std::unique_ptr<UIManager> Ui;
    std::string Diagnostic;
    HierarchyParts Parts;

    bool Build()
    {
        auto panel = std::make_unique<UIElement>();
        panel->AddClass("hierarchy-panel");
        GameEngine::PanelSearchBarBuilt search = GameEngine::BuildPanelSearchBar("hierarchy-search-field");
        Parts.Search = search.Root.get();
        auto bar = std::make_unique<HierarchyNavigationBar>();
        Parts.Bar = bar.get();
        bar->PlaceSearchAbove(Parts.Search);
        panel->AddChild(std::move(search.Root));
        for (const auto& child : bar->GetChildren())
        {
            if (child->HasClass("hierarchy-navigation-bar-size-icon"))
                Parts.SizeIcon = child.get();
            if (child->HasClass("hierarchy-size-slider"))
                Parts.Slider = child.get();
        }
        panel->AddChild(std::move(bar));
        Parts.Panel = panel.get();
        Device = MakeHeadlessDevice();
        if (!Device)
        {
            Diagnostic = "no Vulkan device";
            return false;
        }
        Ui = std::make_unique<UIManager>(Device.get());
        Ui->SetLayoutSizeOverride(kViewportW, kViewportH);
        // An editor sheet that does not read or parse fails the test; only a missing device skips.
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
        Ui->SetRoot(std::move(panel));
        Settle();
        return true;
    }

    // Update owns style resolution and the Yoga solve; nothing here reads a primitive.
    void Settle()
    {
        for (int i = 0; i < kSettleFrames; ++i)
            Ui->Update(kFrameSeconds, /*interactive=*/true);
    }
};

float Bottom(const UIElement& element)
{
    return element.GetLayoutY() + element.GetLayoutHeight();
}

float Right(const UIElement& element)
{
    return element.GetLayoutX() + element.GetLayoutWidth();
}

void SetUserDataRoot(const std::string& root)
{
#ifdef _WIN32
    _putenv_s("GE_EDITOR_USER_DATA_ROOT", root.c_str());
#else
    if (root.empty())
        unsetenv("GE_EDITOR_USER_DATA_ROOT");
    else
        setenv("GE_EDITOR_USER_DATA_ROOT", root.c_str(), 1);
#endif
}

// Points the editor preferences at a scratch directory of this process for one test, puts the
// previous root back afterwards and reloads the search-bar settings from it, so the rest of the
// process reads the preferences and the placement it read before, and removes the directory.
struct ScopedUserDataRoot
{
    std::string Previous;
    GameEngine::TestUtils::ScopedTempDir Directory{
        GameEngine::TestUtils::MakeUniqueTempDirectory("HierarchyNavigationBarLayoutTests_user_data")};

    ScopedUserDataRoot()
    {
        if (const char* previous = std::getenv("GE_EDITOR_USER_DATA_ROOT"))
            Previous = previous;
        SetUserDataRoot(Directory.Path().string());
    }
    ~ScopedUserDataRoot()
    {
        SetUserDataRoot(Previous);
        EditorSearchBars::LoadFromPreferences();
    }
};

// Registers the panel search with the search-bar settings, as HierarchyPanel does, for one test.
struct ScopedSearchBarRegistration
{
    UIElement* Search;
    explicit ScopedSearchBarRegistration(UIElement* search)
        : Search(search)
    {
        EditorSearchBars::Register(Search);
    }
    ~ScopedSearchBarRegistration() { EditorSearchBars::Unregister(Search); }
};

} // namespace

TEST(HierarchyNavigationBarLayout, TheSizeBarSitsAtTheBottomOnTheIconColumn)
{
    NavigationBarFixture fixture;
    if (!fixture.Build())
        GTEST_SKIP() << fixture.Diagnostic;
    const HierarchyParts& parts = fixture.Parts;

    ASSERT_GT(parts.Panel->GetLayoutHeight(), 0.0f) << "the panel did not lay out; this test is vacuous";
    ASSERT_NE(parts.Slider, nullptr);
    EXPECT_NEAR(parts.Slider->GetLayoutWidth(), kSliderWidthPx, kLayoutTolerancePx);
    EXPECT_NEAR(parts.Bar->GetLayoutX() - parts.Panel->GetLayoutX(), kBarLeftPx, kLayoutTolerancePx);
    EXPECT_NEAR(Bottom(*parts.Panel) - Bottom(*parts.Bar), kBarInsetPx, kLayoutTolerancePx);
}

// The bar leads with a 16px rows glyph on the icon column, as the Assets bar leads with its view
// glyph, and the slider follows it.
TEST(HierarchyNavigationBarLayout, ARowsGlyphLeadsTheSizeSlider)
{
    NavigationBarFixture fixture;
    if (!fixture.Build())
        GTEST_SKIP() << fixture.Diagnostic;
    const HierarchyParts& parts = fixture.Parts;

    ASSERT_NE(parts.SizeIcon, nullptr);
    ASSERT_NE(parts.Slider, nullptr);
    EXPECT_TRUE(parts.SizeIcon->GetResolvedStyle().Visual.BackgroundImage.HasImage) << "the glyph has no image";
    EXPECT_NEAR(parts.SizeIcon->GetLayoutWidth(), kSizeIconPx, kLayoutTolerancePx);
    EXPECT_NEAR(parts.SizeIcon->GetLayoutHeight(), kSizeIconPx, kLayoutTolerancePx);
    EXPECT_NEAR(parts.SizeIcon->GetLayoutX() - parts.Panel->GetLayoutX(), kBarLeftPx, kLayoutTolerancePx);
    EXPECT_NEAR(parts.Slider->GetLayoutX() - parts.SizeIcon->GetLayoutX(), kSizeIconPx + kSizeIconGapPx,
                kLayoutTolerancePx);
}

// The size bar and a bottom search ride above the tree's horizontal scrollbar together.
TEST(HierarchyNavigationBarLayout, TheBarsRideAboveTheHorizontalScrollbar)
{
    const ScopedUserDataRoot userData;
    {
        GameEngine::Editor::SettingsStore prefs = GameEngine::Editor::OpenEditorPreferences();
        prefs.SetBool("ui.searchBarsAtTop", false);
        std::string error;
        ASSERT_TRUE(prefs.Save(&error)) << error;
    }
    EditorSearchBars::LoadFromPreferences();

    NavigationBarFixture fixture;
    if (!fixture.Build())
        GTEST_SKIP() << fixture.Diagnostic;
    const HierarchyParts& parts = fixture.Parts;
    const ScopedSearchBarRegistration registration{parts.Search};

    parts.Bar->SetAboveHorizontalScrollbar(true);
    fixture.Settle();
    EXPECT_NEAR(Bottom(*parts.Panel) - Bottom(*parts.Bar), kBarAboveScrollbarPx, kLayoutTolerancePx);
    EXPECT_NEAR(parts.Bar->GetLayoutY() - Bottom(*parts.Search), kSearchAboveBarPx, kLayoutTolerancePx)
        << "the bottom search does not follow the size bar up";

    parts.Bar->SetAboveHorizontalScrollbar(false);
    fixture.Settle();
    EXPECT_NEAR(Bottom(*parts.Panel) - Bottom(*parts.Bar), kBarInsetPx, kLayoutTolerancePx);
    EXPECT_NEAR(parts.Bar->GetLayoutY() - Bottom(*parts.Search), kSearchAboveBarPx, kLayoutTolerancePx)
        << "the bottom search does not follow the size bar down";
}

// On a fresh profile the search sits at the top and the size bar stays at the bottom.
TEST(HierarchyNavigationBarLayout, ATopSearchLeavesTheSizeBarAtTheBottom)
{
    const ScopedUserDataRoot userData;
    EditorSearchBars::LoadFromPreferences();

    NavigationBarFixture fixture;
    if (!fixture.Build())
        GTEST_SKIP() << fixture.Diagnostic;
    const HierarchyParts& parts = fixture.Parts;
    const ScopedSearchBarRegistration registration{parts.Search};
    fixture.Settle();

    EXPECT_TRUE(EditorSearchBars::GetAtTop());
    EXPECT_TRUE(parts.Search->HasClass("search-bar-at-top"));
    EXPECT_TRUE(parts.Panel->HasClass("search-bars-at-top"));
    EXPECT_FALSE(parts.Bar->HasClass("search-bar-at-top")) << "the size bar must not move with the search";
    EXPECT_NEAR(parts.Search->GetLayoutY() - parts.Panel->GetLayoutY(), kSearchTopInsetPx, kLayoutTolerancePx);
    EXPECT_NEAR(parts.Search->GetLayoutHeight(), kSearchHeightPx, kLayoutTolerancePx);
    EXPECT_NEAR(Bottom(*parts.Panel) - Bottom(*parts.Bar), kBarInsetPx, kLayoutTolerancePx);
}

// A profile that saved search bars at the bottom keeps them there, above the size bar rather than
// over it: the saved value wins over the default.
TEST(HierarchyNavigationBarLayout, ASavedBottomSearchSitsAboveTheSizeBar)
{
    const ScopedUserDataRoot userData;
    {
        GameEngine::Editor::SettingsStore prefs = GameEngine::Editor::OpenEditorPreferences();
        prefs.SetBool("ui.searchBarsAtTop", false);
        std::string error;
        ASSERT_TRUE(prefs.Save(&error)) << error;
    }
    EditorSearchBars::LoadFromPreferences();

    NavigationBarFixture fixture;
    if (!fixture.Build())
        GTEST_SKIP() << fixture.Diagnostic;
    const HierarchyParts& parts = fixture.Parts;
    const ScopedSearchBarRegistration registration{parts.Search};
    fixture.Settle();

    EXPECT_FALSE(EditorSearchBars::GetAtTop());
    EXPECT_FALSE(parts.Search->HasClass("search-bar-at-top"));
    EXPECT_FALSE(parts.Panel->HasClass("search-bars-at-top"));
    EXPECT_NEAR(Bottom(*parts.Panel) - Bottom(*parts.Bar), kBarInsetPx, kLayoutTolerancePx);
    EXPECT_NEAR(parts.Bar->GetLayoutY() - Bottom(*parts.Search), kSearchAboveBarPx, kLayoutTolerancePx);
    EXPECT_NEAR(parts.Bar->GetLayoutHeight(), kBarHeightPx, kLayoutTolerancePx);
}

// At rest, with no focus and no query, the search reads as a field: a fill and a border that
// differs from it.
TEST(HierarchyNavigationBarLayout, TheSearchReadsAsAFieldAtRest)
{
    NavigationBarFixture fixture;
    if (!fixture.Build())
        GTEST_SKIP() << fixture.Diagnostic;
    const auto& visual = fixture.Parts.Search->GetResolvedStyle().Visual;

    EXPECT_NE(visual.BackgroundColor, 0u) << "the search has no fill at rest";
    EXPECT_NE(visual.BorderColor.Top, 0u) << "the search has no border at rest";
    EXPECT_NE(visual.BorderColor.Top, visual.BackgroundColor) << "the border does not stand out from the fill";
}
