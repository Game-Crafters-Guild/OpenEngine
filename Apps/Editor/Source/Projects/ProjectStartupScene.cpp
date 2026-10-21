#include "Projects/ProjectStartupScene.h"

#include "Engine/Build/GameConfig.h"
#include "Logger/Logger.h"
#include "Types/PathUtils.h"

#include <system_error>

namespace GameEngine::Editor
{

namespace fs = std::filesystem;

namespace
{

constexpr const char* kSceneExtension = ".scene";
// Autosave staging beside a scene; never a scene a project opens on.
constexpr const char* kBackupSceneSuffix = ".backup.scene";

// Spelled with the casing on disk, so two spellings of one file compare equal.
fs::path CanonicalPath(const fs::path& path)
{
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    return (ec ? path : canonical).lexically_normal();
}

bool IsSceneFile(const fs::path& path)
{
    const std::string name = path.filename().string();
    return name.ends_with(kSceneExtension) && !name.ends_with(kBackupSceneSuffix);
}

// The absolute scene an asset-relative name refers to, when it is a scene file
// inside the project's Assets folder.
std::optional<fs::path> ResolveNamedScene(const fs::path& assetsRoot, const std::string& assetRelative,
                                          const char* source)
{
    if (assetRelative.empty())
        return std::nullopt;
    const fs::path named(assetRelative);
    const fs::path absolute = CanonicalPath(assetsRoot / named);
    std::error_code ec;
    if (named.is_absolute() || RelativePathUnderRoot(absolute, assetsRoot).empty() ||
        !IsSceneFile(absolute) || !fs::is_regular_file(absolute, ec))
    {
        Logger::Log::Warning("Editor: the project's {} '{}' is not a scene file under '{}'; skipping it",
                             source, assetRelative, assetsRoot.string());
        return std::nullopt;
    }
    return absolute;
}

} // namespace

std::optional<fs::path> ResolveProjectStartupScene(const fs::path& projectRoot,
                                                   const ProjectStartupSceneCandidates& candidates)
{
    const fs::path assetsRoot = CanonicalPath(projectRoot / "Assets");

    if (!candidates.GlobalBuildScenes.empty())
    {
        if (auto scene = ResolveNamedScene(assetsRoot, candidates.GlobalBuildScenes.front(),
                                           "first build scene"))
            return scene;
    }
    if (auto scene = ResolveNamedScene(assetsRoot, candidates.StartupScene, kProjectStartupSceneKey))
        return scene;

    for (const fs::path& recent : candidates.RecentScenes)
    {
        const fs::path absolute = CanonicalPath(recent);
        std::error_code ec;
        if (!RelativePathUnderRoot(absolute, assetsRoot).empty() && IsSceneFile(absolute) &&
            fs::is_regular_file(absolute, ec))
            return absolute;
    }
    return std::nullopt;
}

std::string FindNewProjectStartupScene(const fs::path& projectRoot)
{
    const fs::path assetsRoot = CanonicalPath(projectRoot / "Assets");
    std::error_code ec;

    const fs::path gameConfigPath = projectRoot / "game.config";
    if (fs::is_regular_file(gameConfigPath, ec))
    {
        const GameConfig config = LoadGameConfig(gameConfigPath);
        if (ResolveNamedScene(assetsRoot, config.startupScene, "game.config startupScene"))
            return fs::path(config.startupScene).generic_string();
    }

    fs::path onlyScene;
    for (fs::recursive_directory_iterator it(assetsRoot, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec) || !IsSceneFile(it->path()))
            continue;
        if (!onlyScene.empty())
            return {};
        onlyScene = it->path();
    }
    return onlyScene.empty() ? std::string{}
                             : RelativePathUnderRoot(CanonicalPath(onlyScene), assetsRoot).generic_string();
}

} // namespace GameEngine::Editor
