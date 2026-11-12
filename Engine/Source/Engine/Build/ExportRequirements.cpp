#include "Engine/Build/ExportRequirements.h"

#include <algorithm>
#include <array>
#include <format>

namespace GameEngine
{

namespace
{
// `xcrun -f metal` finds a stub when the Metal Toolchain is not installed; running the tools tells.
constexpr std::array<std::string_view, 2> kMetalToolchainChecks{"xcrun -sdk macosx metal --version",
                                                                "xcrun -sdk macosx metallib --version"};

constexpr ExportRequirement kMetalToolchain{
    .Name = "Metal Toolchain",
    .Platform = "Mac",
    .CheckCommands = kMetalToolchainChecks,
    .InstallCommand = "xcodebuild -downloadComponent MetalToolchain",
    .Fallback = "Without it, a Mac game compiles each shader stage when it first draws with it.",
};
} // namespace

const ExportRequirement& MetalToolchainRequirement()
{
    return kMetalToolchain;
}

std::vector<const ExportRequirement*> ExportRequirementsForPlatform(std::string_view buildPlatformName)
{
    std::vector<const ExportRequirement*> requirements;
#if defined(__APPLE__)
    if (kMetalToolchain.Platform == buildPlatformName)
        requirements.push_back(&kMetalToolchain);
#else
    (void)buildPlatformName;
#endif
    return requirements;
}

bool IsExportRequirementInstalled(const ExportRequirement& requirement, const ExportRequirementCommandRunner& run)
{
    return std::ranges::all_of(requirement.CheckCommands,
                               [&](std::string_view command) { return run(std::string(command)) == 0; });
}

std::string DescribeMissingExportRequirement(const ExportRequirement& requirement)
{
    return std::format("{} is not installed. {} Install it with '{}', then export again.", requirement.Name,
                       requirement.Fallback, requirement.InstallCommand);
}

} // namespace GameEngine
