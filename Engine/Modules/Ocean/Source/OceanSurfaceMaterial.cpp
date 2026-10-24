#include "Ocean/OceanSurfaceMaterial.h"
#include "OceanSurfaceMaterialInternal.h"

#include <array>
#include <string>

namespace GameEngine::Ocean
{
namespace
{
std::array<GUID, kOceanSurfacePassKeywordSetCount> DeriveSurfaceGuids()
{
    std::array<GUID, kOceanSurfacePassKeywordSetCount> guids{};
    guids[0] = GUID::Derive(GUID{}, "ocean/default_material");
    for (std::size_t index = 1u; index < guids.size(); ++index)
        guids[index] = GUID::Derive(
            GUID{}, "ocean/default_material/pass_keywords/" +
                        std::to_string(static_cast<std::uint64_t>(OceanSurfacePassKeywordSet(index))));
    return guids;
}
} // namespace

// Bit i of a set's index stands for the i-th lowest keyword of kOceanSurfacePassKeywords.
std::size_t OceanSurfacePassKeywordSetIndex(Rendering::MaterialKeyword passKeywords)
{
    const std::uint64_t keywords = static_cast<std::uint64_t>(passKeywords);
    std::uint64_t mask = static_cast<std::uint64_t>(kOceanSurfacePassKeywords);
    std::size_t index = 0u;
    for (std::size_t bit = 0u; mask != 0u; ++bit, mask &= mask - 1u)
        if ((keywords & mask & ~(mask - 1u)) != 0u)
            index |= std::size_t{1} << bit;
    return index;
}

Rendering::MaterialKeyword OceanSurfacePassKeywordSet(std::size_t index)
{
    std::uint64_t mask = static_cast<std::uint64_t>(kOceanSurfacePassKeywords);
    std::uint64_t keywords = 0u;
    for (std::size_t bit = 0u; mask != 0u; ++bit, mask &= mask - 1u)
        if (((index >> bit) & 1u) != 0u)
            keywords |= mask & ~(mask - 1u);
    return static_cast<Rendering::MaterialKeyword>(keywords);
}

const GUID& OceanSurfaceMaterialGuid(Rendering::MaterialKeyword passKeywords)
{
    static const std::array<GUID, kOceanSurfacePassKeywordSetCount> guids = DeriveSurfaceGuids();
    return guids[OceanSurfacePassKeywordSetIndex(passKeywords)];
}

MaterialDocument BuildOceanSurfaceMaterialDocument()
{
    MaterialDocument doc{};
    doc.materialName = "Ocean/Default";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Ocean/ocean_surface.glsl";
    doc.vertexModifier = "Ocean/ocean_vertex_modifier.glsl";
    doc.alphaMode = MaterialAlphaMode::Opaque;
    return doc;
}

Rendering::MaterialKeyword OceanSurfaceMaterialKeywords(bool compatProfile)
{
    using Rendering::MaterialKeyword;
    // HasVertexMod mirrors MaterialRegistry::Register, which stamps it for any
    // document with a vertexModifier (the build service later adds
    // HasVertexOutputMod from capability detection — the runtime key carries
    // BOTH). The offline cook keys with these exact keywords, so leaving it out
    // cooks a variant the runtime never addresses.
    MaterialKeyword keywords = MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows |
                               MaterialKeyword::Instanced |
                               MaterialKeyword::ProceduralVertexOutput |
                               MaterialKeyword::HasVertexMod;
    if (!compatProfile)
        keywords = keywords | MaterialKeyword::IBL;
    return keywords;
}

} // namespace GameEngine::Ocean
