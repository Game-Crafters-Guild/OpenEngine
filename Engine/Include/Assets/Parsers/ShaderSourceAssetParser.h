#pragma once

#include "Assets/ParserRegistry.h"

namespace GameEngine
{

// Shader source files that the shader compiler reads with #include: .glsl, .vert, .frag, .geom,
// .comp and .hlsl. A .shader program descriptor stays with the type registry's factory.
class ShaderSourceAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::Shader; }

    std::vector<std::string> GetSupportedExtensions() const override;

    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager& assetManager) override;

    std::string GetName() const override { return "ShaderSourceAssetParser"; }

    int GetPriority() const override { return 100; }

    // Emits a ShaderInclude edge per #include directive, to the file the shader compiler includes
    // (Rendering::ResolveShaderIncludes), and an Other edge per GUID or asset path in the text.
    // An include in the engine's shader tree gets no edge: the export ships that tree whole.
    bool ExtractDependencies(const GUID& referrer,
                             const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;
};

} // namespace GameEngine
