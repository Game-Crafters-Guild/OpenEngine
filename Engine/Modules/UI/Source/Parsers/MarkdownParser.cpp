#include "MarkdownParser.h"

#include "MarkdownSyntax.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine { namespace UIParsing {

namespace
{

// A line indented by more than this many columns does not open a block (CommonMark 4).
constexpr size_t kMaxBlockIndent = 3;
// An item indented at least this far under another item nests in it.
constexpr size_t kNestedItemIndent = 2;
// The deepest list level MarkdownText draws; deeper items are shown at this one.
constexpr uint8_t kMaxListDepth = 1;
// CommonMark caps an ordered marker at nine digits, so its number fits 32 bits.
constexpr size_t kMaxOrderedMarkerDigits = 9;
// A tab advances to the next multiple of four columns.
constexpr size_t kTabStop = 4;
// The shortest code fence.
constexpr size_t kMinFenceLength = 3;
// The fewest marks a thematic break has.
constexpr size_t kMinRuleMarks = 3;
// The widest table drawn as a table: every cell is an element, and a panel cannot show more
// columns than this readably. A wider header and its rows stay text.
constexpr size_t kMaxTableColumns = 32;
// The most empty cells a table adds to fill its short rows (cmark-gfm bounds the same thing):
// a few short lines must not grow into millions of cells. The row that would pass it ends the
// table, and the lines from there on are text.
constexpr size_t kMaxTablePaddingCells = 1024;

bool IsDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool IsBlank(std::string_view line)
{
    return std::all_of(line.begin(), line.end(), IsWhitespace);
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && IsWhitespace(text.front()))
        text.remove_prefix(1);
    return TrimEnd(text);
}

// A line's indentation in columns, and where its content starts.
struct Indentation
{
    size_t Columns = 0;
    size_t ContentStart = 0;
};

Indentation MeasureIndent(std::string_view line)
{
    Indentation indent;
    while (indent.ContentStart < line.size() && IsSpaceOrTab(line[indent.ContentStart]))
    {
        indent.Columns = line[indent.ContentStart] == '\t' ? (indent.Columns / kTabStop + 1) * kTabStop
                                                          : indent.Columns + 1;
        ++indent.ContentStart;
    }
    return indent;
}

std::vector<std::string_view> SplitLines(std::string_view source)
{
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= source.size())
    {
        size_t end = source.find('\n', start);
        if (end == std::string_view::npos)
            end = source.size();
        std::string_view line = source.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

struct ListMarker
{
    bool Ordered = false;
    uint32_t Number = 0;
    // Where the item's text starts in the line.
    size_t ContentStart = 0;
    bool Empty = true;
};

std::optional<ListMarker> ReadListMarker(std::string_view line, size_t at)
{
    ListMarker marker;
    size_t after = at;
    if (at < line.size() && (line[at] == '-' || line[at] == '*' || line[at] == '+'))
    {
        after = at + 1;
    }
    else
    {
        size_t digits = 0;
        uint32_t number = 0;
        while (at + digits < line.size() && IsDigit(line[at + digits]) && digits < kMaxOrderedMarkerDigits)
        {
            number = number * 10 + static_cast<uint32_t>(line[at + digits] - '0');
            ++digits;
        }
        if (digits == 0 || at + digits >= line.size() || (line[at + digits] != '.' && line[at + digits] != ')'))
            return std::nullopt;
        marker.Ordered = true;
        marker.Number = number;
        after = at + digits + 1;
    }
    if (after < line.size() && !IsSpaceOrTab(line[after]))
        return std::nullopt;
    marker.ContentStart = after;
    while (marker.ContentStart < line.size() && IsSpaceOrTab(line[marker.ContentStart]))
        ++marker.ContentStart;
    marker.Empty = marker.ContentStart >= line.size();
    return marker;
}

bool IsThematicBreak(std::string_view content)
{
    if (content.empty() || (content[0] != '-' && content[0] != '*' && content[0] != '_'))
        return false;
    size_t marks = 0;
    for (const char c : content)
    {
        if (c == content[0])
            ++marks;
        else if (!IsSpaceOrTab(c))
            return false;
    }
    return marks >= kMinRuleMarks;
}

// A setext underline's heading level: 1 for `===`, 2 for `---`; 0 when the line is not one.
uint8_t SetextLevel(std::string_view content)
{
    const std::string_view marks = TrimEnd(content);
    if (marks.empty() || (marks[0] != '=' && marks[0] != '-'))
        return 0;
    if (!std::all_of(marks.begin(), marks.end(), [&](char c) { return c == marks[0]; }))
        return 0;
    return marks[0] == '=' ? 1 : 2;
}

std::vector<std::string_view> SplitTableCells(std::string_view line)
{
    std::string_view row = Trim(line);
    if (row.starts_with('|'))
        row.remove_prefix(1);
    if (row.ends_with('|') && !row.ends_with("\\|"))
        row.remove_suffix(1);
    std::vector<std::string_view> cells;
    size_t start = 0;
    for (size_t at = 0; at <= row.size(); ++at)
    {
        if (at < row.size() && row[at] == '\\')
        {
            ++at;
            continue;
        }
        if (at == row.size() || row[at] == '|')
        {
            cells.push_back(Trim(row.substr(start, at - start)));
            start = at + 1;
        }
    }
    return cells;
}

// The delimiter row under a table's header: `| --- | :-: |`.
bool IsTableDelimiterRow(std::string_view line, size_t columns)
{
    if (line.find('-') == std::string_view::npos)
        return false;
    const std::vector<std::string_view> cells = SplitTableCells(line);
    if (cells.size() != columns)
        return false;
    return std::all_of(cells.begin(), cells.end(), [](std::string_view cell) {
        if (cell.starts_with(':'))
            cell.remove_prefix(1);
        if (cell.ends_with(':'))
            cell.remove_suffix(1);
        return !cell.empty() && std::all_of(cell.begin(), cell.end(), [](char c) { return c == '-'; });
    });
}

class BlockParser
{
public:
    MarkdownDocument Parse(std::string_view source)
    {
        for (const std::string_view line : SplitLines(source))
            ReadLine(line);
        CloseFence();
        CloseOpenBlock();
        return std::move(m_Document);
    }

private:
    enum class OpenBlock : uint8_t
    {
        None,
        Paragraph,
        ListItem,
        Quote,
        Table,
    };

    void ReadLine(std::string_view line)
    {
        if (m_InFence)
        {
            ReadFenceLine(line);
            return;
        }
        if (IsBlank(line))
        {
            CloseOpenBlock();
            return;
        }

        const Indentation indent = MeasureIndent(line);
        const std::string_view content = line.substr(indent.ContentStart);
        // Inside a list a fence or a heading may be indented under its item.
        const size_t maxIndent = m_InList ? kMaxBlockIndent + kTabStop : kMaxBlockIndent;
        if (indent.Columns <= maxIndent && TryOpenFence(content, indent.Columns))
            return;
        if (m_Open == OpenBlock::Table && TryTableRow(content))
            return;
        if (indent.Columns <= kMaxBlockIndent && TrySetextUnderline(content))
            return;
        if (indent.Columns <= maxIndent && TryHeading(content))
            return;
        if (indent.Columns <= maxIndent && IsThematicBreak(content))
        {
            CloseOpenBlock();
            m_InList = false;
            MarkdownBlock rule;
            rule.Kind = MarkdownBlockKind::Rule;
            m_Document.Blocks.push_back(std::move(rule));
            return;
        }
        if (TryListItem(line, indent))
            return;
        if (indent.Columns <= maxIndent && TryQuote(content))
            return;
        if (m_Open == OpenBlock::Paragraph && TryTableHeader(content))
            return;

        // A continuation of the open block (lazily, for an item or a quote), or a new paragraph.
        if (m_Open == OpenBlock::None || m_Open == OpenBlock::Table)
        {
            CloseOpenBlock();
            if (indent.Columns < kNestedItemIndent)
                m_InList = false;
            m_Open = OpenBlock::Paragraph;
            m_Block = MarkdownBlock{};
            m_Text = std::string(content);
            return;
        }
        m_Text += '\n';
        m_Text += content;
    }

    bool TryOpenFence(std::string_view content, size_t indentColumns)
    {
        if (content.empty() || (content[0] != '`' && content[0] != '~'))
            return false;
        const size_t length = CountRun(content, 0, content[0]);
        if (length < kMinFenceLength)
            return false;
        // A backtick fence's info string cannot hold a backtick (it would be a code span).
        if (content[0] == '`' && content.find('`', length) != std::string_view::npos)
            return false;
        CloseOpenBlock();
        m_InFence = true;
        m_FenceChar = content[0];
        m_FenceLength = length;
        m_FenceIndent = indentColumns;
        m_Code.clear();
        m_FenceLines = 0;
        const std::string_view info = Trim(content.substr(length));
        m_FenceLanguage = std::string(info.substr(0, info.find_first_of(" \t")));
        return true;
    }

    void ReadFenceLine(std::string_view line)
    {
        const Indentation indent = MeasureIndent(line);
        const std::string_view content = line.substr(indent.ContentStart);
        if (indent.Columns <= m_FenceIndent + kMaxBlockIndent && !content.empty() && content[0] == m_FenceChar &&
            CountRun(content, 0, m_FenceChar) >= m_FenceLength && IsBlank(content.substr(CountRun(content, 0, m_FenceChar))))
        {
            CloseFence();
            return;
        }
        // The fence's own indentation is removed from each of its lines.
        size_t strip = 0;
        while (strip < line.size() && strip < m_FenceIndent && line[strip] == ' ')
            ++strip;
        if (m_FenceLines++ > 0)
            m_Code += '\n';
        m_Code += line.substr(strip);
    }

    void CloseFence()
    {
        if (!m_InFence)
            return;
        m_InFence = false;
        MarkdownBlock code;
        code.Kind = MarkdownBlockKind::CodeBlock;
        code.Code = std::move(m_Code);
        code.Language = std::move(m_FenceLanguage);
        m_Code.clear();
        m_FenceLanguage.clear();
        m_Document.Blocks.push_back(std::move(code));
    }

    bool TrySetextUnderline(std::string_view content)
    {
        if (m_Open != OpenBlock::Paragraph)
            return false;
        const uint8_t level = SetextLevel(content);
        if (level == 0)
            return false;
        MarkdownBlock heading;
        heading.Kind = MarkdownBlockKind::Heading;
        heading.HeadingLevel = level;
        heading.Inlines = ParseMarkdownInlines(m_Text);
        m_Open = OpenBlock::None;
        m_Text.clear();
        m_Document.Blocks.push_back(std::move(heading));
        return true;
    }

    bool TryHeading(std::string_view content)
    {
        const size_t hashes = CountRun(content, 0, '#');
        constexpr size_t kMaxHeadingLevel = 6;
        if (hashes == 0 || hashes > kMaxHeadingLevel || (hashes < content.size() && !IsSpaceOrTab(content[hashes])))
            return false;
        std::string_view title = Trim(content.substr(hashes));
        // A closing sequence of '#' after a space is not part of the title.
        const size_t closing = title.find_last_not_of('#');
        if (closing == std::string_view::npos)
            title = {};
        else if (closing + 1 < title.size() && IsSpaceOrTab(title[closing]))
            title = TrimEnd(title.substr(0, closing + 1));

        CloseOpenBlock();
        m_InList = false;
        MarkdownBlock heading;
        heading.Kind = MarkdownBlockKind::Heading;
        heading.HeadingLevel = static_cast<uint8_t>(hashes);
        heading.Inlines = ParseMarkdownInlines(title);
        m_Document.Blocks.push_back(std::move(heading));
        return true;
    }

    bool TryListItem(std::string_view line, const Indentation& indent)
    {
        const bool nested = m_InList && indent.Columns >= kNestedItemIndent;
        if (indent.Columns > kMaxBlockIndent && !nested)
            return false;
        const std::optional<ListMarker> marker = ReadListMarker(line, indent.ContentStart);
        if (!marker)
            return false;
        // Only a non-empty bullet or an item numbered 1 interrupts a paragraph (CommonMark 5.2).
        if (m_Open == OpenBlock::Paragraph && (marker->Empty || (marker->Ordered && marker->Number != 1)))
            return false;

        CloseOpenBlock();
        m_InList = true;
        m_Open = OpenBlock::ListItem;
        m_Block = MarkdownBlock{};
        m_Block.Kind = MarkdownBlockKind::ListItem;
        m_Block.ListDepth = nested ? kMaxListDepth : 0;
        m_Block.Ordered = marker->Ordered;
        m_Block.Number = marker->Number;
        m_Text = std::string(line.substr(marker->ContentStart));
        return true;
    }

    bool TryQuote(std::string_view content)
    {
        if (content.empty() || content[0] != '>')
            return false;
        std::string_view quoted = content.substr(1);
        if (!quoted.empty() && IsSpaceOrTab(quoted[0]))
            quoted.remove_prefix(1);
        if (m_Open == OpenBlock::Quote)
        {
            if (IsBlank(quoted))
            {
                m_Text += '\n';
                return true;
            }
            m_Text += '\n';
            m_Text += quoted;
            return true;
        }
        CloseOpenBlock();
        m_InList = false;
        m_Open = OpenBlock::Quote;
        m_Block = MarkdownBlock{};
        m_Block.Kind = MarkdownBlockKind::Quote;
        m_Text = std::string(quoted);
        return true;
    }

    // A one-line paragraph with pipes followed by a delimiter row is a table's header.
    bool TryTableHeader(std::string_view content)
    {
        if (m_Text.find('\n') != std::string::npos || m_Text.find('|') == std::string::npos)
            return false;
        const std::vector<std::string_view> header = SplitTableCells(m_Text);
        if (header.size() > kMaxTableColumns || !IsTableDelimiterRow(content, header.size()))
            return false;
        m_Block = MarkdownBlock{};
        m_Block.Kind = MarkdownBlockKind::Table;
        m_Block.Rows.emplace_back();
        for (const std::string_view cell : header)
            m_Block.Rows.back().push_back(ParseMarkdownInlines(cell));
        m_Open = OpenBlock::Table;
        m_TablePadding = 0;
        m_Text.clear();
        m_InList = false;
        return true;
    }

    bool TryTableRow(std::string_view content)
    {
        if (content.find('|') == std::string_view::npos)
            return false;
        // The delimiter row's count: longer rows lose their extra cells, shorter ones are
        // filled with empty cells while the table's padding budget lasts.
        const size_t columns = m_Block.Rows.front().size();
        std::vector<std::string_view> cells = SplitTableCells(content);
        if (cells.size() < columns)
        {
            m_TablePadding += columns - cells.size();
            if (m_TablePadding > kMaxTablePaddingCells)
                return false;
        }
        cells.resize(columns);
        m_Block.Rows.emplace_back();
        for (const std::string_view cell : cells)
            m_Block.Rows.back().push_back(ParseMarkdownInlines(cell));
        return true;
    }

    void CloseOpenBlock()
    {
        switch (m_Open)
        {
        case OpenBlock::None: return;
        case OpenBlock::Table: break;
        case OpenBlock::Paragraph:
        case OpenBlock::ListItem:
        case OpenBlock::Quote: m_Block.Inlines = ParseMarkdownInlines(m_Text); break;
        }
        m_Document.Blocks.push_back(std::move(m_Block));
        m_Block = MarkdownBlock{};
        m_Text.clear();
        m_Open = OpenBlock::None;
    }

    MarkdownDocument m_Document;
    OpenBlock m_Open = OpenBlock::None;
    // The open block, its inline source gathered in m_Text until it closes.
    MarkdownBlock m_Block;
    std::string m_Text;
    // A list item came last, past blank lines: an indented item nests under it.
    bool m_InList = false;
    // Empty cells the open table has added to its short rows (kMaxTablePaddingCells).
    size_t m_TablePadding = 0;

    bool m_InFence = false;
    char m_FenceChar = 0;
    size_t m_FenceLength = 0;
    size_t m_FenceIndent = 0;
    size_t m_FenceLines = 0;
    std::string m_Code;
    std::string m_FenceLanguage;
};

} // namespace

MarkdownDocument MarkdownParser::Parse(std::string_view source)
{
    return BlockParser().Parse(source);
}

}} // namespace GameEngine::UIParsing
