#include "Assets/Parsers/ShaderSourceAssetParser.h"

#include "AssetCore/DepEdge.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetDependencyExtractor.h"
#include "Assets/AssetManager.h"
#include "Assets/ShaderSourceAsset.h"
#include "Engine/Rendering/PackageShaderDirs.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialShaderIncludeRoots.h"
#include "Types/PathUtils.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{

struct ShaderIncludeRoots
{
    const AssetManager* Manager = nullptr;
    uint64_t SourceSetVersion = 0;
    std::filesystem::path AssetRoot;
    std::filesystem::path AdapterShaderDir;
    std::vector<std::filesystem::path> Roots;
};

// The include roots of `assets`, derived again only when the manager, its asset root or its mount
// set changes. A dependency walk derives them once. The asset root separates two managers that
// one address held in turn.
const ShaderIncludeRoots& IncludeRootsOf(const AssetManager& assets)
{
    thread_local ShaderIncludeRoots cached;
    const uint64_t version = assets.GetSourceSetVersion();
    std::filesystem::path assetRoot = assets.GetAssetRoot();
    if (cached.Manager == &assets && cached.SourceSetVersion == version && cached.AssetRoot == assetRoot)
        return cached;
    Rendering::MaterialBuildContext context;
    FillMaterialSourceRoots(assets, context);
    cached.Manager = &assets;
    cached.SourceSetVersion = version;
    cached.AssetRoot = std::move(assetRoot);
    cached.AdapterShaderDir = context.AdapterShaderDir;
    cached.Roots = Rendering::BuildMaterialIncludeRoots(context, {});
    return cached;
}

void EmitEdge(DepEdgeSink& sink, const GUID& referrer, DepEdgeKind kind, const char* field, uint32_t& ordinal,
              DepEdge edge)
{
    edge.Referrer = referrer;
    edge.Kind = kind;
    edge.FieldLocator = std::string(field) + "[" + std::to_string(ordinal) + "]";
    edge.Ordinal = ordinal++;
    sink.Emit(std::move(edge));
}

} // namespace

std::vector<std::string> ShaderSourceAssetParser::GetSupportedExtensions() const
{
    return {".glsl", ".vert", ".frag", ".geom", ".comp", ".hlsl"};
}

AssetParseResult ShaderSourceAssetParser::Parse(const AssetMetadata& metadata,
                                                [[maybe_unused]] AssetManager& assetManager)
{
    return AssetParseResult(std::make_shared<ShaderSourceAsset>(metadata.Guid, metadata.Path));
}

bool ShaderSourceAssetParser::ExtractDependencies(const GUID& referrer,
                                                  const AssetMetadata& metadata,
                                                  DepEdgeSink& sink) const
{
    uint32_t ordinal = 0;
    // A shader graph source names its textures in its comment block.
    const AssetDependencyInfo textReferences = AssetDependencyExtractor::ExtractDependencies(metadata);
    for (const GUID& target : textReferences.Dependencies)
    {
        DepEdge edge;
        edge.Target = target;
        EmitEdge(sink, referrer, DepEdgeKind::Other, "reference", ordinal, std::move(edge));
    }
    for (const std::string& path : textReferences.DependencyPaths)
    {
        DepEdge edge;
        edge.TargetPath = path;
        EmitEdge(sink, referrer, DepEdgeKind::Other, "reference", ordinal, std::move(edge));
    }

    // The roots a material shader compiles with. Without a current manager, an include resolves
    // next to the file only.
    const ShaderIncludeRoots none;
    const AssetManager* assets = AssetManager::GetThreadCurrent();
    const ShaderIncludeRoots& roots = assets ? IncludeRootsOf(*assets) : none;
    for (const std::filesystem::path& include : Rendering::ResolveShaderIncludes(metadata.Path, roots.Roots))
    {
        if (!RelativePathUnderRoot(include, roots.AdapterShaderDir).empty())
            continue;
        DepEdge edge;
        edge.TargetPath = include.generic_string();
        EmitEdge(sink, referrer, DepEdgeKind::ShaderInclude, "include", ordinal, std::move(edge));
    }
    return true;
}

} // namespace GameEngine
