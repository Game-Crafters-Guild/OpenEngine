#include "PageStreaming/PageStoreFormat.h"

#include "PageStoreSerialization.h"

#include "Mathematics/CubeFaces.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace GameEngine::PageStreaming
{
namespace
{

uint32 PagesFor(uint32 samples)
{
    return (samples + kPageOwnedSamples - 1u) / kPageOwnedSamples;
}

bool MortonBefore(const PageIndexEntry& entry, uint64 code)
{
    return entry.Morton < code;
}

bool MortonLess(const PageIndexEntry& a, const PageIndexEntry& b)
{
    return a.Morton < b.Morton;
}

} // namespace

bool PageStoreLayout::Contains(const PageAddress& address) const
{
    if (address.Face >= Header.FaceCount || address.Level >= Levels.size())
        return false;
    const PageStoreLevel& level = Levels[address.Level];
    return address.X < level.PagesX && address.Z < level.PagesZ;
}

std::optional<std::size_t> PageStoreLayout::PresentIndex(const PageAddress& address) const
{
    const std::size_t rangeIndex = static_cast<std::size_t>(address.Face) * Levels.size() + address.Level;
    if (!Contains(address) || rangeIndex >= Ranges.size())
        return std::nullopt;
    const PageStoreRange& range = Ranges[rangeIndex];
    const uint64 code = PageMortonCode(address.X, address.Z);
    const auto first = Entries.begin() + static_cast<std::ptrdiff_t>(range.First);
    const auto last = first + static_cast<std::ptrdiff_t>(range.Count);
    const auto found = std::lower_bound(first, last, code, MortonBefore);
    if (found == last || found->Morton != code)
        return std::nullopt;
    return static_cast<std::size_t>(found - Entries.begin());
}

const PageIndexEntry* PageStoreLayout::FindPresent(const PageAddress& address) const
{
    const std::optional<std::size_t> index = PresentIndex(address);
    return index ? &Entries[*index] : nullptr;
}

std::optional<PageAddress> PageStoreLayout::ResolvePresent(PageAddress address) const
{
    while (Contains(address))
    {
        if (FindPresent(address))
            return address;
        address = address.Parent();
    }
    return std::nullopt;
}

uint32 PageStoreLayout::StoredBytes(const PageIndexEntry& entry) const
{
    return PageStoredBytes(Header.Format, entry.HasWideWords());
}

uint64 PageStoreLayout::DataOffset() const
{
    return kPageStoreHeaderBytes + Levels.size() * kPageStoreLevelBytes + Ranges.size() * kPageStoreRangeBytes +
           Entries.size() * kPageIndexEntryBytes;
}

uint64 PageStoreLayout::TotalBytes() const
{
    uint64 end = DataOffset();
    for (const PageIndexEntry& entry : Entries)
        if (entry.Offset != 0)
            end = std::max(end, entry.Offset + StoredBytes(entry));
    return end;
}

std::vector<PageStoreLevel> BuildPageStoreLevels(uint32 samplesX, uint32 samplesZ)
{
    std::vector<PageStoreLevel> levels;
    if (samplesX == 0 || samplesZ == 0)
        return levels;
    uint64 firstEntry = 0;
    while (levels.size() < kPageStoreMaxLevels)
    {
        PageStoreLevel level;
        level.SamplesX = samplesX;
        level.SamplesZ = samplesZ;
        level.PagesX = PagesFor(samplesX);
        level.PagesZ = PagesFor(samplesZ);
        level.FirstEntry = firstEntry;
        firstEntry += static_cast<uint64>(level.PagesX) * level.PagesZ;
        levels.push_back(level);
        if (level.PagesX == 1 && level.PagesZ == 1)
            break;
        samplesX = samplesX / 2u + 1u;
        samplesZ = samplesZ / 2u + 1u;
    }
    return levels;
}

uint64 DensePageCount(std::span<const PageStoreLevel> levels)
{
    if (levels.empty())
        return 0;
    return levels.back().FirstEntry + static_cast<uint64>(levels.back().PagesX) * levels.back().PagesZ;
}

bool IsBlockFormat(PageFieldFormat format)
{
    return format == PageFieldFormat::Bc5;
}

uint32 PageStrideTexels(PageFieldFormat format)
{
    return IsBlockFormat(format) ? kPageOwnedSamples + 2u * kPageBlockTexels : kPageStrideSamples;
}

uint32 PageTexelBytes(PageFieldFormat format)
{
    switch (format)
    {
    case PageFieldFormat::Rg8:
    case PageFieldFormat::Bc5:
        return 2u;
    case PageFieldFormat::HeightSteps:
    case PageFieldFormat::Rgba8:
    case PageFieldFormat::Rgba8Srgb:
        return 4u;
    }
    return 0u;
}

void EncodeNormalSample(const Mathematics::Vector3& normal, uint8* outTexel)
{
    constexpr float32 kByteMax = 255.0f;
    outTexel[0] = static_cast<uint8>(std::lround(Math::Clamp01(normal.x * 0.5f + 0.5f) * kByteMax));
    outTexel[1] = static_cast<uint8>(std::lround(Math::Clamp01(normal.z * 0.5f + 0.5f) * kByteMax));
}

uint32 PageStoredBytes(PageFieldFormat format, bool wide)
{
    const uint32 stride = PageStrideTexels(format);
    switch (format)
    {
    case PageFieldFormat::HeightSteps:
        return stride * stride * (wide ? 4u : 2u);
    case PageFieldFormat::Bc5:
        return (stride / kPageBlockTexels) * (stride / kPageBlockTexels) * kBc5BlockBytes;
    case PageFieldFormat::Rgba8:
    case PageFieldFormat::Rgba8Srgb:
    case PageFieldFormat::Rg8:
        return stride * stride * PageTexelBytes(format);
    }
    return 0u;
}

uint32 PagePayloadBytes(PageFieldFormat format)
{
    if (IsBlockFormat(format))
        return PageStoredBytes(format, false);
    const uint32 stride = PageStrideTexels(format);
    return stride * stride * PageTexelBytes(format);
}

std::span<const float32> HeightSamples(std::span<const uint8> bytes)
{
    assert(bytes.size() == PagePayloadBytes(PageFieldFormat::HeightSteps) &&
           reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(float32) == 0 && "not a loaded height page");
    return {reinterpret_cast<const float32*>(bytes.data()), kPageSampleCount};
}

std::span<float32> HeightSamples(std::vector<uint8>& bytes)
{
    assert(bytes.size() == PagePayloadBytes(PageFieldFormat::HeightSteps) &&
           reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(float32) == 0 && "not a loaded height page");
    return {reinterpret_cast<float32*>(bytes.data()), kPageSampleCount};
}

PageStoreLayout AddPresentPages(const PageStoreLayout& layout, std::span<const uint8> present)
{
    std::vector<uint8> merged(present.begin(), present.end());
    std::vector<std::pair<std::size_t, PageAddress>> kept;
    std::size_t page = 0;
    for (uint32 face = 0; face < layout.Header.FaceCount; ++face)
        for (std::size_t level = 0; level < layout.Levels.size(); ++level)
            for (uint32 z = 0; z < layout.Levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < layout.Levels[level].PagesX; ++x, ++page)
                {
                    const PageAddress address{static_cast<uint8>(face), static_cast<uint8>(level), x, z};
                    const std::optional<std::size_t> index = layout.PresentIndex(address);
                    if (!index)
                        continue;
                    if (page < merged.size())
                        merged[page] = 1u;
                    kept.emplace_back(*index, address);
                }
    PageStoreLayout grown = MakePageStoreLayout(layout.Header, merged);
    for (const auto& [index, address] : kept)
        if (const std::optional<std::size_t> to = grown.PresentIndex(address))
            grown.Entries[*to] = layout.Entries[index];
    return grown;
}

PageStoreLayout MakePageStoreLayout(const PageStoreHeader& header, std::span<const uint8> present)
{
    PageStoreLayout layout;
    layout.Header = header;
    layout.Levels = BuildPageStoreLevels(header.SamplesX, header.SamplesZ);
    // Steps past the store's levels are not on disk: zero, so a header compares equal to its read.
    std::fill(layout.Header.StepPerLevel.begin() + static_cast<std::ptrdiff_t>(layout.Levels.size()),
              layout.Header.StepPerLevel.end(), 0.0f);
    if (header.Format != PageFieldFormat::HeightSteps)
    {
        // A store that holds no heights has no height units, reference or steps.
        layout.Header.Units = PageHeightUnits{};
        layout.Header.ReferenceRadius = 0.0;
        layout.Header.StepPerLevel.fill(0.0f);
    }
    const uint64 densePerFace = DensePageCount(layout.Levels);
    layout.Ranges.resize(layout.Levels.size() * header.FaceCount);
    if (!present.empty() && present.size() != densePerFace * header.FaceCount)
        return layout; // every page absent: the caller's presence does not fit the header

    for (uint32 face = 0; face < header.FaceCount; ++face)
    {
        for (std::size_t levelIndex = 0; levelIndex < layout.Levels.size(); ++levelIndex)
        {
            const PageStoreLevel& level = layout.Levels[levelIndex];
            const uint64 dense = face * densePerFace + level.FirstEntry;
            PageStoreRange& range = layout.Ranges[face * layout.Levels.size() + levelIndex];
            range.First = layout.Entries.size();
            for (uint32 z = 0; z < level.PagesZ; ++z)
            {
                for (uint32 x = 0; x < level.PagesX; ++x)
                {
                    const uint64 page = dense + static_cast<uint64>(z) * level.PagesX + x;
                    if (present.empty() || present[static_cast<std::size_t>(page)] != 0)
                        layout.Entries.push_back(PageIndexEntry{PageMortonCode(x, z)});
                }
            }
            std::sort(layout.Entries.begin() + static_cast<std::ptrdiff_t>(range.First), layout.Entries.end(),
                      MortonLess);
            range.Count = layout.Entries.size() - range.First;
        }
    }
    return layout;
}

} // namespace GameEngine::PageStreaming

// The equal-angle functions below are a format constant: they must give the same bits on every
// machine, so contraction is off, the deep decode's rule (CBTDeepDecode.h).
#if defined(_MSC_VER)
#pragma float_control(push)
#pragma fp_contract(off)
#elif defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize ("fp-contract=off")
#else
#pragma STDC FP_CONTRACT OFF
#endif

namespace GameEngine::PageStreaming
{

namespace
{

// r(t) = R0 + R1 t + R2 t^2 + R3 t^3 of EqualAngleFaceCoordinate: the least-squares cubic, on
// Chebyshev nodes of [0, 1], to ((4 / pi) atan(sqrt t) / sqrt t - 1) / (t - 1).
constexpr float64 kEqualAngleR0 = -0.2731202974275542;
constexpr float64 kEqualAngleR1 = 0.1469790459925729;
constexpr float64 kEqualAngleR2 = -0.07874229125948942;
constexpr float64 kEqualAngleR3 = 0.023282467736952254;
// Newton steps of GnomonicFaceCoordinate, from a = s: a fixed count, so every machine stops alike.
constexpr uint32 kGnomonicNewtonSteps = 8u;

float64 EqualAngleRemainder(float64 t)
{
    return kEqualAngleR0 + t * (kEqualAngleR1 + t * (kEqualAngleR2 + t * kEqualAngleR3));
}

float64 EqualAngleRemainderSlope(float64 t)
{
    return kEqualAngleR1 + t * (2.0 * kEqualAngleR2 + t * 3.0 * kEqualAngleR3);
}

bool IsMinSide(PageFaceSide side)
{
    return side == PageFaceSide::MinZ || side == PageFaceSide::MinX;
}

} // namespace

PageFaceEdge PageFaceNeighbor(uint8 face, PageFaceSide side)
{
    // Across the side the face's coordinate on its across axis is -1 or +1: the neighbor's normal.
    const Mathematics::CubeFaceAxes& own = Mathematics::kCubeFaceAxes[face];
    const bool rowSide = side == PageFaceSide::MinZ || side == PageFaceSide::MaxZ;
    const uint8 acrossAxis = rowSide ? own.ZAxis : own.XAxis;
    const int8 acrossSign = static_cast<int8>((rowSide ? own.ZSign : own.XSign) * (IsMinSide(side) ? -1 : 1));
    const int8 alongSign = rowSide ? own.XSign : own.ZSign;
    PageFaceEdge edge;
    for (uint8 other = 0; other < kPageStoreMaxFaces; ++other)
    {
        const Mathematics::CubeFaceAxes& axes = Mathematics::kCubeFaceAxes[other];
        if (axes.NormalAxis != acrossAxis || axes.NormalSign != acrossSign)
            continue;
        // On the neighbor this face's normal is an in-face axis, at +1 or -1 of it.
        const bool neighborColumn = axes.XAxis == own.NormalAxis;
        const int8 neighborSign = static_cast<int8>(own.NormalSign * (neighborColumn ? axes.XSign : axes.ZSign));
        edge.Face = other;
        edge.Side = neighborColumn ? (neighborSign < 0 ? PageFaceSide::MinX : PageFaceSide::MaxX)
                                   : (neighborSign < 0 ? PageFaceSide::MinZ : PageFaceSide::MaxZ);
        const int8 neighborAlongSign = neighborColumn ? axes.ZSign : axes.XSign;
        edge.Reversed = alongSign != neighborAlongSign;
    }
    return edge;
}

float64 EqualAngleFaceCoordinate(float64 gnomonic)
{
    const float64 t = gnomonic * gnomonic;
    return gnomonic + gnomonic * (t - 1.0) * EqualAngleRemainder(t);
}

float64 GnomonicFaceCoordinate(float64 equalAngle)
{
    // p increases (its slope is at least 0.63) and is concave on [0, 1], so Newton from a = s lands
    // at or below the root after the first step and climbs to it: every step stays in [0, 1].
    const float64 target = std::abs(equalAngle);
    float64 a = target;
    for (uint32 step = 0; step < kGnomonicNewtonSteps; ++step)
    {
        const float64 t = a * a;
        const float64 slope = 1.0 + (3.0 * t - 1.0) * EqualAngleRemainder(t) + 2.0 * t * (t - 1.0) * EqualAngleRemainderSlope(t);
        a -= (EqualAngleFaceCoordinate(a) - target) / slope;
    }
    if (target == 1.0)
        a = 1.0;
    return equalAngle < 0.0 ? -a : a;
}

float64 EqualAngleSampleCoordinate(uint32 index, uint32 samples)
{
    const int64 last = static_cast<int64>(samples) - 1;
    return static_cast<float64>(2 * static_cast<int64>(index) - last) / static_cast<float64>(last);
}

} // namespace GameEngine::PageStreaming

#if defined(_MSC_VER)
#pragma float_control(pop)
#elif defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
