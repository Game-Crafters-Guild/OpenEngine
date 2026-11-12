#include "Assets/Parsers/UIStyleAssetParser.h"
#include "Assets/Parsers/ParserExtractionHelpers.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/DepEdge.h"
#include "AssetCore/GUID.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include "UI/Assets/CssImports.h"
#include "UI/ControlAssetPaths.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{

// The source that ships the UI module's assets is the editor mount when one is registered,
// else the project (AssetCollector::CollectUIControlAssets uses the same rule). A sheet at a
// kControlAssetPaths path in that source is a control sheet.
StyleOrigin CascadeOriginForSheet(const std::filesystem::path& path, const AssetManager& assets)
{
    std::filesystem::path root = assets.GetSourceRoot(kAssetSourceAliasEditor);
    if (root.empty())
        root = assets.GetAssetRoot();
    std::string relative;
    if (!AssetRegistry::TryComputeCanonicalRelativePath(root, path, relative))
        return StyleOrigin::Author;
    // The registry folds the path's case on Windows.
    const bool controlSheet =
        std::any_of(UI::kControlAssetPaths.begin(), UI::kControlAssetPaths.end(),
                    [&relative](std::string_view controlPath) { return EqualsIgnoreCase(controlPath, relative); });
    return controlSheet ? StyleOrigin::Control : StyleOrigin::Author;
}

// Strip a CSS mount-prefix like "editor:" / "project:" / "@editor/" — any
// registered source alias, including package mounts ("water-core:..."), not
// a hard-coded list. The path-form DepEdge stores canonical mount-relative
// paths; the registry's ResolvePathTarget iterates all sources to find
// the right mount, so the prefix is informational at this layer.
bool IsAliasChar(char c)
{
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_' || c == '-';
}

std::string_view StripMountPrefix(std::string_view url)
{
    if (url.empty())
        return url;

    // "@<alias>/rel" form.
    if (url.front() == '@')
    {
        const size_t slash = url.find('/');
        if (slash == std::string_view::npos || slash == 1 || slash + 1 >= url.size())
            return url;
        for (size_t i = 1; i < slash; ++i)
        {
            if (!IsAliasChar(url[i]))
                return url;
        }
        return url.substr(slash + 1);
    }

    // "<alias>:rel" form. A single leading letter is a Windows drive, not an
    // alias (mirrors TrySplitAssetSourcePrefix).
    const size_t colon = url.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= url.size())
        return url;
    if (colon == 1 && std::isalpha(static_cast<unsigned char>(url[0])))
        return url;
    for (size_t i = 0; i < colon; ++i)
    {
        if (!IsAliasChar(url[i]))
            return url;
    }
    return url.substr(colon + 1);
}

// Find every `url(...)` occurrence in the CSS body and emit the inner
// value to the callback. Handles single-quoted, double-quoted, and
// unquoted forms, with leading/trailing whitespace trimmed. Comments
// (`/* ... */`) are skipped to avoid false positives in commented-out
// rules.
template <typename Emit>
void ScanUrls(std::string_view body, Emit emit)
{
    size_t i = 0;
    while (i < body.size())
    {
        // Skip CSS block comments.
        if (i + 1 < body.size() && body[i] == '/' && body[i + 1] == '*')
        {
            const size_t end = body.find("*/", i + 2);
            if (end == std::string_view::npos)
                return;
            i = end + 2;
            continue;
        }

        // Look for `url(`.
        if (body.compare(i, 4, "url(") != 0)
        {
            ++i;
            continue;
        }
        size_t valueStart = i + 4;
        while (valueStart < body.size() &&
               (body[valueStart] == ' ' || body[valueStart] == '\t'))
            ++valueStart;
        if (valueStart >= body.size())
            return;

        // Detect quoting style.
        char quote = 0;
        if (body[valueStart] == '"' || body[valueStart] == '\'')
        {
            quote = body[valueStart];
            ++valueStart;
        }
        size_t valueEnd = valueStart;
        if (quote)
        {
            valueEnd = body.find(quote, valueStart);
            if (valueEnd == std::string_view::npos)
                return;
        }
        else
        {
            valueEnd = body.find(')', valueStart);
            if (valueEnd == std::string_view::npos)
                return;
            // Trim trailing whitespace on unquoted form.
            while (valueEnd > valueStart &&
                   (body[valueEnd - 1] == ' ' || body[valueEnd - 1] == '\t'))
                --valueEnd;
        }
        emit(body.substr(valueStart, valueEnd - valueStart));

        // Advance past the closing `)`.
        size_t closeParen = body.find(')', valueEnd);
        if (closeParen == std::string_view::npos)
            return;
        i = closeParen + 1;
    }
}

} // namespace

AssetParseResult UIStyleAssetParser::Parse(const AssetMetadata& metadata, AssetManager& assetManager)
{
    return AssetParseResult(std::make_shared<UIStyleAsset>(metadata.Guid, metadata.Path,
                                                           CascadeOriginForSheet(metadata.Path, assetManager)));
}

bool UIStyleAssetParser::ExtractDependencies(const GUID& referrer,
                                             const AssetMetadata& metadata,
                                             DepEdgeSink& sink) const
{
    const std::string body = ParserExtraction::ReadFileBody(metadata.Path);
    if (body.empty())
        return false;

    // The url() of an @import names a style sheet, not an image, so the url() scan reads a copy
    // with each @import statement blanked.
    const std::vector<UI::CssImportStatement> imports = UI::FindTopLevelCssImports(body);
    std::string urlText = body;
    for (const UI::CssImportStatement& statement : imports)
        std::fill(urlText.begin() + statement.Begin, urlText.begin() + statement.End, ' ');

    uint32_t ordinal = 0;
    ScanUrls(urlText, [&](std::string_view rawUrl)
    {
        if (rawUrl.empty())
            return;

        // Skip data: URIs and external http(s) — those don't resolve
        // through the asset registry.
        if (rawUrl.size() >= 5 && (rawUrl.substr(0, 5) == "data:" ||
                                    rawUrl.substr(0, 5) == "http:"))
            return;
        if (rawUrl.size() >= 6 && rawUrl.substr(0, 6) == "https:")
            return;

        const std::string_view stripped = StripMountPrefix(rawUrl);
        if (stripped.empty())
            return;

        DepEdge e;
        e.Referrer = referrer;
        e.Kind = DepEdgeKind::UIElementImage;
        e.FieldLocator = "url[" + std::to_string(ordinal) + "]";
        e.Ordinal = ordinal++;
        if (ParserExtraction::LooksLikeGuid(stripped))
            e.Target = GUID(std::string(stripped).c_str());
        else
            e.TargetPath = ParserExtraction::NormalizeAuthoredPath(stripped);
        if (IsValidDepEdge(e))
            sink.Emit(std::move(e));
    });

    // The edge names the file the runtime @import loads, so it resolves by the runtime's rule.
    for (const UI::CssImportStatement& statement : imports)
    {
        const std::filesystem::path imported =
            UI::ResolveCssImportPath(metadata.Path, statement.Target, AssetManager::GetThreadCurrent());
        if (imported.empty())
        {
            Logger::Log::Debug("UIStyleAssetParser: the @import '{}' of '{}' resolves to no file", statement.Target,
                               metadata.Path.generic_string());
            continue;
        }
        DepEdge e;
        e.Referrer = referrer;
        e.Kind = DepEdgeKind::UIStyleImport;
        e.FieldLocator = "import[" + std::to_string(ordinal) + "]";
        e.Ordinal = ordinal++;
        e.TargetPath = imported.generic_string();
        sink.Emit(std::move(e));
    }

    return true;
}

} // namespace GameEngine
