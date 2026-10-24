#pragma once

#include <array>
#include <filesystem>
#include <string_view>

namespace GameEngine
{
namespace ScriptingPaths
{
/// File name of the compiled project-script assembly, in the script assemblies directory and
/// in a packaged build's Managed/ directory.
inline constexpr std::string_view kScriptsAssemblyFileName = "GameEngine.Scripts.dll";

/// The folder beside the scripts assembly that holds the runtime assemblies of the project's
/// NuGet packages. The scripts' dependency context loads it; the export ships it.
inline constexpr std::string_view kScriptDependenciesFolderName = "Dependencies";

/// Engine assemblies native code loads by file name from the engine managed directory:
/// CoreBridge (the CLR entry point, ResolveCoreBridgeDll), HotReload (its runtimeconfig.json
/// boots the CLR, ResolveHotReloadDll) and Scripting.Runtime (the GameSystem runner
/// ManagedSystemBridge drives). A packaged game's managed set is these, their deps.json
/// closure and the engine ABI surfaces (StageManagedRuntimeAssemblies).
inline constexpr std::string_view kCoreBridgeAssemblyFileName = "GameEngine.CoreBridge.dll";
inline constexpr std::string_view kHotReloadAssemblyFileName = "GameEngine.HotReload.dll";
inline constexpr std::string_view kScriptingRuntimeAssemblyFileName = "GameEngine.Scripting.Runtime.dll";
inline constexpr std::array<std::string_view, 3> kHostLoadedEngineAssemblyFileNames = {
    kCoreBridgeAssemblyFileName, kHotReloadAssemblyFileName, kScriptingRuntimeAssemblyFileName};

/// The generated C# bindings of the engine's script API (Managed/Scripting.Bindings).
inline constexpr std::string_view kScriptBindingsAssemblyFileName = "GameEngine.Scripting.Bindings.dll";

/// The assemblies scripts compile against, by file name: the generated bindings and the
/// hand-written engine ABI surfaces, the editor's own (GameEngine.Editor.Scripting.ABI.dll)
/// included. A list, not a pattern: a file the build no longer stages, left in an existing
/// output folder, is never referenced or shipped. Compile-server requests carry it
/// (ScriptSurfaceRequestField), so the server holds no copy.
inline constexpr std::array<std::string_view, 5> kScriptSurfaceAssemblyFileNames = {
    kScriptBindingsAssemblyFileName, "GameEngine.Scripting.ABI.dll", "GameEngine.ECS.ABI.dll",
    "GameEngine.Platform.ABI.dll", "GameEngine.Editor.Scripting.ABI.dll"};

/// True for an assembly of kScriptSurfaceAssemblyFileNames.
inline bool IsScriptSurfaceAssembly(std::string_view fileName)
{
    for (std::string_view name : kScriptSurfaceAssemblyFileNames)
        if (name == fileName)
            return true;
    return false;
}

/**
 * @brief Directory holding the engine's managed assemblies.
 *
 * The script surface (IsScriptSurfaceAssembly), CoreBridge, HotReload, Scripting.Runtime and SourceGenerators/ —
 * the EngineBinDir every generated csproj, compile-server request and dotnet build resolves
 * engine references from. The executable directory, except inside a macOS app bundle, where
 * the assemblies are staged as resources under Contents/Resources/Managed (Contents/MacOS
 * stays code-only).
 */
std::filesystem::path ResolveEngineManagedDirectory();

/**
 * @brief ResolveEngineManagedDirectory() for an explicit executable directory.
 *
 * Same rules; for tests and specialized hosts that do not run from the engine executable.
 */
std::filesystem::path ResolveEngineManagedDirectoryFrom(const std::filesystem::path& exeDir);

/**
 * @brief Resolve GameEngine.CoreBridge.dll from the engine managed directory.
 * @param preferred Returned when the managed directory holds no CoreBridge assembly.
 */
std::filesystem::path ResolveCoreBridgeDll(const std::filesystem::path& preferred = {});

/**
 * @brief Resolve GameEngine.HotReload.dll from the engine managed directory; empty when absent.
 */
std::filesystem::path ResolveHotReloadDll();

/**
 * @brief Resolve the runtimeconfig.json that boots the CLR for scripting.
 *
 * Probes CoreBridge's, then HotReload's runtimeconfig in the engine managed directory, then an
 * optional GameEngine.Scripts.runtimeconfig.json under <exe>/ScriptAssemblies; empty when none exists.
 */
std::filesystem::path ResolveScriptsRuntimeConfig();

/**
 * @brief Directory where script assemblies (e.g. GameEngine.Scripts.dll) are emitted.
 *
 * <exe>/ScriptAssemblies — ApplicationPaths::GetScriptsAssemblyDirectory(), which creates it on demand.
 */
std::filesystem::path ResolveScriptsAssemblyDirectory();

/**
 * @brief Directory holding the native GameEngine library, or the executable directory when
 *        no probed directory holds it.
 *
 * Reads and sets no environment. GE_NATIVE_DIR is set by the test harnesses and by the managed
 * EngineNativeBinding once it has loaded the library.
 */
std::filesystem::path ResolveNativeLibraryDirectory();

/**
 * @brief Full path to the native GameEngine library (GameEngine.Native.dll/.so/.dylib).
 *
 * Prefers the platform binary name under the native library directory and falls back to a
 * small set of common candidates. Empty means "not found"; callers may fall back to
 * ResolveNativeLibraryDirectory().
 */
std::filesystem::path ResolveNativeLibraryPath();

/**
 * @brief ResolveNativeLibraryPath() for explicit executable and working directories; same
 *        probing rules. An empty @p currentDir probes no working directory.
 */
std::filesystem::path ResolveNativeLibraryPathFrom(
    const std::filesystem::path& exeDir,
    const std::filesystem::path& currentDir);

/**
 * @brief ResolveNativeLibraryDirectory() for explicit executable and working directories
 *        (tests and specialized hosts); same probing rules.
 */
std::filesystem::path ResolveNativeLibraryDirectoryFrom(
    const std::filesystem::path& exeDir,
    const std::filesystem::path& currentDir);
} // namespace ScriptingPaths
} // namespace GameEngine
