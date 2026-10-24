#pragma once

#include "Lexical.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace GameEngine::ComponentScanner
{

// Associates a marker comment (captured by line number in MarkerScanner) with the struct or
// field it annotates. A marker "attaches" when it sits on the declaration's own line(s) or
// on the contiguous comment/blank run immediately preceding it; a line of code ends that
// run, so a marker on a neighbouring declaration never leaks across.
//
// `lines` is the raw source split into 1-based-addressable lines (line N == lines[N-1]),
// shared with MarkerScanner so the split happens once per header.
namespace MarkerAttachment
{

// A line that contributes to an "attached leading comment" run: empty/whitespace, or a line
// whose only non-whitespace content is a "//" or single-line "/* ... */" comment. Interior
// lines of a multi-line /* */ block start without "//" so they break the run, which is
// acceptable since the marker convention is a // line comment.
inline bool IsBlankOrCommentLine(std::string_view line)
{
    const std::string_view t = Lexical::TrimView(line);
    if (t.empty())
        return true;
    if (Lexical::StartsWith(t, "//"))
        return true;
    return Lexical::StartsWith(t, "/*") && Lexical::EndsWith(t, "*/");
}

// The marker line, if any, within the contiguous comment/blank run immediately above
// `declLine`; a line of code ends the run.
template <typename TFound>
std::optional<int> InPrecedingCommentRun(const std::vector<std::string>& lines, int declLine, TFound isMarkerLine)
{
    for (int line = declLine - 1; line >= 1; --line)
    {
        const std::string_view text = (static_cast<std::size_t>(line - 1) < lines.size()) ? std::string_view(lines[static_cast<std::size_t>(line - 1)]) : std::string_view();
        if (!IsBlankOrCommentLine(text))
            break;
        if (isMarkerLine(line))
            return line;
    }
    return std::nullopt;
}

// The line of the struct-level marker ("// @ge-no-add") that attaches to the struct whose
// declaration head sits on `declLine`: the declaration line itself, or a line of the contiguous
// comment/blank run immediately preceding it.
inline std::optional<int> StructMarkerLine(const std::vector<std::string>& lines, int declLine, const std::set<int>& markerLines)
{
    if (declLine < 1 || markerLines.empty())
        return std::nullopt;
    if (markerLines.count(declLine) != 0)
        return declLine;
    return InPrecedingCommentRun(lines, declLine, [&](int line) { return markerLines.count(line) != 0; });
}

// True when a struct-level marker attaches to the struct whose declaration head sits on `declLine`.
inline bool StructMarkerAttached(const std::vector<std::string>& lines, int declLine, const std::set<int>& markerLines)
{
    return StructMarkerLine(lines, declLine, markerLines).has_value();
}

// True when a field-level marker ("// @ge-readonly" / "// @ge-hidden") attaches to a field
// whose declaration spans lines [declLine, endLine]: anywhere within that range (a trailing
// "// @ge-..." on the field's line, including the line of a multi-line initializer's
// terminating ';'), or on the preceding comment/blank run.
inline bool FieldMarkerAttached(const std::vector<std::string>& lines, int declLine, int endLine, const std::set<int>& markerLines)
{
    if (markerLines.empty() || declLine < 1)
        return false;

    const int last = endLine >= declLine ? endLine : declLine;
    for (int line = declLine; line <= last; ++line)
        if (markerLines.count(line) != 0)
            return true;

    return InPrecedingCommentRun(lines, declLine, [&](int line) { return markerLines.count(line) != 0; }).has_value();
}

// The field-level marker payload ("// @ge-tooltip Text", "// @ge-range 0 1") attached to a
// field, using the same line-range / preceding-comment-run rules as FieldMarkerAttached.
template <typename TValue>
std::optional<TValue> FieldValueMarkerAttached(const std::vector<std::string>& lines, int declLine, int endLine,
                                               const std::map<int, TValue>& markerLines)
{
    if (markerLines.empty() || declLine < 1)
        return std::nullopt;

    const int last = endLine >= declLine ? endLine : declLine;
    for (int line = declLine; line <= last; ++line)
        if (auto it = markerLines.find(line); it != markerLines.end())
            return it->second;

    if (std::optional<int> line = InPrecedingCommentRun(lines, declLine, [&](int l) { return markerLines.count(l) != 0; }))
        return markerLines.at(*line);
    return std::nullopt;
}

} // namespace MarkerAttachment

} // namespace GameEngine::ComponentScanner
