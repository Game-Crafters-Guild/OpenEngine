#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

/// MaterialVariantCook beside this executable, or empty when it is missing. The Editor build
/// stages it into `<bin>/Tools` (ge_set_output_tools), a sibling of the Apps/ tree the Editor is
/// staged into.
std::filesystem::path FindMaterialVariantCook();

/// A root of material assets the cook scans beside the project, and the alias the runtime mounts
/// it under (a package's alias).
struct MaterialCookScanRoot
{
    std::filesystem::path Path;
    std::string Alias;
};

/// What a desktop export's material cook reads and where it writes.
struct DesktopMaterialCookInputs
{
    /// The project's asset root, mounted as "project".
    std::filesystem::path ProjectAssets;
    /// The engine shader tree the export ships (the adapters and includes the runtime composes
    /// against, byte for byte).
    std::filesystem::path EngineShaders;
    /// Shader roots of the engine modules and enabled packages, as the runtime lists them.
    std::vector<std::filesystem::path> PackageShaderDirs;
    /// Enabled packages' asset roots. The cook compiles every material under them and under
    /// `ProjectAssets`, and they name the aliases of their materials' graph surfaces.
    std::vector<MaterialCookScanRoot> ScanRoots;
    /// The shader cache the editor compiles into; a program it holds is copied into `Cache` instead
    /// of compiled. Empty when the editor has none.
    std::filesystem::path ReuseCache;
    /// The cook's own cache. It persists between exports, so an unchanged material is not
    /// compiled again; the export ships only the packages the cook names.
    std::filesystem::path Cache;
};

/// The command line of a desktop cook of `inputs`, every cooked variant verified as a runtime
/// without a compiler resolves it.
std::vector<std::string> DesktopMaterialCookArguments(const DesktopMaterialCookInputs& inputs);

/// The shader packages a cook's output names, each once, in output order: one per
/// `  ok   <material> [<variant>] -> <package>` line.
std::vector<std::filesystem::path> CookedShaderPackages(std::string_view cookOutput);

/// The materials a cook's output reports as failed, in output order, each once with the variants
/// that failed: `<material>: <variant>, <variant>` from its `  FAIL <material> [<variant>]` lines.
std::vector<std::string> FailedCookMaterials(std::string_view cookOutput);

/// Copies each of `packages`, which lie under `cookCache`, to the same path under `bundleCache`
/// (its content-key directory included), the read-only cache a packaged game probes
/// (ApplicationPaths::GetBundledShaderCacheRoot), and the cook's program index (ShaderProgramIndex) beside
/// them when it wrote one. Returns false with `error` naming the file that could not be copied, or a
/// package outside `cookCache`.
bool StageCookedShaderPackages(const std::vector<std::filesystem::path>& packages,
                               const std::filesystem::path& cookCache, const std::filesystem::path& bundleCache,
                               std::string& error);

} // namespace GameEngine
