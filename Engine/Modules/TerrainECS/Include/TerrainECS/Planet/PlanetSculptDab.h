#pragma once

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::PageStreaming
{
class HeightPageLayer;
struct PageFaceRect;
} // namespace GameEngine::PageStreaming

namespace GameEngine::TerrainECS
{

/// One planet sculpt brush dab (design page-streaming-planets 3.6, 3.13): a smoothstep cone of
/// `amplitude` meters over a cap of `angularRadius` radians around a unit direction from the planet's
/// center. The falloff reads the tangent-plane distance over the cap's tangent radius, so the cone
/// resolves in float32 at any brush and planet size.
class PlanetSculptDab
{
public:
    /// A dab centered on `center` (any length but zero). A zero center or radius makes a dab that adds
    /// nothing.
    PlanetSculptDab(const Mathematics::Vector3& center, float32 angularRadius, float32 amplitude);

    /// The height, in meters, the dab adds at unit direction `direction`.
    float32 HeightAt(const Mathematics::Vector3& direction) const;

    /// Adds the dab to `layer`, a planet layer, at the level whose grid holds the cap in at most
    /// HeightPageLayer::kWritePagesAcross pages across (PlanetLayerCap), each sample on the lattice of
    /// kHeightLayerStep so a saved layer keeps it bit for bit. Appends the face rects whose samples
    /// change to `changed`.
    void AddTo(PageStreaming::HeightPageLayer& layer, std::vector<PageStreaming::PageFaceRect>& changed) const;

private:
    Mathematics::Vector3 m_Center;
    Mathematics::Vector3 m_TangentX; // a unit tangent at m_Center
    Mathematics::Vector3 m_TangentZ; // m_Center x m_TangentX
    float32 m_AngularRadius = 0.0f;
    float32 m_SinRadius = 0.0f; // the cap's tangent radius on the unit sphere
    float32 m_Amplitude = 0.0f;
};

} // namespace GameEngine::TerrainECS
