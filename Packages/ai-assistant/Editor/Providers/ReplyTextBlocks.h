#pragma once

#include <string>
#include <string_view>

namespace GameEngine
{
/// Joins the text blocks of one reply. A provider streams a reply as blocks (the Messages
/// API's text content blocks, Codex's agent messages) whose deltas carry their own spacing,
/// but nothing marks the boundary between two blocks: the model wrote them as separate
/// paragraphs, typically around a tool call. BeginBlock() marks the boundary; the first text
/// appended after it starts a new paragraph. A block that adds no text adds no break.
class ReplyTextBlocks
{
public:
    /// A new text block starts.
    void BeginBlock() { m_BlockBegun = true; }

    /// Appends `text` to `reply`, preceded by the paragraph break a new block needs, and
    /// returns what was appended: the delta to report. Empty when `text` is.
    std::string_view Append(std::string& reply, std::string_view text);

private:
    bool m_BlockBegun = false;
};
} // namespace GameEngine
