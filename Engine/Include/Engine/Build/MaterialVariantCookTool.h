#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
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

/// The folders that hold the shader packages of a desktop export: the engine shader tree, the package
/// shader roots, the cook's cache and the editor's cache. A reader scans each with its subfolders, so
/// a folder inside another one is left out.
std::vector<std::filesystem::path> DesktopShaderPackageRoots(const DesktopMaterialCookInputs& inputs);

/// The cook inputs of a desktop export staged at `contentRoot`, for the project at `projectRoot` and its
/// enabled packages' asset roots. The cook reads the project assets and the engine shaders the export
/// staged, so its program index names only the shader sources the game ships. The caller sets `ReuseCache`.
DesktopMaterialCookInputs ExportMaterialCookInputs(const std::filesystem::path& contentRoot,
                                                   const std::filesystem::path& projectRoot,
                                                   const std::vector<MaterialCookScanRoot>& packageRoots);

/// One failure of a cook, as its "FAIL" row names it: a variant that did not cook, or a material that
/// did not load or prepare. For a material, `Material` is its path, `Variant` is empty and `Reason` is its error.
struct MaterialCookFailure
{
    std::string Material;
    std::string Variant;
    std::string Reason;
};

/// What one cook into a cache produced, in the cook's row order. The file `cook-record.txt` in a cook's
/// cache holds the record of the last cook into that cache. The cook writes it after its last row, also
/// when rows failed. A record with no rows is a cook of no material; no file is a cook that stopped
/// before its rows. Its first line is `material-cook-record`, then one tab-separated row per line:
/// `package <path>` or `failed <material> <variant> <reason>`, UTF-8.
struct MaterialCookRecord
{
    /// The shader packages to ship, each once.
    std::vector<std::filesystem::path> Packages;
    std::vector<MaterialCookFailure> Failures;
};

/// Writes `record` (`cook-record.txt`) into `cookCache`, each package once, through a
/// temporary sibling, so a reader sees the whole record or none. Returns false with `error` on failure.
bool WriteMaterialCookRecord(const std::filesystem::path& cookCache, const MaterialCookRecord& record,
                             std::string& error);

/// The record of the last cook into `cookCache`. Empty when there is no record or the file is not one.
std::optional<MaterialCookRecord> ReadMaterialCookRecord(const std::filesystem::path& cookCache);

/// Deletes the cook record and the program index (ShaderProgramIndex) from `cookCache`, so a cook
/// that stops before its rows leaves neither from an earlier cook.
void RemoveMaterialCookOutputs(const std::filesystem::path& cookCache);

/// What an export takes from its cook into a cache: the record when the cook wrote one, the warning
/// of a cook that failed, and the error that stops the export.
struct MaterialCookOutcome
{
    std::optional<MaterialCookRecord> Record;
    std::string Warning;
    std::string Error;
};

/// The outcome of a cook into `cookCache` that exited with `exitCode` and printed `outputTail` last.
/// The warning names each failed material of the record once: `<path>: <error>` for a material that the
/// cook could not load or prepare, `<material>: <variant>, <variant>` else. A cook that exits 0 and writes no record gives an error.
MaterialCookOutcome ReadMaterialCookOutcome(const std::filesystem::path& cookCache, int exitCode,
                                            std::string_view outputTail);

/// Copies each of `packages`, which lie under `cookCache`, to the same path under `bundleCache`
/// (its content-key directory included), the read-only cache a packaged game probes
/// (ApplicationPaths::GetBundledShaderCacheRoot), and the cook's program index (ShaderProgramIndex) beside
/// them when it wrote one. Returns false with `error` naming the package that is not on disk, the
/// file that could not be copied, or a package outside `cookCache`.
bool StageCookedShaderPackages(const std::vector<std::filesystem::path>& packages,
                               const std::filesystem::path& cookCache, const std::filesystem::path& bundleCache,
                               std::string& error);

} // namespace GameEngine
