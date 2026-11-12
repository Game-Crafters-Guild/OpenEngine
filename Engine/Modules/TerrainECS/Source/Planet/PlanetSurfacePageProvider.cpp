#include "TerrainECS/Planet/PlanetSurfacePageProvider.h"

#include "Core/CpuProfiler.h"
#include "Mathematics/CubeFaces.h"
#include "Mathematics/WorldPosition.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageLoader.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "TerrainECS/TerrainSplatComposite.h"
#include "TerrainECS/TerrainSurface/TerrainSurfaceRuleEval.h"
#include "TerrainSurface/TerrainSurfaceRuleRow.h"

#include <algorithm>
#include <cassert>
#include <span>
#include <tuple>
#include <utility>

namespace GameEngine::TerrainECS
{
namespace
{

constexpr uint32 kWindowMargin = PageStreaming::kPageSurfaceWindowMarginSamples;
constexpr uint32 kWindowStride = PageStreaming::kPageOwnedSamples + 2u * kWindowMargin;
constexpr uint32 kSplatTexelBytes = 4u;
constexpr uint32 kNormalTexelBytes = 2u;

Mathematics::WorldPosition SurfacePoint(const PlanetSurfaceWindow& window, std::size_t index, float64 referenceRadius)
{
    const Mathematics::Vector3& direction = window.Directions[index];
    const float64 radius = referenceRadius + static_cast<float64>(window.Heights[index]);
    return Mathematics::WorldPosition{direction.x * radius, direction.y * radius, direction.z * radius};
}

// The surface's central-difference tangent at window sample `center` along `step` (1: a row, Stride: a
// column): (R + h) (d+ - d-) + (h+ - h-) d, the fragment's gradient terms. Its exact
// negation when the two sides swap.
Mathematics::Vector3 CentralTangent(const PlanetSurfaceWindow& window, std::size_t center, std::size_t step,
                                    float64 referenceRadius)
{
    const float32 radius = static_cast<float32>(referenceRadius + static_cast<float64>(window.Heights[center]));
    const float32 rise = window.Heights[center + step] - window.Heights[center - step];
    return (window.Directions[center + step] - window.Directions[center - step]) * radius +
           window.Directions[center] * rise;
}

// `tangent` or its exact negation, whichever has its first nonzero component positive.
Mathematics::Vector3 CanonicalSign(const Mathematics::Vector3& tangent)
{
    const float32 lead = tangent.x != 0.0f ? tangent.x : (tangent.y != 0.0f ? tangent.y : tangent.z);
    return lead < 0.0f ? tangent * -1.0f : tangent;
}

// The rule inputs and the weights of one splat row, kPageStrideSamples texels.
struct SplatRowScratch
{
    std::vector<float32> SlopeNormalized;
    std::vector<float32> SlopeDegrees;
    std::vector<float32> HeightMeters;
    std::vector<float32> LatitudeDegrees;
    // Empty when no row reads the noise.
    std::vector<Mathematics::WorldPosition> Positions;
    // One plane of weights a rule, in rule order.
    std::vector<float32> Weights;
};

// The buffers a fill reuses on its thread. Fills run at once on many workers, so each thread has its own.
struct FillScratch
{
    PlanetSurfaceWindow Window;
    std::vector<Mathematics::Vector3> WorldNormals;
    SplatRowScratch SplatRow;
};

FillScratch& ThreadFillScratch()
{
    thread_local FillScratch scratch;
    return scratch;
}

bool LexicographicLess(const Mathematics::Vector3& a, const Mathematics::Vector3& b)
{
    return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
}

// The unit vector of world axis `axis` times `sign`.
Mathematics::Vector3 AxisVector(uint8 axis, int8 sign)
{
    const float32 value = static_cast<float32>(sign);
    return Mathematics::Vector3(axis == 0 ? value : 0.0f, axis == 1 ? value : 0.0f, axis == 2 ? value : 0.0f);
}

} // namespace

Mathematics::Vector3 PlanetWindowNormal(const PlanetSurfaceWindow& window, uint32 column, uint32 row,
                                        float64 referenceRadius)
{
    assert(column >= 1u && row >= 1u && column + 1u < window.Stride && row + 1u < window.Stride);
    const std::size_t center = static_cast<std::size_t>(row) * window.Stride + column;
    Mathematics::Vector3 first = CanonicalSign(CentralTangent(window, center, 1u, referenceRadius));
    Mathematics::Vector3 second = CanonicalSign(CentralTangent(window, center, window.Stride, referenceRadius));
    // Across a face edge the two faces see a sample's tangents swapped or negated: the canonical sign
    // and order give the cross product the same operands, so the normal is bit-equal on both faces.
    if (LexicographicLess(second, first))
        std::swap(first, second);
    const Mathematics::Vector3& direction = window.Directions[center];
    const Mathematics::Vector3 normal = Mathematics::Vector3::Cross(first, second).NormalizeOr(direction);
    return Mathematics::Vector3::Dot(normal, direction) < 0.0f ? normal * -1.0f : normal;
}

PlanetSurfacePageProvider::PlanetSurfacePageProvider(
    std::shared_ptr<const PageStreaming::GeneratedHeightPageProvider> heights,
    std::span<const PageStreaming::HeightPageLayer* const> layers, const Components::TerrainSurfaceRulesEffect& rules,
    float64 referenceRadius)
    : m_Heights(std::move(heights))
    , m_Rules(rules)
    , m_ReferenceRadius(referenceRadius)
{
    m_Rules.RuleCount = std::min<uint32>(m_Rules.RuleCount, Components::kMaxTerrainSurfaceRules);
    for (const PageStreaming::HeightPageLayer* layer : layers)
        if (layer)
            m_Layers.push_back(*layer);
}

std::shared_ptr<const PageStreaming::GeneratedPagePlanes> PlanetSurfacePageProvider::MakePagePlanes(
    std::shared_ptr<const PlanetSurfacePageProvider> provider)
{
    auto planes = std::make_shared<PageStreaming::GeneratedPagePlanes>();
    const std::span<const PageStreaming::PageStoreLevel> levels = provider->m_Heights->Levels();
    planes->Levels.assign(levels.begin(), levels.end());
    planes->FaceCount = PageStreaming::kPageStoreMaxFaces;
    planes->Formats = {PageStreaming::PageFieldFormat::Rgba8, PageStreaming::PageFieldFormat::Bc5};
    planes->MarginSamples = kWindowMargin;
    planes->Fill = [provider = std::move(provider)](const PageStreaming::PageAddress& address,
                                                    std::span<const std::span<uint8>> filled) {
        return provider->FillPage(address, filled[0], filled[1]);
    };
    return planes;
}

bool PlanetSurfacePageProvider::FillWindow(const PageStreaming::PageAddress& address, PlanetSurfaceWindow& out) const
{
    out.Stride = kWindowStride;
    out.Heights.resize(static_cast<std::size_t>(kWindowStride) * kWindowStride);
    out.Directions.resize(out.Heights.size());
    if (!m_Heights || !m_Heights->FillPlanetWindow(address, kWindowMargin, out.Heights, out.Directions))
        return false;
    for (const PageStreaming::HeightPageLayer& layer : m_Layers)
        layer.ApplyToWindow(address, kWindowMargin, out.Heights);
    return true;
}

bool PlanetSurfacePageProvider::FillPage(const PageStreaming::PageAddress& address, std::span<uint8> splat,
                                         std::span<uint8> normal) const
{
    GE_CPU_PROFILE_SCOPE("Terrain.PlanetSurfacePage");
    constexpr uint32 kSplatStride = PageStreaming::kPageStrideSamples;
    const uint32 normalStride = PageStreaming::PageStrideTexels(PageStreaming::PageFieldFormat::Bc5);
    if (splat.size() != static_cast<std::size_t>(kSplatStride) * kSplatStride * kSplatTexelBytes ||
        normal.size() != static_cast<std::size_t>(normalStride) * normalStride * kNormalTexelBytes)
        return false;
    FillScratch& scratch = ThreadFillScratch();
    PlanetSurfaceWindow& window = scratch.Window;
    if (!FillWindow(address, window))
        return false;

    // The face's tangent frame: x is the face's X axis made normal to the direction, z = x cross it.
    const Mathematics::CubeFaceAxes& axes = Mathematics::kCubeFaceAxes[address.Face];
    const Mathematics::Vector3 faceX = AxisVector(axes.XAxis, axes.XSign);
    const uint32 normalOffset = kWindowMargin - (normalStride - PageStreaming::kPageOwnedSamples) / 2u;
    // The world normal of each normal-plane texel; the splat texels are a subset and read them here.
    std::vector<Mathematics::Vector3>& worldNormals = scratch.WorldNormals;
    worldNormals.resize(static_cast<std::size_t>(normalStride) * normalStride);
    for (uint32 row = 0; row < normalStride; ++row)
        for (uint32 column = 0; column < normalStride; ++column)
        {
            const uint32 windowColumn = column + normalOffset;
            const uint32 windowRow = row + normalOffset;
            const Mathematics::Vector3 world = PlanetWindowNormal(window, windowColumn, windowRow, m_ReferenceRadius);
            worldNormals[static_cast<std::size_t>(row) * normalStride + column] = world;
            const Mathematics::Vector3& direction =
                window.Directions[static_cast<std::size_t>(windowRow) * kWindowStride + windowColumn];
            const Mathematics::Vector3 tangentX =
                (faceX - direction * Mathematics::Vector3::Dot(faceX, direction)).Normalize();
            const Mathematics::Vector3 tangentZ = Mathematics::Vector3::Cross(tangentX, direction);
            const Mathematics::Vector3 local(Mathematics::Vector3::Dot(world, tangentX),
                                             Mathematics::Vector3::Dot(world, direction),
                                             Mathematics::Vector3::Dot(world, tangentZ));
            PageStreaming::EncodeNormalSample(
                local, &normal[(static_cast<std::size_t>(row) * normalStride + column) * kNormalTexelBytes]);
        }

    const uint32 splatOffset = kWindowMargin - PageStreaming::kPageApronSamples;
    static_assert(PageStreaming::kPageApronSamples <= PageStreaming::kPageBlockTexels,
                  "the splat texels lie inside the normal plane's texels");
    std::fill(splat.begin(), splat.end(), uint8{0});
    const std::span<const Components::TerrainSurfaceRule> rules(m_Rules.Rules, m_Rules.RuleCount);
    const TerrainRuleMeasurements measured = TerrainRuleMeasurementsOf(rules);
    SplatRowScratch& rowScratch = scratch.SplatRow;
    for (std::vector<float32>* plane : {&rowScratch.SlopeNormalized, &rowScratch.SlopeDegrees,
                                        &rowScratch.HeightMeters, &rowScratch.LatitudeDegrees})
        plane->resize(kSplatStride);
    rowScratch.Positions.resize(measured.Noise ? kSplatStride : 0u);
    rowScratch.Weights.resize(static_cast<std::size_t>(rules.size()) * kSplatStride);
    const TerrainRuleSampleRow samples{rowScratch.SlopeNormalized, rowScratch.SlopeDegrees, rowScratch.HeightMeters,
                                       rowScratch.LatitudeDegrees};
    for (uint32 row = 0; row < kSplatStride; ++row)
    {
        const uint32 windowRow = row + splatOffset;
        for (uint32 column = 0; column < kSplatStride; ++column)
        {
            const uint32 windowColumn = column + splatOffset;
            const std::size_t index = static_cast<std::size_t>(windowRow) * kWindowStride + windowColumn;
            const Mathematics::WorldPosition position = SurfacePoint(window, index, m_ReferenceRadius);
            const std::size_t normalIndex =
                static_cast<std::size_t>(windowRow - normalOffset) * normalStride + (windowColumn - normalOffset);
            const TerrainRuleSample sample =
                MakePlanetRuleSample(position, worldNormals[normalIndex], m_ReferenceRadius, measured);
            rowScratch.SlopeNormalized[column] = sample.SlopeNormalized;
            rowScratch.SlopeDegrees[column] = sample.SlopeDegrees;
            rowScratch.HeightMeters[column] = sample.HeightMeters;
            rowScratch.LatitudeDegrees[column] = sample.LatitudeDegrees;
            if (measured.Noise)
                rowScratch.Positions[column] = position;
        }
        // A weight does not read the splat bytes, so each rule's row of weights comes first; the composite
        // then makes the same calls in the same order as one texel at a time.
        for (std::size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex)
            if (rules[ruleIndex].MaterialSlot < kSplatTexelBytes)
                EvaluatePlanetSurfaceRuleWeightRow(
                    rules[ruleIndex], samples, rowScratch.Positions,
                    std::span(rowScratch.Weights).subspan(ruleIndex * kSplatStride, kSplatStride));
        for (uint32 column = 0; column < kSplatStride; ++column)
        {
            uint8* texel = &splat[(static_cast<std::size_t>(row) * kSplatStride + column) * kSplatTexelBytes];
            for (std::size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex)
                if (rules[ruleIndex].MaterialSlot < kSplatTexelBytes)
                    CompositeSplatTexel(texel, rules[ruleIndex].MaterialSlot,
                                        rowScratch.Weights[ruleIndex * kSplatStride + column],
                                        rules[ruleIndex].Replace);
        }
    }
    return true;
}

} // namespace GameEngine::TerrainECS
