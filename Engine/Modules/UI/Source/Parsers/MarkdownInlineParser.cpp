#include "MarkdownSyntax.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine { namespace UIParsing {

namespace
{

// Every step below is linear in the text (amortized): the parser runs on the UI thread, once
// per streamed delta, over model output nobody vetted. The CommonMark reference algorithms
// give that bound: a bracket stack instead of a scan per '[', a delimiter list whose openers
// are searched no further back than the last failed search (openers_bottom), matched spans
// recorded as intervals and applied in one sweep, and backtick runs indexed by length. No
// step recurses, so nesting depth costs no stack. Nodes hold offsets into one text pool and
// spans are built once at the end, which keeps the constant small in unoptimized builds too.

// Trailing spaces before a newline that make it a hard line break.
constexpr size_t kHardBreakSpaces = 2;
// CommonMark lets an implementation limit parentheses nested in a link destination (it asks
// for at least three levels; a real address nests one, as in a "(disambiguation)" page). The
// limit also bounds how far each failed destination is scanned, so a reply of unclosed
// destinations costs a few characters per link, not the rest of the reply.
constexpr int kMaxDestinationParens = 8;

constexpr size_t kNoNode = static_cast<size_t>(-1);

bool IsControl(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7F;
}

// Characters that may start inline syntax; every other byte is text, copied in runs.
constexpr std::array<bool, 256> MakeSyntaxTable()
{
    std::array<bool, 256> table{};
    for (const char c : std::string_view("\\`*_[!]<\nh"))
        table[static_cast<unsigned char>(c)] = true;
    return table;
}
constexpr std::array<bool, 256> kStartsSyntax = MakeSyntaxTable();

// One piece of a block's inline content before emphasis and links are resolved: text (a slice
// of the parser's pool) with its look, or a run of `*` or `_` that may open or close emphasis.
struct InlineNode
{
    size_t TextStart = 0;
    size_t TextLength = 0;
    bool Code = false;
    char Delimiter = 0;
    size_t Count = 0;
    size_t OriginalCount = 0;
    bool CanOpen = false;
    bool CanClose = false;
    // Neighbours in the delimiter list (delimiter nodes only); kNoNode at the ends.
    size_t PreviousDelimiter = kNoNode;
    size_t NextDelimiter = kNoNode;
    bool InDelimiterList = false;
};

// An open '[' (or '![') waiting for its ']'.
struct BracketOpener
{
    size_t Node = 0;
    // The last delimiter before the bracket: emphasis inside the link text is resolved above it.
    size_t DelimiterBottom = kNoNode;
    bool Active = true;
};

// A matched emphasis pair: the nodes strictly between Opener and Closer take the look.
struct EmphasisPair
{
    size_t Opener = 0;
    size_t Closer = 0;
};

// The nodes [Begin, End) are the text of a link to Url.
struct LinkNodes
{
    size_t Begin = 0;
    size_t End = 0;
    std::string Url;
};

struct LinkTail
{
    std::string Url;
    size_t End = 0;
};

// The length of the bare web address starting at `at` (GitHub's autolink extension), 0 when
// none starts there. Trailing sentence punctuation is not part of it.
size_t BareUrlLength(std::string_view text, size_t at)
{
    const std::string_view rest = text.substr(at);
    if (!IsWebAddress(rest))
        return 0;
    size_t end = 0;
    while (end < rest.size() && !IsWhitespace(rest[end]) && rest[end] != '<' && !IsControl(rest[end]))
        ++end;
    constexpr std::string_view kTrailingPunctuation = ".,;:!?)'\"*_";
    while (end > 0 && kTrailingPunctuation.find(rest[end - 1]) != std::string_view::npos)
        --end;
    const size_t scheme = rest.starts_with("https://") ? std::string_view("https://").size()
                                                         : std::string_view("http://").size();
    return end > scheme ? end : 0;
}

class InlineParser
{
public:
    explicit InlineParser(std::string_view text) : m_Text(text)
    {
        m_Pool.reserve(text.size());
    }

    MarkdownInlines Parse()
    {
        while (m_At < m_Text.size())
            ReadNext();
        FlushText();
        ResolveEmphasis(kNoNode);
        return Flatten();
    }

private:
    void ReadNext()
    {
        const char c = m_Text[m_At];
        switch (c)
        {
        case '\\': ReadEscape(); return;
        case '`': ReadCodeSpan(); return;
        case '*':
        case '_': ReadDelimiterRun(c); return;
        case '[': OpenBracket(1); return;
        case '!':
            if (m_At + 1 < m_Text.size() && m_Text[m_At + 1] == '[')
            {
                OpenBracket(2);
                return;
            }
            break;
        case ']': CloseBracket(); return;
        case '<': ReadAutolink(); return;
        case '\n': ReadLineBreak(); return;
        case 'h':
            if (ReadBareUrl())
                return;
            break;
        default: ReadTextRun(); return;
        }
        m_Pool.push_back(c);
        ++m_At;
    }

    // Copies the run of ordinary characters at m_At in one step.
    void ReadTextRun()
    {
        size_t end = m_At + 1;
        while (end < m_Text.size() && !kStartsSyntax[static_cast<unsigned char>(m_Text[end])])
            ++end;
        m_Pool.append(m_Text.data() + m_At, end - m_At);
        m_At = end;
    }

    void ReadEscape()
    {
        if (m_At + 1 < m_Text.size() && IsAsciiPunctuation(m_Text[m_At + 1]))
        {
            m_Pool.push_back(m_Text[m_At + 1]);
            m_At += 2;
            return;
        }
        if (m_At + 1 < m_Text.size() && m_Text[m_At + 1] == '\n')
        {
            m_Pool.push_back('\n');
            m_At += 2;
            SkipLineIndent();
            return;
        }
        m_Pool.push_back('\\');
        ++m_At;
    }

    // ---- Code spans ----

    // Start positions of every backtick run, by run length, in text order; built on the
    // first backtick, so a closing run is found by a binary search, not a scan.
    void IndexBacktickRuns()
    {
        m_BacktickRunsIndexed = true;
        for (size_t at = 0; at < m_Text.size();)
        {
            if (m_Text[at] != '`')
            {
                ++at;
                continue;
            }
            const size_t length = CountRun(m_Text, at, '`');
            m_BacktickRuns[length].push_back(at);
            at += length;
        }
    }

    std::optional<size_t> FindClosingRun(size_t length, size_t after)
    {
        if (!m_BacktickRunsIndexed)
            IndexBacktickRuns();
        const auto runs = m_BacktickRuns.find(length);
        if (runs == m_BacktickRuns.end())
            return std::nullopt;
        const auto next = std::lower_bound(runs->second.begin(), runs->second.end(), after);
        if (next == runs->second.end())
            return std::nullopt;
        return *next;
    }

    void ReadCodeSpan()
    {
        const size_t open = CountRun(m_Text, m_At, '`');
        const std::optional<size_t> close = FindClosingRun(open, m_At + open);
        if (!close)
        {
            m_Pool.append(open, '`');
            m_At += open;
            return;
        }
        std::string_view code = m_Text.substr(m_At + open, *close - m_At - open);
        const bool padded = code.size() >= 2 && (code.front() == ' ' || code.front() == '\n') &&
                            (code.back() == ' ' || code.back() == '\n') &&
                            code.find_first_not_of(" \n") != std::string_view::npos;
        if (padded)
            code = code.substr(1, code.size() - 2);
        FlushText();
        const size_t start = m_Pool.size();
        m_Pool.append(code);
        std::replace(m_Pool.begin() + static_cast<std::ptrdiff_t>(start), m_Pool.end(), '\n', ' ');
        InlineNode node;
        node.TextStart = start;
        node.TextLength = code.size();
        node.Code = true;
        m_Nodes.push_back(node);
        m_PendingStart = m_Pool.size();
        m_At = *close + open;
    }

    // ---- Emphasis delimiters ----

    void ReadDelimiterRun(char delimiter)
    {
        const size_t count = CountRun(m_Text, m_At, delimiter);
        const char before = m_At == 0 ? ' ' : m_Text[m_At - 1];
        const char after = m_At + count >= m_Text.size() ? ' ' : m_Text[m_At + count];
        // Bytes of a multi-byte UTF-8 sequence count as letters: neither space nor punctuation.
        const bool beforeSpace = IsWhitespace(before);
        const bool afterSpace = IsWhitespace(after);
        const bool beforePunct = IsAsciiPunctuation(before);
        const bool afterPunct = IsAsciiPunctuation(after);
        const bool leftFlanking = !afterSpace && (!afterPunct || beforeSpace || beforePunct);
        const bool rightFlanking = !beforeSpace && (!beforePunct || afterSpace || afterPunct);

        FlushText();
        InlineNode node;
        node.Delimiter = delimiter;
        node.Count = count;
        node.OriginalCount = count;
        if (delimiter == '*')
        {
            node.CanOpen = leftFlanking;
            node.CanClose = rightFlanking;
        }
        else
        {
            // `_` inside a word (snake_case) neither opens nor closes.
            node.CanOpen = leftFlanking && (!rightFlanking || beforePunct);
            node.CanClose = rightFlanking && (!leftFlanking || afterPunct);
        }
        const size_t index = m_Nodes.size();
        node.PreviousDelimiter = m_LastDelimiter;
        node.InDelimiterList = true;
        m_Nodes.push_back(node);
        if (m_LastDelimiter != kNoNode)
            m_Nodes[m_LastDelimiter].NextDelimiter = index;
        m_LastDelimiter = index;
        m_At += count;
    }

    void RemoveDelimiter(size_t index)
    {
        InlineNode& node = m_Nodes[index];
        if (!node.InDelimiterList)
            return;
        node.InDelimiterList = false;
        if (node.PreviousDelimiter != kNoNode)
            m_Nodes[node.PreviousDelimiter].NextDelimiter = node.NextDelimiter;
        if (node.NextDelimiter != kNoNode)
            m_Nodes[node.NextDelimiter].PreviousDelimiter = node.PreviousDelimiter;
        else
            m_LastDelimiter = node.PreviousDelimiter;
    }

    // Node indices grow along the delimiter list, so "above a floor" is an index comparison;
    // it holds even after the floor's own node has left the list. kNoNode is no floor.
    static bool Above(size_t node, size_t floor)
    {
        return node != kNoNode && (floor == kNoNode || node > floor);
    }

    // CommonMark's "process emphasis" over the delimiters above `bottom`: each closer, left to
    // right, takes the nearest opener of its character above the bottom recorded for its kind.
    // Matched pairs are recorded; every delimiter above `bottom` leaves the list.
    void ResolveEmphasis(size_t bottom)
    {
        // openers_bottom: [character][closer can open][closer length mod 3].
        std::array<std::array<std::array<size_t, 3>, 2>, 2> openersBottom{};
        for (auto& byChar : openersBottom)
            for (auto& byOpen : byChar)
                byOpen.fill(bottom);

        // The head of the run above `bottom`: walking back from the end costs only the
        // delimiters above it, which this call removes.
        size_t closer = kNoNode;
        for (size_t at = m_LastDelimiter; Above(at, bottom); at = m_Nodes[at].PreviousDelimiter)
            closer = at;

        while (closer != kNoNode)
        {
            InlineNode& close = m_Nodes[closer];
            if (!close.CanClose)
            {
                closer = close.NextDelimiter;
                continue;
            }
            size_t& limit = openersBottom[close.Delimiter == '*' ? 0 : 1][close.CanOpen ? 1 : 0][close.OriginalCount % 3];
            size_t opener = close.PreviousDelimiter;
            while (Above(opener, limit) && !Pairs(m_Nodes[opener], close))
                opener = m_Nodes[opener].PreviousDelimiter;
            if (!Above(opener, limit))
            {
                // Every opener below this closer was tried: later closers of this kind start
                // their search at the closer itself (which may still open).
                limit = closer - 1;
                const size_t next = close.NextDelimiter;
                if (!close.CanOpen)
                    RemoveDelimiter(closer);
                closer = next;
                continue;
            }

            InlineNode& open = m_Nodes[opener];
            const size_t used = (open.Count >= 2 && close.Count >= 2) ? 2 : 1;
            (used == 2 ? m_Strong : m_Emphasis).push_back({opener, closer});
            open.Count -= used;
            close.Count -= used;
            // The delimiters between the pair can no longer pair across it: unlinked in one step
            // (they are never reached again, so their own links are left as they were).
            open.NextDelimiter = closer;
            close.PreviousDelimiter = opener;
            if (open.Count == 0)
                RemoveDelimiter(opener);
            if (close.Count == 0)
            {
                const size_t next = close.NextDelimiter;
                RemoveDelimiter(closer);
                closer = next;
            }
        }
        // Delimiters left above the bottom can match nothing outside it.
        while (Above(m_LastDelimiter, bottom))
            RemoveDelimiter(m_LastDelimiter);
    }

    static bool Pairs(const InlineNode& open, const InlineNode& close)
    {
        if (open.Delimiter != close.Delimiter || !open.CanOpen || open.Count == 0)
            return false;
        // The rule of three: a run that can both open and close pairs only with lengths that
        // do not sum to a multiple of three, unless both are multiples of three.
        const bool eitherBoth = open.CanClose || close.CanOpen;
        const size_t sum = open.OriginalCount + close.OriginalCount;
        return !(eitherBoth && sum % 3 == 0 && (open.OriginalCount % 3 != 0 || close.OriginalCount % 3 != 0));
    }

    // ---- Links and images ----

    void OpenBracket(size_t markerLength)
    {
        FlushText();
        m_Brackets.push_back({m_Nodes.size(), m_LastDelimiter, true});
        m_Pool.append(m_Text.data() + m_At, markerLength);
        FlushText();
        m_At += markerLength;
    }

    // A ']' closes the nearest open bracket into a link when an address follows; links do not
    // nest, so every earlier bracket then stays text. Otherwise the ']' is text.
    void CloseBracket()
    {
        if (m_Brackets.empty() || !m_Brackets.back().Active)
        {
            if (!m_Brackets.empty())
                m_Brackets.pop_back();
            m_Pool.push_back(']');
            ++m_At;
            return;
        }
        std::optional<LinkTail> tail = ReadLinkTail(m_At + 1);
        if (!tail)
        {
            m_Brackets.pop_back();
            m_Pool.push_back(']');
            ++m_At;
            return;
        }
        FlushText();
        const BracketOpener opener = m_Brackets.back();
        m_Brackets.pop_back();
        ResolveEmphasis(opener.DelimiterBottom);
        m_Nodes[opener.Node].TextLength = 0;
        if (opener.Node + 1 == m_Nodes.size())
        {
            // An empty link text shows the address.
            m_Pool.append(tail->Url);
            FlushText();
        }
        if (IsWebAddress(tail->Url))
            m_Links.push_back({opener.Node + 1, m_Nodes.size(), std::move(tail->Url)});
        // Brackets are pushed active and only ever deactivated all at once, so the active ones
        // are the top of the stack: stopping at the first inactive one deactivates each
        // bracket once in the whole parse, not once per link.
        for (auto earlier = m_Brackets.rbegin(); earlier != m_Brackets.rend() && earlier->Active; ++earlier)
            earlier->Active = false;
        m_At = tail->End;
    }

    // Reads `(destination "optional title")` after a link's text: `open` is the '('. A
    // destination holds no line break and no control character.
    std::optional<LinkTail> ReadLinkTail(size_t open)
    {
        if (open >= m_Text.size() || m_Text[open] != '(')
            return std::nullopt;
        size_t at = open + 1;
        while (at < m_Text.size() && IsSpaceOrTab(m_Text[at]))
            ++at;

        std::string url;
        if (at < m_Text.size() && m_Text[at] == '<')
        {
            const size_t start = at + 1;
            at = start;
            while (at < m_Text.size() && m_Text[at] != '>' && m_Text[at] != '<' && !IsControl(m_Text[at]))
                ++at;
            if (at >= m_Text.size() || m_Text[at] != '>')
                return std::nullopt;
            url = std::string(m_Text.substr(start, at - start));
            ++at;
        }
        else
        {
            int parens = 0;
            const size_t start = at;
            while (at < m_Text.size() && !IsWhitespace(m_Text[at]) && !IsControl(m_Text[at]))
            {
                if (m_Text[at] == '(' && ++parens > kMaxDestinationParens)
                    return std::nullopt;
                if (m_Text[at] == ')' && parens-- == 0)
                    break;
                ++at;
            }
            url = std::string(m_Text.substr(start, at - start));
        }

        while (at < m_Text.size() && IsWhitespace(m_Text[at]))
            ++at;
        if (at < m_Text.size() && (m_Text[at] == '"' || m_Text[at] == '\''))
        {
            const std::optional<size_t> close = FindQuote(m_Text[at], at + 1);
            if (!close)
                return std::nullopt;
            at = *close + 1;
            while (at < m_Text.size() && IsWhitespace(m_Text[at]))
                ++at;
        }
        if (at >= m_Text.size() || m_Text[at] != ')')
            return std::nullopt;
        return LinkTail{std::move(url), at + 1};
    }

    // The next `quote` at or after `from`. Once a search finds none, no later search from a
    // later position looks again.
    std::optional<size_t> FindQuote(char quote, size_t from)
    {
        size_t& noneFrom = quote == '"' ? m_NoDoubleQuoteFrom : m_NoSingleQuoteFrom;
        if (from >= noneFrom)
            return std::nullopt;
        const size_t close = m_Text.find(quote, from);
        if (close == std::string_view::npos)
        {
            noneFrom = from;
            return std::nullopt;
        }
        return close;
    }

    // `<https://...>`: an address with no space, line break, control character or '<'. Only a
    // web address is a link; `<mailto:...>` shows as its text.
    void ReadAutolink()
    {
        size_t end = m_At + 1;
        while (end < m_Text.size() && m_Text[end] != '>' && m_Text[end] != '<' && !IsWhitespace(m_Text[end]) &&
               !IsControl(m_Text[end]))
            ++end;
        const std::string_view inside = m_Text.substr(m_At + 1, end - m_At - 1);
        const bool closed = end < m_Text.size() && m_Text[end] == '>';
        if (!closed || (!IsWebAddress(inside) && !inside.starts_with("mailto:")))
        {
            m_Pool.push_back('<');
            ++m_At;
            return;
        }
        PushAddress(inside, IsWebAddress(inside));
        m_At = end + 1;
    }

    bool ReadBareUrl()
    {
        const bool wordStart = m_At == 0 || IsWhitespace(m_Text[m_At - 1]) || m_Text[m_At - 1] == '(';
        const size_t length = wordStart ? BareUrlLength(m_Text, m_At) : 0;
        if (length == 0)
            return false;
        PushAddress(m_Text.substr(m_At, length), true);
        m_At += length;
        return true;
    }

    // An address shown as its own text: a node of its own, a link when `isLink`.
    void PushAddress(std::string_view address, bool isLink)
    {
        FlushText();
        m_Pool.append(address);
        FlushText();
        if (isLink)
            m_Links.push_back({m_Nodes.size() - 1, m_Nodes.size(), std::string(address)});
    }

    // ---- Line breaks ----

    // A newline: a hard break after two spaces or before a blank line (a quote's paragraphs),
    // else a soft break, which reads as a space.
    void ReadLineBreak()
    {
        size_t trailing = 0;
        while (m_Pool.size() > m_PendingStart && m_Pool.back() == ' ')
        {
            m_Pool.pop_back();
            ++trailing;
        }
        ++m_At;
        SkipLineIndent();
        bool blankLine = false;
        while (m_At < m_Text.size() && m_Text[m_At] == '\n')
        {
            blankLine = true;
            ++m_At;
            SkipLineIndent();
        }
        m_Pool.push_back(trailing >= kHardBreakSpaces || blankLine ? '\n' : ' ');
    }

    void SkipLineIndent()
    {
        while (m_At < m_Text.size() && IsSpaceOrTab(m_Text[m_At]))
            ++m_At;
    }

    // ---- Nodes ----

    // Ends the pool's pending text as a text node.
    void FlushText()
    {
        if (m_Pool.size() == m_PendingStart)
            return;
        InlineNode node;
        node.TextStart = m_PendingStart;
        node.TextLength = m_Pool.size() - m_PendingStart;
        m_Nodes.push_back(node);
        m_PendingStart = m_Pool.size();
    }

    // Counts, per node, the pairs whose look covers it: +1 after each opener, -1 at its closer.
    std::vector<int> Coverage(const std::vector<EmphasisPair>& pairs) const
    {
        std::vector<int> depth(m_Nodes.size() + 1, 0);
        for (const EmphasisPair& pair : pairs)
        {
            ++depth[pair.Opener + 1];
            --depth[pair.Closer];
        }
        for (size_t index = 1; index < depth.size(); ++index)
            depth[index] += depth[index - 1];
        return depth;
    }

    // Applies the matched looks and addresses, turns unmatched delimiters into their characters
    // and builds one span per run of nodes with one look.
    MarkdownInlines Flatten()
    {
        const std::vector<int> strong = Coverage(m_Strong);
        const std::vector<int> emphasis = Coverage(m_Emphasis);
        // The link each node belongs to. Links never overlap (an earlier bracket deactivates
        // when one closes), so one pass over each link's own nodes is linear in total.
        constexpr size_t kNoLink = static_cast<size_t>(-1);
        std::vector<size_t> linkOf(m_Nodes.size(), kNoLink);
        for (size_t link = 0; link < m_Links.size(); ++link)
            for (size_t index = m_Links[link].Begin; index < m_Links[link].End; ++index)
                linkOf[index] = link;

        MarkdownInlines spans;
        size_t lastLink = kNoLink;
        for (size_t index = 0; index < m_Nodes.size(); ++index)
        {
            const InlineNode& node = m_Nodes[index];
            const size_t length = node.Delimiter != 0 ? node.Count : node.TextLength;
            if (length == 0)
                continue;
            const bool isStrong = strong[index] > 0;
            const bool isEmphasis = emphasis[index] > 0;
            const size_t link = linkOf[index];
            const bool continues = !spans.empty() && spans.back().Strong == isStrong &&
                                   spans.back().Emphasis == isEmphasis && spans.back().Code == node.Code &&
                                   lastLink == link;
            if (!continues)
            {
                MarkdownSpan span;
                span.Strong = isStrong;
                span.Emphasis = isEmphasis;
                span.Code = node.Code;
                if (link != kNoLink)
                    span.Url = m_Links[link].Url;
                spans.push_back(std::move(span));
                lastLink = link;
            }
            if (node.Delimiter != 0)
                spans.back().Text.append(node.Count, node.Delimiter);
            else
                spans.back().Text.append(m_Pool, node.TextStart, node.TextLength);
        }
        return spans;
    }

    std::string_view m_Text;
    size_t m_At = 0;
    // The text of every node, appended in order; a node is a slice of it.
    std::string m_Pool;
    // Where the text not yet ended as a node starts in m_Pool.
    size_t m_PendingStart = 0;
    std::vector<InlineNode> m_Nodes;
    size_t m_LastDelimiter = kNoNode;
    std::vector<BracketOpener> m_Brackets;
    std::vector<EmphasisPair> m_Strong;
    std::vector<EmphasisPair> m_Emphasis;
    std::vector<LinkNodes> m_Links;
    bool m_BacktickRunsIndexed = false;
    std::unordered_map<size_t, std::vector<size_t>> m_BacktickRuns;
    size_t m_NoDoubleQuoteFrom = static_cast<size_t>(-1);
    size_t m_NoSingleQuoteFrom = static_cast<size_t>(-1);
};

} // namespace

MarkdownInlines ParseMarkdownInlines(std::string_view text)
{
    return InlineParser(TrimEnd(text)).Parse();
}

}} // namespace GameEngine::UIParsing
