#pragma once

// The parts of the script package restore (Scripting/ScriptPackageRestore.h) that only the script
// manager's file watch and the tests use: which files change a restore's result, and how the
// restore's outputs are read.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

/// True when `file` has the name of a file whose text changes the restore of `projectPath`: the
/// csproj's own name, Directory.Build.props, Directory.Build.targets, Directory.Packages.props or
/// nuget.config (compared without case, as NuGet and MSBuild find them, and as the file watcher
/// reports them on Windows).
bool IsScriptRestoreInput(const std::filesystem::path& file, const std::filesystem::path& projectPath);

/// The files whose text changes the restore of `projectPath`, the csproj first: the engine props
/// and targets beside it, then every Directory.Build.props, Directory.Build.targets,
/// Directory.Packages.props and nuget.config in the csproj's folder and in each folder above it,
/// nearest first. Only files that exist.
std::vector<std::filesystem::path> ScriptRestoreInputFiles(const std::filesystem::path& projectPath);

/// The runtime assemblies a project.assets.json resolves for `targetFramework`, as absolute
/// paths in the package folder, sorted by file name, and one line in `outWarnings` per package
/// whose native or platform-specific assets are not staged. False, with `error`, when the text
/// is not a restore result or holds no target for that framework.
bool ReadRestoredRuntimeAssemblies(std::string_view assetsJson, std::string_view targetFramework,
                                   std::vector<std::filesystem::path>& outAssemblies,
                                   std::vector<std::string>& outWarnings, std::string& error);

/// The csproj's own Reference and ProjectReference items, from the output of
/// `dotnet msbuild -getItem:Reference -getItem:ProjectReference`. A Reference with a HintPath
/// adds its absolute path to `outReferences`; one the engine props declare, or one without a
/// HintPath (a framework assembly the compile already has), is skipped. A ProjectReference adds
/// one line to `outWarnings`. False, with `error`, when the output is not that JSON or a
/// HintPath names no file.
bool ReadEvaluatedReferences(std::string_view evaluationJson, std::vector<std::filesystem::path>& outReferences,
                             std::vector<std::string>& outWarnings, std::string& error);

} // namespace GameEngine
