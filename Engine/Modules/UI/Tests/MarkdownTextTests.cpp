#include <gtest/gtest.h>

#include "FixedScalePlatform.h"
#include "UIRgTestHarness.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/MarkdownText.h"
#include "UI/Controls/ScrollView.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

void CollectLabels(const UIElement& root, std::vector<const Label*>& out)
{
    if (const auto* label = dynamic_cast<const Label*>(&root))
        out.push_back(label);
    for (const auto& child : root.GetChildren())
        if (child)
            CollectLabels(*child, out);
}

// Every word label's text under `root`, in reading order: what the reader sees of the text.
std::string ShownText(const UIElement& root)
{
    std::vector<const Label*> labels;
    CollectLabels(root, labels);
    std::string text;
    for (const Label* label : labels)
        if (label->HasClass("markdown-word") || label->HasClass("markdown-code-text"))
            text += label->GetText();
    return text;
}

Label* LabelWithText(const UIElement& root, const std::string& text)
{
    std::vector<const Label*> labels;
    CollectLabels(root, labels);
    for (const Label* label : labels)
        if (label->GetText() == text)
            return const_cast<Label*>(label);
    return nullptr;
}

const UIElement* FindByClass(const UIElement& root, const std::string& cls)
{
    if (root.HasClass(cls))
        return &root;
    for (const auto& child : root.GetChildren())
        if (const UIElement* found = child ? FindByClass(*child, cls) : nullptr)
            return found;
    return nullptr;
}

const UIElement& Block(const MarkdownText& text, size_t index)
{
    return *text.GetChildren().at(index);
}

void Send(UIElement& target, EventId id)
{
    UIEvent event{};
    event.Id = id;
    event.Button = 0;
    event.Target = &target;
    event.CurrentTarget = &target;
    target.DispatchEvent(event);
}

} // namespace

// The reply from issue #3436 as the panel shows it: one block per Markdown block, the words
// without their markup, each word's look as a class the default sheet draws.
TEST(MarkdownTextTests, EachBlockIsOneChildAndTheWordsCarryTheirLookNotTheirMarkup)
{
    MarkdownText text;
    text.SetMarkdown("## Done\n"
                     "I placed **Lantern_01** with `intensity` 2.\n"
                     "- first\n"
                     "  - nested\n"
                     "```cpp\nlong line of code\n```");

    ASSERT_EQ(text.GetChildren().size(), 5u);
    EXPECT_TRUE(Block(text, 0).HasClass("markdown-heading"));
    EXPECT_TRUE(Block(text, 0).HasClass("markdown-heading-2"));
    EXPECT_TRUE(Block(text, 1).HasClass("markdown-paragraph"));
    EXPECT_TRUE(Block(text, 2).HasClass("markdown-list-item"));
    EXPECT_FALSE(Block(text, 2).HasClass("markdown-list-item-nested"));
    EXPECT_TRUE(Block(text, 3).HasClass("markdown-list-item-nested"));
    EXPECT_TRUE(Block(text, 4).HasClass("markdown-code-block"));
    for (size_t index = 0; index < 5; ++index)
        EXPECT_TRUE(Block(text, index).HasClass("markdown-block")) << index;

    EXPECT_EQ(ShownText(Block(text, 1)), "I placed Lantern_01 with intensity 2.");
    const std::string shown = ShownText(text);
    EXPECT_EQ(shown.find('*'), std::string::npos) << shown;
    EXPECT_EQ(shown.find('`'), std::string::npos) << shown;
    EXPECT_EQ(shown.find('#'), std::string::npos) << shown;

    const Label* strong = LabelWithText(text, "Lantern_01 ");
    ASSERT_NE(strong, nullptr) << "a word keeps the space after it";
    EXPECT_TRUE(strong->HasClass("markdown-word"));
    EXPECT_TRUE(strong->HasClass("markdown-strong"));
    const Label* code = LabelWithText(text, "intensity");
    ASSERT_NE(code, nullptr) << "the space after code stays outside its tint";
    EXPECT_TRUE(code->HasClass("markdown-code"));
    EXPECT_FALSE(code->HasClass("markdown-strong"));

    EXPECT_NE(FindByClass(Block(text, 2), "markdown-bullet"), nullptr) << "a first-level item has a filled dot";
    EXPECT_EQ(FindByClass(Block(text, 2), "markdown-bullet-nested"), nullptr);
    EXPECT_NE(FindByClass(Block(text, 3), "markdown-bullet-nested"), nullptr) << "a nested item has a ring";

    const UIElement& codeBlock = Block(text, 4);
    ASSERT_EQ(codeBlock.GetChildren().size(), 2u) << "a header, then the code";
    EXPECT_TRUE(codeBlock.GetChildren()[0]->HasClass("markdown-code-header"));
    EXPECT_NE(LabelWithText(*codeBlock.GetChildren()[0], "cpp"), nullptr) << "the header names the language";
    EXPECT_NE(dynamic_cast<const ScrollView*>(codeBlock.GetChildren()[1].get()), nullptr)
        << "code scrolls sideways rather than wrapping";
    const Label* codeText = LabelWithText(codeBlock, "long line of code");
    ASSERT_NE(codeText, nullptr);
    EXPECT_TRUE(codeText->HasClass("markdown-code-text"));
}

// A word whose look changes inside it is one group, so a line never breaks between a bold
// name and the comma after it; a hard break is a full-width break between the words.
TEST(MarkdownTextTests, AWordWithTwoLooksIsOneGroupAndAHardBreakBreaksTheLine)
{
    MarkdownText text;
    text.SetMarkdown("**Lantern_01**, then  \nthe torch");
    ASSERT_EQ(text.GetChildren().size(), 1u);
    const UIElement& flow = Block(text, 0);
    ASSERT_EQ(flow.GetChildren().size(), 5u);
    const UIElement& group = *flow.GetChildren()[0];
    EXPECT_TRUE(group.HasClass("markdown-word-group"));
    ASSERT_EQ(group.GetChildren().size(), 2u);
    EXPECT_EQ(static_cast<const Label&>(*group.GetChildren()[0]).GetText(), "Lantern_01");
    EXPECT_TRUE(group.GetChildren()[0]->HasClass("markdown-strong"));
    EXPECT_EQ(static_cast<const Label&>(*group.GetChildren()[1]).GetText(), ", ");
    EXPECT_FALSE(group.GetChildren()[1]->HasClass("markdown-strong"));
    EXPECT_EQ(static_cast<const Label&>(*flow.GetChildren()[1]).GetText(), "then");
    EXPECT_TRUE(flow.GetChildren()[2]->HasClass("markdown-break"));
}

// A code span is one piece, spaces and all: its tint is one chip, never one per word.
TEST(MarkdownTextTests, ACodeSpanIsOnePieceWithItsSpaces)
{
    MarkdownText text;
    text.SetMarkdown("run `set_play_mode enter` now");
    const Label* code = LabelWithText(text, "set_play_mode enter");
    ASSERT_NE(code, nullptr);
    EXPECT_TRUE(code->HasClass("markdown-code"));
    EXPECT_EQ(ShownText(text), "run set_play_mode enter now");
}

// Only a web address is a link: it is styled and named in its tooltip, and hovering any word
// of it lights every word of that link (and no other); any other address is its text alone.
TEST(MarkdownTextTests, OnlyWebAddressesBecomeLinksAndAHoverLightsTheWholeLink)
{
    MarkdownText text;
    text.SetMarkdown("[the lighting guide](https://example.com) and [docs](https://example.org) and "
                     "[file](file:///C:/x.txt)");
    Label* the = LabelWithText(text, "the ");
    Label* guide = LabelWithText(text, "guide");
    Label* docs = LabelWithText(text, "docs");
    ASSERT_TRUE(the && guide && docs);
    EXPECT_TRUE(guide->HasClass("markdown-link"));
    EXPECT_EQ(guide->GetTooltip(), "Open https://example.com in the browser");
    const Label* space = LabelWithText(text, " ");
    ASSERT_NE(space, nullptr) << "the space after a link is a piece of its own";
    EXPECT_FALSE(space->HasClass("markdown-link")) << "so the underline stops at the link's last letter";

    Send(*the, kEventMouseEnter);
    EXPECT_TRUE(the->HasClass("markdown-link-hover"));
    EXPECT_TRUE(guide->HasClass("markdown-link-hover")) << "the whole link lights";
    EXPECT_FALSE(docs->HasClass("markdown-link-hover")) << "another link does not";
    Send(*the, kEventMouseLeave);
    EXPECT_FALSE(guide->HasClass("markdown-link-hover"));

    const Label* file = LabelWithText(text, "file");
    ASSERT_NE(file, nullptr);
    EXPECT_FALSE(file->HasClass("markdown-link"));
    EXPECT_TRUE(file->GetTooltip().empty());
}

// The space above each block follows from the block before it: none above the first, little
// under a heading, a small step between items of one list, a block gap between two lists of
// different kinds and between paragraphs.
TEST(MarkdownTextTests, TheSpaceAboveABlockFollowsTheBlockBeforeIt)
{
    MarkdownText text;
    text.SetMarkdown("# Title\nFirst.\n\nSecond.\n- a\n- b\n1. one\n2. two");
    // Heading, two paragraphs, two bulleted items, two numbered items.
    ASSERT_EQ(text.GetChildren().size(), 7u);
    EXPECT_TRUE(Block(text, 0).HasClass("markdown-block-first"));
    EXPECT_TRUE(Block(text, 1).HasClass("markdown-after-heading"));
    for (const char* cls : {"markdown-block-first", "markdown-after-heading", "markdown-list-continued"})
    {
        EXPECT_FALSE(Block(text, 2).HasClass(cls)) << "paragraph after paragraph: " << cls;
        EXPECT_FALSE(Block(text, 3).HasClass(cls)) << "list after paragraph: " << cls;
        EXPECT_FALSE(Block(text, 5).HasClass(cls)) << "numbered list after bulleted: " << cls;
    }
    EXPECT_TRUE(Block(text, 4).HasClass("markdown-list-continued"));
    EXPECT_TRUE(Block(text, 6).HasClass("markdown-list-continued"));
}

// A code block's Copy puts the code on the UI's clipboard and says so until the pointer
// leaves the button.
TEST(MarkdownTextTests, CopyPutsTheCodeOnTheClipboard)
{
    auto* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "Device init failed";
    UITesting::FixedScalePlatform platform(1.0f);
    UIManager ui(device);
    ui.SetPlatform(&platform);
    auto root = std::make_unique<UIElement>();
    auto owned = std::make_unique<MarkdownText>();
    MarkdownText* text = owned.get();
    root->AddChild(std::move(owned));
    ui.SetRoot(std::move(root));
    text->SetMarkdown("```\nset_play_mode enter\nsave_scene\n```");

    auto* copy = const_cast<Button*>(dynamic_cast<const Button*>(FindByClass(*text, "markdown-code-copy")));
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->GetText(), "Copy");
    Send(*copy, kEventButtonClick);
    EXPECT_EQ(platform.GetClipboardText(), "set_play_mode enter\nsave_scene");
    EXPECT_EQ(copy->GetText(), "Copied");
    Send(*copy, kEventMouseLeave);
    EXPECT_EQ(copy->GetText(), "Copy");
}

// A streamed reply grows at its end: the blocks before the first one that changed keep their
// elements, and only the rest is rebuilt.
TEST(MarkdownTextTests, GrowingTextRebuildsOnlyFromTheFirstChangedBlock)
{
    MarkdownText text;
    text.SetMarkdown("# Title\n\nFirst paragraph.\n\nSecond par");
    ASSERT_EQ(text.GetChildren().size(), 3u);
    // A mark on each element: a kept element still carries it, a rebuilt one does not (an
    // address cannot tell them apart, since a new element may reuse a freed one's).
    for (const auto& child : text.GetChildren())
        child->AddClass("probe-built-first");
    const UIElement* title = text.GetChildren()[0].get();
    const UIElement* first = text.GetChildren()[1].get();

    text.SetMarkdown("# Title\n\nFirst paragraph.\n\nSecond paragraph.\n\n- item");
    ASSERT_EQ(text.GetChildren().size(), 4u);
    EXPECT_EQ(text.GetChildren()[0].get(), title);
    EXPECT_EQ(text.GetChildren()[1].get(), first);
    EXPECT_TRUE(text.GetChildren()[1]->HasClass("probe-built-first"));
    EXPECT_FALSE(text.GetChildren()[2]->HasClass("probe-built-first")) << "the changed block is rebuilt";
    EXPECT_EQ(ShownText(*text.GetChildren()[2]), "Second paragraph.");
    EXPECT_TRUE(text.GetChildren()[3]->HasClass("markdown-list-item"));

    text.SetMarkdown("Replaced");
    ASSERT_EQ(text.GetChildren().size(), 1u);
    EXPECT_EQ(ShownText(text), "Replaced");
}

namespace
{
// The element tree under `root` as text: classes and label text, one line per element.
void Dump(const UIElement& element, std::string& out, int depth)
{
    out += std::string(static_cast<size_t>(depth), ' ');
    for (const std::string& cls : element.GetClasses())
        out += cls + ".";
    if (const auto* label = dynamic_cast<const Label*>(&element))
        out += "[" + label->GetText() + "]";
    out += "\n";
    for (const auto& child : element.GetChildren())
        if (child)
            Dump(*child, out, depth + 1);
}

std::string DumpOf(const MarkdownText& text)
{
    std::string out;
    for (const auto& child : text.GetChildren())
        if (child)
            Dump(*child, out, 0);
    return out;
}
} // namespace

// Text set piece by piece, as a reply streams, ends as the same elements as the whole text
// set once: a marker split across deltas, a split UTF-8 sequence, a fence opened three deltas
// back, a setext underline and a table delimiter row that arrive late (each changes a block
// already built).
TEST(MarkdownTextTests, StreamedDeltasEqualTheOneShotParse)
{
    const std::vector<std::vector<std::string>> streams = {
        {"I placed **Lantern_0", "1** at the gate."},
        {"caf\xC3", "\xA9 is warm. \xE2\x82", "\xAC 5"},
        {"Intro text.\n\n``", "`cpp\nint x = 1;\n", "int y;\n", "```\n\nAfter **bold**.\n\n- item"},
        {"# Title\n\nPara one\n", "===\n\nnext"},
        {"| a | b |\n", "|---|---|\n| 1 | 2 |\n\nafter"},
    };
    for (const auto& deltas : streams)
    {
        MarkdownText streamed;
        std::string text;
        for (const std::string& delta : deltas)
        {
            text += delta;
            streamed.SetMarkdown(text);
        }
        MarkdownText once;
        once.SetMarkdown(text);
        EXPECT_EQ(DumpOf(streamed), DumpOf(once)) << "stream: " << text;
    }
}
