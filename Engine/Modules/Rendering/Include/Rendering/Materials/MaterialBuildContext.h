#pragma once

// MaterialBuildContext: the pre-resolved filesystem environment for one
// material compile. The Rendering module never talks to the asset system —
// the caller (Engine/Editor/Player) resolves every directory below via
// AssetManager and hands the context in as a VALUE snapshot, so compiles on
// worker threads never touch live mount state.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class ShaderProgramIndex;

struct MaterialBuildContext
{
    // The engine's built-in shader tree (the editor mount's Shaders/ dir).
    // Home of the adapter templates — adapters always come from here and are
    // never package-overridable — and the FINAL fallback root for surface /
    // vertex-modifier references.
    std::filesystem::path AdapterShaderDir;

    // Source ROOT(s) of the open project mount (not Shaders/ subdirs — a
    // project's Shaders/ dir already rides PackageShaderDirs). Consulted for
    // surface / vertex-modifier references right after the material's own
    // directory, so a project-relative reference still resolves against the
    // LIVE project root when the material's asset path could not be derived
    // (cache-driven compiles) instead of silently collapsing to the engine
    // tree. Derived from the asset mount table by CollectProjectRoots.
    std::vector<std::filesystem::path> ProjectRoots;

    // One mounted asset source: its alias ("project", "editor", a package's
    // alias) and its root directory.
    struct AssetSourceRoot
    {
        std::string Alias;
        std::filesystem::path Root;
    };

    // Every mounted asset source: the project, the editor mount and each
    // package. Derived data named after a material (a graph material's
    // generated surface) uses "<alias>/<path under the root>" from the deepest
    // root holding the material, which stays the same when the tree moves.
    // Derived from the asset mount table by CollectAssetSourceRoots; an
    // offline cook lists its --project and --scan roots.
    std::vector<AssetSourceRoot> AssetSourceRoots;

    // Shaders/ directories of mounted packages, highest mount priority first.
    // Shader references ("Surfaces/x.glsl", "VertexModifiers/y.glsl") resolve
    // materialDir -> ProjectRoots -> PackageShaderDirs (in order) ->
    // AdapterShaderDir, so a package can ship shaders under
    // <assetsDir>/Shaders/ without materials knowing where the file physically
    // lives. Derived from the asset mount table (the single authority) by
    // CollectPackageShaderDirs.
    std::vector<std::filesystem::path> PackageShaderDirs;

    // Shader caches probed in order before CacheRoot and never written: a packaged game's cooked
    // cache (ApplicationPaths::GetBundledShaderCacheRoot), or the editor cache an offline cook reuses
    // (--reuse-cache). Empty unless that folder exists.
    std::vector<std::filesystem::path> ReadOnlyCacheRoots;
    // The program index of a read-only root (the cook writes one beside its programs): a build
    // whose inputs it holds loads the program it names without composing the source or computing
    // the cache key. Null when no read-only root carries a valid index (an editor, a tool, a cook).
    std::shared_ptr<const ShaderProgramIndex> ProgramIndex;
    // The writable shader cache: probed last, a miss compiles into it, and derived files (a graph
    // material's generated surface) are written under it.
    std::filesystem::path CacheRoot;
    std::vector<std::filesystem::path> IncludeDirs;
};

// The material at `materialPath` in the asset system's derived-identity spelling,
// "<alias>/<path under the root>", taken from the deepest of `context`'s AssetSourceRoots that
// holds it. Sources sharing one root spelling tie and the one listed first wins; AssetSourceRoots
// lists sources in mount priority order, so a Player development run that mounts the project and
// the editor at one folder names it "project", as the cook does. Moving the tree keeps the
// identity; a project material and a package material with the same relative path get two.
// Empty when no mounted source holds the material (a synthetic stand-in path).
std::filesystem::path MaterialSourceIdentity(const std::filesystem::path& materialPath,
                                             const MaterialBuildContext& context);

// A shader source file the build reads, named the same wherever the trees sit: "engine/<path under
// AdapterShaderDir>" for the engine shader tree (checked first: a packaged game mounts its project
// at the folder that holds that tree), otherwise MaterialSourceIdentity's "<alias>/<path under the
// root>". Empty when the file lies under none of them (a surface generated under the cache root).
std::string ShaderSourceIdentity(const std::filesystem::path& file, const MaterialBuildContext& context);

// The file a ShaderSourceIdentity names under `context`'s roots; empty when no root carries its
// label.
std::filesystem::path ResolveShaderSourceIdentity(const std::string& identity, const MaterialBuildContext& context);

} // namespace Rendering
} // namespace GameEngine
