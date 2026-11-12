#pragma once

#include "PageStreaming/Height/HeightPageLayer.h"

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <optional>
#include <vector>

namespace GameEngine::TerrainECS
{

/// The samples of a planet's height page layer that a cap on the planet reaches, at the level whose
/// grid holds the cap in at most HeightPageLayer::kWritePagesAcross pages a side
/// (HeightPageLayer::WriteLevel): one rect a face. A cap at a face side holds the edge samples of
/// both faces that store them. The brush dab and the modifier bake write a planet layer through it.
class PlanetLayerCap
{
public:
    /// The cap of `angularRadius` radians around `center` (any length but zero) on `layer`, a layer of
    /// the six cube faces. A zero center or radius reaches nothing.
    PlanetLayerCap(const PageStreaming::HeightPageLayer& layer, const Mathematics::Vector3& center,
                   float32 angularRadius);

    /// The level the cap's samples are on.
    uint32 Level() const { return m_Level; }
    /// The samples of level Level() of face `face` the cap reaches, or none.
    const std::optional<PageStreaming::PageSampleRect>& Rect(uint8 face) const { return m_Rects[face]; }
    /// The unit direction from the planet's center of each sample of Rect(face), row-major.
    void Directions(uint8 face, std::vector<Mathematics::Vector3>& out) const;

private:
    PageStreaming::PageFaceRects m_Rects;
    uint32 m_Level = 0;
    uint32 m_FaceSamples = 0;
};

} // namespace GameEngine::TerrainECS
