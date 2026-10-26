#pragma once

#include "Mathematics/Vector3.h"

#include <algorithm>
#include <iterator>

namespace GameEngine::Engine::Renderer
{
/// Persistent atlas contents only need refreshing outside the solve window when
/// their interpretation changes. Commit this snapshot after declaring uploads.
struct DDGIAtlasUploadState
{
    bool Valid = false;
    Mathematics::Vector3 GridMin;
    Mathematics::Vector3 GridSize;
    float FilterStrength = 0.0f;
    float FilterSmoothness = 0.0f;

    bool NeedsFullUpload(const Mathematics::Vector3& gridMin, const Mathematics::Vector3& gridSize,
                         float strength, float smoothness) const
    {
        return !Valid || !std::equal(std::begin(GridMin.v), std::end(GridMin.v), gridMin.v) ||
               !std::equal(std::begin(GridSize.v), std::end(GridSize.v), gridSize.v) ||
               FilterStrength != strength || FilterSmoothness != smoothness;
    }

    void Commit(const Mathematics::Vector3& gridMin, const Mathematics::Vector3& gridSize, float strength,
                float smoothness)
    {
        GridMin = gridMin;
        GridSize = gridSize;
        FilterStrength = strength;
        FilterSmoothness = smoothness;
        Valid = true;
    }
};
}  // namespace GameEngine::Engine::Renderer
