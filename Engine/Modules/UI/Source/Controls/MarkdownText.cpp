#include "UI/Controls/MarkdownText.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "../Parsers/MarkdownParser.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UIPlatform.h"

#include "Platform/Shell.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{

using UIParsing::MarkdownBlock;
using UIParsing::MarkdownBlockKind;
using UIParsing::MarkdownDocument;
using UIParsing::MarkdownInlines;
using UIParsing::MarkdownSpan;

namespace
{

// UIEvent::Button for the left mouse button.
constexpr int kLeftButton = 0;

// U+00A0: gives a bullet marker a line box and a baseline to sit on, without drawing anything.
constexpr const char* kNoBreakSpace = "\xC2\xA0";

constexpr const char* kCopyLabel = "Copy";
constexpr const char* kCopiedLabel = "Copied";

void OpenWebLink(UIEvent& event, const std::string& url)
{
    if (event.Button != kLeftButton)
        return;
    event.Stop();
    (void)Platform::OpenUrl(url);
}

// The labels of one link occurrence: hovering any of its words lights all of them.
using LinkLabels = std::vector<Label*>;

void SetLinkHovered(const LinkLabels& labels, bool hovered)
{
    for (Label* label : labels)
    {
        if (hovered)
            label->AddClass("markdown-link-hover");
        else
            label->RemoveClass("markdown-link-hover");
    }
}

std::unique_ptr<UIElement> MakeElement(const char* cssClass)
{
    auto element = std::make_unique<UIElement>();
    element->AddClass(cssClass);
    return element;
}

// One piece of a word: its text and its span's look.
struct WordPiece
{
    std::string Text;
    const MarkdownSpan* Span = nullptr;
    // Part of a word that may be wider than the line: it wraps inside itself.
    bool MayWrap = false;
};

// A word longer than this may not fit a narrow line (20 characters of 14 px text are about
// 150 px): it breaks after its separators, and a part still this long wraps inside itself. A
// label that wraps loses the space at its end (as a block's trailing space does in a browser),
// so such a part's spaces are a label of their own; every shorter word keeps its spaces on its
// own label (white-space: pre) and is one label.
constexpr size_t kMayOverflowLineBytes = 20;

// Spaces after these looks stay outside them: no tint or underline under the space.
bool KeepsTrailingSpacesOut(const MarkdownSpan& span)
{
    return span.Code || !span.Url.empty();
}

// Lays a block's inline content into `flow` (a wrapping row): one label per word, the spaces
// after a word kept on it so words read spaced as written. A word whose looks change inside it
// (`**Lantern_01**,`) is one group, so the line never breaks inside the word. A code span is one
// piece, spaces and all. A word wider than the line wraps inside itself (the default sheet's
// overflow-wrap), so nothing is cut off.
class InlineFlowBuilder
{
public:
    explicit InlineFlowBuilder(UIElement& flow) : m_Flow(flow) {}

    void Append(const MarkdownInlines& inlines)
    {
        for (const MarkdownSpan& span : inlines)
            AppendSpan(span);
        FlushWord();
    }

private:
    void AppendSpan(const MarkdownSpan& span)
    {
        const std::string_view text = span.Text;
        if (span.Code)
        {
            AppendPiece(text, span, false);
            return;
        }
        size_t at = 0;
        while (at < text.size())
        {
            if (text[at] == '\n')
            {
                FlushWord();
                m_Flow.AddChild(MakeElement("markdown-break"));
                ++at;
                continue;
            }
            size_t end = at;
            while (end < text.size() && text[end] != ' ' && text[end] != '\n')
                ++end;
            const size_t wordEnd = end;
            while (end < text.size() && text[end] == ' ')
                ++end;
            AppendPiece(text.substr(at, end - at), span, wordEnd == at);
            if (end > wordEnd)
                FlushWord();
            at = end;
        }
    }

    // Spaces alone (a span that starts with the space after the previous span's word) join the
    // word's last piece, so a bold word and its space are one label; after code or a link they
    // stay a piece of their own.
    void AppendPiece(std::string_view text, const MarkdownSpan& span, bool spacesOnly)
    {
        if (spacesOnly && !m_Word.empty() && !KeepsTrailingSpacesOut(*m_Word.back().Span))
        {
            m_Word.back().Text += text;
            return;
        }
        m_Word.push_back({std::string(text), &span});
    }

    void FlushWord()
    {
        if (m_Word.empty())
            return;
        if (Letters(m_Word) <= kMayOverflowLineBytes)
        {
            AddItem(m_Word);
        }
        else
        {
            // A long word (a path, an address) breaks after its separators, each part a flex
            // item of its own: the line wraps between parts, and the next word goes on after the
            // last part. A part still too long to fit wraps inside itself.
            for (std::vector<WordPiece>& part : SplitAtBreakPoints(m_Word))
            {
                if (Letters(part) > kMayOverflowLineBytes)
                    MarkToWrap(part);
                AddItem(part);
            }
        }
        m_Word.clear();
    }

    // One flex item: a label, or a no-wrap group when its look changes inside it.
    void AddItem(const std::vector<WordPiece>& pieces)
    {
        if (pieces.size() == 1)
        {
            m_Flow.AddChild(MakePieceLabel(pieces.front()));
            return;
        }
        auto group = MakeElement("markdown-word-group");
        for (const WordPiece& piece : pieces)
            group->AddChild(MakePieceLabel(piece));
        m_Flow.AddChild(std::move(group));
    }

    // The characters of `pieces`, without the spaces after each.
    static size_t Letters(const std::vector<WordPiece>& pieces)
    {
        size_t length = 0;
        for (const WordPiece& piece : pieces)
            length += piece.Text.find_last_not_of(' ') + 1;
        return length;
    }

    static bool IsBreakAfter(char c)
    {
        constexpr std::string_view kSeparators = "/\\_-.?&=#:,;";
        return kSeparators.find(c) != std::string_view::npos;
    }

    // Cuts a word after each separator (never after its last letter), keeping each piece's
    // look; the spaces after the word stay with its last part. Separators are ASCII, so a cut
    // never splits a UTF-8 sequence.
    static std::vector<std::vector<WordPiece>> SplitAtBreakPoints(const std::vector<WordPiece>& word)
    {
        const size_t total = Letters(word);
        std::vector<std::vector<WordPiece>> parts(1);
        size_t seen = 0;
        for (const WordPiece& piece : word)
        {
            const size_t letters = piece.Text.find_last_not_of(' ') + 1;
            size_t from = 0;
            for (size_t at = 0; at < letters; ++at)
            {
                ++seen;
                if (!IsBreakAfter(piece.Text[at]) || seen == total)
                    continue;
                parts.back().push_back({piece.Text.substr(from, at + 1 - from), piece.Span, false});
                parts.emplace_back();
                from = at + 1;
            }
            if (from < piece.Text.size())
                parts.back().push_back({piece.Text.substr(from), piece.Span, false});
        }
        if (parts.back().empty())
            parts.pop_back();
        return parts;
    }

    // Marks every piece of a part that may not fit the line to wrap, and moves the spaces after
    // a piece into a piece of their own, which keeps them (see kMayOverflowLineBytes).
    static void MarkToWrap(std::vector<WordPiece>& part)
    {
        std::vector<WordPiece> split;
        split.reserve(part.size() * 2);
        for (WordPiece& piece : part)
        {
            const size_t letters = piece.Text.find_last_not_of(' ') + 1;
            std::string spaces = piece.Text.substr(letters);
            piece.Text.resize(letters);
            piece.MayWrap = true;
            const MarkdownSpan* span = piece.Span;
            if (!piece.Text.empty())
                split.push_back(std::move(piece));
            if (!spaces.empty())
                split.push_back({std::move(spaces), span, false});
        }
        part = std::move(split);
    }

    std::unique_ptr<Label> MakePieceLabel(const WordPiece& piece)
    {
        auto label = std::make_unique<Label>();
        label->SetText(piece.Text);
        label->AddClass("markdown-word");
        if (piece.MayWrap)
            label->AddClass("markdown-word-long");
        const MarkdownSpan& span = *piece.Span;
        if (span.Strong)
            label->AddClass("markdown-strong");
        if (span.Emphasis)
            label->AddClass("markdown-emphasis");
        if (span.Code)
            label->AddClass("markdown-code");
        // The parser gives only a web address a URL (UIParsing::IsWebAddress).
        if (!span.Url.empty())
            JoinLink(*label, span.Url);
        else
            m_Link.reset();
        return label;
    }

    // Consecutive pieces with one address are one link occurrence; its labels share a hover.
    void JoinLink(Label& label, const std::string& url)
    {
        if (!m_Link || m_LinkUrl != url)
        {
            m_Link = std::make_shared<LinkLabels>();
            m_LinkUrl = url;
        }
        m_Link->push_back(&label);
        label.AddClass("markdown-link");
        label.SetTooltip("Open " + url + " in the browser");
        label.RegisterEventHandler(kEventMouseUp, [url](UIEvent& event) { OpenWebLink(event, url); });
        label.RegisterEventHandler(kEventMouseEnter, [link = m_Link](UIEvent&) { SetLinkHovered(*link, true); });
        label.RegisterEventHandler(kEventMouseLeave, [link = m_Link](UIEvent&) { SetLinkHovered(*link, false); });
    }

    UIElement& m_Flow;
    std::vector<WordPiece> m_Word;
    std::shared_ptr<LinkLabels> m_Link;
    std::string m_LinkUrl;
};

std::unique_ptr<UIElement> MakeFlow(const MarkdownInlines& inlines, const char* cssClass)
{
    auto flow = MakeElement("markdown-flow");
    flow->AddClass(cssClass);
    InlineFlowBuilder(*flow).Append(inlines);
    return flow;
}

// Levels 1 and 2 step the size up; deeper headings are bold at the text's size.
const char* HeadingSizeClass(uint8_t level)
{
    switch (level)
    {
    case 1: return "markdown-heading-1";
    case 2: return "markdown-heading-2";
    default: return nullptr;
    }
}

// A number for an ordered item; for a bulleted one, a drawn dot (filled at the first level, a
// ring when nested) on a no-break space that gives it the line's box and baseline.
std::unique_ptr<Label> MakeListMarker(const MarkdownBlock& block)
{
    auto marker = std::make_unique<Label>();
    marker->AddClass("markdown-list-marker");
    if (block.Ordered)
    {
        marker->SetText(std::to_string(block.Number) + ".");
        return marker;
    }
    marker->SetText(kNoBreakSpace);
    marker->AddChild(MakeElement(block.ListDepth > 0 ? "markdown-bullet-nested" : "markdown-bullet"));
    return marker;
}

std::unique_ptr<UIElement> BuildListItem(const MarkdownBlock& block)
{
    auto item = MakeElement("markdown-list-item");
    if (block.ListDepth > 0)
        item->AddClass("markdown-list-item-nested");
    item->AddChild(MakeListMarker(block));
    item->AddChild(MakeFlow(block.Inlines, "markdown-list-text"));
    return item;
}

// Puts `code` on the clipboard and says so on the button until the pointer leaves it.
void CopyCode(Button& button, const std::string& code)
{
    const UIManager* ui = button.GetOwnerManager();
    UI::IPlatformApi* platform = ui ? ui->GetPlatform() : nullptr;
    if (!platform)
        return;
    platform->SetClipboardText(code.c_str());
    button.SetText(kCopiedLabel);
}

std::unique_ptr<UIElement> BuildCodeHeader(const MarkdownBlock& block)
{
    auto header = MakeElement("markdown-code-header");
    auto language = std::make_unique<Label>();
    language->AddClass("markdown-code-language");
    language->SetText(block.Language);
    header->AddChild(std::move(language));

    // "small" and "secondary" are the button sizes a theme defines (the editor's draws its own
    // reply-row buttons with them); without such a theme the default sheet's
    // .markdown-code-copy is the whole look.
    auto copy = std::make_unique<Button>();
    copy->AddClass("small");
    copy->AddClass("secondary");
    copy->AddClass("markdown-code-copy");
    copy->SetFocusable(false);
    copy->SetText(kCopyLabel);
    copy->SetTooltip("Copy this code");
    Button* raw = copy.get();
    copy->SetOnClick([raw, code = block.Code](UIEvent&) { CopyCode(*raw, code); });
    copy->RegisterEventHandler(kEventMouseLeave, [raw](UIEvent&) { raw->SetText(kCopyLabel); });
    header->AddChild(std::move(copy));
    return header;
}

// Code keeps its lines: it scrolls sideways instead of wrapping, and a vertical wheel over it
// scrolls the page around it (the default sheet sets --ui_scrollview_keep_wheel_axis). Its
// header names the language and copies the code.
std::unique_ptr<UIElement> BuildCodeBlock(const MarkdownBlock& block)
{
    auto frame = MakeElement("markdown-code-block");
    frame->AddChild(BuildCodeHeader(block));
    auto scroll = std::make_unique<ScrollView>();
    scroll->AddClass("markdown-code-scroll");
    auto code = std::make_unique<Label>();
    code->AddClass("markdown-code-text");
    code->SetText(block.Code);
    scroll->AddContent(std::move(code));
    frame->AddChild(std::move(scroll));
    return frame;
}

std::unique_ptr<UIElement> BuildTable(const MarkdownBlock& block)
{
    auto table = MakeElement("markdown-table");
    for (size_t rowIndex = 0; rowIndex < block.Rows.size(); ++rowIndex)
    {
        auto row = MakeElement(rowIndex == 0 ? "markdown-table-header" : "markdown-table-row");
        for (const MarkdownInlines& cell : block.Rows[rowIndex])
            row->AddChild(MakeFlow(cell, "markdown-table-cell"));
        table->AddChild(std::move(row));
    }
    return table;
}

std::unique_ptr<UIElement> BuildBlock(const MarkdownBlock& block)
{
    switch (block.Kind)
    {
    case MarkdownBlockKind::Paragraph: return MakeFlow(block.Inlines, "markdown-paragraph");
    case MarkdownBlockKind::Heading:
    {
        auto heading = MakeFlow(block.Inlines, "markdown-heading");
        if (const char* sizeClass = HeadingSizeClass(block.HeadingLevel))
            heading->AddClass(sizeClass);
        return heading;
    }
    case MarkdownBlockKind::ListItem: return BuildListItem(block);
    case MarkdownBlockKind::CodeBlock: return BuildCodeBlock(block);
    case MarkdownBlockKind::Quote: return MakeFlow(block.Inlines, "markdown-quote");
    case MarkdownBlockKind::Rule: return MakeElement("markdown-rule");
    case MarkdownBlockKind::Table: return BuildTable(block);
    }
    return MakeElement("markdown-rule");
}

// The space above a block follows from the block before it: none for the first, a small step
// between items of one list, little under a heading, a block gap otherwise (the default
// sheet's .markdown-block, with more above a heading).
const char* SpacingClass(const MarkdownBlock* previous, const MarkdownBlock& block)
{
    if (!previous)
        return "markdown-block-first";
    if (previous->Kind == MarkdownBlockKind::Heading)
        return "markdown-after-heading";
    if (previous->Kind == MarkdownBlockKind::ListItem && block.Kind == MarkdownBlockKind::ListItem &&
        previous->Ordered == block.Ordered)
        return "markdown-list-continued";
    return nullptr;
}

} // namespace

MarkdownText::MarkdownText()
{
    AddClass("markdown-text");
}

MarkdownText::~MarkdownText() = default;

void MarkdownText::SetMarkdown(std::string_view markdown)
{
    MarkdownDocument document = UIParsing::MarkdownParser::Parse(markdown);

    size_t kept = 0;
    const size_t comparable = std::min(m_Blocks.size(), document.Blocks.size());
    while (kept < comparable && m_Blocks[kept] == document.Blocks[kept])
        ++kept;

    for (size_t index = kept; index < m_BlockElements.size(); ++index)
        RemoveChild(m_BlockElements[index]);
    m_BlockElements.resize(kept);

    for (size_t index = kept; index < document.Blocks.size(); ++index)
    {
        const MarkdownBlock& block = document.Blocks[index];
        std::unique_ptr<UIElement> element = BuildBlock(block);
        element->AddClass("markdown-block");
        if (const char* spacing = SpacingClass(index > 0 ? &document.Blocks[index - 1] : nullptr, block))
            element->AddClass(spacing);
        m_BlockElements.push_back(element.get());
        AddChild(std::move(element));
    }
    m_Blocks = std::move(document.Blocks);
}

} // namespace GameEngine
