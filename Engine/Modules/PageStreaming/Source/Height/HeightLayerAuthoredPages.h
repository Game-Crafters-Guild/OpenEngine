#pragma once

#include "PageStreaming/Height/HeightPageLayer.h"

#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// The authored pages of a HeightPageLayer, for the height layer store only (SaveHeightLayer and
/// LoadHeightLayer): HeightPageLayer::ExportAuthoredPages and ImportAuthoredPages.
class HeightLayerAuthoredPages
{
public:
    static std::vector<HeightPageLayerPage> Export(const HeightPageLayer& layer) { return layer.ExportAuthoredPages(); }
    static bool Import(HeightPageLayer& layer, std::span<const HeightPageLayerPage> pages)
    {
        return layer.ImportAuthoredPages(pages);
    }
};

} // namespace GameEngine::PageStreaming
