#pragma once

#include "MarkdownDocument.h"

#include <string_view>

namespace GameEngine { namespace UIParsing {

/// Parses the Markdown subset language models write into a MarkdownDocument.
///
/// Blocks: paragraphs, ATX (`#`) and setext headings, bulleted (`-`, `*`, `+`) and numbered
/// (`1.`, `1)`) list items nested one level deep, fenced code blocks (``` and ~~~), block
/// quotes, thematic breaks and pipe tables. Inlines: strong and emphasis (`*` and `_`, with
/// CommonMark's flanking rules, so `snake_case` and `2 * 3` stay as written), code spans,
/// links (`[text](url)`, `<https://...>`), images (shown as a link to the image), backslash
/// escapes and hard line breaks.
///
/// Anything else is text: an unmatched delimiter shows as the character it is, never as a
/// directive the reader has to decode. A fence still open at the end of the source closes
/// there, so a reply that streams a code block shows it as code while it arrives.
class MarkdownParser
{
public:
    [[nodiscard]] static MarkdownDocument Parse(std::string_view source);
};

}} // namespace GameEngine::UIParsing
