#pragma once

#include "Assets/Packages/PackageCodeModules.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace GameEngine
{

/// The C# a packaged build compiles, decided from the sources, never from whether a
/// generated script project exists: the editor writes that project for every project it
/// opens, native-only ones included.
struct ShippedScriptSources
{
    /// The project's scripts root when it holds a .cs file outside obj/ and bin/; empty
    /// otherwise.
    std::filesystem::path AuthoredRoot;
    /// The runtime C# package modules.
    std::vector<PackageCodeModule> PackageModules;

    bool IsEmpty() const { return AuthoredRoot.empty() && PackageModules.empty(); }
};

/// True when a .cs file exists under `scriptsRoot` outside obj/ and bin/ build litter.
bool HasAuthoredCSharpSources(const std::filesystem::path& scriptsRoot);

/// The authored scripts under `scriptsRoot` and the runtime C# modules among `packageModules`.
ShippedScriptSources CollectShippedScriptSources(const std::filesystem::path& scriptsRoot,
                                                 const std::vector<PackageCodeModule>& packageModules);

/// What an export does with its C#.
enum class ScriptCompileStep : std::uint8_t
{
    Skip,           ///< There is no C# to compile.
    Compile,        ///< Compile the sources.
    MissingProject, ///< The build compiles the generated script project, and it does not exist.
};

/// The step for `sources`. A NativeAOT build (`nativeAot`) compiles the sources into a
/// project of its own and needs no generated project; the managed build compiles the
/// generated project, so it cannot compile without one.
ScriptCompileStep DecideScriptCompile(const ShippedScriptSources& sources, bool generatedProjectExists,
                                      bool nativeAot);

} // namespace GameEngine
