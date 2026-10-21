#include "ApiMarkerLines.h"

#include "Lexical.h"

#include <algorithm>
#include <iterator>

namespace GameEngine::ComponentScanner
{
namespace
{

using namespace Lexical;

constexpr std::string_view kScriptMarkerPrefix = "@ge-script";

constexpr std::pair<std::string_view, Marker> kMarkers[] = {
    {"ge-script", Marker::Script},
    {"ge-script-handle", Marker::Handle},
    {"ge-script-event", Marker::Event},
    {"ge-script-try", Marker::Try},
    {"ge-context", Marker::Context},
    {"ge-engine-thread", Marker::EngineThread},
    {"ge-any-thread", Marker::AnyThread},
    {"ge-script-flags", Marker::Flags},
};

} // namespace

bool IsPrimary(Marker marker)
{
    return marker == Marker::Script || marker == Marker::Handle || marker == Marker::Event;
}

bool LineMarkers::HasScriptMarker() const
{
    return !Markers.empty() || DocMarker ||
           std::any_of(Unknown.begin(), Unknown.end(),
                       [](const std::string& word) { return StartsWith(word, kScriptMarkerPrefix); });
}

namespace ApiMarkerLines
{

std::string MarkerList()
{
    std::string list;
    for (const auto& [name, marker] : kMarkers)
        list += (list.empty() ? "@" : ", @") + std::string(name);
    return list;
}

LineMarkers ReadLine(std::string_view line)
{
    LineMarkers result;
    const std::size_t slash = line.find("//");
    const std::string_view trimmed = TrimView(line);
    if (slash == std::string_view::npos)
    {
        result.HasCode = !trimmed.empty();
        return result;
    }
    result.HasCode = !TrimView(line.substr(0, slash)).empty();
    result.CommentOnly = !result.HasCode;
    // A marker comment starts with its markers ("// @ge-script @ge-context"); prose that
    // mentions a marker further into a comment carries none. A /// line is documentation.
    std::string_view comment = StartsWith(trimmed, "///") ? std::string_view() : TrimView(line.substr(slash + 2));
    while (StartsWith(comment, "@ge-"))
    {
        std::size_t end = 1;
        while (end < comment.size() && (IsAsciiLetter(comment[end]) || comment[end] == '-'))
            ++end;
        const std::string_view token = comment.substr(1, end - 1);
        comment = TrimView(comment.substr(end));
        const auto known = std::find_if(std::begin(kMarkers), std::end(kMarkers),
                                        [token](const auto& entry)
                                        { return entry.first == token; });
        if (known == std::end(kMarkers))
        {
            result.Unknown.push_back("@" + std::string(token));
            continue;
        }
        result.Markers.push_back(known->second);
        if (known->second == Marker::Event && !comment.empty() && comment[0] != '@')
        {
            result.EventPayload = FirstToken(comment);
            comment = TrimView(comment.substr(result.EventPayload->size()));
        }
    }

    if (result.CommentOnly && StartsWith(trimmed, "///"))
    {
        std::string_view text = trimmed.substr(3);
        if (StartsWith(TrimView(text), "@ge-"))
        {
            result.DocMarker = true;
            return result;
        }
        if (!text.empty() && text[0] == ' ')
            text.remove_prefix(1);
        result.Doc = TrimEnd(text);
    }
    return result;
}

std::vector<LineCondition> ConditionsByLine(const std::vector<std::string>& lines)
{
    enum class State
    {
        Dead,
        Live,
        Conditional,
    };
    struct Group
    {
        State GroupState = State::Conditional;
        int Line = 0;
        std::string Text;
    };

    // The include guard: the first two directives `#ifndef X` and `#define X`, and the `#endif`
    // that closes the first is the last directive of the header.
    std::vector<std::pair<std::size_t, std::string_view>> directives;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (const std::string_view t = TrimView(lines[i]); StartsWith(t, "#"))
            directives.emplace_back(i, TrimView(t.substr(1)));
    std::size_t guardLine = lines.size();
    if (directives.size() >= 3 && FirstToken(directives[0].second) == "ifndef" && FirstToken(directives[1].second) == "define" &&
        Trim(directives[0].second.substr(6)) == FirstToken(Trim(directives[1].second.substr(6))) &&
        FirstToken(directives.back().second) == "endif")
    {
        int depth = 0;
        std::size_t close = directives.size();
        for (std::size_t d = 0; d < directives.size() && close == directives.size(); ++d)
        {
            const std::string word = FirstToken(directives[d].second);
            if (word == "if" || word == "ifdef" || word == "ifndef")
                ++depth;
            else if (word == "endif" && --depth == 0)
                close = d;
        }
        if (close == directives.size() - 1)
            guardLine = directives[0].first;
    }

    std::vector<LineCondition> conditions(lines.size() + 1);
    std::vector<Group> stack;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const int lineNumber = static_cast<int>(i) + 1;
        LineCondition& condition = conditions[static_cast<std::size_t>(lineNumber)];
        for (const Group& group : stack)
        {
            if (group.GroupState == State::Dead)
                condition.Dead = true;
            else if (group.GroupState == State::Conditional)
            {
                condition.GroupLine = group.Line;
                condition.GroupText = group.Text;
            }
        }

        const std::string_view trimmed = TrimView(lines[i]);
        if (!StartsWith(trimmed, "#"))
            continue;
        const std::string_view directive = TrimView(trimmed.substr(1));
        const std::string word = FirstToken(directive);
        const std::string text = "#" + std::string(directive);
        const std::string rest = Trim(directive.substr(word.size()));
        if (i == guardLine)
            stack.push_back(Group{State::Live, lineNumber, text});
        else if (word == "if")
            stack.push_back(Group{rest == "0" ? State::Dead : State::Conditional, lineNumber, text});
        else if (word == "ifdef" || word == "ifndef")
            stack.push_back(Group{State::Conditional, lineNumber, text});
        else if ((word == "elif" || word == "elifdef" || word == "elifndef") && !stack.empty())
            stack.back() = Group{State::Conditional, lineNumber, text};
        else if (word == "else" && !stack.empty())
        {
            if (stack.back().GroupState == State::Dead)
                stack.back().GroupState = State::Live;
            else if (stack.back().GroupState == State::Live)
                stack.back().GroupState = State::Dead;
        }
        else if (word == "endif" && !stack.empty())
            stack.pop_back();
    }
    return conditions;
}

std::vector<LineMarkers> MarkersByLine(const std::vector<std::string>& lines, const std::vector<LineCondition>& conditions)
{
    std::vector<LineMarkers> markers(lines.size() + 1);
    bool inBlock = false;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string_view line = lines[i];
        // Whether the line's first // starts inside a /* */ block, and the block state after
        // the line. String literals are not tracked: a marker line holds none.
        const bool startsInBlock = inBlock;
        bool slashInBlock = false;
        bool lineComment = false;
        for (std::size_t c = 0; c + 1 < line.size(); ++c)
        {
            if (inBlock)
            {
                if (line[c] == '*' && line[c + 1] == '/')
                {
                    inBlock = false;
                    ++c;
                }
                else if (line[c] == '/' && line[c + 1] == '/')
                    slashInBlock = true;
                continue;
            }
            if (line[c] == '/' && line[c + 1] == '/')
            {
                lineComment = true;
                break;
            }
            if (line[c] == '/' && line[c + 1] == '*')
            {
                inBlock = true;
                ++c;
            }
        }
        if (conditions[i + 1].Dead)
            continue;
        if (slashInBlock || (startsInBlock && !lineComment))
        {
            markers[i + 1].CommentOnly = true;
            continue;
        }
        markers[i + 1] = ReadLine(line);
    }
    return markers;
}

} // namespace ApiMarkerLines

} // namespace GameEngine::ComponentScanner
