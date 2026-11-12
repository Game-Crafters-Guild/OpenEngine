// The planet's generated surface pages: the splat and the normal from the composed height window and
// the rule rows, on six faces, with the apron equal to the neighbor face's owned texel.

#include <gtest/gtest.h>

#include "Mathematics/CubeFaces.h"
#include "Mathematics/WorldPosition.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageLoader.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "TerrainECS/Planet/PlanetSurfacePageProvider.h"
#include "TerrainECS/TerrainDefaultSurfaceRules.h"
#include "TerrainECS/TerrainRuleNoise.h"
#include "TerrainECS/TerrainSplatComposite.h"
#include "TerrainECS/TerrainSurface/TerrainSurfaceRuleEval.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::PageStreaming;
using namespace GameEngine::TerrainECS;

namespace
{

constexpr float64 kRadius = 2000.0;
constexpr uint32 kFaceSamples = 2u * kPageOwnedSamples + 1u;
constexpr uint32 kMargin = kPageSurfaceWindowMarginSamples;
const uint32 kNormalStride = PageStrideTexels(PageFieldFormat::Bc5);
const uint32 kNormalApron = (kNormalStride - kPageOwnedSamples) / 2u;

float32 Relief(const Mathematics::Vector3& d, float32)
{
    return 40.0f * std::sin(7.0f * d.x) * std::cos(5.0f * d.y) + 25.0f * std::sin(9.0f * d.z + 1.0f);
}

// Ridges about 60 m apart and up to 18 m high, flat on the crests and steep between them, so the rock
// slope row changes within one level-0 sample.
float32 SteepRelief(const Mathematics::Vector3& d, float32)
{
    return 12.0f * std::sin(210.0f * d.x) * std::cos(190.0f * d.z) + 6.0f * std::sin(170.0f * d.y);
}

std::shared_ptr<const GeneratedHeightPageProvider> Heights(
    float32 (*relief)(const Mathematics::Vector3&, float32) = Relief)
{
    GeneratedPlanetLattice lattice;
    lattice.ReferenceRadius = kRadius;
    lattice.FaceSamples = kFaceSamples;
    lattice.HeightRow = [relief](std::span<const Mathematics::Vector3> directions, float32 minResolvedArc,
                                 std::span<float32> outHeights) {
        for (std::size_t i = 0; i < directions.size(); ++i)
            outHeights[i] = relief(directions[i], minResolvedArc);
    };
    lattice.HeightRange = [](const Mathematics::Vector3&, float32, float32) { return PageHeightRange{-70.0f, 70.0f}; };
    return std::make_shared<const GeneratedHeightPageProvider>(std::move(lattice));
}

Components::TerrainSurfaceRulesEffect Rows()
{
    return MakeDefaultPlanetSurfaceRules(-65.0f, 65.0f);
}

// The rows' composite at a surface point of `position` with world normal `normal`.
std::array<uint8, 4> RowsComposite(const Components::TerrainSurfaceRulesEffect& rows,
                                   const Mathematics::WorldPosition& position, const Mathematics::Vector3& normal)
{
    const TerrainRuleSample sample = MakePlanetRuleSample(position, normal, kRadius, TerrainRuleMeasurements{});
    const auto noise = [&position](float32, float32, float32 frequency, uint32 seed)
    { return PlanetRuleNoiseSample(position, frequency, seed); };
    std::array<uint8, 4> composite{};
    for (uint32 i = 0; i < rows.RuleCount; ++i)
        CompositeSplatTexel(composite.data(), rows.Rules[i].MaterialSlot,
                            EvaluateTerrainSurfaceRuleWeight(rows.Rules[i], sample, noise), rows.Rules[i].Replace);
    return composite;
}

struct SurfacePage
{
    std::vector<uint8> Splat;
    std::vector<uint8> Normal;
    PlanetSurfaceWindow Window;
};

// Pages of one provider, each filled once.
class SurfacePages
{
public:
    SurfacePages()
        : m_Provider(Heights(), {}, Rows(), kRadius)
    {
    }

    const SurfacePage& Page(const PageAddress& address)
    {
        SurfacePage& page = m_Pages[address];
        if (page.Splat.empty())
        {
            page.Splat.resize(std::size_t(kPageSampleCount) * 4u);
            page.Normal.resize(std::size_t(kNormalStride) * kNormalStride * 2u);
            EXPECT_TRUE(m_Provider.FillPage(address, page.Splat, page.Normal));
            EXPECT_TRUE(m_Provider.FillWindow(address, page.Window));
        }
        return page;
    }

private:
    PlanetSurfacePageProvider m_Provider;
    std::map<PageAddress, SurfacePage> m_Pages;
};

// The page of a level-0 face coordinate in [-margin, last + margin]: the page whose window holds it.
uint32 PageOf(int64 index)
{
    return static_cast<uint32>(std::clamp<int64>(index, 0, kFaceSamples - 1) / kPageOwnedSamples);
}

// Sample (along, inward) of `side` in face coordinates; a negative inward is past the side.
std::pair<int64, int64> SideSample(PageFaceSide side, int64 along, int64 inward)
{
    const int64 last = kFaceSamples - 1;
    switch (side)
    {
    case PageFaceSide::MinX:
        return {inward, along};
    case PageFaceSide::MaxX:
        return {last - inward, along};
    case PageFaceSide::MinZ:
        return {along, inward};
    case PageFaceSide::MaxZ:
        return {along, last - inward};
    }
    return {0, 0};
}

// The world normal a page's normal sample at face coordinate (x, z) stores, from the face's tangent frame.
Mathematics::Vector3 DecodedNormal(const SurfacePage& page, uint8 face, int64 x, int64 z, const PageAddress& address)
{
    const int64 column = x - int64(address.X) * kPageOwnedSamples + kNormalApron;
    const int64 row = z - int64(address.Z) * kPageOwnedSamples + kNormalApron;
    const uint8* texel = &page.Normal[(std::size_t(row) * kNormalStride + std::size_t(column)) * 2u];
    const float32 lx = texel[0] / 255.0f * 2.0f - 1.0f;
    const float32 lz = texel[1] / 255.0f * 2.0f - 1.0f;
    const float32 ly = std::sqrt(std::max(0.0f, 1.0f - lx * lx - lz * lz));
    const Mathematics::CubeFaceAxes& axes = Mathematics::kCubeFaceAxes[face];
    const std::size_t windowIndex = std::size_t(row - kNormalApron + kMargin) * page.Window.Stride +
                                    std::size_t(column - kNormalApron + kMargin);
    const Mathematics::Vector3& d = page.Window.Directions[windowIndex];
    const float32 sign = static_cast<float32>(axes.XSign);
    const Mathematics::Vector3 faceX(axes.XAxis == 0 ? sign : 0.0f, axes.XAxis == 1 ? sign : 0.0f,
                                     axes.XAxis == 2 ? sign : 0.0f);
    const Mathematics::Vector3 tx = (faceX - d * Mathematics::Vector3::Dot(faceX, d)).Normalize();
    const Mathematics::Vector3 tz = Mathematics::Vector3::Cross(tx, d);
    return tx * lx + d * ly + tz * lz;
}

} // namespace

// Every owned texel of every page of levels 0 and 1 on six faces is the rows' composite at the sample:
// its height from the height page, its direction from the lattice, its normal from the window.
TEST(PlanetSurfacePageProvider, AGeneratedPageIsTheRuleOracleAtEveryLatticePointOnSixFaces)
{
    const std::shared_ptr<const GeneratedHeightPageProvider> heights = Heights();
    SurfacePages pages;
    const Components::TerrainSurfaceRulesEffect rows = Rows();
    std::vector<float32> heightPage(kPageSampleCount);
    uint32 mismatches = 0;
    uint32 compared = 0;
    std::array<uint32, 4> dominant{};
    for (uint8 level : {uint8{0}, uint8{1}})
    {
        const PageStoreLevel& shape = heights->Levels()[level];
        for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
            for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
                for (uint32 px = 0; px < shape.PagesX; ++px)
                {
                    const PageAddress address{face, level, px, pz};
                    const SurfacePage& page = pages.Page(address);
                    ASSERT_TRUE(heights->FillPage(address, heightPage));
                    for (uint32 r = 0; r < kPageOwnedSamples && pz * kPageOwnedSamples + r < shape.SamplesZ; ++r)
                        for (uint32 c = 0; c < kPageOwnedSamples && px * kPageOwnedSamples + c < shape.SamplesX; ++c)
                        {
                            const uint32 x = (px * kPageOwnedSamples + c) << level;
                            const uint32 z = (pz * kPageOwnedSamples + r) << level;
                            const Mathematics::Vector3 d = EqualAngleSampleDirection(face, x, z, kFaceSamples);
                            const float64 radius = kRadius + heightPage[(r + 1u) * kPageStrideSamples + c + 1u];
                            const Mathematics::WorldPosition position{d.x * radius, d.y * radius, d.z * radius};
                            const Mathematics::Vector3 normal =
                                PlanetWindowNormal(page.Window, c + kMargin, r + kMargin, kRadius);
                            const std::array<uint8, 4> expected = RowsComposite(rows, position, normal);
                            const uint8* texel = &page.Splat[((r + 1u) * kPageStrideSamples + c + 1u) * 4u];
                            mismatches += !std::equal(expected.begin(), expected.end(), texel);
                            ++dominant[std::max_element(texel, texel + 4) - texel];
                            ++compared;
                        }
                }
    }
    EXPECT_EQ(mismatches, 0u) << "of " << compared;
    // The rows paint more than one material over the relief.
    EXPECT_GE(std::count_if(dominant.begin(), dominant.end(), [](uint32 n) { return n > 0u; }), 2);
}

// Every splat texel, the apron included, on every level-0 and level-1 page of six faces is the rows'
// composite with the normal at its own window sample. The relief is steep enough that the normal one
// sample to the side changes the composite, so a splat that reads the wrong normal texel fails.
TEST(PlanetSurfacePageProvider, EverySplatTexelReadsTheNormalAtItsOwnSampleOnSixFaces)
{
    const std::shared_ptr<const GeneratedHeightPageProvider> heights = Heights(SteepRelief);
    const Components::TerrainSurfaceRulesEffect rows = Rows();
    const PlanetSurfacePageProvider provider(heights, {}, rows, kRadius);
    std::vector<uint8> splat(std::size_t(kPageSampleCount) * 4u);
    std::vector<uint8> normal(std::size_t(kNormalStride) * kNormalStride * 2u);
    PlanetSurfaceWindow window;
    uint32 mismatches = 0;
    uint32 compared = 0;
    uint32 sideNormalDiffers = 0;
    for (uint8 level : {uint8{0}, uint8{1}})
    {
        const PageStoreLevel& shape = heights->Levels()[level];
        for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
            for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
                for (uint32 px = 0; px < shape.PagesX; ++px)
                {
                    const PageAddress address{face, level, px, pz};
                    ASSERT_TRUE(provider.FillPage(address, splat, normal));
                    ASSERT_TRUE(provider.FillWindow(address, window));
                    for (uint32 row = 0; row < kPageStrideSamples; ++row)
                        for (uint32 column = 0; column < kPageStrideSamples; ++column)
                        {
                            const uint32 windowColumn = column + kMargin - kPageApronSamples;
                            const uint32 windowRow = row + kMargin - kPageApronSamples;
                            const std::size_t index = std::size_t(windowRow) * window.Stride + windowColumn;
                            const Mathematics::Vector3& d = window.Directions[index];
                            const float64 radius = kRadius + window.Heights[index];
                            const Mathematics::WorldPosition position{d.x * radius, d.y * radius, d.z * radius};
                            const std::array<uint8, 4> expected = RowsComposite(
                                rows, position, PlanetWindowNormal(window, windowColumn, windowRow, kRadius));
                            const uint8* texel = &splat[(std::size_t(row) * kPageStrideSamples + column) * 4u];
                            mismatches += !std::equal(expected.begin(), expected.end(), texel);
                            sideNormalDiffers +=
                                RowsComposite(rows, position,
                                              PlanetWindowNormal(window, windowColumn + 1u, windowRow, kRadius)) !=
                                expected;
                            ++compared;
                        }
                }
    }
    EXPECT_EQ(mismatches, 0u) << "of " << compared;
    EXPECT_GT(sideNormalDiffers, compared / 1000u)
        << "the normal one sample to the side rarely changes the composite: the test proves nothing";
    RecordProperty("SideNormalDiffers", std::to_string(sideNormalDiffers));
}

// Past a face side the splat apron texel is the neighbor face's owned texel bit for bit, and so is the
// window's world normal to the normal plane's apron depth. The normal plane stores the normal in each
// face's tangent frame, so its decoded apron normal equals the neighbor's within the encode.
TEST(PlanetSurfacePageProvider, TheApronIsTheNeighborFacesTexelAcrossEveryFaceEdge)
{
    SurfacePages pages;
    constexpr std::array<PageFaceSide, kPageFaceSides> kSides{PageFaceSide::MinZ, PageFaceSide::MaxZ,
                                                              PageFaceSide::MinX, PageFaceSide::MaxX};
    const int64 last = kFaceSamples - 1;
    uint32 splatMismatches = 0;
    uint32 normalMismatches = 0;
    float32 worstEncoded = 0.0f;
    uint32 compared = 0;
    for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
        for (PageFaceSide side : kSides)
        {
            const PageFaceEdge edge = PageFaceNeighbor(face, side);
            for (int64 along = kMargin + 1; along < last - kMargin; along += 3)
            {
                const int64 theirs = edge.Reversed ? last - along : along;
                for (int64 depth = 1; depth <= int64(kNormalApron); ++depth)
                {
                    const auto [ownX, ownZ] = SideSample(side, along, -depth);
                    const auto [theirX, theirZ] = SideSample(edge.Side, theirs, depth);
                    const PageAddress own{face, 0, PageOf(ownX), PageOf(ownZ)};
                    const PageAddress their{edge.Face, 0, PageOf(theirX), PageOf(theirZ)};
                    const SurfacePage& ownPage = pages.Page(own);
                    const SurfacePage& theirPage = pages.Page(their);
                    if (depth == 1)
                    {
                        const auto splatTexel = [](const SurfacePage& page, const PageAddress& address, int64 x,
                                                   int64 z) {
                            const int64 column = x - int64(address.X) * kPageOwnedSamples + kPageApronSamples;
                            const int64 row = z - int64(address.Z) * kPageOwnedSamples + kPageApronSamples;
                            return &page.Splat[(std::size_t(row) * kPageStrideSamples + std::size_t(column)) * 4u];
                        };
                        splatMismatches += !std::equal(splatTexel(ownPage, own, ownX, ownZ),
                                                       splatTexel(ownPage, own, ownX, ownZ) + 4,
                                                       splatTexel(theirPage, their, theirX, theirZ));
                    }
                    const auto windowNormal = [](const SurfacePage& page, const PageAddress& address, int64 x,
                                                 int64 z) {
                        return PlanetWindowNormal(page.Window,
                                                  uint32(x - int64(address.X) * kPageOwnedSamples + kMargin),
                                                  uint32(z - int64(address.Z) * kPageOwnedSamples + kMargin), kRadius);
                    };
                    const Mathematics::Vector3 a = windowNormal(ownPage, own, ownX, ownZ);
                    const Mathematics::Vector3 b = windowNormal(theirPage, their, theirX, theirZ);
                    normalMismatches += a.x != b.x || a.y != b.y || a.z != b.z;
                    worstEncoded = std::max(worstEncoded, (DecodedNormal(ownPage, face, ownX, ownZ, own) -
                                                           DecodedNormal(theirPage, edge.Face, theirX, theirZ, their))
                                                              .Length());
                    ++compared;
                }
            }
        }
    EXPECT_GT(compared, 0u);
    EXPECT_EQ(splatMismatches, 0u);
    EXPECT_EQ(normalMismatches, 0u) << "of " << compared;
    // Two Rg8 encodes in different frames: at most two steps of 2/255 on each channel.
    EXPECT_LE(worstEncoded, 0.03f);
    RecordProperty("WorstEncodedApronNormalDifference", std::to_string(worstEncoded));
}

namespace
{

// The splat, then the normal channels, of page `address` of `planes`.
std::vector<uint8> FilledPlanes(const GeneratedPagePlanes& planes, const PageAddress& address)
{
    std::vector<uint8> splat(std::size_t(kPageSampleCount) * 4u);
    std::vector<uint8> normal(std::size_t(kNormalStride) * kNormalStride * 2u);
    const std::array<std::span<uint8>, 2> spans = {std::span<uint8>(splat), std::span<uint8>(normal)};
    EXPECT_TRUE(planes.Fill(address, spans));
    splat.insert(splat.end(), normal.begin(), normal.end());
    return splat;
}

} // namespace

// A layer written in place while a provider over it fills a page does not reach that provider's pages:
// the provider fills from its own copy of the layer, so a fill never reads a half-written layer.
TEST(PlanetSurfacePageProvider, ALayerWriteDuringAFillDoesNotReachTheProvidersPages)
{
    const std::shared_ptr<const GeneratedHeightPageProvider> heights = Heights();
    HeightPageLayer layer(kPageStoreMaxFaces, heights->Levels());
    const PageSampleRect rect{40u, 40u, 90u, 90u};
    const std::size_t rectSamples = std::size_t(rect.MaxX - rect.MinX + 1u) * (rect.MaxZ - rect.MinZ + 1u);
    layer.WriteLevel0(0u, rect, std::vector<float32>(rectSamples, 30.0f));
    const std::array<const HeightPageLayer*, 1> layers = {&layer};
    const auto provider = std::make_shared<const PlanetSurfacePageProvider>(heights, layers, Rows(), kRadius);
    const std::shared_ptr<const GeneratedPagePlanes> planes = PlanetSurfacePageProvider::MakePagePlanes(provider);
    ASSERT_EQ(planes->FaceCount, kPageStoreMaxFaces);
    ASSERT_EQ(planes->Levels.size(), heights->Levels().size());

    const PageAddress address{0u, 0u, 0u, 0u};
    const std::vector<uint8> before = FilledPlanes(*planes, address);

    // A sloped ramp in place of the bump: the slope and the normals of the page change.
    std::vector<float32> ramp(rectSamples);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = 2.0f * static_cast<float32>(i % (rect.MaxX - rect.MinX + 1u));
    layer.WriteLevel0(0u, rect, ramp);
    EXPECT_EQ(FilledPlanes(*planes, address), before) << "a write to the live layer reached a fill of the provider made before it";

    const std::array<const HeightPageLayer*, 1> written = {&layer};
    const auto after = std::make_shared<const PlanetSurfacePageProvider>(heights, written, Rows(), kRadius);
    EXPECT_NE(FilledPlanes(*PlanetSurfacePageProvider::MakePagePlanes(after), address), before)
        << "the write does not change the page: the test proves nothing";
}

// Each window sample has the sample direction of its face sample (the neighbor face's past a side, the
// corner sample past a cube corner) and the relief at that direction, bit for bit, on every page of six
// faces. The test records the FNV-1a 64 of the planes of all pages as PlanesFnv1a64.
TEST(PlanetSurfacePageProvider, TheWindowIsTheReliefAtEachSampleDirectionOnSixFaces)
{
    const std::shared_ptr<const GeneratedHeightPageProvider> heights = Heights();
    SurfacePages pages;
    uint32 mismatches = 0;
    uint32 compared = 0;
    uint64 planesHash = Hashing::kFnv1a64OffsetBasis;
    for (uint8 level : {uint8{0}, uint8{1}})
    {
        const PageStoreLevel& shape = heights->Levels()[level];
        for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
            for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
                for (uint32 px = 0; px < shape.PagesX; ++px)
                {
                    const PageAddress address{face, level, px, pz};
                    const SurfacePage& page = pages.Page(address);
                    const int64 firstX = int64{px} * kPageOwnedSamples - kMargin;
                    const int64 firstZ = int64{pz} * kPageOwnedSamples - kMargin;
                    for (uint32 row = 0; row < page.Window.Stride; ++row)
                        for (uint32 column = 0; column < page.Window.Stride; ++column)
                        {
                            const PageFaceSample sample =
                                PageCubeWindowSampleAt(face, firstX + column, firstZ + row, shape.SamplesX, kMargin);
                            const Mathematics::Vector3 expected =
                                EqualAngleSampleDirection(sample.Face, sample.X << level, sample.Z << level, kFaceSamples);
                            const std::size_t index = std::size_t(row) * page.Window.Stride + column;
                            const Mathematics::Vector3& direction = page.Window.Directions[index];
                            mismatches += direction.x != expected.x || direction.y != expected.y ||
                                          direction.z != expected.z || page.Window.Heights[index] != Relief(expected, 0.0f);
                            ++compared;
                        }
                    planesHash = Hashing::Fnv1a64(page.Splat.data(), page.Splat.size(), planesHash);
                    planesHash = Hashing::Fnv1a64(page.Normal.data(), page.Normal.size(), planesHash);
                }
    }
    EXPECT_EQ(mismatches, 0u) << "of " << compared;
    RecordProperty("PlanesFnv1a64", std::to_string(planesHash));
}
