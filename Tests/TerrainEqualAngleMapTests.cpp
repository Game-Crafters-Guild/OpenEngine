// The GPU equal-angle map (CBT_EqualAngleTexel) against the CPU map (PageStreaming::EqualAngleFaceCoordinate),
// and the planet page point (CBT_PlanetPagePoint) through the page walk. The GLSL blocks compile here as C++
// (ExtractShaderBlock.cmake, GlslShim.h) with contraction off, so the CPU runs the GPU's fp32 operations.

#include <gtest/gtest.h>

#include "GlslShim.h"
#include "TerrainECS/PageTable.h"
#include "Mathematics/CubeFaces.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "PageStreaming/PageTableEntry.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#if defined(_MSC_VER)
#pragma fp_contract(off)
#else
#pragma STDC FP_CONTRACT OFF
#endif

namespace
{

// A six-face page table whose every page answers one value over its whole stride.
struct ConstantPageField
{
    GameEngine::TerrainECS::PageTable Table;
    std::vector<float> SlotValues;
};
const ConstantPageField* s_Field = nullptr;
constexpr uint32_t kFieldSlotsPerRow = 32u;

namespace Shader
{
#define precise
using GameEngine::GlslShim::clamp;
using GameEngine::GlslShim::floor;
using GameEngine::GlslShim::min;
using GameEngine::GlslShim::uint;
using std::fma;
using std::ldexp;
#include "PageResolveTypesExtracted.h"

uint CBT_PageTableWord(uint index)
{
    return s_Field->Table.Words()[index];
}

float CBT_PageSlotFade(uint)
{
    return 1.0f;
}

CBTPageLevel CBT_PageLevelShape(uint level)
{
    const GameEngine::PageStreaming::PageStoreLevel& shape = s_Field->Table.Levels()[level];
    return CBTPageLevel{s_Field->Table.LevelOffsets()[level], shape.PagesX, shape.PagesZ, 0u};
}

float CBT_PageCacheBilinear(float texelX, float texelZ)
{
    const float stride = float(GameEngine::PageStreaming::kPageStrideSamples);
    const uint slot = uint(texelZ / stride) * kFieldSlotsPerRow + uint(texelX / stride);
    return s_Field->SlotValues[slot];
}

#include "PageResolveExtracted.h"
#include "EqualAngleExtracted.h"
#undef precise
} // namespace Shader

// The CPU map's texel coordinate of numerator / 2^numSubdiv, in float64.
double CpuTexelCoordinate(int64_t numerator, uint32_t numSubdiv, uint32_t samples)
{
    const double gnomonic = std::ldexp(static_cast<double>(numerator), -static_cast<int>(numSubdiv));
    const double equalAngle = GameEngine::PageStreaming::EqualAngleFaceCoordinate(gnomonic);
    return (equalAngle + 1.0) * 0.5 * static_cast<double>(samples - 1u);
}

// Faces of about the sizes of the scenes P-2, P-50 and P-600, and the largest below 2^22 samples.
constexpr uint32_t kFaceSamples[] = {6283u, 131073u, 2094397u, (1u << 22) - 1u};
constexpr uint32_t kSubdivisions[] = {20u, 32u, 44u, 50u};
// The fraction's bound: 2^-20 of a texel.
constexpr double kFractionTolerance = 1.0 / 1048576.0;

} // namespace

TEST(EqualAngleMap, TheGlslMapIsTheCpuMapOnASweepOfNumerators)
{
    std::mt19937_64 random(0x5EEDull);
    uint32_t compared = 0;
    double worst = 0.0;
    for (uint32_t samples : kFaceSamples)
        for (uint32_t numSubdiv : kSubdivisions)
        {
            const int64_t scale = int64_t(1) << numSubdiv;
            for (uint32_t i = 0; i < 4096u; ++i)
            {
                int64_t numerator = static_cast<int64_t>(random() % static_cast<uint64_t>(2 * scale + 1)) - scale;
                if (i < 3u)
                    numerator = (static_cast<int64_t>(i) - 1) * scale; // the face's edges and its center
                const double coordinate = CpuTexelCoordinate(numerator, numSubdiv, samples);
                const double cpuTexel = std::floor(coordinate);
                const double cpuFraction = coordinate - cpuTexel;
                const Shader::CBTFaceTexel gpu = Shader::CBT_EqualAngleTexel(numerator, numSubdiv, samples);
                // Equal on this sweep. Within an fp32 ulp of a texel boundary the GPU can give
                // the next texel with fraction 0, which resolves to the same height.
                ASSERT_EQ(static_cast<double>(gpu.Texel), cpuTexel)
                    << "samples " << samples << " numSubdiv " << numSubdiv << " numerator " << numerator;
                const double error = std::abs(static_cast<double>(gpu.Fraction) - cpuFraction);
                ASSERT_LE(error, kFractionTolerance)
                    << "samples " << samples << " numSubdiv " << numSubdiv << " numerator " << numerator;
                ASSERT_GE(gpu.Fraction, 0.0f);
                ASSERT_LT(gpu.Fraction, 1.0f);
                worst = std::max(worst, error);
                ++compared;
            }
        }
    EXPECT_EQ(compared, 4u * 4u * 4096u);
    RecordProperty("WorstFractionError", std::to_string(worst));
}

namespace
{

constexpr uint32_t kEdgeFaceSamples = 1025u; // five levels: 8, 4, 2, 1 and 1 pages a side
constexpr uint32_t kEdgeSubdiv = 30u;

// The value every page of `level` on `face` answers.
float PageValue(uint32_t face, uint32_t level)
{
    return 1000.0f + 100.0f * float(face) + float(level);
}

// Face 0 (+X) holds every level, face 2 (+Y) level 2 and coarser, every other face its top level alone,
// so a point on a face edge or corner resolves to a different value on each face that holds it.
ConstantPageField MakeEdgeField()
{
    using GameEngine::PageStreaming::PageAddress;
    ConstantPageField field;
    constexpr uint32_t kSlots = 512u;
    field.Table.Reset(kEdgeFaceSamples, kEdgeFaceSamples, GameEngine::PageStreaming::kPageStoreMaxFaces, kSlots,
                      GameEngine::TerrainECS::PageTableBudget{1u << 21, 1u, 1u});
    field.SlotValues.assign(kSlots, 0.0f);
    uint32_t slot = 0;
    for (uint8_t face = 0; face < GameEngine::PageStreaming::kPageStoreMaxFaces; ++face)
        for (uint32_t level = 0; level < field.Table.Levels().size(); ++level)
        {
            const bool held = face == 0u || (face == 2u && level >= 2u) || level + 1u == field.Table.Levels().size();
            if (!held)
                continue;
            const GameEngine::PageStreaming::PageStoreLevel& shape = field.Table.Levels()[level];
            for (uint32_t z = 0; z < shape.PagesZ; ++z)
                for (uint32_t x = 0; x < shape.PagesX; ++x)
                {
                    field.Table.SetEntry(PageAddress{face, uint8_t(level), x, z},
                                         GameEngine::PageStreaming::PackPageEntry(slot, level));
                    field.SlotValues[slot++] = PageValue(face, level);
                }
        }
    return field;
}

Shader::CBTPageField FieldShape(const ConstantPageField& field)
{
    Shader::CBTPageField shape{};
    shape.Level0SamplesX = kEdgeFaceSamples;
    shape.Level0SamplesZ = kEdgeFaceSamples;
    shape.LevelCount = uint32_t(field.Table.Levels().size());
    shape.SlotsPerRow = kFieldSlotsPerRow;
    shape.FloorLevel = field.Table.FloorLevel();
    shape.TierCount = field.Table.TierCount();
    shape.DirectoryOffset = field.Table.DirectoryOffset();
    return shape;
}

// The height a root on `rootFace` resolves at `cubePoint` (lambda 0: the finest resident level).
float RootHeight(uint32_t rootFace, const std::array<int64_t, 3>& cubePoint, const Shader::CBTPageField& shape)
{
    const Shader::CBTPagePoint point =
        Shader::CBT_PlanetPagePoint(rootFace, cubePoint[0], cubePoint[1], cubePoint[2], kEdgeSubdiv, kEdgeFaceSamples);
    return Shader::CBT_PageSamplePoint(point, 0.0f, shape);
}

} // namespace

TEST(PlanetPagePoint, ACubeEdgeOrCornerGivesOneHeightFromEveryRootWhenItsFacesHoldDifferentLevels)
{
    const ConstantPageField field = MakeEdgeField();
    s_Field = &field;
    const Shader::CBTPageField shape = FieldShape(field);
    const int64_t scale = int64_t(1) << kEdgeSubdiv;
    std::mt19937_64 random(0xED6Eull);
    uint32_t compared = 0;
    for (uint32_t i = 0; i < 512u; ++i)
    {
        const int64_t along = static_cast<int64_t>(random() % static_cast<uint64_t>(2 * scale + 1)) - scale;
        // The +X / +Y edge: face 0 at level 0, face 2 at level 2.
        const std::array<int64_t, 3> xy{scale, scale, along};
        EXPECT_EQ(RootHeight(2u, xy, shape), RootHeight(0u, xy, shape)) << "+X/+Y edge, z " << along;
        EXPECT_EQ(RootHeight(0u, xy, shape), PageValue(0u, 0u)) << "the edge resolves on face 0, the lowest";
        // The -Y / +Z edge: faces 3 and 4, at their top level each.
        const std::array<int64_t, 3> yz{along, -scale, scale};
        EXPECT_EQ(RootHeight(4u, yz, shape), RootHeight(3u, yz, shape)) << "-Y/+Z edge, x " << along;
        compared += 2u;
    }
    // The +X +Y +Z corner on faces 0, 2 and 4; the -X -Y -Z corner on faces 1, 3 and 5.
    const std::array<int64_t, 3> high{scale, scale, scale};
    const std::array<int64_t, 3> low{-scale, -scale, -scale};
    EXPECT_EQ(RootHeight(2u, high, shape), RootHeight(0u, high, shape));
    EXPECT_EQ(RootHeight(4u, high, shape), RootHeight(0u, high, shape));
    EXPECT_EQ(RootHeight(3u, low, shape), RootHeight(1u, low, shape));
    EXPECT_EQ(RootHeight(5u, low, shape), RootHeight(1u, low, shape));
    EXPECT_EQ(RootHeight(5u, low, shape), PageValue(1u, shape.LevelCount - 1u)) << "the corner resolves on face 1";
    EXPECT_EQ(compared, 1024u);
    s_Field = nullptr;
}

TEST(PlanetPagePoint, AnInteriorPointIsItsFaceAndTheCpuMapTexelsAlongTheFaceAxes)
{
    const int64_t scale = int64_t(1) << kEdgeSubdiv;
    std::mt19937_64 random(0xFACEull);
    for (uint32_t face = 0; face < GameEngine::Mathematics::kCubeFaceCount; ++face)
    {
        const GameEngine::Mathematics::CubeFaceAxes& axes = GameEngine::Mathematics::kCubeFaceAxes[face];
        for (uint32_t i = 0; i < 256u; ++i)
        {
            const int64_t alongX = static_cast<int64_t>(random() % static_cast<uint64_t>(2 * scale - 1)) - (scale - 1);
            const int64_t alongZ = static_cast<int64_t>(random() % static_cast<uint64_t>(2 * scale - 1)) - (scale - 1);
            std::array<int64_t, 3> cubePoint{};
            cubePoint[axes.NormalAxis] = axes.NormalSign * scale;
            cubePoint[axes.XAxis] = axes.XSign * alongX;
            cubePoint[axes.ZAxis] = axes.ZSign * alongZ;
            const Shader::CBTPagePoint point =
                Shader::CBT_PlanetPagePoint(face, cubePoint[0], cubePoint[1], cubePoint[2], kEdgeSubdiv,
                                            kEdgeFaceSamples);
            ASSERT_EQ(point.Face, face);
            const double x = CpuTexelCoordinate(alongX, kEdgeSubdiv, kEdgeFaceSamples);
            const double z = CpuTexelCoordinate(alongZ, kEdgeSubdiv, kEdgeFaceSamples);
            ASSERT_EQ(double(point.TexelX), std::floor(x)) << "face " << face;
            ASSERT_EQ(double(point.TexelZ), std::floor(z)) << "face " << face;
            EXPECT_LE(std::abs(double(point.FractionX) - (x - std::floor(x))), kFractionTolerance);
            EXPECT_LE(std::abs(double(point.FractionZ) - (z - std::floor(z))), kFractionTolerance);
        }
    }
}

TEST(EqualAngleMap, TheFaceEdgesAndCenterAreExact)
{
    for (uint32_t samples : kFaceSamples)
        for (uint32_t numSubdiv : kSubdivisions)
        {
            const int64_t scale = int64_t(1) << numSubdiv;
            const Shader::CBTFaceTexel low = Shader::CBT_EqualAngleTexel(-scale, numSubdiv, samples);
            const Shader::CBTFaceTexel high = Shader::CBT_EqualAngleTexel(scale, numSubdiv, samples);
            const Shader::CBTFaceTexel center = Shader::CBT_EqualAngleTexel(0, numSubdiv, samples);
            EXPECT_EQ(low.Texel, 0u);
            EXPECT_EQ(low.Fraction, 0.0f);
            EXPECT_EQ(high.Texel, samples - 1u);
            EXPECT_EQ(high.Fraction, 0.0f);
            EXPECT_EQ(static_cast<double>(center.Texel) + center.Fraction, 0.5 * (samples - 1u));
        }
}
