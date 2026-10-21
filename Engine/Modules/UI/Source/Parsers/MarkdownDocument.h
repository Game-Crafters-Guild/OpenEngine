#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine { namespace UIParsing {

/// A run of inline text with one look. Whitespace inside Text is the source's, with soft line
/// breaks already turned into spaces; a '\n' left in Text is a hard line break.
struct MarkdownSpan
{
    std::string Text;
    bool Strong = false;
    bool Emphasis = false;
    /// Inline code: shown in the monospace face, its text taken literally.
    bool Code = false;
    /// The link's destination; empty when the span is not a link.
    std::string Url;

    bool operator==(const MarkdownSpan&) const = default;
};

/// The inline content of one block, in reading order.
using MarkdownInlines = std::vector<MarkdownSpan>;

enum class MarkdownBlockKind : uint8_t
{
    Paragraph,
    Heading,
    ListItem,
    CodeBlock,
    Quote,
    Rule,
    Table,
};

/// One block of a Markdown document: what MarkdownText draws as one child element.
struct MarkdownBlock
{
    MarkdownBlockKind Kind = MarkdownBlockKind::Paragraph;
    /// Heading: its level, 1 to 6.
    uint8_t HeadingLevel = 0;
    /// ListItem: its nesting depth, 0 or 1 (deeper items are shown at depth 1).
    uint8_t ListDepth = 0;
    /// ListItem: numbered rather than bulleted.
    bool Ordered = false;
    /// ListItem on an ordered list: the number its marker shows.
    uint32_t Number = 0;
    /// Paragraph, Heading, ListItem and Quote: the inline content.
    MarkdownInlines Inlines;
    /// CodeBlock: the code, its lines joined by '\n', without a trailing newline.
    std::string Code;
    /// CodeBlock: the first word of the fence's info string ("cpp" for a fence opened with
    /// three backticks and cpp); empty when the fence names none.
    std::string Language;
    /// Table: every row's cells, the header row first; every row has the header's cell count.
    std::vector<std::vector<MarkdownInlines>> Rows;

    bool operator==(const MarkdownBlock&) const = default;
};

/// A parsed Markdown document: its blocks in reading order.
struct MarkdownDocument
{
    std::vector<MarkdownBlock> Blocks;
};

}} // namespace GameEngine::UIParsing
