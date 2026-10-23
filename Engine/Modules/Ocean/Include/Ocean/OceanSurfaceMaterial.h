#pragma once

#include "AssetCore/GUID.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace GameEngine::Ocean
{

/// The built-in ocean surface material, in one place for its two consumers:
/// OceanForwardContributor registers it at runtime, and MaterialVariantCook
/// cooks its variants offline so a compiler-less runtime (web) can still draw
/// the surface. Both must agree byte-for-byte on the document and keywords —
/// the shader cache is keyed on the composed source and the variant key.

/// The keywords of its pass that the surface compiles in; the other pass keywords leave its program
/// unchanged. HeightFog: the surface takes the view's height fog by its own distance.
/// SSSRNormalRoughness: the pass keeps writing the screen-space reflection slices, so the
/// reflections replace the water's own reflection.
inline constexpr Rendering::MaterialKeyword kOceanSurfacePassKeywords =
    Rendering::MaterialKeyword::HeightFog | Rendering::MaterialKeyword::SSSRNormalRoughness;

/// The number of subsets of kOceanSurfacePassKeywords: the surface registers once per subset.
inline constexpr std::size_t kOceanSurfacePassKeywordSetCount =
    std::size_t{1} << std::popcount(static_cast<std::uint64_t>(kOceanSurfacePassKeywords));

/// The subset of kOceanSurfacePassKeywords at `index` (below kOceanSurfacePassKeywordSetCount). Bit i
/// of `index` stands for the i-th lowest keyword of kOceanSurfacePassKeywords.
Rendering::MaterialKeyword OceanSurfacePassKeywordSet(std::size_t index);

/// Stable derived GUID for the surface registered with `passKeywords & kOceanSurfacePassKeywords`
/// (not a real asset GUID).
const GUID& OceanSurfaceMaterialGuid(Rendering::MaterialKeyword passKeywords);

/// The material document the surface registers: StandardPBR lighting with the
/// ocean surface shader and clipmap vertex modifier.
MaterialDocument BuildOceanSurfaceMaterialDocument();

/// The pass keywords the surface registers with. IBL is included only when the
/// process is NOT on the compat shader profile — the compat fragment stage has
/// no sampler budget for the IBL trio (same budget that gates world materials).
Rendering::MaterialKeyword OceanSurfaceMaterialKeywords(bool compatProfile);

} // namespace GameEngine::Ocean
