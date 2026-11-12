#include "Engine/Build/ShipRules.h"

#include "AssetCore/AssetIgnoreRules.h"
#include "AssetCore/PathNormalization.h"
#include "AssetCore/SharedFileRead.h"
#include "AssetDatabase/AssetRecord.h"
#include "Assets/AssetRegistry.h"
#include "Engine/Build/AssetCollector.h"
#include "Types/PathUtils.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{

namespace
{

constexpr std::string_view kShipKey = "ship";

std::string FoldedKey(std::string_view relativePath)
{
    return AssetPaths::FoldStorePathKey(AssetPaths::CanonicalizeStorePath(std::string(relativePath)));
}

// Removes the last segment of a folded key: "data/maps" becomes "data", "data" becomes "".
void ToParentKey(std::string& key)
{
    const size_t slash = key.rfind('/');
    key.resize(slash == std::string::npos ? 0 : slash);
}

// Reads one folder file. False with `outError` set when the file is not {"ship": <rule>}.
bool TryReadFolderFile(const std::filesystem::path& file, const std::string& displayPath, ShipRule& outRule,
                       std::string& outError)
{
    String text;
    if (!ReadFileTextShared(file, text))
    {
        outError = displayPath + " cannot be read. Check that no other program holds it.";
        return false;
    }
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    const std::string fix = ". Fix the file or delete it.";
    if (!json.is_object())
    {
        outError = displayPath + " is not valid: it must be a JSON object such as {\"ship\": \"always\"}" + fix;
        return false;
    }
    for (const auto& [key, value] : json.items())
    {
        if (key != kShipKey)
        {
            outError = displayPath + " is not valid: \"" + key + "\" is not a folder setting; only \"ship\" is" + fix;
            return false;
        }
    }
    const auto ship = json.find(kShipKey);
    const std::optional<ShipRule> rule =
        ship != json.end() && ship->is_string() ? ShipRules::Parse(ship->get<std::string>()) : std::nullopt;
    if (!rule)
    {
        const std::string found = ship == json.end() ? "nothing" : ship->dump();
        outError = displayPath + " is not valid: \"ship\" must be \"always\", \"never\" or \"inferred\" (found " +
                   found + ")" + fix;
        return false;
    }
    outRule = *rule;
    return true;
}

} // namespace

ShipRules ShipRules::Load(const std::filesystem::path& assetRoot, std::vector<std::string>& outErrors)
{
    namespace fs = std::filesystem;
    ShipRules rules;
    std::error_code ec;
    if (!fs::is_directory(assetRoot, ec))
        return rules;

    const AssetIgnoreRules ignore = AssetIgnoreRules::LoadForAssetRoot(assetRoot);
    // The last entry read, for the error: the iterator holds no entry after a failed step.
    fs::path lastEntry = assetRoot;
    for (auto it = fs::recursive_directory_iterator(assetRoot, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        lastEntry = it->path();
        std::error_code entryError;
        if (it->is_directory(entryError))
        {
            if (ignore.ShouldIgnoreDirectory(it->path(), assetRoot))
                it.disable_recursion_pending();
            continue;
        }
        if (it->path().filename() != kFolderFileName)
            continue;

        std::string folder;
        const fs::path parent = it->path().parent_path();
        if (!AssetPaths::TryMakeCanonicalRelativePath(assetRoot, parent, folder) &&
            !fs::equivalent(parent, assetRoot, entryError))
            continue;
        rules.ReadFolderFile(assetRoot, folder, outErrors);
    }
    if (ec)
        outErrors.push_back("The folder files under " + PathToUtf8(assetRoot) +
                            " cannot all be read: the read stopped after " + PathToUtf8(lastEntry) + " (" +
                            ec.message() + "). Check the folder and export again.");
    return rules;
}

ShipRules ShipRules::LoadAbove(const std::filesystem::path& assetRoot, std::span<const std::string> relativePaths,
                               std::vector<std::string>& outErrors)
{
    ShipRules rules;
    for (const std::string& relativePath : relativePaths)
    {
        std::string folder = AssetPaths::CanonicalizeStorePath(relativePath);
        std::error_code ec;
        if (!std::filesystem::is_directory(assetRoot / PathFromUtf8(folder), ec))
            ToParentKey(folder);
        // The folder itself, then each parent, then the root ("").
        while (true)
        {
            if (rules.m_Folders.Find(AssetPaths::FoldStorePathKey(folder)) == nullptr)
                rules.ReadFolderFile(assetRoot, folder, outErrors);
            if (folder.empty())
                break;
            ToParentKey(folder);
        }
    }
    return rules;
}

void ShipRules::ReadFolderFile(const std::filesystem::path& assetRoot, const std::string& folder,
                               std::vector<std::string>& outErrors)
{
    const std::filesystem::path file = (folder.empty() ? assetRoot : assetRoot / PathFromUtf8(folder)) / kFolderFileName;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec))
        return;
    std::string folderDisplay = folder;
    if (!folderDisplay.empty())
        folderDisplay.push_back('/');
    std::string error;
    ShipRule rule = ShipRule::Inferred;
    if (!TryReadFolderFile(file, folderDisplay + std::string(kFolderFileName), rule, error))
    {
        outErrors.push_back(std::move(error));
        return;
    }
    m_Folders.InsertOrAssign(AssetPaths::FoldStorePathKey(folder), Resolved{rule, Source::Folder, folderDisplay});
}

void ShipRules::LoadAssetRules(const AssetRegistry& registry, std::string_view sourceAlias,
                               const std::filesystem::path& assetRoot, std::vector<std::string>& outErrors)
{
    const std::string metaKey(kAssetMetaKey);
    registry.VisitSourceStoreRecords(sourceAlias, [&](const AssetDatabase::AssetRecord& record)
    {
        const auto stored = record.kv.find(metaKey);
        if (record.missing || stored == record.kv.end())
            return;
        const std::optional<ShipRule> rule = Parse(stored->second);
        const std::filesystem::path file = PathFromUtf8(record.path);
        const std::string display = SpelledRelativePath(assetRoot, assetRoot / file);
        if (!rule)
            outErrors.push_back("The ship rule stored for " + display + " is \"" + stored->second +
                                "\". Set it again in the inspector.");
        else if (*rule == ShipRule::Always && !AssetCollector::IsShippableContent(file))
            outErrors.push_back(display + " is set to Always ship, but source files, Editor/ folders, build output "
                                          "and scene backups never ship. Clear the rule.");
        else
            SetAssetRule(record.path, *rule);
    });
}

void ShipRules::SetAssetRule(std::string_view relativePath, ShipRule rule)
{
    m_Assets.InsertOrAssign(FoldedKey(relativePath), rule);
}

ShipRules::Resolved ShipRules::Resolve(std::string_view relativePath) const
{
    if (IsEmpty())
        return {};
    const std::string key = FoldedKey(relativePath);
    if (const ShipRule* own = m_Assets.Find(key))
        return {*own, Source::Asset, {}};
    if (const Resolved* ownFolder = m_Folders.Find(key))
        return *ownFolder;
    return ResolveInherited(relativePath);
}

std::optional<ShipRule> ShipRules::OwnRule(std::string_view relativePath) const
{
    const std::string key = FoldedKey(relativePath);
    if (const ShipRule* own = m_Assets.Find(key))
        return *own;
    if (const Resolved* ownFolder = m_Folders.Find(key))
        return ownFolder->Rule;
    return std::nullopt;
}

ShipRules::Resolved ShipRules::ResolveInherited(std::string_view relativePath) const
{
    if (m_Folders.Empty())
        return {};
    std::string key = FoldedKey(relativePath);
    // The nearest folder first: the path's own folder, then each parent, then the root.
    while (!key.empty())
    {
        ToParentKey(key);
        if (const Resolved* found = m_Folders.Find(key))
            return *found;
    }
    return {};
}

void ShipRules::ReportNeverEntries(const AssetManifest& manifest, const std::filesystem::path& assetRoot,
                                   std::string_view consequence, std::vector<std::string>& outErrors) const
{
    if (IsEmpty())
        return;
    for (const AssetManifestEntry& entry : manifest.entries)
    {
        std::string relative;
        if (!AssetPaths::TryMakeCanonicalRelativePath(assetRoot, entry.sourcePath, relative))
            continue;
        const Resolved resolved = Resolve(relative);
        if (resolved.Rule == ShipRule::Never)
            outErrors.push_back(DescribeNever(SpelledRelativePath(assetRoot, entry.sourcePath), resolved) + ", but " +
                                std::string(consequence));
    }
}

std::string ShipRules::SpelledRelativePath(const std::filesystem::path& assetRoot, const std::filesystem::path& file)
{
    // The registry folds paths on case-insensitive file systems. fs::relative reads the spelling on disk.
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(file, assetRoot, ec);
    if (!ec && !relative.empty() && *relative.begin() != "..")
        return PathToUtf8(relative);
    std::string canonical;
    return AssetPaths::TryMakeCanonicalRelativePath(assetRoot, file, canonical) ? canonical : std::string();
}

std::string ShipRules::DescribeNever(std::string_view relativePath, const Resolved& resolved)
{
    std::string sentence = std::string(relativePath) + " is set to Never ship";
    if (resolved.From == Source::Asset)
        sentence += " (from its own rule)";
    else if (resolved.From == Source::Folder)
        sentence += " (from " + DescribeFolder(resolved) + ")";
    return sentence;
}

std::string ShipRules::DescribeFolder(const Resolved& resolved)
{
    return resolved.Folder.empty() ? "the asset root's " + std::string(kFolderFileName) : "its folder " + resolved.Folder;
}

std::optional<ShipRule> ShipRules::Parse(std::string_view text)
{
    if (text == "always")
        return ShipRule::Always;
    if (text == "never")
        return ShipRule::Never;
    if (text == "inferred")
        return ShipRule::Inferred;
    return std::nullopt;
}

std::string_view ShipRules::ToText(ShipRule rule)
{
    switch (rule)
    {
    case ShipRule::Always:
        return "always";
    case ShipRule::Never:
        return "never";
    case ShipRule::Inferred:
        break;
    }
    return "inferred";
}

} // namespace GameEngine
