#pragma once

#include "UI/UIElement.h"

#include <string_view>
#include <vector>

namespace GameEngine
{

namespace UIParsing
{
struct MarkdownBlock;
}

/// Read-only text written in Markdown, drawn as its blocks: paragraphs, headings, bulleted and
/// numbered lists (one level of nesting), fenced code (monospace, scrolling sideways rather than
/// wrapping), quotes, rules and tables, with strong, emphasis, inline code and links inside them
/// (the UI module's Markdown parser defines the subset). A link opens in the browser when its address is
/// http or https; any other address is shown as its text.
///
/// Each block is one child element. Inline text flows as one label per word, so a word keeps
/// its look and the line wraps between words; the element's own font, size, line height and
/// color are inherited by every block. The UI module's default sheet styles what Markdown adds,
/// so the control works in any document; a theme recolors it through the
/// `--ui_color_markdown_*` variables.
class MarkdownText : public UIElement
{
public:
    MarkdownText();
    ~MarkdownText() override;

    /// Shows `markdown`. Blocks before the first one that changed keep their elements, so text
    /// that grows at its end (a streamed reply) rebuilds only its last block.
    void SetMarkdown(std::string_view markdown);

private:
    /// What the children show, one block per child.
    std::vector<UIParsing::MarkdownBlock> m_Blocks;
    /// The child drawing each of m_Blocks. Kept here because RemoveChild may defer, so the
    /// children list can still hold a removed block during event dispatch.
    std::vector<UIElement*> m_BlockElements;
};

} // namespace GameEngine
