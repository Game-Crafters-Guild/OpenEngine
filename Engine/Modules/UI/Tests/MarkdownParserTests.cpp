#include <gtest/gtest.h>

#include "../Source/Parsers/MarkdownParser.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::UIParsing;

namespace
{

// The block's inline text as a reader sees it, without its looks.
std::string PlainText(const MarkdownInlines& inlines)
{
    std::string text;
    for (const MarkdownSpan& span : inlines)
        text += span.Text;
    return text;
}

MarkdownSpan Span(std::string text, bool strong = false, bool emphasis = false, bool code = false)
{
    MarkdownSpan span;
    span.Text = std::move(text);
    span.Strong = strong;
    span.Emphasis = emphasis;
    span.Code = code;
    return span;
}

MarkdownInlines OneParagraph(std::string_view source)
{
    const MarkdownDocument document = MarkdownParser::Parse(source);
    EXPECT_EQ(document.Blocks.size(), 1u) << source;
    if (document.Blocks.empty())
        return {};
    EXPECT_EQ(document.Blocks[0].Kind, MarkdownBlockKind::Paragraph) << source;
    return document.Blocks[0].Inlines;
}

} // namespace

// The reply from issue #3436: the asterisks are the look, not the text.
TEST(MarkdownParserTests, StrongAndEmphasisTakeTheirLookAndLoseTheirDelimiters)
{
    const MarkdownDocument item = MarkdownParser::Parse("- **Lantern_01** was created as entity 1048584");
    ASSERT_EQ(item.Blocks.size(), 1u);
    EXPECT_EQ(item.Blocks[0].Kind, MarkdownBlockKind::ListItem);
    EXPECT_EQ(item.Blocks[0].Inlines,
              (MarkdownInlines{Span("Lantern_01", true), Span(" was created as entity 1048584")}));

    EXPECT_EQ(OneParagraph("a *soft* and _quiet_ light"),
              (MarkdownInlines{Span("a "), Span("soft", false, true), Span(" and "), Span("quiet", false, true),
                               Span(" light")}));
    EXPECT_EQ(OneParagraph("***both*** and __strong__"),
              (MarkdownInlines{Span("both", true, true), Span(" and "), Span("strong", true)}));
    EXPECT_EQ(OneParagraph("**bold *and italic* inside**"),
              (MarkdownInlines{Span("bold ", true), Span("and italic", true, true), Span(" inside", true)}));
}

// What is not emphasis stays as written: an identifier's underscores, arithmetic, a delimiter
// that never closes (a reply still streaming), a backslash-escaped asterisk.
TEST(MarkdownParserTests, DelimitersThatAreNotEmphasisStayAsText)
{
    EXPECT_EQ(OneParagraph("call set_play_mode on my_entity_name"),
              (MarkdownInlines{Span("call set_play_mode on my_entity_name")}));
    EXPECT_EQ(OneParagraph("2 * 3 * 4 = 24"), (MarkdownInlines{Span("2 * 3 * 4 = 24")}));
    EXPECT_EQ(OneParagraph("**Lanter"), (MarkdownInlines{Span("**Lanter")}));
    EXPECT_EQ(OneParagraph("\\*not emphasis\\*"), (MarkdownInlines{Span("*not emphasis*")}));
}

TEST(MarkdownParserTests, CodeSpansAreLiteralAndMonospace)
{
    EXPECT_EQ(OneParagraph("use `**raw**` and ``a ` b``"),
              (MarkdownInlines{Span("use "), Span("**raw**", false, false, true), Span(" and "),
                               Span("a ` b", false, false, true)}));
    EXPECT_EQ(OneParagraph("an `open code span"), (MarkdownInlines{Span("an `open code span")}));
}

TEST(MarkdownParserTests, HeadingsTakeTheirLevel)
{
    const MarkdownDocument document =
        MarkdownParser::Parse("# The Lantern #\n## Placing it\n#### Deep\nSetext title\n===\n#hashtag");
    ASSERT_EQ(document.Blocks.size(), 5u);
    EXPECT_EQ(document.Blocks[0].Kind, MarkdownBlockKind::Heading);
    EXPECT_EQ(document.Blocks[0].HeadingLevel, 1);
    EXPECT_EQ(PlainText(document.Blocks[0].Inlines), "The Lantern");
    EXPECT_EQ(document.Blocks[1].HeadingLevel, 2);
    EXPECT_EQ(document.Blocks[2].HeadingLevel, 4);
    EXPECT_EQ(document.Blocks[3].Kind, MarkdownBlockKind::Heading);
    EXPECT_EQ(document.Blocks[3].HeadingLevel, 1);
    EXPECT_EQ(PlainText(document.Blocks[3].Inlines), "Setext title");
    EXPECT_EQ(document.Blocks[4].Kind, MarkdownBlockKind::Paragraph) << "no space after #: not a heading";
    EXPECT_EQ(PlainText(document.Blocks[4].Inlines), "#hashtag");
}

// Lists as models write them: right after a paragraph line, numbered, nested one level under
// an item, and a wrapped item's next line continuing it.
TEST(MarkdownParserTests, ListItemsCarryTheirMarkerAndDepth)
{
    const MarkdownDocument document = MarkdownParser::Parse("I placed three lights:\n"
                                                            "1. A lantern\n"
                                                            "2. A torch\n"
                                                            "   - lit\n"
                                                            "      - very lit\n"
                                                            "3. A candle that\n"
                                                            "burns slowly\n"
                                                            "\n"
                                                            "* a bullet\n"
                                                            "+ another");
    ASSERT_EQ(document.Blocks.size(), 8u);
    EXPECT_EQ(document.Blocks[0].Kind, MarkdownBlockKind::Paragraph);
    for (size_t index = 1; index < 8; ++index)
        EXPECT_EQ(document.Blocks[index].Kind, MarkdownBlockKind::ListItem) << index;
    EXPECT_TRUE(document.Blocks[1].Ordered);
    EXPECT_EQ(document.Blocks[1].Number, 1u);
    EXPECT_EQ(document.Blocks[2].Number, 2u);
    EXPECT_EQ(document.Blocks[3].ListDepth, 1) << "nested under the torch";
    EXPECT_FALSE(document.Blocks[3].Ordered);
    EXPECT_EQ(document.Blocks[4].ListDepth, 1) << "deeper nesting is shown one level deep";
    EXPECT_EQ(document.Blocks[5].ListDepth, 0);
    EXPECT_EQ(PlainText(document.Blocks[5].Inlines), "A candle that burns slowly");
    EXPECT_FALSE(document.Blocks[6].Ordered);
    EXPECT_EQ(PlainText(document.Blocks[7].Inlines), "another");

    const MarkdownDocument notAList = MarkdownParser::Parse("It cost\n2. dollars");
    ASSERT_EQ(notAList.Blocks.size(), 1u) << "only an item numbered 1 interrupts a paragraph";
    EXPECT_EQ(PlainText(notAList.Blocks[0].Inlines), "It cost 2. dollars");
}

// A fence keeps its lines and their indentation; one still open when the text ends (a reply
// streaming it) is code up to there.
TEST(MarkdownParserTests, FencedCodeKeepsItsLinesAndAnOpenFenceClosesAtTheEnd)
{
    const MarkdownDocument document =
        MarkdownParser::Parse("Run this:\n```cpp\nint main()\n{\n    return *p;\n}\n```\nDone.\n~~~\nopen **fence**");
    ASSERT_EQ(document.Blocks.size(), 4u);
    EXPECT_EQ(document.Blocks[1].Kind, MarkdownBlockKind::CodeBlock);
    EXPECT_EQ(document.Blocks[1].Code, "int main()\n{\n    return *p;\n}");
    EXPECT_EQ(document.Blocks[1].Language, "cpp") << "the info string's first word";
    EXPECT_EQ(PlainText(document.Blocks[2].Inlines), "Done.");
    EXPECT_EQ(document.Blocks[3].Kind, MarkdownBlockKind::CodeBlock);
    EXPECT_EQ(document.Blocks[3].Code, "open **fence**");
    EXPECT_TRUE(document.Blocks[3].Language.empty()) << "a fence that names no language";

    const MarkdownDocument inItem = MarkdownParser::Parse("- step\n  ```\n  make\n  ```\n- next");
    ASSERT_EQ(inItem.Blocks.size(), 3u);
    EXPECT_EQ(inItem.Blocks[1].Code, "make") << "the fence's indentation is not part of the code";
    EXPECT_EQ(inItem.Blocks[2].Kind, MarkdownBlockKind::ListItem);
}

// A web link keeps its address; a bare address is a link without its sentence's full stop.
TEST(MarkdownParserTests, LinksKeepTheirTextAndAddress)
{
    MarkdownSpan docs = Span("the docs");
    docs.Url = "https://example.com/a_(b)";
    MarkdownSpan bare = Span("https://example.com/x");
    bare.Url = "https://example.com/x";
    EXPECT_EQ(OneParagraph("See [the docs](https://example.com/a_(b) \"Title\") or https://example.com/x."),
              (MarkdownInlines{Span("See "), docs, Span(" or "), bare, Span(".")}));

    MarkdownSpan strongLink = Span("here", true);
    strongLink.Url = "https://example.com";
    EXPECT_EQ(OneParagraph("**[here](https://example.com)**"), (MarkdownInlines{strongLink}));

    MarkdownSpan angle = Span("https://example.com");
    angle.Url = "https://example.com";
    EXPECT_EQ(OneParagraph("<https://example.com>"), (MarkdownInlines{angle}));
    EXPECT_EQ(OneParagraph("[not a link] (x)"), (MarkdownInlines{Span("[not a link] (x)")}));
}

// A single newline joins lines with a space; two trailing spaces or a backslash break the line.
TEST(MarkdownParserTests, SoftBreaksReadAsSpacesAndHardBreaksAsNewlines)
{
    EXPECT_EQ(PlainText(OneParagraph("one\ntwo")), "one two");
    EXPECT_EQ(PlainText(OneParagraph("one  \ntwo\\\nthree")), "one\ntwo\nthree");
    const MarkdownDocument paragraphs = MarkdownParser::Parse("first\n\n\nsecond");
    EXPECT_EQ(paragraphs.Blocks.size(), 2u);
}

TEST(MarkdownParserTests, QuotesRulesAndTables)
{
    const MarkdownDocument document = MarkdownParser::Parse("> quoted **text**\n> more\n>\n> after\n"
                                                            "\n---\n\n"
                                                            "| Light | Intensity |\n"
                                                            "| --- | :-: |\n"
                                                            "| `Lantern` | 2.5 |\n"
                                                            "| Torch |\n"
                                                            "after the table");
    ASSERT_EQ(document.Blocks.size(), 4u);
    EXPECT_EQ(document.Blocks[0].Kind, MarkdownBlockKind::Quote);
    EXPECT_EQ(PlainText(document.Blocks[0].Inlines), "quoted text more\nafter");
    EXPECT_EQ(document.Blocks[1].Kind, MarkdownBlockKind::Rule);
    const MarkdownBlock& table = document.Blocks[2];
    ASSERT_EQ(table.Kind, MarkdownBlockKind::Table);
    ASSERT_EQ(table.Rows.size(), 3u);
    EXPECT_EQ(PlainText(table.Rows[0][1]), "Intensity");
    EXPECT_EQ(table.Rows[1][0], (MarkdownInlines{Span("Lantern", false, false, true)}));
    ASSERT_EQ(table.Rows[2].size(), 2u) << "a short row is padded to the header's columns";
    EXPECT_TRUE(table.Rows[2][1].empty());
    EXPECT_EQ(document.Blocks[3].Kind, MarkdownBlockKind::Paragraph);

    const MarkdownDocument pipes = MarkdownParser::Parse("a | b\nnot a delimiter");
    ASSERT_EQ(pipes.Blocks.size(), 1u);
    EXPECT_EQ(pipes.Blocks[0].Kind, MarkdownBlockKind::Paragraph);
}

namespace
{
std::string Repeat(std::string_view unit, size_t count)
{
    std::string text;
    text.reserve(unit.size() * count);
    for (size_t index = 0; index < count; ++index)
        text += unit;
    return text;
}

double ParseMilliseconds(const std::string& text, MarkdownDocument& out)
{
    const auto start = std::chrono::steady_clock::now();
    out = MarkdownParser::Parse(text);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

size_t TableCells(const MarkdownDocument& document)
{
    size_t cells = 0;
    for (const MarkdownBlock& block : document.Blocks)
        for (const auto& row : block.Rows)
            cells += row.size();
    return cells;
}
} // namespace

// Link text nested 10,000 deep is parsed without recursion: the innermost link is a link, every
// outer bracket is text (links do not nest), and the stack is untouched by the depth.
TEST(MarkdownParserTests, LinkTextNestedTenThousandDeepIsParsedFlat)
{
    constexpr size_t kDepth = 10000;
    const std::string text = std::string(kDepth, '[') + "x" + Repeat("](https://a.example)", kDepth);
    const MarkdownInlines inlines = OneParagraph(text);
    MarkdownSpan link = Span("x");
    link.Url = "https://a.example";
    ASSERT_GE(inlines.size(), 3u);
    EXPECT_EQ(inlines[0], Span(std::string(kDepth - 1, '[')));
    EXPECT_EQ(inlines[1], link);
    // The outer brackets' tails stay as written (their bare addresses become links of their own).
    EXPECT_EQ(PlainText(inlines), std::string(kDepth - 1, '[') + "x" + Repeat("](https://a.example)", kDepth - 1));
}

// Hostile replies (the review of #3950 measured each at 5 to 18 s, or a stack overflow) parse
// in linear time: each input at its full size costs under 8 times the same input at a quarter
// of it (linear is about 4, quadratic about 16), and under a second. Wall-clock time on a
// shared machine moves several-fold from run to run, so the full-size times are printed, not
// held to a figure; the ratio of two runs taken back to back is what holds.
TEST(MarkdownParserTests, HostileInputsParseInLinearTime)
{
    constexpr double kMaxScaling = 8.0;
    // Times below this are too short for a ratio to mean anything.
    constexpr double kNoiseFloorMs = 2.0;
    constexpr double kMaxFullSizeMs = 1000.0;
    const auto backtickRuns = [](size_t count) {
        std::string text;
        for (size_t length = 1; length <= count; ++length)
            text += std::string(length, '`') + "a";
        return text;
    };
    // Each input at a quarter of the reviewer's size, then at the full size.
    const std::vector<std::pair<const char*, std::pair<std::string, std::string>>> inputs = {
        {"'[' x 100,000", {std::string(25000, '['), std::string(100000, '[')}},
        {"'[a](' x 25,000", {Repeat("[a](", 6250), Repeat("[a](", 25000)}},
        {"'a* ' x 100,000", {Repeat("a* ", 25000), Repeat("a* ", 100000)}},
        {"'**a ' x 50,000 then 'a** ' x 50,000",
         {Repeat("**a ", 12500) + Repeat("a** ", 12500), Repeat("**a ", 50000) + Repeat("a** ", 50000)}},
        {"nested link text, depth 10,000",
         {std::string(2500, '[') + "x" + Repeat("](u)", 2500), std::string(10000, '[') + "x" + Repeat("](u)", 10000)}},
        {"unmatched backtick runs 1..1,400", {backtickRuns(700), backtickRuns(1400)}},
        // Every link close deactivates the brackets below it: once each, not once per link.
        {"'[' x 500,000 then '[a](u)' x 83,000",
         {std::string(125000, '[') + Repeat("[a](u)", 20750), std::string(500000, '[') + Repeat("[a](u)", 83000)}},
        // '_' closers that find no '_' opener must not search back through every '*' opener.
        {"'*a ' x 50,000 then 'a_ ' x 50,000",
         {Repeat("*a ", 12500) + Repeat("a_ ", 12500), Repeat("*a ", 50000) + Repeat("a_ ", 50000)}},
    };
    for (const auto& [name, texts] : inputs)
    {
        const auto fastest = [](const std::string& text) {
            MarkdownDocument document;
            double ms = ParseMilliseconds(text, document);
            for (int run = 1; run < 3; ++run)
                ms = std::min(ms, ParseMilliseconds(text, document));
            EXPECT_FALSE(document.Blocks.empty());
            return ms;
        };
        const double quarter = fastest(texts.first);
        const double full = fastest(texts.second);
        std::printf("[markdown] %s (%zu bytes): %.1f ms; a quarter of it: %.1f ms (fastest of 3)\n", name,
                    texts.second.size(), full, quarter);
        EXPECT_LT(full, kMaxScaling * std::max(quarter, kNoiseFloorMs)) << name;
        EXPECT_LT(full, kMaxFullSizeMs) << name;
    }
}

// A table header wider than 32 columns is no table, so a short header over many one-pipe
// lines (108 KB that once grew into 100,002,000 cells) stays text.
TEST(MarkdownParserTests, AHeaderWiderThanThirtyTwoColumnsIsText)
{
    const std::string text = Repeat("| h ", 2000) + "|\n" + Repeat("|---", 2000) + "|\n" + Repeat("|\n", 50000);
    MarkdownDocument document;
    const double ms = ParseMilliseconds(text, document);
    std::printf("[markdown] table bomb (%zu bytes): %.1f ms, %zu cells\n", text.size(), ms, TableCells(document));
    EXPECT_EQ(TableCells(document), 0u);
    for (const MarkdownBlock& block : document.Blocks)
        EXPECT_NE(block.Kind, MarkdownBlockKind::Table);
}

// Short rows are filled with empty cells only up to 1,024 of them: the row that would pass
// that ends the table, and the lines from there on are text.
TEST(MarkdownParserTests, ShortRowsArePaddedOnlyWithinABudget)
{
    const std::string text = Repeat("| h ", 32) + "|\n" + Repeat("|---", 32) + "|\n" + Repeat("| c |\n", 100);
    const MarkdownDocument document = MarkdownParser::Parse(text);
    ASSERT_GE(document.Blocks.size(), 2u);
    const MarkdownBlock& table = document.Blocks[0];
    ASSERT_EQ(table.Kind, MarkdownBlockKind::Table);
    const size_t padded = (table.Rows.size() - 1) * 31;
    EXPECT_LE(padded, 1024u);
    EXPECT_EQ(table.Rows.size() - 1, 1024u / 31) << "every row that fits the budget is kept";
    EXPECT_EQ(document.Blocks[1].Kind, MarkdownBlockKind::Paragraph) << "the rest is text";
}

// An address in angle brackets holds no line break and no control character; only a web
// address is a link, any other shows as its text.
TEST(MarkdownParserTests, OnlyWholeWebAddressesBecomeLinks)
{
    for (const char* text : {"<https://a.example\nb>", "[a](<https://a.example\nb>)", "<https://a.example\x01>",
                             "[a](<https://a.example\x01>)", "[file](file:///C:/x.txt)", "<mailto:a@b.example>"})
    {
        for (const MarkdownSpan& span : OneParagraph(text))
            EXPECT_TRUE(span.Url.empty()) << text;
    }
    EXPECT_EQ(PlainText(OneParagraph("<mailto:a@b.example>")), "mailto:a@b.example");
}
