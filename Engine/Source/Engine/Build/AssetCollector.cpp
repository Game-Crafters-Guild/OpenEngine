#include "Engine/Build/AssetCollector.h"
#include "AssetCore/EditorOnlyAssetPath.h"
#include "AssetCore/GUID.h"
#include "AssetCore/PathNormalization.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Packages/PackageResolver.h"
#include "Assets/Parsers/ParserExtractionHelpers.h"
#include "Assets/Parsers/SceneAssetParser.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Engine/GameUI/GameUIHost.h"
#include "UI/ControlAssetPaths.h"
#include "Logger/Logger.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <algorithm>
#include <unordered_set>

namespace GameEngine {

// --- AssetManifest ---

const AssetManifestEntry* AssetManifest::FindByGuid(const GUID& guid) const
{
    for (const auto& e : entries)
        if (e.guid == guid)
            return &e;
    return nullptr;
}

bool AssetManifest::ContainsGuid(const GUID& guid) const
{
    return FindByGuid(guid) != nullptr;
}

// --- AssetCollector ---

AssetCollector::AssetCollector(AssetManager& assetManager)
    : m_AssetManager(assetManager)
{
    // Cache roots once to avoid allocating GetRegisteredSources() per asset.
    m_ProjectAssetRoot = m_AssetManager.GetRegistry().GetAssetRoot();
    for (const auto& source : m_AssetManager.GetRegistry().GetRegisteredSources())
        m_SourceRoots.push_back({source.Alias, source.Root});
}

std::filesystem::path AssetCollector::PackageAssetOutputPath(std::string_view alias,
                                                             const std::filesystem::path& relPath)
{
    return std::filesystem::path("Packages") / alias / "Assets" / relPath.lexically_normal();
}

uint64_t AssetCollector::ComputeContentHash(const AssetMetadata& meta)
{
    // Simple hash combining file size and last-modified time for change detection.
    // Future: use actual content hash (e.g., xxHash of file contents).
    uint64_t h = static_cast<uint64_t>(meta.FileSize);
    const auto mtime = meta.LastModified.time_since_epoch().count();
    h ^= static_cast<uint64_t>(mtime) * 0x9E3779B97F4A7C15ULL;
    return h;
}

// C++/C# source and project/build files are registered asset types but must
// never ship in a redistributable game bundle (source/IP leak + bloat). Shader
// sources (.glsl/.frag/.vert/.comp) ARE runtime content and stay.
//
// Pass source-root-RELATIVE paths where available: the Editor/ convention below
// scopes to segments under the asset root, and an absolute path whose ancestry
// happens to contain an Editor/ directory above the root would deny wrongly.
bool AssetCollector::IsShippableContent(const std::filesystem::path& path)
{
    const std::string name = PathToUtf8(path.filename());
    if (!name.empty() && name[0] == '.') // .DS_Store and other dotfiles
        return false;

    const std::string ext = LowerCaseExtension(path);
    static const std::unordered_set<std::string> kDenied = {
        ".cs", ".csproj", ".sln", ".cpp", ".cc", ".cxx", ".c",
        ".h", ".hpp", ".hxx", ".hh", ".meta"};
    if (kDenied.count(ext) != 0)
        return false;
    // The scene editor's backup copy of a scene (<name>.backup.scene).
    if (AssetPaths::FoldStorePathKey(name).ends_with(".backup.scene"))
        return false;

    // obj/ and bin/ build-output directories, any segment.
    for (const auto& seg : path)
    {
        const std::string s = PathToUtf8(seg);
        if (s == "obj" || s == "bin")
            return false;
    }

    // Editor-only content is mounted in the editor and never staged into a
    // build; the folder convention is shared with package code discovery.
    return !IsEditorOnlyAssetPath(path);
}

bool AssetCollector::TryAddAsset(const GUID& guid, AssetManifest& manifest)
{
    if (guid.IsNull())
        return false;
    if (manifest.ContainsGuid(guid))
        return false;

    auto& registry = m_AssetManager.GetRegistry();
    AssetMetadata meta;
    if (!registry.TryGetAssetMetadata(guid, meta))
    {
        // Typically an asset whose mount is absent (disabled/unresolved package).
        // Recorded for build validation to attribute and fail loudly on.
        Logger::Log::Warning("AssetCollector: Could not find metadata for GUID {}", guid.ToString());
        manifest.unresolvedDependencies.push_back(guid);
        return false;
    }

    // Skip assets without a valid source path.
    if (meta.Path.empty())
        return false;

    namespace fs = std::filesystem;

    // Source-root-relative path first: it keys the output layout below AND is
    // the correct scope for the shippable check (the Editor/ deny must only see
    // segments under the owning asset root, not the root's own ancestry).
    // Relative metadata paths are project-source records already. The deepest
    // matching root wins so an asset under a nested root relativizes against
    // its own mount. Package-owned assets stage under Packages/<alias>/Assets/,
    // so same-relative-path assets from different mounts can never collide in
    // the staged output.
    fs::path relPath = meta.Path;
    std::string ownerAlias(kAssetSourceAliasProject);
    if (meta.Path.is_absolute())
    {
        size_t bestRootLen = 0;
        for (const auto& source : m_SourceRoots)
        {
            if (source.Root.empty())
                continue;
            std::error_code ec;
            fs::path candidate = fs::relative(meta.Path, source.Root, ec);
            if (ec || candidate.empty() || candidate.string().find("..") == 0)
                continue;
            const size_t rootLen = source.Root.generic_string().size();
            if (rootLen > bestRootLen)
            {
                bestRootLen = rootLen;
                relPath = std::move(candidate);
                ownerAlias = source.Alias;
            }
        }
        if (relPath.is_absolute())
            relPath = meta.Path.filename();
    }

    // Don't ship source/build files or Editor/-convention folders.
    if (!IsShippableContent(relPath))
        return false;

    AssetManifestEntry entry;
    entry.guid = guid;
    entry.type = meta.Type;

    // Ensure sourcePath is absolute for CopyAssets to find the file.
    entry.sourcePath = meta.Path.is_absolute()
        ? meta.Path
        : (m_ProjectAssetRoot / meta.Path).lexically_normal();
    // Project and editor assets keep the flat Assets/ layout; package assets get
    // their alias-scoped subtree (see PackageAssetOutputPath).
    const bool isPackageAlias =
        ownerAlias != kAssetSourceAliasProject && ownerAlias != kAssetSourceAliasEditor;
    entry.outputPath = isPackageAlias ? PackageAssetOutputPath(ownerAlias, relPath)
                                      : fs::path("Assets") / relPath.lexically_normal();
    entry.sourceAlias = std::move(ownerAlias);
    entry.contentHash = ComputeContentHash(meta);

    manifest.entries.push_back(std::move(entry));
    return true;
}

void AssetCollector::WalkDependencies(const GUID& root, AssetManifest& manifest, EdgeRefresh refresh)
{
    // BFS transitive dependency walk with cycle detection.
    // The visited set replaces manifest.ContainsGuid() for O(1) duplicate checks
    // instead of O(n) linear scan per asset.
    std::vector<GUID> queue;
    std::unordered_set<GUID> visited;
    visited.reserve(512);
    // The asset whose edge first reached each node, for the chain a Never error names.
    FlatMap<GUID, GUID> reachedFrom;

    queue.push_back(root);
    visited.insert(root);

    // A parser resolves a reference by the runtime's rule, which reads the mounts of this manager.
    AssetManager::ScopedThreadAssetManager currentManager(&m_AssetManager);
    auto& registry = m_AssetManager.GetRegistry();

    while (!queue.empty())
    {
        GUID current = queue.back();
        queue.pop_back();

        if (current != root && IsNeverShipped(current))
        {
            std::vector<std::string> chain{DisplayPath(current)};
            for (const GUID* from = reachedFrom.Find(current); from != nullptr; from = reachedFrom.Find(*from))
                chain.push_back(DisplayPath(*from));
            std::reverse(chain.begin(), chain.end());
            m_ShipErrors.push_back(ShipRules::DescribeNever(chain.back(), m_ShipRules.Resolve(ProjectRelativePath(current))) +
                                   ", but a shipped asset uses it: " + Join(chain, " → ") +
                                   ". Remove the reference, or change the rule.");
            continue;
        }
        TryAddAsset(current, manifest);

        // A cached edge set can be stale: a root (a built scene or pipeline) may be new, and a style
        // sheet or shader source resolves @import and #include through the thread's manager, which the
        // cache does not record. These re-extract now; other nodes rely on the modified-file invalidation.
        AssetMetadata meta;
        const bool haveMeta = registry.TryGetAssetMetadata(current, meta);
        const bool reextract =
            current == root || (haveMeta && (meta.Type == AssetType::UIStyle || meta.Type == AssetType::Shader));
        Vector<GUID> deps;
        // An Always walk re-extracts every node once per collection: the cache can miss by-path references.
        if (refresh == EdgeRefresh::EveryNode)
        {
            const Vector<GUID>* fresh = m_FreshEdges.Find(current);
            deps = fresh != nullptr ? *fresh : m_FreshEdges.InsertOrAssign(current, registry.RefreshDependencies(current));
        }
        else
        {
            deps = reextract ? registry.RefreshDependencies(current) : registry.GetDependencies(current);
        }

        // Loud per-asset trace: which assets contributed edges to the shipped
        // manifest is exactly what a "package X never referenced" build report
        // needs to be debuggable.
        Logger::Log::Info("AssetCollector: '{}' -> {} dependency edge(s){}",
                          haveMeta ? meta.Path.generic_string() : current.ToString(),
                          deps.size(),
                          (refresh == EdgeRefresh::EveryNode || reextract) ? " (re-extracted)" : "");

        for (const GUID& dep : deps)
        {
            if (!dep.IsNull() && visited.find(dep) == visited.end())
            {
                visited.insert(dep);
                reachedFrom.InsertOrAssign(dep, current);
                queue.push_back(dep);
            }
        }
    }
}

bool AssetCollector::CollectGameContent(const std::vector<std::string>& scenePaths,
                                        const std::string& renderPipelinePath,
                                        const std::filesystem::path& projectAssetRoot, AssetManifest& outManifest,
                                        std::vector<std::string>& outWarnings, std::string& outError)
{
    namespace fs = std::filesystem;
    m_ShipErrors.clear();
    m_FreshEdges.Clear();
    m_ShipRules = ShipRules::Load(projectAssetRoot, m_ShipErrors);
    // The folder rules have keys relative to projectAssetRoot, and the walk has keys relative to the registry's root.
    std::error_code rootError;
    if (!m_ShipRules.IsEmpty() && !fs::equivalent(projectAssetRoot, m_ProjectAssetRoot, rootError))
        m_ShipErrors.push_back("The export reads folder rules under " + PathToUtf8(projectAssetRoot) +
                               ", but the asset registry's root is " + PathToUtf8(m_ProjectAssetRoot) +
                               ", so no rule can apply. Open the project again and export again.");
    // The asset rules have keys from the registry's store, so they load after the root check.
    m_ShipRules.LoadAssetRules(m_AssetManager.GetRegistry(), kAssetSourceAliasProject, projectAssetRoot, m_ShipErrors);
    if (!m_ShipErrors.empty())
    {
        outError = Join(m_ShipErrors, "\n");
        return false;
    }

    std::vector<std::string> shippedScenes;
    for (const std::string& scenePath : scenePaths)
    {
        const ShipRules::Resolved resolved = m_ShipRules.Resolve(scenePath);
        if (resolved.Rule != ShipRule::Never)
            shippedScenes.push_back(scenePath);
        else
            m_ShipErrors.push_back(ShipRules::DescribeNever(scenePath, resolved) +
                                   ", but it is in the build's scene list. Remove it from the list, or change the rule.");
    }
    outManifest = CollectFromScenes(shippedScenes);
    CollectAlwaysShipped(outManifest, outWarnings);
    if (!m_ShipErrors.empty())
    {
        outError = Join(m_ShipErrors, "\n");
        return false;
    }

    // If the scene and Always walks found nothing, fall back to the project's
    // registered assets. This handles cases where the asset registry hasn't
    // scanned the project yet or scene paths don't match.
    if (outManifest.entries.empty())
    {
        Logger::Log::Warning("AssetCollector: no assets found via the dependency graph, falling back to the project's assets");
        outManifest = CollectAllAssets();
    }

    // If the registry holds nothing either, stage the shippable files of the
    // project's asset directory: the dependency graph may not be populated when
    // the build runs on a background thread.
    if (outManifest.entries.empty())
    {
        std::error_code ec;
        if (fs::is_directory(projectAssetRoot, ec))
        {
            for (const auto& entry : fs::recursive_directory_iterator(projectAssetRoot, ec))
            {
                if (!entry.is_regular_file())
                    continue;
                // Skip Shaders: the export stages the engine's shaders separately.
                const fs::path rel = fs::relative(entry.path(), projectAssetRoot);
                if (rel.generic_string().rfind("Shaders/", 0) == 0)
                    continue;
                // Checked on the root-relative path so the Editor/ deny only sees
                // segments under the asset root.
                if (!IsShippableContent(rel) || m_ShipRules.Resolve(PathToUtf8(rel)).Rule == ShipRule::Never)
                    continue;
                AssetManifestEntry manifestEntry;
                manifestEntry.sourcePath = entry.path();
                manifestEntry.outputPath = fs::path("Assets") / rel;
                outManifest.entries.push_back(std::move(manifestEntry));
            }
            Logger::Log::Info("AssetCollector: Added {} asset files from the project directory",
                              outManifest.entries.size());
        }
    }

    // Whichever way the content was collected, the pipeline game.config names ships
    // in the manifest: the Player resolves it by path through the .assetmanifest. A
    // packaged game has no stand-in pipeline, so one missing from the package draws nothing.
    if (!CollectRenderPipeline(renderPipelinePath, outManifest))
        outWarnings.push_back("Selected render pipeline not found; the game will draw nothing: " + renderPipelinePath);

    if (!CollectRuntimeHumanoidProfile(outManifest))
    {
        outError = "Runtime humanoid profile is missing or conflicts with project content; see the asset collection log";
        return false;
    }
    if (!CollectDefaultUIFont(outManifest))
    {
        outError = "Default game UI font is missing; see the asset collection log";
        return false;
    }
    for (const std::string& missing : CollectUIControlAssets(outManifest))
        outWarnings.push_back("UI control asset not found; the control draws unstyled: " + missing);
    // The walks and the fallbacks add no Never asset, so a Never entry here is an engine default.
    m_ShipRules.ReportNeverEntries(outManifest, projectAssetRoot,
                                   "every game ships it as an engine default (the render pipeline, the runtime "
                                   "humanoid profile, the default UI font or the UI control assets). Change the rule.",
                                   m_ShipErrors);
    if (!m_ShipErrors.empty())
    {
        outError = Join(m_ShipErrors, "\n");
        return false;
    }
    return true;
}

void AssetCollector::CollectAlwaysShipped(AssetManifest& manifest, std::vector<std::string>& outWarnings)
{
    if (m_ShipRules.IsEmpty())
        return;
    // Sorted by path, so the manifest order does not depend on the registry's order.
    std::vector<std::pair<std::string, GUID>> roots;
    std::vector<std::string> keptOut;
    for (const GUID& guid : m_AssetManager.GetRegistry().GetAssetsForSource(std::string(kAssetSourceAliasProject)))
    {
        std::string relative = ProjectRelativePath(guid);
        if (relative.empty() || !IsShippableContent(PathFromUtf8(relative)))
            continue;
        const ShipRules::Resolved resolved = m_ShipRules.Resolve(relative);
        if (resolved.Rule == ShipRule::Always)
        {
            roots.emplace_back(std::move(relative), guid);
            continue;
        }
        // An own Never inside an Always folder: the folder would ship it, so the build says it does not.
        const ShipRules::Resolved inherited = m_ShipRules.ResolveInherited(relative);
        if (resolved.From == ShipRules::Source::Asset && resolved.Rule == ShipRule::Never &&
            inherited.Rule == ShipRule::Always)
            keptOut.push_back(ShipRules::DescribeNever(DisplayPath(guid), resolved) +
                              ", so it does not ship, although " + ShipRules::DescribeFolder(inherited) + " is Always.");
    }
    std::sort(keptOut.begin(), keptOut.end());
    outWarnings.insert(outWarnings.end(), keptOut.begin(), keptOut.end());
    std::sort(roots.begin(), roots.end());
    const size_t before = manifest.entries.size();
    for (const auto& [relative, guid] : roots)
        WalkDependencies(guid, manifest, EdgeRefresh::EveryNode);
    Logger::Log::Info("AssetCollector: {} asset(s) set to Always ship add {} asset(s)", roots.size(),
                      manifest.entries.size() - before);
}

std::string AssetCollector::ProjectRelativePath(const GUID& guid)
{
    AssetMetadata meta;
    if (!m_AssetManager.GetRegistry().TryGetAssetMetadata(guid, meta) || meta.Path.empty())
        return {};
    if (meta.Path.is_relative())
        return AssetPaths::CanonicalizeStorePath(PathToUtf8(meta.Path));
    std::string relative;
    if (!AssetPaths::TryMakeCanonicalRelativePath(m_ProjectAssetRoot, meta.Path, relative))
        return {};
    return relative;
}

std::string AssetCollector::DisplayPath(const GUID& guid)
{
    AssetMetadata meta;
    if (!m_AssetManager.GetRegistry().TryGetAssetMetadata(guid, meta) || meta.Path.empty())
        return guid.ToString();
    const std::filesystem::path file = meta.Path.is_absolute() ? meta.Path : m_ProjectAssetRoot / meta.Path;
    std::string relative = ShipRules::SpelledRelativePath(m_ProjectAssetRoot, file);
    return relative.empty() ? PathToUtf8(meta.Path) : relative;
}

bool AssetCollector::IsNeverShipped(const GUID& guid)
{
    if (m_ShipRules.IsEmpty())
        return false;
    const std::string relative = ProjectRelativePath(guid);
    return !relative.empty() && m_ShipRules.Resolve(relative).Rule == ShipRule::Never;
}

AssetManifest AssetCollector::CollectFromScenes(const std::vector<std::string>& scenePaths)
{
    AssetManifest manifest;
    auto& registry = m_AssetManager.GetRegistry();

    // Resolve scene paths to GUIDs and walk their dependencies.
    for (const auto& scenePath : scenePaths)
    {
        AssetMetadata sceneMeta;
        if (registry.TryGetAssetMetadata(std::filesystem::path(scenePath), sceneMeta))
        {
            Logger::Log::Info("AssetCollector: Walking dependencies for scene '{}'", scenePath);
            WalkDependencies(sceneMeta.Guid, manifest);
        }
        else
        {
            Logger::Log::Warning("AssetCollector: Scene '{}' not found in registry", scenePath);
        }
    }

    Logger::Log::Info("AssetCollector: Collected {} assets from {} scene(s)",
                      manifest.entries.size(), scenePaths.size());
    return manifest;
}

AssetManifest AssetCollector::CollectAllAssets()
{
    AssetManifest manifest;
    auto& registry = m_AssetManager.GetRegistry();

    // Project source ONLY. This is the fallback when the dependency walk finds
    // nothing; it must ship the game's content, never the unconditionally-mounted
    // 'editor' source (chrome icons/fonts/UI and ALL default rendergraphs). The
    // selected render pipeline is appended by CollectRenderPipeline, the default
    // game UI font by CollectDefaultUIFont.
    auto guids = registry.GetAssetsForSource(std::string(kAssetSourceAliasProject));
    for (const GUID& guid : guids)
    {
        if (!IsNeverShipped(guid))
            TryAddAsset(guid, manifest);
    }

    Logger::Log::Info("AssetCollector: Collected {} project asset(s) (fallback)", manifest.entries.size());
    return manifest;
}

bool AssetCollector::CollectRuntimeHumanoidProfile(AssetManifest& manifest)
{
    const auto guid = ResolveRuntimeHumanoidProfile(m_AssetManager);
    if (guid.IsNull())
        return false;
    const auto* existing = manifest.FindByGuid(guid);
    AssetManifest pending;
    if (!existing && !TryAddAsset(guid, pending))
        return false;
    const auto& profile = existing ? *existing : pending.entries.front();
    const auto key = AssetPaths::FoldStorePathKey(profile.outputPath.generic_string());
    for (const auto& entry : manifest.entries)
    {
        if (entry.guid != guid && AssetPaths::FoldStorePathKey(entry.outputPath.generic_string()) == key)
        {
            Logger::Log::Error("AssetCollector: runtime humanoid profile conflicts with '{}' at '{}'; "
                               "move the project-authored profile to a distinct asset path",
                               entry.sourcePath.string(), profile.outputPath.generic_string());
            return false;
        }
    }
    if (!existing)
        manifest.entries.push_back(std::move(pending.entries.front()));
    return true;
}

bool AssetCollector::CollectDefaultUIFont(AssetManifest& manifest)
{
    const std::filesystem::path source = m_AssetManager.ResolveAssetPath(GameUIHost::kDefaultFontPath);
    AssetMetadata meta;
    std::error_code ec;
    if (source.empty() || !std::filesystem::exists(source, ec) ||
        !m_AssetManager.GetRegistry().TryGetAssetMetadata(source, meta))
    {
        Logger::Log::Error("AssetCollector: default UI font '{}' is not supplied by any asset source",
                           GameUIHost::kDefaultFontPath);
        return false;
    }
    if (manifest.ContainsGuid(meta.Guid) || TryAddAsset(meta.Guid, manifest))
        return true;

    Logger::Log::Error("AssetCollector: default UI font '{}' resolves to '{}', which is not shippable content; "
                       "move it out of there",
                       GameUIHost::kDefaultFontPath, source.string());
    return false;
}

std::vector<std::string> AssetCollector::CollectUIControlAssets(AssetManifest& manifest)
{
    const std::string_view source = m_AssetManager.GetSourceRoot(kAssetSourceAliasEditor).empty()
                                        ? kAssetSourceAliasProject
                                        : kAssetSourceAliasEditor;
    std::vector<std::string> missing;
    for (std::string_view path : UI::kControlAssetPaths)
    {
        const GUID guid = m_AssetManager.ResolveAssetGuid(std::string(path), source);
        if (!guid.IsNull())
            WalkDependencies(guid, manifest);
        if (guid.IsNull() || !manifest.ContainsGuid(guid))
            missing.emplace_back(path);
    }
    return missing;
}

bool AssetCollector::CollectRenderPipeline(const std::string& renderPipelinePath, AssetManifest& manifest)
{
    if (renderPipelinePath.empty())
        return true;

    // game.config names the pipeline by asset path. Look it up where the mounts
    // resolve it, not against the project root: the default pipelines live in the
    // editor mount.
    const std::filesystem::path source = m_AssetManager.ResolveAssetPath(renderPipelinePath);
    AssetMetadata meta;
    std::error_code ec;
    if (source.empty() || !std::filesystem::exists(source, ec) ||
        !m_AssetManager.GetRegistry().TryGetAssetMetadata(source, meta))
    {
        Logger::Log::Warning("AssetCollector: Render pipeline '{}' is not supplied by any asset source",
                             renderPipelinePath);
        return false;
    }

    Logger::Log::Info("AssetCollector: Walking dependencies for pipeline '{}'", renderPipelinePath);
    WalkDependencies(meta.Guid, manifest);
    if (manifest.ContainsGuid(meta.Guid))
        return true;

    Logger::Log::Warning("AssetCollector: Render pipeline '{}' resolves to '{}', which is not shippable content "
                         "(an Editor/, obj/ or bin/ folder, a dotfile or a source file); move it out of there",
                         renderPipelinePath, source.string());
    return false;
}

std::unordered_set<std::string> AssetCollector::CollectComponentNames(const AssetManifest& manifest)
{
    std::unordered_set<std::string> names;
    for (const auto& entry : manifest.entries)
    {
        if (entry.type == AssetType::Scene || entry.type == AssetType::Prefab)
            SceneAssetParser::CollectComponentNames(ParserExtraction::ReadFileBody(entry.sourcePath), names);
    }
    return names;
}

namespace
{

// True when the canonical asset path `path` is `root` or lies under it; both are
// folded store keys, so case and separators do not decide.
bool IsAtOrUnder(const std::string& path, const std::string& root)
{
    return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
                            path[root.size()] == '/');
}

} // namespace

std::vector<std::string> AssetCollector::CollectPackageRuntimeAssets(
    const ResolvedPackage& package, const std::unordered_set<std::string>& usedComponents, AssetManifest& manifest)
{
    struct ShippedRoot
    {
        std::string Key;
        std::string Label; // "<component>: <path>" for the unmatched report; empty for Shaders/
        bool Matched = false;
    };
    std::vector<ShippedRoot> roots{{AssetPaths::FoldStorePathKey("Shaders"), {}, true}};
    for (const auto& [component, paths] : package.Manifest.RuntimeAssets)
    {
        if (usedComponents.count(component) == 0)
            continue;
        for (const std::string& path : paths)
            roots.push_back({AssetPaths::FoldStorePathKey(AssetPaths::CanonicalizeStorePath(path)),
                             component + ": " + path, false});
    }

    auto& registry = m_AssetManager.GetRegistry();
    const std::filesystem::path sourceRoot = m_AssetManager.GetSourceRoot(package.Alias);
    const size_t before = manifest.entries.size();
    for (const GUID& guid : registry.GetAssetsForSource(package.Alias))
    {
        AssetMetadata meta;
        std::string relative;
        if (!registry.TryGetAssetMetadata(guid, meta) ||
            !AssetPaths::TryMakeCanonicalRelativePath(sourceRoot, meta.Path, relative))
            continue;
        const std::string key = AssetPaths::FoldStorePathKey(relative);
        bool ships = false;
        for (ShippedRoot& root : roots)
        {
            if (IsAtOrUnder(key, root.Key))
                ships = root.Matched = true;
        }
        if (ships)
            WalkDependencies(guid, manifest);
    }

    std::vector<std::string> unmatched;
    for (const ShippedRoot& root : roots)
    {
        if (!root.Matched)
            unmatched.push_back(root.Label);
    }
    Logger::Log::Info("AssetCollector: package '{}' adds {} asset(s) from its Shaders/ folder and runtimeAssets",
                      package.Alias, manifest.entries.size() - before);
    return unmatched;
}

} // namespace GameEngine
