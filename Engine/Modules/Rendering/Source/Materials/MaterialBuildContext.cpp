#include "Rendering/Materials/MaterialBuildContext.h"

#include <string_view>

namespace GameEngine::Rendering
{
std::filesystem::path MaterialSourceIdentity(const std::filesystem::path& materialPath,
                                             const MaterialBuildContext& context)
{
    namespace fs = std::filesystem;
    const fs::path material = fs::absolute(materialPath).lexically_normal();
    fs::path identity;
    size_t matchedRootLength = 0;
    for (const MaterialBuildContext::AssetSourceRoot& source : context.AssetSourceRoots)
    {
        const fs::path root = fs::absolute(source.Root).lexically_normal();
        const fs::path relative = material.lexically_relative(root);
        const size_t rootLength = root.native().size();
        if (relative.empty() || *relative.begin() == ".." || rootLength <= matchedRootLength)
            continue;
        identity = fs::path(source.Alias) / relative;
        matchedRootLength = rootLength;
    }
    return identity;
}

namespace
{
constexpr std::string_view kEngineShaderLabel = "engine";
}

std::string ShaderSourceIdentity(const std::filesystem::path& file, const MaterialBuildContext& context)
{
    namespace fs = std::filesystem;
    if (!context.AdapterShaderDir.empty())
    {
        const fs::path relative = fs::absolute(file).lexically_normal().lexically_relative(
            fs::absolute(context.AdapterShaderDir).lexically_normal());
        if (!relative.empty() && *relative.begin() != "..")
            return std::string(kEngineShaderLabel) + "/" + relative.generic_string();
    }
    return MaterialSourceIdentity(file, context).generic_string();
}

std::filesystem::path ResolveShaderSourceIdentity(const std::string& identity, const MaterialBuildContext& context)
{
    const size_t slash = identity.find('/');
    if (slash == std::string::npos)
        return {};
    const std::string_view label = std::string_view(identity).substr(0, slash);
    const std::filesystem::path relative = identity.substr(slash + 1);
    if (label == kEngineShaderLabel)
        return context.AdapterShaderDir.empty() ? std::filesystem::path{} : context.AdapterShaderDir / relative;
    for (const MaterialBuildContext::AssetSourceRoot& source : context.AssetSourceRoots)
        if (source.Alias == label)
            return source.Root / relative;
    return {};
}
} // namespace GameEngine::Rendering
