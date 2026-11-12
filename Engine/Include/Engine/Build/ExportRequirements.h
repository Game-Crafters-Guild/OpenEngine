#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

/// A tool that an export needs on the build machine: how to check for it, how to install it, and
/// what the export does without it.
struct ExportRequirement
{
    /// The name the editor shows.
    std::string_view Name;
    /// The stored name of the build platform whose export needs the tool (BuildPlatformInfo::Name).
    std::string_view Platform;
    /// Shell commands that each exit with 0 when the tool is installed.
    std::span<const std::string_view> CheckCommands;
    /// The shell command that installs the tool.
    std::string_view InstallCommand;
    /// What an export does without the tool.
    std::string_view Fallback;
};

/// The tool a Mac export needs to compile the recorded shader stages into Metal libraries.
const ExportRequirement& MetalToolchainRequirement();

/// The tools an export to @p buildPlatformName needs on this machine. Empty when this machine cannot
/// export to that platform: a Mac export runs only on a Mac.
std::vector<const ExportRequirement*> ExportRequirementsForPlatform(std::string_view buildPlatformName);

/// Runs one shell command and gives its exit code.
using ExportRequirementCommandRunner = std::function<int(const std::string& command)>;

/// True when each check command of @p requirement exits with 0. Stops at the first that does not.
bool IsExportRequirementInstalled(const ExportRequirement& requirement, const ExportRequirementCommandRunner& run);

/// The warning of an export without @p requirement: what the export does instead, and how to install it.
std::string DescribeMissingExportRequirement(const ExportRequirement& requirement);

} // namespace GameEngine
