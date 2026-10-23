#pragma once

// The NuGet packages and assembly references of a project's scripts: restored with `dotnet restore`
// on the user's GameEngine.Scripts.csproj and read from the project's MSBuild evaluation, so the
// editor resolves exactly what the IDE and `dotnet build` resolve (conditions, central package
// versions, Directory.Build.props, nuget.config feeds, transitive packages). The runtime
// assemblies the restore picks for the scripts' target framework, and the files the csproj's
// Reference items name, are copied into Dependencies/ beside the scripts assembly: the compile
// references them there, the scripts' dependency context loads them from there, and the export
// ships them from there.

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

struct ScriptPackageRestoreResult
{
    bool Success = false;
    std::string Error; // the failing packages or references and the fix, when Success is false
    bool Stopped = false; // a newer compile stopped the restore; the time limit is an error, not a stop
    std::vector<std::string> Warnings; // what the restore found and does not stage; only when it ran
    std::vector<std::filesystem::path> StagedAssemblies; // in the Dependencies folder, by name
};

/// Restores the packages of `projectPath` and stages their runtime assemblies and the csproj's
/// Reference files into `dependenciesDir`, deleting staged assemblies the set no longer holds.
/// The restore runs only when a file that changes its result changed since the last successful
/// one (the csproj, the engine props and targets beside it, and every Directory.Build.props,
/// Directory.Build.targets, Directory.Packages.props and nuget.config from the csproj's folder up
/// to the root), or when its result or an assembly it recorded is missing. A project that
/// declares no reference items and has no Directory.* MSBuild file runs nothing and empties
/// `dependenciesDir`. A set `cancelRequested` stops a running restore.
ScriptPackageRestoreResult RestoreScriptPackages(const std::filesystem::path& projectPath,
                                                 const std::filesystem::path& dependenciesDir,
                                                 const std::string& engineBinDir,
                                                 const std::atomic<bool>* cancelRequested);

/// Where the scripts project keeps its package versions.
struct CentralPackageVersions
{
    bool Enabled = false;       // a restore input sets ManagePackageVersionsCentrally to true
    std::filesystem::path File; // the nearest Directory.Packages.props; empty when there is none
};

/// Reads the restore inputs of `projectPath` (the files RestoreScriptPackages keys on) for central
/// package versions.
CentralPackageVersions FindCentralPackageVersions(const std::filesystem::path& projectPath);

} // namespace GameEngine
