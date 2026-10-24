#include "Scripting/ScriptsProjectFile.h"

#include "ScriptsProjectFileInternal.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>

namespace GameEngine::ScriptsProjectFile
{
namespace
{
constexpr std::string_view kPackageReferenceItem = "PackageReference";
constexpr std::string_view kPackageVersionItem = "PackageVersion";

// The text with every comment's characters replaced by spaces, so a search never matches
// inside a comment and every offset still points into the original text.
std::string MaskComments(std::string_view text)
{
    std::string masked(text);
    std::size_t pos = 0;
    while ((pos = masked.find("<!--", pos)) != std::string::npos)
    {
        std::size_t end = masked.find("-->", pos + 4);
        end = (end == std::string::npos) ? masked.size() : end + 3;
        std::fill(masked.begin() + static_cast<std::ptrdiff_t>(pos), masked.begin() + static_cast<std::ptrdiff_t>(end),
                  ' ');
        pos = end;
    }
    return masked;
}

std::string_view LineEnding(std::string_view text)
{
    return text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
}

bool IsNameChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == ':';
}

// A tag that starts with `name` followed by whitespace, '/' or '>' (so "<PackageReference"
// never matches "<PackageReferences").
std::size_t FindTag(std::string_view masked, std::string_view name, std::size_t from)
{
    while ((from = masked.find(name, from)) != std::string_view::npos)
    {
        const std::size_t after = from + name.size();
        if (after < masked.size() && !IsNameChar(masked[after]))
            return from;
        from = after;
    }
    return std::string_view::npos;
}

// The span of an attribute's value inside the open tag [tagStart, tagEnd).
struct ValueSpan
{
    std::size_t Begin = 0;
    std::size_t End = 0;
};

std::optional<ValueSpan> FindAttribute(std::string_view masked, std::size_t tagStart, std::size_t tagEnd,
                                       std::string_view attribute)
{
    std::size_t pos = tagStart;
    while (pos < tagEnd)
    {
        pos = masked.find(attribute, pos);
        if (pos == std::string_view::npos || pos >= tagEnd)
            return std::nullopt;
        const bool startsName = !IsNameChar(masked[pos - 1]);
        std::size_t cursor = pos + attribute.size();
        while (cursor < tagEnd && std::isspace(static_cast<unsigned char>(masked[cursor])))
            ++cursor;
        if (startsName && cursor < tagEnd && masked[cursor] == '=')
        {
            ++cursor;
            while (cursor < tagEnd && std::isspace(static_cast<unsigned char>(masked[cursor])))
                ++cursor;
            if (cursor < tagEnd && (masked[cursor] == '"' || masked[cursor] == '\''))
            {
                const char quote = masked[cursor];
                const std::size_t close = masked.find(quote, cursor + 1);
                if (close == std::string_view::npos || close >= tagEnd)
                    return std::nullopt;
                return ValueSpan{cursor + 1, close};
            }
        }
        pos += attribute.size();
    }
    return std::nullopt;
}

// One PackageReference or PackageVersion element: [Start, End) covers the whole element,
// [Start, OpenTagEnd) its open tag. Version is either the attribute or the text of a Version child element.
struct PackageElement
{
    std::size_t Start = 0;
    std::size_t OpenTagEnd = 0;
    std::size_t End = 0;
    ValueSpan Include;
    std::optional<ValueSpan> Version;
};

std::vector<PackageElement> FindPackageElements(std::string_view masked, std::string_view item)
{
    const std::string openTag = "<" + std::string(item);
    const std::string closeTag = "</" + std::string(item) + ">";
    std::vector<PackageElement> elements;
    std::size_t pos = 0;
    while ((pos = FindTag(masked, openTag, pos)) != std::string_view::npos)
    {
        const std::size_t close = masked.find('>', pos);
        if (close == std::string_view::npos)
            break;
        PackageElement element;
        element.Start = pos;
        element.OpenTagEnd = close + 1;
        const bool selfClosing = close > pos && masked[close - 1] == '/';
        if (selfClosing)
        {
            element.End = close + 1;
        }
        else
        {
            const std::size_t closing = masked.find(closeTag, close);
            element.End = closing == std::string_view::npos ? close + 1 : closing + closeTag.size();
        }
        const std::optional<ValueSpan> include = FindAttribute(masked, pos, close, "Include");
        if (include)
        {
            element.Include = *include;
            element.Version = FindAttribute(masked, pos, close, "Version");
            if (!element.Version && !selfClosing)
            {
                const std::size_t child = masked.find("<Version>", element.OpenTagEnd);
                if (child != std::string_view::npos && child < element.End)
                {
                    const std::size_t valueEnd = masked.find("</Version>", child);
                    if (valueEnd != std::string_view::npos && valueEnd < element.End)
                        element.Version = ValueSpan{child + 9, valueEnd};
                }
            }
            elements.push_back(element);
        }
        pos = element.End;
    }
    return elements;
}

std::string_view Slice(std::string_view text, ValueSpan span)
{
    return text.substr(span.Begin, span.End - span.Begin);
}

// True when an Import element names `fileName` as the last part of its Project attribute.
bool HasImport(std::string_view masked, std::string_view fileName)
{
    std::size_t pos = 0;
    while ((pos = FindTag(masked, "<Import", pos)) != std::string_view::npos)
    {
        const std::size_t close = masked.find('>', pos);
        if (close == std::string_view::npos)
            return false;
        if (const std::optional<ValueSpan> project = FindAttribute(masked, pos, close, "Project"))
        {
            const std::string_view value = Slice(masked, *project);
            if (value.size() >= fileName.size() && value.substr(value.size() - fileName.size()) == fileName)
            {
                const std::size_t before = value.size() - fileName.size();
                if (before == 0 || value[before - 1] == '/' || value[before - 1] == '\\' || value[before - 1] == ')')
                    return true;
            }
        }
        pos = close;
    }
    return false;
}

std::string ImportLine(std::string_view fileName)
{
    return "  <Import Project=\"" + std::string(fileName) + "\" />";
}

// Offset of the start of the line that holds `pos`.
std::size_t LineStart(std::string_view text, std::size_t pos)
{
    const std::size_t newline = text.rfind('\n', pos == 0 ? 0 : pos - 1);
    return (newline == std::string_view::npos || pos == 0) ? 0 : newline + 1;
}

// Where a new ItemGroup goes: before the engine targets import line when there is one,
// else before the closing Project tag.
std::size_t NewItemGroupOffset(std::string_view text, std::string_view masked)
{
    std::size_t pos = 0;
    while ((pos = FindTag(masked, "<Import", pos)) != std::string_view::npos)
    {
        const std::size_t close = masked.find('>', pos);
        if (close == std::string_view::npos)
            break;
        if (const std::optional<ValueSpan> project = FindAttribute(masked, pos, close, "Project");
            project && Slice(masked, *project).find(kEngineTargetsFileName) != std::string_view::npos)
            return LineStart(text, pos);
        pos = close;
    }
    const std::size_t end = masked.rfind("</Project>");
    return end == std::string_view::npos ? text.size() : LineStart(text, end);
}
// The packages the `item` elements of `text` name, in file order. `Item` is PackageReference or
// PackageVersion.
template <typename Item>
std::vector<Item> ReadItems(std::string_view text, std::string_view item)
{
    const std::string masked = MaskComments(text);
    std::vector<Item> packages;
    for (const PackageElement& element : FindPackageElements(masked, item))
    {
        Item package;
        package.Id = std::string(Slice(text, element.Include));
        if (element.Version)
            package.Version = std::string(Slice(text, *element.Version));
        packages.push_back(std::move(package));
    }
    return packages;
}

// Sets the version of the `item` element with the package's id, or adds one. An empty version
// writes no Version attribute and leaves an existing element as it is.
void SetItem(std::string& text, std::string_view item, std::string_view id, std::string_view version)
{
    const std::string masked = MaskComments(text);
    const std::vector<PackageElement> elements = FindPackageElements(masked, item);
    for (const PackageElement& element : elements)
    {
        if (!EqualsIgnoreCase(Slice(text, element.Include), id))
            continue;
        if (version.empty())
            return;
        if (element.Version)
            text.replace(element.Version->Begin, element.Version->End - element.Version->Begin, version);
        else
            text.insert(element.Include.End + 1, " Version=\"" + std::string(version) + "\"");
        return;
    }

    const std::string_view newline = LineEnding(text);
    std::string line = "    <" + std::string(item) + " Include=\"" + std::string(id) + "\"";
    if (!version.empty())
        line += " Version=\"" + std::string(version) + "\"";
    line += " />";
    if (!elements.empty())
    {
        text.insert(elements.back().End, std::string(newline) + line);
        return;
    }
    const std::string group =
        "  <ItemGroup>" + std::string(newline) + line + std::string(newline) + "  </ItemGroup>" + std::string(newline);
    text.insert(NewItemGroupOffset(text, masked), group);
}

} // namespace

std::string NewProjectText()
{
    std::string text;
    text += "<Project Sdk=\"Microsoft.NET.Sdk\">\n";
    text += "  <!-- The engine's settings for the scripts, rewritten by the editor on every load. -->\n";
    text += ImportLine(kEnginePropsFileName) + "\n";
    text += "\n";
    text += "  <!-- Add NuGet packages here as PackageReference items, or on the editor's Script Packages settings page. -->\n";
    text += "\n";
    text += ImportLine(kEngineTargetsFileName) + "\n";
    text += "</Project>\n";
    return text;
}

ReconcileResult ReconcileEngineImports(std::string& text)
{
    ReconcileResult result;
    std::string masked = MaskComments(text);
    const std::string_view newline = LineEnding(text);

    const std::size_t projectTag = FindTag(masked, "<Project", 0);
    if (projectTag == std::string::npos)
    {
        result.SdkMissing = true;
        return result;
    }
    const std::size_t projectTagEnd = masked.find('>', projectTag);
    if (projectTagEnd == std::string::npos)
    {
        result.SdkMissing = true;
        return result;
    }
    result.SdkMissing = !FindAttribute(masked, projectTag, projectTagEnd, "Sdk").has_value();

    if (!HasImport(masked, kEnginePropsFileName))
    {
        text.insert(projectTagEnd + 1, std::string(newline) + ImportLine(kEnginePropsFileName));
        result.PropsImportAdded = true;
        masked = MaskComments(text);
    }
    if (!HasImport(masked, kEngineTargetsFileName))
    {
        const std::size_t closing = masked.rfind("</Project>");
        if (closing == std::string::npos)
        {
            text += std::string(newline) + ImportLine(kEngineTargetsFileName);
        }
        else
        {
            const std::size_t lineStart = LineStart(text, closing);
            const bool closingStartsLine =
                std::all_of(text.begin() + static_cast<std::ptrdiff_t>(lineStart),
                            text.begin() + static_cast<std::ptrdiff_t>(closing), [](char c) { return c == ' ' || c == '\t'; });
            if (closingStartsLine)
                text.insert(lineStart, ImportLine(kEngineTargetsFileName) + std::string(newline));
            else
                text.insert(closing, std::string(newline) + ImportLine(kEngineTargetsFileName) + std::string(newline));
        }
        result.TargetsImportAdded = true;
    }
    return result;
}

bool DeclaresReferenceItems(std::string_view text)
{
    const std::string masked = MaskComments(text);
    return FindTag(masked, "<PackageReference", 0) != std::string_view::npos ||
           FindTag(masked, "<Reference", 0) != std::string_view::npos ||
           FindTag(masked, "<ProjectReference", 0) != std::string_view::npos;
}

std::vector<PackageReference> ReadPackageReferences(std::string_view text)
{
    return ReadItems<PackageReference>(text, kPackageReferenceItem);
}

void SetPackageReference(std::string& text, const PackageReference& package)
{
    SetItem(text, kPackageReferenceItem, package.Id, package.Version);
}

bool RemovePackageReference(std::string& text, std::string_view id)
{
    const std::string masked = MaskComments(text);
    for (const PackageElement& element : FindPackageElements(masked, kPackageReferenceItem))
    {
        if (!EqualsIgnoreCase(Slice(text, element.Include), id))
            continue;
        // Take the element's own line with it when nothing else shares that line.
        std::size_t begin = element.Start;
        std::size_t end = element.End;
        const std::size_t lineStart = LineStart(text, begin);
        const bool onlyWhitespaceBefore =
            std::all_of(text.begin() + static_cast<std::ptrdiff_t>(lineStart), text.begin() + static_cast<std::ptrdiff_t>(begin),
                        [](char c) { return c == ' ' || c == '\t'; });
        std::size_t lineEnd = end;
        while (lineEnd < text.size() && (text[lineEnd] == ' ' || text[lineEnd] == '\t'))
            ++lineEnd;
        if (onlyWhitespaceBefore && (lineEnd == text.size() || text[lineEnd] == '\r' || text[lineEnd] == '\n'))
        {
            begin = lineStart;
            end = lineEnd;
            if (end < text.size() && text[end] == '\r')
                ++end;
            if (end < text.size() && text[end] == '\n')
                ++end;
        }
        text.erase(begin, end - begin);
        return true;
    }
    return false;
}

std::vector<PackageVersion> ReadPackageVersions(std::string_view text)
{
    return ReadItems<PackageVersion>(text, kPackageVersionItem);
}

void SetPackageVersion(std::string& text, const PackageVersion& package)
{
    SetItem(text, kPackageVersionItem, package.Id, package.Version);
}

bool SetsCentralPackageVersions(std::string_view text)
{
    const std::string masked = MaskComments(text);
    constexpr std::string_view kOpen = "<ManagePackageVersionsCentrally>";
    for (std::size_t pos = masked.find(kOpen); pos != std::string::npos; pos = masked.find(kOpen, pos + 1))
    {
        const std::size_t begin = pos + kOpen.size();
        const std::size_t end = masked.find('<', begin);
        if (end == std::string::npos)
            return false;
        std::string_view value = std::string_view(masked).substr(begin, end - begin);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.remove_suffix(1);
        if (EqualsIgnoreCase(value, "true"))
            return true;
    }
    return false;
}

bool IsValidPackageId(std::string_view id)
{
    if (id.empty() || id.size() > 100)
        return false;
    const char first = id.front();
    if (!(std::isalnum(static_cast<unsigned char>(first)) || first == '_'))
        return false;
    return std::all_of(id.begin(), id.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'; });
}

bool IsValidPackageVersion(std::string_view version)
{
    if (version.empty() || version.size() > 64)
        return false;
    return std::all_of(version.begin(), version.end(),
                       [](char c)
                       {
                           return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+' ||
                                  c == '*' || c == ',' || c == '[' || c == ']' || c == '(' || c == ')' || c == ' ';
                       });
}

} // namespace GameEngine::ScriptsProjectFile
