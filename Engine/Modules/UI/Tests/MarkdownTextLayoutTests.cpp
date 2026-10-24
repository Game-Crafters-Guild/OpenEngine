#include <gtest/gtest.h>

#include "UIRgTestHarness.h"

#include "Assets/AssetManager.h"
#include "Core/ApplicationPaths.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/MarkdownText.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

std::string ReadStagedSheet(const char* relative)
{
    std::ifstream in(ApplicationPaths::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Which stylesheets the column's document has besides the UI module's default sheet.
enum class Theme
{
    // The editor's shipped theme tokens, as in the AI Assistant panel.
    Editor,
    // None: a game document that never styles the control.
    None,
};

// A MarkdownText in a 360 px column. The control's rules come from the UI module's default
// sheet, which every UIManager loads; the editor theme only recolors them.
class MarkdownLayoutFixture
{
public:
    bool Init(std::string* why, Theme theme = Theme::Editor)
    {
        m_Device = SharedHeadlessDevice();
        if (!m_Device)
        {
            *why = "Device init failed";
            return false;
        }
        UIRegistration::RegisterBuiltInControls();

        namespace fs = std::filesystem;
        const fs::path assetRoot = ApplicationPaths::GetExecutableDirectory() / "Assets";
        const fs::path scratch = fs::temp_directory_path() / "ui_markdown_text_layout_tests";
        std::error_code ec;
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch, ec);

        // A game's project holds its own assets, none of the editor's.
        const fs::path projectRoot = theme == Theme::Editor ? assetRoot : scratch / "GameAssets";
        fs::create_directories(projectRoot, ec);

        m_Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        if (!m_Assets.Initialize(projectRoot, m_Pool.get(), scratch / "AssetDatabase.assetdb",
                                 scratch / ".Cache" / "AssetDatabase"))
        {
            *why = "AssetManager init failed";
            return false;
        }
        // A game has no editor mount: nothing of the editor's can style the control there.
        if (theme == Theme::Editor)
        {
            AssetSourceDesc editorSource{};
            editorSource.Alias = std::string(kAssetSourceAliasEditor);
            editorSource.Root = assetRoot;
            if (!m_Assets.RegisterSource(editorSource))
            {
                *why = "editor source registration failed";
                return false;
            }
            m_Assets.WaitForStartupScan(kAssetSourceAliasEditor);
        }
        m_Assets.WaitForStartupScan(kAssetSourceAliasProject);

        m_Rg = std::make_unique<UiRgHarness>(m_Device);
        m_Ui = std::make_unique<UIManager>(m_Device, &m_Assets);
        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        m_Ui->SetRoot(std::move(root));

        const std::string tokens = theme == Theme::Editor ? ReadStagedSheet("theme/tokens.css") : std::string();
        if (theme == Theme::Editor && tokens.empty())
        {
            *why = "theme/tokens.css not staged next to the test exe";
            return false;
        }
        // The surface's own text style, as the AI Assistant's reply row sets it.
        const std::string column = "#root { width: 360px; height: 800px; flex-direction: column; }\n";
        const std::string surface = theme == Theme::Editor
                                        ? column + "#reply { font-family: var(--ui_font_family); font-size: 14px; "
                                                   "line-height: 1.45; color: var(--ui_color_text); }\n"
                                        : column;
        Stylesheet sheet{};
        if (!UIParsing::CSSParser::ParseStylesFromString(tokens + "\n" + surface, sheet))
        {
            *why = "theme stylesheet did not parse";
            return false;
        }
        m_Ui->AddStylesheet(std::make_shared<const Stylesheet>(sheet));

        auto text = std::make_unique<MarkdownText>();
        text->SetId("reply");
        m_Text = text.get();
        m_Ui->GetRootElement()->AddChild(std::move(text));
        return true;
    }

    MarkdownText& Text() { return *m_Text; }

    void Settle(int frames = 4)
    {
        for (int i = 0; i < frames; ++i)
        {
            m_Ui->Update(0.016f, /*interactive=*/true);
            DriveUiRender(*m_Ui, *m_Rg);
        }
    }

private:
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement m_ReleaseRetirement;
    Rendering::IDevice* m_Device = nullptr;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Pool;
    AssetManager m_Assets;
    std::unique_ptr<UiRgHarness> m_Rg;
    std::unique_ptr<UIManager> m_Ui;
    MarkdownText* m_Text = nullptr;
};

const Label* FindLabel(const UIElement& root, const std::string& text)
{
    if (const auto* label = dynamic_cast<const Label*>(&root); label && label->GetText() == text)
        return label;
    for (const auto& child : root.GetChildren())
        if (const Label* found = child ? FindLabel(*child, text) : nullptr)
            return found;
    return nullptr;
}

const ScrollView* FindScrollView(const UIElement& root)
{
    if (const auto* view = dynamic_cast<const ScrollView*>(&root))
        return view;
    for (const auto& child : root.GetChildren())
        if (const ScrollView* found = child ? FindScrollView(*child) : nullptr)
            return found;
    return nullptr;
}

} // namespace

// The shipped sheet draws each look (bold, the monospace face for code), a paragraph wraps
// between its words inside the column, and a code block is as tall as its line and scrolls
// its long line sideways.
TEST(MarkdownTextLayoutTests, ShippedSheetDrawsTheLooksWrapsWordsAndScrollsCode)
{
    MarkdownLayoutFixture fx;
    if (std::string why; !fx.Init(&why))
        GTEST_SKIP() << why;

    const std::string longLine(240, 'x');
    fx.Text().SetMarkdown("The lantern at the gate is **bright** and the torch by the well burns low, "
                          "so the square reads warm at dusk.\n\n```\n" +
                          longLine + "\n```");
    fx.Settle();

    const Label* strong = FindLabel(fx.Text(), "bright ");
    ASSERT_NE(strong, nullptr);
    EXPECT_EQ(strong->GetResolvedStyle().Visual.FontWeight, 700);
    const Label* plain = FindLabel(fx.Text(), "lantern ");
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(plain->GetResolvedStyle().Visual.FontWeight, 400);

    const UIElement& paragraph = *fx.Text().GetChildren().at(0);
    const float line = plain->GetLayoutHeight();
    ASSERT_GT(line, 0.0f);
    EXPECT_GT(paragraph.GetLayoutHeight(), 2.0f * line) << "the paragraph wraps to three lines or more";
    for (const auto& word : paragraph.GetChildren())
        EXPECT_LE(word->GetLayoutX() + word->GetLayoutWidth(), paragraph.GetLayoutWidth() + 0.5f)
            << "a word overflows the line";

    const Label* code = FindLabel(fx.Text(), longLine);
    ASSERT_NE(code, nullptr);
    const auto& family = code->GetResolvedStyle().Visual.FontFamily;
    ASSERT_TRUE(family && !family->empty());
    EXPECT_EQ(family->front(), "Roboto Mono");
    const UIElement& codeBlock = *fx.Text().GetChildren().at(1);
    EXPECT_LE(codeBlock.GetLayoutWidth(), 360.0f + 0.5f) << "the block stays in the column";
    EXPECT_GT(codeBlock.GetLayoutHeight(), code->GetLayoutHeight() - 0.5f);
    EXPECT_LT(codeBlock.GetLayoutHeight(), 4.0f * code->GetLayoutHeight() + 24.0f)
        << "the block is sized to its header, its line and a scrollbar, not stretched";
    const ScrollView* scroll = FindScrollView(codeBlock);
    ASSERT_NE(scroll, nullptr);
    EXPECT_GT(scroll->GetContentWidth(), scroll->GetViewportWidth() + 100.0f) << "the long line is not wrapped";
}

// Over a code block a vertical wheel is the page's: the block neither turns it into a
// sideways scroll nor consumes it, so the conversation around it scrolls (as in Chrome).
TEST(MarkdownTextLayoutTests, AVerticalWheelOverACodeBlockIsLeftToThePage)
{
    MarkdownLayoutFixture fx;
    if (std::string why; !fx.Init(&why))
        GTEST_SKIP() << why;
    fx.Text().SetMarkdown("```\n" + std::string(240, 'y') + "\n```");
    fx.Settle();

    auto* scroll = const_cast<ScrollView*>(FindScrollView(fx.Text()));
    ASSERT_NE(scroll, nullptr);
    ASSERT_GT(scroll->GetContentWidth(), scroll->GetViewportWidth());

    UIEvent wheel{};
    wheel.Id = kEventScroll;
    wheel.ScrollY = 40.0f;
    wheel.Target = const_cast<Label*>(FindLabel(fx.Text(), std::string(240, 'y')));
    wheel.CurrentTarget = scroll;
    scroll->OnEvent(wheel);
    EXPECT_FALSE(wheel.Handled) << "the wheel was consumed by the code block";
    EXPECT_EQ(scroll->GetScrollX(), 0.0f);

    UIEvent sideways{};
    sideways.Id = kEventScroll;
    sideways.ScrollX = 40.0f;
    sideways.Target = wheel.Target;
    sideways.CurrentTarget = scroll;
    scroll->OnEvent(sideways);
    EXPECT_TRUE(sideways.Handled);
    EXPECT_GT(scroll->GetScrollX(), 0.0f) << "a sideways wheel scrolls the code";
}

// A game document that never styles the control still gets it whole from the default sheet:
// words flow in lines and wrap between them, bold is bold, code is monospace.
TEST(MarkdownTextLayoutTests, WithoutAThemeTheWordsStillFlowInLines)
{
    MarkdownLayoutFixture fx;
    if (std::string why; !fx.Init(&why, Theme::None))
        GTEST_SKIP() << why;
    fx.Text().SetMarkdown("The lantern at the gate is **bright** and the torch by the well burns low, "
                          "so the square reads warm at dusk. Use `F` to frame it.");
    fx.Settle();

    const Label* first = FindLabel(fx.Text(), "The ");
    const Label* second = FindLabel(fx.Text(), "lantern ");
    ASSERT_TRUE(first && second);
    EXPECT_NEAR(second->GetLayoutY(), first->GetLayoutY(), 0.5f) << "the first two words share a line";
    EXPECT_GT(second->GetLayoutX(), first->GetLayoutX()) << "the second word follows the first";
    const UIElement& paragraph = *fx.Text().GetChildren().at(0);
    EXPECT_GT(paragraph.GetLayoutHeight(), first->GetLayoutHeight() + 0.5f) << "the paragraph wraps";
    EXPECT_LT(paragraph.GetLayoutHeight(), 8.0f * first->GetLayoutHeight()) << "a line holds more than one word";
    // The wrap is the row's (flex-wrap), not words squeezed into one row and wrapping inside
    // themselves: the last word sits on a later line and every word is one line tall.
    const Label* last = FindLabel(fx.Text(), "it.");
    ASSERT_NE(last, nullptr);
    EXPECT_GT(last->GetLayoutY(), first->GetLayoutY() + 0.5f * first->GetLayoutHeight()) << "the row wraps";
    for (const auto& word : paragraph.GetChildren())
        EXPECT_LT(word->GetLayoutHeight(), 1.5f * first->GetLayoutHeight()) << "a word wrapped inside itself";

    const Label* strong = FindLabel(fx.Text(), "bright ");
    ASSERT_NE(strong, nullptr);
    EXPECT_EQ(strong->GetResolvedStyle().Visual.FontWeight, 700);
    const Label* code = FindLabel(fx.Text(), "F");
    ASSERT_NE(code, nullptr);
    const auto& family = code->GetResolvedStyle().Visual.FontFamily;
    ASSERT_TRUE(family && !family->empty());
    EXPECT_EQ(family->back(), "monospace");
}

// A path wider than the line breaks after its separators and shows whole, and the next word
// goes on after its last part; a long word with no break point wraps inside itself, whole.
// Paragraphs sit a block gap apart and the items of one list closer.
TEST(MarkdownTextLayoutTests, ALongWordWrapsWholeAndListItemsSitCloserThanParagraphs)
{
    MarkdownLayoutFixture fx;
    if (std::string why; !fx.Init(&why))
        GTEST_SKIP() << why;
    const std::string path =
        "C:/Dev/Projects/Village/Assets/Props/Lanterns/Lantern_01_LOD0_Variant_Warm_With_A_Long_Suffix.mesh";
    const std::string unbroken(60, 'x');
    fx.Text().SetMarkdown("Para one.\n\nPara two.\n\n- a\n- b\n\nSee " + path + " now.\n\nThen " + unbroken + " end.");
    fx.Settle();

    const auto& blocks = fx.Text().GetChildren();
    ASSERT_EQ(blocks.size(), 6u);
    const auto gapAbove = [&blocks](size_t index) {
        return blocks[index]->GetLayoutY() - (blocks[index - 1]->GetLayoutY() + blocks[index - 1]->GetLayoutHeight());
    };
    EXPECT_NEAR(gapAbove(1), 10.0f, 0.5f) << "between paragraphs";
    EXPECT_NEAR(gapAbove(2), 10.0f, 0.5f) << "above a list";
    EXPECT_NEAR(gapAbove(3), 3.0f, 0.5f) << "between items of one list";

    const UIElement& paragraph = *blocks[4];
    const Label* see = FindLabel(fx.Text(), "See ");
    const Label* now = FindLabel(fx.Text(), "now.");
    ASSERT_TRUE(see && now);
    const float line = see->GetLayoutHeight();
    std::string shown;
    const UIElement* lastPart = nullptr;
    for (const auto& item : paragraph.GetChildren())
    {
        const auto* label = dynamic_cast<const Label*>(item.get());
        ASSERT_NE(label, nullptr);
        shown += label->GetText();
        EXPECT_LE(label->GetLayoutX() + label->GetLayoutWidth(), paragraph.GetLayoutWidth() + 0.5f) << label->GetText();
        EXPECT_LT(label->GetLayoutHeight(), 1.5f * line) << "a part of the path wraps inside itself: " << label->GetText();
        if (label != now)
            lastPart = label;
    }
    EXPECT_EQ(shown, "See " + path + " now.") << "the path shows whole, in order";
    EXPECT_GT(now->GetLayoutY(), see->GetLayoutY() + 0.5f * line) << "the path took more than one line";
    ASSERT_NE(lastPart, nullptr);
    EXPECT_NEAR(now->GetLayoutY(), lastPart->GetLayoutY(), 0.5f) << "the next word goes on after the path's last part";

    const Label* word = FindLabel(fx.Text(), unbroken);
    ASSERT_NE(word, nullptr) << "a word with no break point is one label, whole";
    EXPECT_LE(word->GetLayoutWidth(), blocks[5]->GetLayoutWidth() + 0.5f) << "it fits the line";
    EXPECT_GT(word->GetLayoutHeight(), 1.5f * line) << "it wraps inside itself";
}

// Words are spaced as written: a word's label holds the space after it, so the next word
// starts a space's width after the previous word's last letter.
TEST(MarkdownTextLayoutTests, WordsKeepTheSpaceBetweenThem)
{
    MarkdownLayoutFixture fx;
    if (std::string why; !fx.Init(&why))
        GTEST_SKIP() << why;
    // The second paragraph's lone "Para" has no space after it: the width of the word alone.
    fx.Text().SetMarkdown("Para one.\n\nPara");
    fx.Settle();

    const Label* spaced = FindLabel(fx.Text(), "Para ");
    const Label* bare = FindLabel(fx.Text(), "Para");
    const Label* next = FindLabel(fx.Text(), "one.");
    ASSERT_TRUE(spaced && bare && next);
    EXPECT_GT(spaced->GetLayoutWidth(), bare->GetLayoutWidth() + 2.0f) << "the label holds its space";
    EXPECT_GE(next->GetLayoutX() - spaced->GetLayoutX(), bare->GetLayoutWidth() + 2.0f)
        << "the next word starts a space after this one";
}
