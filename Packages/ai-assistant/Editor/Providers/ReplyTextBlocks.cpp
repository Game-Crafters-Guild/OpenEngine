#include "ReplyTextBlocks.h"

#include <cstddef>

namespace GameEngine
{
namespace
{
// A blank line between two blocks: Markdown's paragraph break.
constexpr size_t kParagraphBreakNewlines = 2;
} // namespace

std::string_view ReplyTextBlocks::Append(std::string& reply, std::string_view text)
{
    if (text.empty())
        return {};
    const size_t start = reply.size();
    if (m_BlockBegun && !reply.empty())
    {
        // A reply that already ends in a newline needs fewer to reach the blank line.
        size_t trailing = 0;
        while (trailing < kParagraphBreakNewlines && trailing < reply.size() &&
               reply[reply.size() - 1 - trailing] == '\n')
            ++trailing;
        reply.append(kParagraphBreakNewlines - trailing, '\n');
    }
    m_BlockBegun = false;
    reply.append(text);
    return std::string_view(reply).substr(start);
}
} // namespace GameEngine
