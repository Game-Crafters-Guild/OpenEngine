#include "Planet/PlanetLayerCap.h"

#include "PageStreaming/PageStore/PageStoreFormat.h"

#include "CBTTerrain/CBTPlanetShading.h"
#include "CBTTerrain/CBTSphereFaceMap.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace GameEngine::TerrainECS
{

namespace
{

// A face UV this far inside a classified rect lies on the rect's own face, never on a neighbor.
constexpr float32 kCornerInset = 1e-6f;
// The classifier bounds the cap with a ring of 24 directions; a rect grows by this share of its extent
// (more than the ring's 0.9 % sagitta) and two samples, so it holds every sample the cap reaches.
constexpr uint32 kMarginShare = 64;
constexpr uint32 kMarginSamples = 2;

Mathematics::Vector3 FaceUVDirection(uint32 face, float32 u, float32 v)
{
    Mathematics::Vector3 direction;
    CBTTerrain::FaceUVToWorldDir(face, u, v, direction.x, direction.y, direction.z);
    return direction;
}

uint32 LowSample(float64 value, float64 margin)
{
    return static_cast<uint32>(std::max(std::floor(value - margin), 0.0));
}

uint32 HighSample(float64 value, float64 margin, float64 last)
{
    return static_cast<uint32>(std::min(std::ceil(value + margin), last));
}

// The level-0 samples of the layer's face that hold classified rect `rect`, grown by the margin.
std::optional<std::pair<uint8, PageStreaming::PageSampleRect>> LayerRectOf(const CBTTerrain::SphereFaceUVRect& rect,
                                                                           uint32 faceSamples)
{
    const uint8 face = PageStreaming::EqualAngleFacePointOf(
                           FaceUVDirection(rect.Face, 0.5f * (rect.MinU + rect.MaxU), 0.5f * (rect.MinV + rect.MaxV)),
                           faceSamples)
                           .Face;
    float64 minX = static_cast<float64>(faceSamples), minZ = minX, maxX = 0.0, maxZ = 0.0;
    const float32 u0 = std::min(rect.MinU + kCornerInset, rect.MaxU);
    const float32 u1 = std::max(rect.MaxU - kCornerInset, rect.MinU);
    const float32 v0 = std::min(rect.MinV + kCornerInset, rect.MaxV);
    const float32 v1 = std::max(rect.MaxV - kCornerInset, rect.MinV);
    for (const auto& [u, v] : {std::pair{u0, v0}, std::pair{u1, v0}, std::pair{u0, v1}, std::pair{u1, v1}})
    {
        const PageStreaming::PageFacePoint point =
            PageStreaming::EqualAngleFacePointOf(FaceUVDirection(rect.Face, u, v), faceSamples);
        if (point.Face != face)
            continue;
        minX = std::min(minX, point.X);
        maxX = std::max(maxX, point.X);
        minZ = std::min(minZ, point.Z);
        maxZ = std::max(maxZ, point.Z);
    }
    if (minX > maxX || minZ > maxZ)
        return std::nullopt;
    const float64 margin =
        std::max(maxX - minX, maxZ - minZ) / static_cast<float64>(kMarginShare) + static_cast<float64>(kMarginSamples);
    const float64 last = static_cast<float64>(faceSamples - 1u);
    return std::pair{face, PageStreaming::PageSampleRect{LowSample(minX, margin), LowSample(minZ, margin),
                                                         HighSample(maxX, margin, last), HighSample(maxZ, margin, last)}};
}

} // namespace

PlanetLayerCap::PlanetLayerCap(const PageStreaming::HeightPageLayer& layer, const Mathematics::Vector3& center,
                               float32 angularRadius)
{
    const float32 length = center.Length();
    if (layer.FaceCount() != PageStreaming::kPageStoreMaxFaces || layer.Levels().empty() || !(length > 0.0f) ||
        !(angularRadius > 0.0f))
        return;
    m_FaceSamples = layer.Levels()[0].SamplesX;
    if (m_FaceSamples < 2u)
        return;
    const Mathematics::Vector3 direction = center * (1.0f / length);
    const CBTTerrain::SphereEditRegions regions = CBTTerrain::ClassifySphereCapEdit(
        direction.x, direction.y, direction.z, std::min(angularRadius, 0.5f * std::numbers::pi_v<float32>));
    PageStreaming::PageFaceRects level0;
    std::vector<PageStreaming::PageFaceRect> bands;
    for (uint32 index = 0; index < regions.Count; ++index)
    {
        const auto layerRect = LayerRectOf(regions.Rects[index], m_FaceSamples);
        if (!layerRect)
            continue;
        PageStreaming::IncludePageFaceRect(level0, layerRect->first, layerRect->second);
        // An edge sample is stored on both faces: a rect at a side holds the neighbor's level-0 edge too.
        // The coarser levels' bands are 2^L samples long; they go to the refresh and never set the level.
        layer.WriteRects(layerRect->first, layerRect->second, bands);
        for (std::size_t band = 1; band < bands.size(); ++band)
            PageStreaming::IncludePageFaceRect(level0, bands[band].Face, bands[band].Level0Rect);
    }
    uint32 extent = 0;
    for (const std::optional<PageStreaming::PageSampleRect>& rect : level0)
        if (rect)
            extent = std::max({extent, rect->Width(), rect->Height()});
    m_Level = layer.WriteLevel(extent);
    const uint32 levelSamples = layer.Levels()[m_Level].SamplesX;
    const uint32 round = (1u << m_Level) - 1u;
    for (uint8 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
        if (const std::optional<PageStreaming::PageSampleRect>& rect = level0[face])
            m_Rects[face] = PageStreaming::PageSampleRect{rect->MinX >> m_Level, rect->MinZ >> m_Level,
                                                          std::min((rect->MaxX + round) >> m_Level, levelSamples - 1u),
                                                          std::min((rect->MaxZ + round) >> m_Level, levelSamples - 1u)};
}

void PlanetLayerCap::Directions(uint8 face, std::vector<Mathematics::Vector3>& out) const
{
    out.clear();
    const std::optional<PageStreaming::PageSampleRect>& rect = m_Rects[face];
    if (!rect)
        return;
    // Level-L sample i sits on level-0 sample i * 2^L (the coarsest level's last sample may pass the side).
    // Each axis's gnomonic coordinate is found once.
    const auto coordinate = [this](uint32 index) {
        return PageStreaming::GnomonicFaceCoordinate(
            PageStreaming::EqualAngleSampleCoordinate(std::min(index << m_Level, m_FaceSamples - 1u), m_FaceSamples));
    };
    std::vector<float64> alongX(rect->Width());
    std::vector<float64> alongZ(rect->Height());
    for (uint32 x = rect->MinX; x <= rect->MaxX; ++x)
        alongX[x - rect->MinX] = coordinate(x);
    for (uint32 z = rect->MinZ; z <= rect->MaxZ; ++z)
        alongZ[z - rect->MinZ] = coordinate(z);
    out.reserve(alongX.size() * alongZ.size());
    for (const float64 z : alongZ)
        for (const float64 x : alongX)
            out.push_back(PageStreaming::GnomonicFaceDirection(face, x, z));
}

} // namespace GameEngine::TerrainECS
