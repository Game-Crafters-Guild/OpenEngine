#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine {

/**
 * @brief The places the running application reads from and writes to: the executable's directory,
 * the staged engine assets and shader cache beside it, and the user's cache, data and documents
 * directories. The lexical path helpers live in Types/PathUtils.h.
 */
class ApplicationPaths {
public:
    /**
     * @brief Get the directory containing the current executable
     * @return Absolute path to the executable directory
     */
    static std::filesystem::path GetExecutableDirectory();

    /**
     * @brief Get the root of the engine assets staged for the executable: beside it, or in
     * Contents/Resources inside a macOS app bundle.
     *
     * This is where the build stages engine-owned runtime content (shaders, fonts,
     * textures), so it is the only correct anchor for loading it. Staged content must never
     * be resolved against the process working directory: a shipped or relocated build has
     * no repo to walk up into, and the working directory is whatever happened to launch the
     * process. The layout rule is InstallAssetsRootFor(), applied to GetExecutableDirectory()
     * once per process and cached: callers may resolve through it on every load.
     *
     * @return Absolute path to the staged engine asset root, empty if the executable
     *         directory cannot be determined
     */
    static std::filesystem::path GetInstallAssetsRoot();

    /**
     * @brief A cooked shader cache the build ships alongside the runtime, or empty
     * where there is none: `.Cache/Shaders` in the content root of a desktop export
     * (InstallContentRootFor: `Contents/Resources/.Cache/Shaders` in a macOS bundle,
     * `<executable directory>/.Cache/Shaders` on Windows and Linux), `/.Cache/Shaders`
     * in the browser's preload. The engine probes it, read-only, before the
     * workspace's own cache.
     */
    static std::filesystem::path GetBundledShaderCacheRoot();

    /**
     * @brief The first regular file at @p relative under @p start or any of its
     * ancestors, or empty. The walk runs to the filesystem root rather than a fixed
     * depth: a staged executable sits at different depths inside a build tree on
     * each platform (nine directories down inside a macOS bundle).
     */
    static std::filesystem::path FindFileUpward(std::filesystem::path start, const std::filesystem::path& relative);

    /**
     * @brief The staged asset root for an executable that lives in @p executableDirectory:
     * `InstallContentRootFor(executableDirectory) / "Assets"`.
     *
     * @return Absolute path to the staged asset root for that executable directory; empty
     *         only when @p executableDirectory is empty
     */
    static std::filesystem::path InstallAssetsRootFor(const std::filesystem::path& executableDirectory);

    /**
     * @brief The directory that holds everything the build stages for an executable that
     * lives in @p executableDirectory, apart from code: `Assets/`, and in a packaged game
     * `game.config`, `Packages/` and `NativeScripts/`.
     *
     * The executable directory itself everywhere, with one exception: inside a macOS app
     * bundle (GetBundleResourcesDirectory() is non-empty) it is the bundle's
     * `Contents/Resources`, because `Contents/MacOS` holds code only. codesign seals a file
     * there as code, and refuses a directory whose name looks like a nested bundle. The rule
     * is structural, as GetBundleResourcesDirectory() is: nothing is probed for existence, so
     * a flat layout (test executables, CLI tools) never moves to a `Resources/` sibling that
     * happens to exist. A trailing separator on @p executableDirectory is ignored.
     *
     * @return The content root for that executable directory; empty only when
     *         @p executableDirectory is empty
     */
    static std::filesystem::path InstallContentRootFor(const std::filesystem::path& executableDirectory);

    /**
     * @brief Resources directory of the macOS app bundle containing an executable directory.
     *
     * A bundled executable lives at <App>.app/Contents/MacOS; the bundle's resources at
     * <App>.app/Contents/Resources. Structural only — nothing is probed for existence. A
     * trailing separator on @p executableDirectory is ignored.
     *
     * @return The bundle's Contents/Resources directory; empty when @p executableDirectory is
     *         not a bundle's Contents/MacOS, and always empty on platforms other than macOS,
     *         whose layouts are executable-relative
     */
    static std::filesystem::path GetBundleResourcesDirectory(const std::filesystem::path& executableDirectory);

    /**
     * @brief Get the scripts assembly output directory relative to executable
     * @return Path to where script assemblies should be built and loaded from
     */
    static std::filesystem::path GetScriptsAssemblyDirectory();

    /**
     * @brief Ensure working directory matches executable directory
     * This fixes path resolution issues when the executable is launched from different directories
     */
    static void EnsureWorkingDirectoryMatchesExecutable();

    /**
     * @brief Ensure working directory matches the provided directory.
     *
     * Callers should typically pass an absolute path. When the directory does not exist,
     * changing the working directory will fail; callers may create it first if appropriate.
     */
    static void EnsureWorkingDirectoryMatches(const std::filesystem::path& directory);

    /**
     * @brief Get a user-writable cache directory suitable for derived data, caches, thumbnails, etc.
     *
     * Windows: %LOCALAPPDATA% (fallback: %APPDATA%)
     * macOS:   ~/Library/Caches
     * Linux:   $XDG_CACHE_HOME (fallback: ~/.cache)
     */
    static std::filesystem::path GetUserCacheDirectory();

    /**
     * @brief Get a user-writable data directory suitable for save data and user settings.
     *
     * Windows: %APPDATA% (fallback: %LOCALAPPDATA%)
     * macOS:   ~/Library/Application Support
     * Linux:   $XDG_DATA_HOME (fallback: ~/.local/share)
     */
    static std::filesystem::path GetUserDataDirectory();

    /**
     * @brief Get a user-writable Documents directory suitable for user-facing save data.
     *
     * Windows: Known Folder Documents (fallback: %USERPROFILE%\\Documents)
     * macOS:   ~/Documents
     * Linux:   $XDG_DOCUMENTS_DIR via user-dirs.dirs when available (fallback: ~/Documents)
     *
     * NOTE: This is distinct from GetUserDataDirectory(), which is intended for application
     * support data/config rather than user-facing documents.
     */
    static std::filesystem::path GetUserDocumentsDirectory();

    /**
     * @brief Sanitize an identifier for safe use as a single folder name.
     *
     * Replaces characters illegal or problematic on common filesystems with '_',
     * trims trailing spaces/dots (Windows), and collapses empty to "App".
     */
    static std::string SanitizeForFolderName(std::string_view name);
};

} // namespace GameEngine
