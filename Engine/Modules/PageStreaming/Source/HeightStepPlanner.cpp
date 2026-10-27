#include "HeightStepPlanner.h"

#include "HeightPageCodec.h"
#include "PageStore/PageStoreSerialization.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::PageStreaming
{
namespace
{

// The 16-bit test's margin for the floor of the base and the rounding of the top: a range of r steps
// spans at most r + 2 integers between them.
constexpr float64 kSixteenBitSteps = 65533.0;

// The exponent below which the largest height would pass kHeightIntegerMax steps.
int32 MagnitudeExponent(float32 magnitude)
{
    if (!(magnitude > 0.0f))
        return kHeightStepExponentMin;
    int32 exponent = 0;
    std::frexp(static_cast<float64>(magnitude) / kHeightIntegerMax, &exponent);
    return exponent;
}

// The exponent at which at most max(1, kHeightWidePageShare x pages) of the pages (each needing
// `needed[i]`) are wide, and never every page.
int32 LevelExponent(std::vector<int32>& needed)
{
    const std::size_t share = static_cast<std::size_t>(kHeightWidePageShare * static_cast<float64>(needed.size()));
    const std::size_t allowed = std::min(std::max<std::size_t>(1u, share), needed.size() - 1u);
    const auto rank = needed.begin() + static_cast<std::ptrdiff_t>(needed.size() - 1u - allowed);
    std::nth_element(needed.begin(), rank, needed.end());
    return *rank;
}

} // namespace

int32 SixteenBitStepExponent(float32 range)
{
    if (!(range > 0.0f))
        return kHeightStepExponentMin;
    if (!std::isfinite(range))
        return std::numeric_limits<int8>::max(); // a span past float32: no finite step fits it
    int32 exponent = 0;
    std::frexp(static_cast<float64>(range) / kSixteenBitSteps, &exponent); // range / 65533 < 2^exponent
    exponent = std::max(exponent - 1, kHeightStepExponentMin);
    while (static_cast<float64>(range) > std::ldexp(kSixteenBitSteps, exponent))
        ++exponent;
    return exponent;
}

HeightStepPlanner::HeightStepPlanner(uint32 samplesX, uint32 samplesZ, uint32 faces)
    : m_Levels(BuildPageStoreLevels(samplesX, samplesZ))
    , m_Faces(faces)
{
    if (m_Levels.empty())
        return;
    m_FacePages = static_cast<std::size_t>(m_Levels[0].PagesX) * m_Levels[0].PagesZ;
    m_PageMin.assign(m_FacePages * faces, std::numeric_limits<float32>::max());
    m_PageMax.assign(m_FacePages * faces, std::numeric_limits<float32>::lowest());
}

void HeightStepPlanner::Push(std::span<const float32> samples)
{
    if (m_Levels.empty())
        return;
    const uint32 width = m_Levels[0].SamplesX;
    while (!samples.empty() && m_Face < m_Faces)
    {
        const uint32 span = static_cast<uint32>(std::min<std::size_t>(samples.size(), width - m_X));
        PushRowSpan(samples.first(span), m_X);
        samples = samples.subspan(span);
        m_X += span;
        if (m_X == width)
        {
            m_X = 0;
            ++m_Z;
        }
        if (m_Z == m_Levels[0].SamplesZ)
        {
            m_Z = 0;
            ++m_Face;
        }
    }
}

void HeightStepPlanner::PushRowSpan(std::span<const float32> samples, uint32 firstX)
{
    // Page p's footprint is samples 128p to 128p + 128 on each axis: the shared edge belongs to both
    // pages it separates.
    const PageStoreLevel& level = m_Levels[0];
    const uint32 rowPage = std::min(m_Z / kPageOwnedSamples, level.PagesZ - 1u);
    const bool sharedRow = m_Z % kPageOwnedSamples == 0 && m_Z > 0;
    const uint32 end = firstX + static_cast<uint32>(samples.size());
    uint32 page = firstX / kPageOwnedSamples;
    if (firstX % kPageOwnedSamples == 0 && firstX > 0)
        --page; // the span starts on the shared edge of the page before
    for (; page < level.PagesX && page * kPageOwnedSamples < end; ++page)
    {
        const uint32 from = std::max(firstX, page * kPageOwnedSamples);
        const uint32 to = std::min(end, page * kPageOwnedSamples + kPageOwnedSamples + 1u);
        if (from >= to)
            continue;
        float32 lowest = std::numeric_limits<float32>::max();
        float32 highest = std::numeric_limits<float32>::lowest();
        for (uint32 x = from; x < to; ++x)
        {
            const float32 value = samples[x - firstX];
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);
        }
        m_Magnitude = std::max({m_Magnitude, std::abs(lowest), std::abs(highest)});
        for (uint32 pageRow = sharedRow ? rowPage - 1u : rowPage; pageRow <= rowPage; ++pageRow)
        {
            const std::size_t index = m_Face * m_FacePages + static_cast<std::size_t>(pageRow) * level.PagesX + page;
            m_PageMin[index] = std::min(m_PageMin[index], lowest);
            m_PageMax[index] = std::max(m_PageMax[index], highest);
        }
    }
}

std::array<int8, kPageStoreMaxLevels> HeightStepPlanner::StepExponents() const
{
    std::array<int8, kPageStoreMaxLevels> exponents{};
    const int32 finest = std::max(kHeightStepExponentMin, MagnitudeExponent(m_Magnitude));
    std::vector<float32> lowest = m_PageMin;
    std::vector<float32> highest = m_PageMax;
    std::vector<int32> needed;
    for (std::size_t levelIndex = 0; levelIndex < m_Levels.size(); ++levelIndex)
    {
        const PageStoreLevel& level = m_Levels[levelIndex];
        if (levelIndex > 0)
        {
            // A coarser page's level-0 footprint is its four finer pages', clamped to the finer
            // level as PagePyramidBuilder clamps it.
            const PageStoreLevel& finer = m_Levels[levelIndex - 1u];
            const std::size_t facePages = static_cast<std::size_t>(level.PagesX) * level.PagesZ;
            const std::size_t finerFacePages = static_cast<std::size_t>(finer.PagesX) * finer.PagesZ;
            std::vector<float32> coarseMin(facePages * m_Faces, std::numeric_limits<float32>::max());
            std::vector<float32> coarseMax(coarseMin.size(), std::numeric_limits<float32>::lowest());
            for (uint32 face = 0; face < m_Faces; ++face)
                for (uint32 z = 0; z < level.PagesZ; ++z)
                    for (uint32 x = 0; x < level.PagesX; ++x)
                        for (uint32 d = 0; d < 4u; ++d)
                        {
                            const uint32 childX = std::min(2u * x + (d & 1u), finer.PagesX - 1u);
                            const uint32 childZ = std::min(2u * z + (d >> 1u), finer.PagesZ - 1u);
                            const std::size_t child =
                                face * finerFacePages + static_cast<std::size_t>(childZ) * finer.PagesX + childX;
                            const std::size_t page = face * facePages + static_cast<std::size_t>(z) * level.PagesX + x;
                            coarseMin[page] = std::min(coarseMin[page], lowest[child]);
                            coarseMax[page] = std::max(coarseMax[page], highest[child]);
                        }
            lowest = std::move(coarseMin);
            highest = std::move(coarseMax);
        }
        needed.resize(lowest.size());
        for (std::size_t page = 0; page < lowest.size(); ++page)
            needed[page] = lowest[page] <= highest[page] ? SixteenBitStepExponent(highest[page] - lowest[page])
                                                         : kHeightStepExponentMin;
        exponents[levelIndex] = static_cast<int8>(std::clamp(LevelExponent(needed), finest, kHeightStepExponentMax));
    }
    return exponents;
}

} // namespace GameEngine::PageStreaming
