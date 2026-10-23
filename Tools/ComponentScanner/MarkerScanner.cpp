#include "MarkerScanner.h"

#include "Lexical.h"
#include "NumberFormat.h"

#include <cmath>

namespace GameEngine::ComponentScanner::MarkerScanner
{
namespace
{

using namespace Lexical;

constexpr std::string_view kNoAdd = "@ge-no-add";
constexpr std::string_view kEditorOnly = "@ge-editor-only";
constexpr std::string_view kEngineInternal = "@ge-engine-internal";
constexpr std::string_view kReadOnly = "@ge-readonly";
constexpr std::string_view kHidden = "@ge-hidden";
constexpr std::string_view kTooltip = "@ge-tooltip";
constexpr std::string_view kRange = "@ge-range";

// DoNotSerialize marker, matched case-insensitively. The canonical spelling is
// "[DoNotSerialize]"; [NonSerializable] / [DontSerialize] and the "@" forms are accepted
// aliases.
constexpr std::string_view kDoNotSerialize[] = {
    "[donotserialize]", "[nonserializable]", "[dontserialize]",
    "@donotserialize",  "@nonserializable",  "@dontserialize",
};

// Strip a leading ':' or '=' separator and the whitespace after it.
std::string_view StripSeparator(std::string_view text)
{
    if (!text.empty() && (text[0] == ':' || text[0] == '='))
    {
        text = text.substr(1);
        while (!text.empty() && IsWhiteSpace(text[0]))
            text = text.substr(1);
    }
    return text;
}

// "Min Max" or "Min" (open maximum), optionally followed by free text. A missing Min or a
// Max below Min is malformed.
std::optional<FieldRange> ParseRange(std::string_view payload)
{
    const std::string_view text = StripSeparator(TrimView(payload));
    const std::vector<std::string> tokens = SplitWhitespace(text);
    if (tokens.empty())
        return std::nullopt;
    const std::optional<double> min = NumberFormat::ParseDouble(tokens[0]);
    if (!min || !std::isfinite(*min))
        return std::nullopt;
    if (tokens.size() < 2)
        return FieldRange{*min, std::nullopt};
    const std::optional<double> max = NumberFormat::ParseDouble(tokens[1]);
    if (!max || !std::isfinite(*max))
        return FieldRange{*min, std::nullopt};
    if (*max < *min)
        return std::nullopt;
    return FieldRange{*min, max};
}

std::string ParseTooltipText(std::string_view payload)
{
    const std::string_view text = StripSeparator(TrimView(payload));
    if (text.empty())
        return "";

    if (text[0] != '"')
        return Trim(text);

    std::string result;
    bool escaped = false;
    for (std::size_t i = 1; i < text.size(); ++i)
    {
        const char c = text[i];
        if (escaped)
        {
            switch (c)
            {
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            default: result.push_back(c); break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\')
        {
            escaped = true;
            continue;
        }
        if (c == '"')
            return result;
        result.push_back(c);
    }
    return result;
}

} // namespace

MarkerLines Scan(const std::vector<std::string>& lines, std::ostream& warnings)
{
    MarkerLines result;

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string_view line = lines[i];
        const std::size_t slash = line.find("//");
        if (slash == std::string_view::npos)
            continue;

        const int lineNumber = static_cast<int>(i) + 1;
        const std::string_view comment = line.substr(slash);
        if (comment.find(kNoAdd) != std::string_view::npos)
            result.NoAdd.insert(lineNumber);
        if (comment.find(kEditorOnly) != std::string_view::npos)
            result.EditorOnly.insert(lineNumber);
        if (comment.find(kEngineInternal) != std::string_view::npos)
            result.EngineInternal.insert(lineNumber);
        if (comment.find(kReadOnly) != std::string_view::npos)
            result.ReadOnly.insert(lineNumber);
        if (comment.find(kHidden) != std::string_view::npos)
            result.Hidden.insert(lineNumber);

        if (const std::size_t tooltip = comment.find(kTooltip); tooltip != std::string_view::npos)
            result.Tooltips[lineNumber] = ParseTooltipText(comment.substr(tooltip + kTooltip.size()));

        if (const std::size_t range = comment.find(kRange); range != std::string_view::npos)
        {
            if (std::optional<FieldRange> parsed = ParseRange(comment.substr(range + kRange.size())))
                result.Ranges[lineNumber] = *parsed;
            else
                warnings << "warning: malformed @ge-range marker (expected 'Min [Max]'): " << TrimView(line) << "\n";
        }

        const std::string commentLower = ToLowerAscii(comment);
        for (std::string_view alias : kDoNotSerialize)
        {
            if (commentLower.find(alias) != std::string::npos)
            {
                result.DoNotSerialize.insert(lineNumber);
                break;
            }
        }
    }

    return result;
}

} // namespace GameEngine::ComponentScanner::MarkerScanner
