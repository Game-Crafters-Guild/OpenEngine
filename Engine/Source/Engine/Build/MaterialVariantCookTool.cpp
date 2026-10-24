#include "Engine/Build/MaterialVariantCookTool.h"

#include "Core/ApplicationPaths.h"
#include "Rendering/ShaderCache/ShaderProgramIndex.h"

#include <algorithm>
#include <system_error>

namespace GameEngine
{

std::filesystem::path FindMaterialVariantCook()
{
#if defined(_WIN32)
    const std::filesystem::path relative = std::filesystem::path("Tools") / "MaterialVariantCook.exe";
#else
    const std::filesystem::path relative = std::filesystem::path("Tools") / "MaterialVariantCook";
#endif
    return ApplicationPaths::FindFileUpward(ApplicationPaths::GetExecutableDirectory(), relative);
}

std::vector<std::string> DesktopMaterialCookArguments(const DesktopMaterialCookInputs& inputs)
{
    std::vector<std::string> arguments{"--project", inputs.ProjectAssets.string(),
                                       "--engine-shaders", inputs.EngineShaders.string(),
                                       "--cache", inputs.Cache.string(),
                                       "--verify"};
    for (const std::filesystem::path& directory : inputs.PackageShaderDirs)
    {
        arguments.emplace_back("--package-shaders");
        arguments.push_back(directory.string());
    }
    for (const MaterialCookScanRoot& scan : inputs.ScanRoots)
    {
        arguments.emplace_back("--scan");
        arguments.push_back(scan.Path.string());
        arguments.emplace_back("--scan-alias");
        arguments.push_back(scan.Alias);
    }
    if (!inputs.ReuseCache.empty())
    {
        arguments.emplace_back("--reuse-cache");
        arguments.push_back(inputs.ReuseCache.string());
    }
    return arguments;
}

std::vector<std::filesystem::path> CookedShaderPackages(std::string_view cookOutput)
{
    constexpr std::string_view kCookedLine = "  ok   ";
    constexpr std::string_view kArrow = "] -> ";
    std::vector<std::filesystem::path> packages;
    std::size_t lineStart = 0;
    while (lineStart < cookOutput.size())
    {
        std::size_t lineEnd = cookOutput.find_first_of("\r\n", lineStart);
        if (lineEnd == std::string_view::npos)
            lineEnd = cookOutput.size();
        const std::string_view line = cookOutput.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        if (!line.starts_with(kCookedLine))
            continue;
        const std::size_t arrow = line.find(kArrow);
        if (arrow == std::string_view::npos)
            continue;
        std::filesystem::path package(line.substr(arrow + kArrow.size()));
        if (std::find(packages.begin(), packages.end(), package) == packages.end())
            packages.push_back(std::move(package));
    }
    return packages;
}

std::vector<std::string> FailedCookMaterials(std::string_view cookOutput)
{
    constexpr std::string_view kFailedLine = "  FAIL ";
    std::vector<std::string> materials;
    std::vector<std::vector<std::string>> variants;
    std::size_t lineStart = 0;
    while (lineStart < cookOutput.size())
    {
        std::size_t lineEnd = cookOutput.find_first_of("\r\n", lineStart);
        if (lineEnd == std::string_view::npos)
            lineEnd = cookOutput.size();
        const std::string_view line = cookOutput.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        if (!line.starts_with(kFailedLine))
            continue;
        const std::string_view rest = line.substr(kFailedLine.size());
        const std::size_t open = rest.rfind(" [");
        const std::size_t close = rest.rfind(']');
        if (open == std::string_view::npos || close == std::string_view::npos || close < open)
            continue;
        const std::string material(rest.substr(0, open));
        const std::string variant(rest.substr(open + 2, close - open - 2));
        const auto found = std::find(materials.begin(), materials.end(), material);
        if (found == materials.end())
        {
            materials.push_back(material);
            variants.push_back({variant});
            continue;
        }
        std::vector<std::string>& list = variants[static_cast<std::size_t>(found - materials.begin())];
        if (std::find(list.begin(), list.end(), variant) == list.end())
            list.push_back(variant);
    }
    for (std::size_t i = 0; i < materials.size(); ++i)
    {
        materials[i] += ": ";
        for (std::size_t v = 0; v < variants[i].size(); ++v)
            materials[i] += (v == 0 ? "" : ", ") + variants[i][v];
    }
    return materials;
}

bool StageCookedShaderPackages(const std::vector<std::filesystem::path>& packages,
                               const std::filesystem::path& cookCache, const std::filesystem::path& bundleCache,
                               std::string& error)
{
    const std::filesystem::path cacheRoot = cookCache.lexically_normal();
    for (const std::filesystem::path& package : packages)
    {
        const std::filesystem::path relative = package.lexically_normal().lexically_relative(cacheRoot);
        if (relative.empty() || *relative.begin() == "..")
        {
            error = "The material cook named the shader package '" + package.string() +
                    "', which is outside its cache '" + cookCache.string() + "'.";
            return false;
        }
        const std::filesystem::path destination = bundleCache / relative;
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (!ec)
            std::filesystem::copy_file(package, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = "Could not copy the cooked shader package '" + package.string() + "' to '" +
                    destination.string() + "': " + ec.message();
            return false;
        }
    }
    // The program index names the packages of the variants this cook reported, so it ships with them.
    const std::filesystem::path index = cookCache / Rendering::kShaderProgramIndexFileName;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(index, ec))
        return true;
    std::filesystem::create_directories(bundleCache, ec);
    if (!ec)
        std::filesystem::copy_file(index, bundleCache / Rendering::kShaderProgramIndexFileName,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        error = "Could not copy the material cook's program index '" + index.string() + "' to '" +
                bundleCache.string() + "': " + ec.message();
        return false;
    }
    return true;
}

} // namespace GameEngine
