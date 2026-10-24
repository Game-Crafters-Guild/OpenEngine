#include "OceanShaderDirectory.h"

#include "Core/ApplicationPaths.h"

#include <system_error>

namespace GameEngine::Ocean
{

std::filesystem::path OceanShaderDirectory(const char* requiredShaderFile)
{
    const std::filesystem::path directory = ApplicationPaths::GetInstallAssetsRoot() / "Shaders" / "Ocean";
    std::error_code ec;
    if (!std::filesystem::exists(directory / requiredShaderFile, ec))
        return {};
    return directory;
}

} // namespace GameEngine::Ocean
