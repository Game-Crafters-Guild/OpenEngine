#include "TerrainECS/Planet/PlanetSculptDab.h"
#include "Planet/PlanetLayerCap.h"

#include "PageStreaming/Height/HeightLayerStore.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"

#include "CBTTerrain/CBTPlanetShading.h"
#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

namespace GameEngine::TerrainECS
{

PlanetSculptDab::PlanetSculptDab(const Mathematics::Vector3& center, float32 angularRadius, float32 amplitude)
{
    const float32 length = center.Length();
    if (!(length > 0.0f) || !(angularRadius > 0.0f))
        return;
    m_Center = center * (1.0f / length);
    m_TangentX = CBTTerrain::AnyTangent(m_Center);
    m_TangentZ = Mathematics::Vector3::Cross(m_Center, m_TangentX);
    m_AngularRadius = std::min(angularRadius, 0.5f * std::numbers::pi_v<float32>);
    m_SinRadius = std::sin(m_AngularRadius);
    m_Amplitude = amplitude;
}

float32 PlanetSculptDab::HeightAt(const Mathematics::Vector3& direction) const
{
    if (!(m_SinRadius > 0.0f) || Mathematics::Vector3::Dot(direction, m_Center) <= 0.0f)
        return 0.0f; // no cap, or the far hemisphere
    const float32 alongX = Mathematics::Vector3::Dot(direction, m_TangentX);
    const float32 alongZ = Mathematics::Vector3::Dot(direction, m_TangentZ);
    const float32 t = Math::Clamp01(1.0f - std::sqrt(alongX * alongX + alongZ * alongZ) / m_SinRadius);
    return m_Amplitude * Math::SmoothStep(0.0f, 1.0f, t);
}

void PlanetSculptDab::AddTo(PageStreaming::HeightPageLayer& layer, std::vector<PageStreaming::PageFaceRect>& changed) const
{
    if (!(m_SinRadius > 0.0f) || m_Amplitude == 0.0f)
        return;
    const PlanetLayerCap cap(layer, m_Center, m_AngularRadius);
    std::vector<Mathematics::Vector3> directions;
    std::vector<float32> deltas;
    for (uint8 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
    {
        const std::optional<PageStreaming::PageSampleRect>& rect = cap.Rect(face);
        if (!rect)
            continue;
        cap.Directions(face, directions);
        deltas.resize(directions.size());
        for (std::size_t sample = 0; sample < directions.size(); ++sample)
            deltas[sample] = PageStreaming::QuantizeHeight(HeightAt(directions[sample]), PageStreaming::kHeightLayerStep);
        changed.push_back(PageStreaming::PageFaceRect{face, layer.AddAtLevel(face, cap.Level(), *rect, deltas)});
    }
}

} // namespace GameEngine::TerrainECS
