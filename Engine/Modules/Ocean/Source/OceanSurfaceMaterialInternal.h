#pragma once

#include "Ocean/OceanSurfaceMaterial.h"

#include <cstddef>

namespace GameEngine::Ocean
{

/// The index of `passKeywords & kOceanSurfacePassKeywords` among the subsets, below
/// kOceanSurfacePassKeywordSetCount.
std::size_t OceanSurfacePassKeywordSetIndex(Rendering::MaterialKeyword passKeywords);

} // namespace GameEngine::Ocean
