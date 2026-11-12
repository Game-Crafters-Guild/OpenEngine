// The planet fragment's height gradient from its height pages (cbt_planet_page.glsl) against the central
// difference of the generated provider's heights. The GLSL blocks compile here as C++ (ExtractShaderBlock.cmake,
// GlslShim.h) with contraction off, so the CPU runs the fragment's fp32 operations.

#include <gtest/gtest.h>

#include "GlslShim.h"
#include "CBTTerrain/CBTPlanetShading.h"
#include "Mathematics/CubeFaces.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "PageStreaming/PageTableEntry.h"
#include "TerrainECS/PageTable.h"
#include "TerrainECS/Planet/PlanetSurfacePageProvider.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace
{

using GameEngine::PageStreaming::PageAddress;

constexpr uint32_t kFaceSamples = 1025u; // 9 level-0 pages a face side
constexpr double kRadius = 2000.0;
constexpr float kAmplitude = 30.0f;
constexpr float kFrequency = 6.0f;
constexpr uint32_t kOctaves = 4u;
constexpr uint32_t kSlotsPerRow = 16u;
constexpr uint32_t kSlots = kSlotsPerRow * kSlotsPerRow;
constexpr uint32_t kStride = GameEngine::PageStreaming::kPageStrideSamples;
constexpr uint32_t kOwned = GameEngine::PageStreaming::kPageOwnedSamples;

// The level-0 pages of a six-face field that the test reads, filled from the planet's generated provider.
struct PlanetPageField
{
    GameEngine::PageStreaming::GeneratedHeightPageProvider Provider;
    GameEngine::TerrainECS::PageTable Table;
    std::vector<float> Texels;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint32_t> Slots; // (face, level, x, z) -> slot
    const GameEngine::PageStreaming::HeightPageLayer* Overlay = nullptr; // added to each page as it is filled

    PlanetPageField()
        : Provider(MakeLattice())
    {
        Table.Reset(kFaceSamples, kFaceSamples, GameEngine::PageStreaming::kPageStoreMaxFaces, kSlots,
                    GameEngine::TerrainECS::PageTableBudget{1ull << 40, 1u, 1u});
        Texels.assign(std::size_t(kSlotsPerRow * kStride) * (kSlotsPerRow * kStride), 0.0f);
    }

    static GameEngine::PageStreaming::GeneratedPlanetLattice MakeLattice()
    {
        GameEngine::PageStreaming::GeneratedPlanetLattice lattice;
        lattice.ReferenceRadius = kRadius;
        lattice.FaceSamples = kFaceSamples;
        lattice.HeightRow = [](std::span<const GameEngine::Mathematics::Vector3> directions, float minResolvedArc,
                               std::span<float> outHeights) {
            GameEngine::CBTTerrain::PlanetReliefRow(directions, kAmplitude, kFrequency, kOctaves, minResolvedArc,
                                                    outHeights);
        };
        lattice.HeightRange = [](const GameEngine::Mathematics::Vector3&, float, float) {
            return GameEngine::PageStreaming::PageHeightRange{-2.0f * kAmplitude, 2.0f * kAmplitude};
        };
        return lattice;
    }

    // The slot of page (pageX, pageZ) of `level` on `face`, filled from the provider on first use.
    uint32_t Slot(uint32_t face, uint32_t level, uint32_t pageX, uint32_t pageZ)
    {
        const auto key = std::make_tuple(face, level, pageX, pageZ);
        if (const auto found = Slots.find(key); found != Slots.end())
            return found->second;
        const uint32_t slot = uint32_t(Slots.size());
        EXPECT_LT(slot, kSlots);
        const PageAddress address{uint8_t(face), uint8_t(level), pageX, pageZ};
        std::vector<float> samples(GameEngine::PageStreaming::kPageSampleCount);
        EXPECT_TRUE(Provider.FillPage(address, samples));
        if (Overlay != nullptr)
            Overlay->ApplyToPage(address, samples);
        for (uint32_t r = 0; r < kStride; ++r)
            for (uint32_t c = 0; c < kStride; ++c)
                Texels[std::size_t((slot / kSlotsPerRow) * kStride + r) * (kSlotsPerRow * kStride) +
                       (slot % kSlotsPerRow) * kStride + c] = samples[r * kStride + c];
        Table.SetEntry(address, GameEngine::PageStreaming::PackPageEntry(slot, 0u));
        Slots.emplace(key, slot);
        return slot;
    }

    // The cache texel of level-0 sample (x, z) of `face`: its page's owned sample, filled from the provider.
    float& Height(uint32_t face, uint32_t x, uint32_t z)
    {
        const uint32_t slot = Slot(face, 0u, std::min(x / kOwned, 8u), std::min(z / kOwned, 8u));
        const uint32_t column = x - std::min(x / kOwned, 8u) * kOwned + 1u;
        const uint32_t row = z - std::min(z / kOwned, 8u) * kOwned + 1u;
        return Texels[std::size_t((slot / kSlotsPerRow) * kStride + row) * (kSlotsPerRow * kStride) +
                      (slot % kSlotsPerRow) * kStride + column];
    }

    // Every page of every level from 1 up on `face`.
    void FillCoarseLevels(uint32_t face)
    {
        for (uint32_t level = 1u; level < Table.Levels().size(); ++level)
            for (uint32_t z = 0; z < Table.Levels()[level].PagesZ; ++z)
                for (uint32_t x = 0; x < Table.Levels()[level].PagesX; ++x)
                    Slot(face, level, x, z);
    }
};
PlanetPageField* s_Field = nullptr;
uint32_t s_TableWordReads = 0;
uint32_t s_FadingSlotModulus = 0; // 0: every slot settled; else a slot divisible by it is half faded in

namespace Shader
{
#define precise
using GameEngine::GlslShim::abs;
using GameEngine::GlslShim::clamp;
using GameEngine::GlslShim::cross;
using GameEngine::GlslShim::dot;
using GameEngine::GlslShim::exp2;
using GameEngine::GlslShim::floor;
using GameEngine::GlslShim::max;
using GameEngine::GlslShim::min;
using GameEngine::GlslShim::normalize;
using GameEngine::GlslShim::sqrt;
using GameEngine::GlslShim::uint;
using GameEngine::GlslShim::vec3;
#include "PageResolveTypesExtracted.h"

uint CBT_PageTableWord(uint index)
{
    ++s_TableWordReads;
    return s_Field->Table.Words()[index];
}

float CBT_PageSlotFade(uint slot)
{
    return s_FadingSlotModulus != 0u && slot % s_FadingSlotModulus == 0u ? 0.5f : 1.0f;
}

void CBT_PageUnresolved()
{
    ADD_FAILURE() << "a tap found no resident page";
}

CBTPageLevel CBT_PageLevelShape(uint level)
{
    const GameEngine::PageStreaming::PageStoreLevel& shape = s_Field->Table.Levels()[level];
    return CBTPageLevel{s_Field->Table.LevelOffsets()[level], shape.PagesX, shape.PagesZ, 0u};
}

// The cache texel at (x, z), clamped to the cache's edge.
float CacheTexelClamped(float x, float z)
{
    const uint32_t dim = kSlotsPerRow * kStride;
    const uint32_t cx = uint32_t(std::clamp(x, 0.0f, float(dim - 1u)));
    const uint32_t cz = uint32_t(std::clamp(z, 0.0f, float(dim - 1u)));
    return s_Field->Texels[std::size_t(cz) * dim + cx];
}

// The linear-clamp tap at absolute cache texel coordinates (an integer is a texel's center).
float CBT_PageCacheBilinear(float texelX, float texelZ)
{
    const float x0 = std::floor(texelX);
    const float z0 = std::floor(texelZ);
    const float fx = texelX - x0;
    const float fz = texelZ - z0;
    const float t00 = CacheTexelClamped(x0, z0);
    const float t10 = CacheTexelClamped(x0 + 1.0f, z0);
    const float t01 = CacheTexelClamped(x0, z0 + 1.0f);
    const float t11 = CacheTexelClamped(x0 + 1.0f, z0 + 1.0f);
    const float top = t00 + (t10 - t00) * fx;
    const float bottom = t01 + (t11 - t01) * fx;
    return top + (bottom - top) * fz;
}

#include "PageResolveExtracted.h"
#include "PlanetPageExtracted.h"
#undef precise
} // namespace Shader

Shader::CBTPageField FieldShape(const PlanetPageField& field)
{
    Shader::CBTPageField shape{};
    shape.Level0SamplesX = kFaceSamples;
    shape.Level0SamplesZ = kFaceSamples;
    shape.LevelCount = uint32_t(field.Table.Levels().size());
    shape.SlotsPerRow = kSlotsPerRow;
    shape.FloorLevel = field.Table.FloorLevel();
    shape.TierCount = field.Table.TierCount();
    shape.DirectoryOffset = field.Table.DirectoryOffset();
    return shape;
}

// The page point of level-0 lattice coordinates (x, z) on `face`.
Shader::CBTPagePoint FacePoint(uint32_t face, float x, float z)
{
    return Shader::CBT_PlanetLatticePoint(face, 0, x, 0, z, kFaceSamples);
}

// float64 for the oracle: a tangent differences two unit directions 0.003 apart.
struct Double3
{
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
};

// The unit direction of level-0 sample (x, z) of `face` in float64 (EqualAngleSampleDirection, before its
// fp32 result).
Double3 Direction(uint32_t face, uint32_t x, uint32_t z)
{
    using namespace GameEngine::PageStreaming;
    const GameEngine::Mathematics::CubeFaceAxes& axes = GameEngine::Mathematics::kCubeFaceAxes[face];
    double v[3] = {};
    v[axes.NormalAxis] = axes.NormalSign;
    v[axes.XAxis] = axes.XSign * GnomonicFaceCoordinate(EqualAngleSampleCoordinate(x, kFaceSamples));
    v[axes.ZAxis] = axes.ZSign * GnomonicFaceCoordinate(EqualAngleSampleCoordinate(z, kFaceSamples));
    const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return {v[0] / length, v[1] / length, v[2] / length};
}

Double3 Cross(const Double3& a, const Double3& b)
{
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}

Double3 Normalized(const Double3& v)
{
    const double length = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
    return {v.X / length, v.Y / length, v.Z / length};
}

// One axis of the surface's central-difference tangent at lattice sample (x, z): (R + h) (d+ - d-) + (h+ - h-) d.
Double3 CentralTangent(PlanetPageField& field, uint32_t face, uint32_t x, uint32_t z, bool alongZ)
{
    const uint32_t plusX = alongZ ? x : x + 1u;
    const uint32_t minusX = alongZ ? x : x - 1u;
    const uint32_t plusZ = alongZ ? z + 1u : z;
    const uint32_t minusZ = alongZ ? z - 1u : z;
    const Double3 plus = Direction(face, plusX, plusZ);
    const Double3 minus = Direction(face, minusX, minusZ);
    const Double3 center = Direction(face, x, z);
    const double base = kRadius + double(field.Height(face, x, z));
    const double rise = double(field.Height(face, plusX, plusZ)) - double(field.Height(face, minusX, minusZ));
    return {base * (plus.X - minus.X) + rise * center.X, base * (plus.Y - minus.Y) + rise * center.Y,
            base * (plus.Z - minus.Z) + rise * center.Z};
}

// The unit normal of the two central-difference tangents at lattice sample (x, z), on the side of its direction.
Double3 CentralNormal(PlanetPageField& field, uint32_t face, uint32_t x, uint32_t z)
{
    const Double3 d = Direction(face, x, z);
    const Double3 n =
        Normalized(Cross(CentralTangent(field, face, x, z, true), CentralTangent(field, face, x, z, false)));
    return n.X * d.X + n.Y * d.Y + n.Z * d.Z < 0.0 ? Double3{-n.X, -n.Y, -n.Z} : n;
}

double Distance(const Double3& a, const Double3& b)
{
    return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
}

// The anchor of `face` for the eye at `eye`, the lattice point of its projection split in float64.
std::tuple<int, int, float, float> Anchor(uint32_t face, const Shader::vec3& eye)
{
    const GameEngine::Mathematics::CubeFaceAxes& axes = GameEngine::Mathematics::kCubeFaceAxes[face];
    const double e[3] = {eye.x, eye.y, eye.z};
    const double normal = axes.NormalSign * e[axes.NormalAxis];
    const double half = 0.5 * double(kFaceSamples - 1u);
    using GameEngine::PageStreaming::EqualAngleFaceCoordinate;
    const double x = (EqualAngleFaceCoordinate(axes.XSign * e[axes.XAxis] / normal) + 1.0) * half;
    const double z = (EqualAngleFaceCoordinate(axes.ZSign * e[axes.ZAxis] / normal) + 1.0) * half;
    return {int(std::floor(x)), int(std::floor(z)), float(x - std::floor(x)), float(z - std::floor(z))};
}

// The bound on |GPU normal - central-difference normal|: the fragment's fp32 operations, ten times the worst
// seen on this sweep.
constexpr double kNormalTolerance = 1e-5;
// The bound on |surface page normal - fragment normal|: the CPU twin builds the normal from fp32 sample
// directions; 1/80 of the Rg8 encode step (1/127) the normal plane stores.
constexpr double kSurfaceNormalTolerance = 1e-4;

} // namespace

TEST(PlanetPageNormal, TheFragmentNormalAtALatticePointIsTheCentralDifferenceOfTheProviderHeights)
{
    PlanetPageField field;
    s_Field = &field;
    std::mt19937 random(0x0A11u);
    std::uniform_int_distribution<uint32_t> interior(1u, kFaceSamples - 2u);
    double worst[2] = {0.0, 0.0}; // no anchor, anchored
    double worstRelief = 0.0;
    uint32_t compared = 0;
    for (uint32_t face = 0; face < GameEngine::Mathematics::kCubeFaceCount; ++face)
        for (uint32_t i = 0; i < 12u; ++i)
        {
            const uint32_t x = interior(random);
            const uint32_t z = interior(random);
            // The pages of the five taps, resident before the fragment reads them.
            field.Height(face, x - 1u, z);
            field.Height(face, x + 1u, z);
            field.Height(face, x, z - 1u);
            field.Height(face, x, z + 1u);
            const Shader::CBTPageField shape = FieldShape(field);

            const Double3 d = Direction(face, x, z);
            const double r = kRadius + double(field.Height(face, x, z));
            const Double3 tangentX = CentralTangent(field, face, x, z, false);
            const Double3 tangentZ = CentralTangent(field, face, x, z, true);
            Double3 expected = Normalized(Cross(tangentZ, tangentX));
            if (expected.X * d.X + expected.Y * d.Y + expected.Z * d.Z < 0.0)
                expected = {-expected.X, -expected.Y, -expected.Z};

            // The eye 300 m above and 150 m along the surface; every other sample has no anchor.
            const Double3 position{d.X * r, d.Y * r, d.Z * r};
            const Double3 side = Normalized(Cross(d, Double3{0.3, 0.5, 0.8}));
            const Double3 eye{position.X + 300.0 * d.X + 150.0 * side.X, position.Y + 300.0 * d.Y + 150.0 * side.Y,
                              position.Z + 300.0 * d.Z + 150.0 * side.Z};
            const Shader::vec3 eyeF(float(eye.X), float(eye.Y), float(eye.Z));
            const Shader::vec3 positionF(float(position.X), float(position.Y), float(position.Z));
            const Shader::vec3 fromEyeF(float(position.X - double(eyeF.x)), float(position.Y - double(eyeF.y)),
                                        float(position.Z - double(eyeF.z)));
            const bool anchored = (i % 2u) == 0u;
            const auto [anchorX, anchorZ, fractionX, fractionZ] = Anchor(face, eyeF);
            const Shader::CBTPlanetPageSurface surface = Shader::CBT_PlanetPageSurfaceAt(
                positionF, fromEyeF, eyeF, anchored ? anchorX : Shader::CBT_PLANET_PAGE_NO_ANCHOR, anchorZ, fractionX,
                fractionZ, 0.0f, shape);

            const Shader::vec3 dir = Shader::normalize(positionF);
            const Shader::vec3 normal =
                Shader::normalize(dir - surface.Gradient * (1.0f / (float(kRadius) + surface.Height)));
            const double error = std::sqrt((normal.x - expected.X) * (normal.x - expected.X) +
                                           (normal.y - expected.Y) * (normal.y - expected.Y) +
                                           (normal.z - expected.Z) * (normal.z - expected.Z));
            EXPECT_LE(error, kNormalTolerance) << "face " << face << " texel (" << x << ", " << z << ")"
                                               << (anchored ? " anchored" : " no anchor");
            EXPECT_NEAR(surface.Height, field.Height(face, x, z), 1e-3f) << "face " << face;
            worst[anchored ? 1 : 0] = std::max(worst[anchored ? 1 : 0], error);
            worstRelief = std::max(worstRelief, 1.0 - (expected.X * d.X + expected.Y * d.Y + expected.Z * d.Z));
            ++compared;
        }
    EXPECT_EQ(compared, 72u);
    // The relief tilts the normal: the comparison is not of two radial normals.
    EXPECT_GT(worstRelief, 1e-3);
    RecordProperty("WorstNormalErrorNoAnchor", std::to_string(worst[0]));
    RecordProperty("WorstNormalErrorAnchored", std::to_string(worst[1]));
    s_Field = nullptr;
}

// The surface page's normal (PlanetWindowNormal, the CPU twin) against the fragment's normal from the
// same heights on a sweep of lattice points on six faces (the planets note's risk 4).
TEST(PlanetPageNormal, TheSurfacePageNormalIsTheFragmentNormalOnASweep)
{
    using namespace GameEngine::PageStreaming;
    PlanetPageField field;
    s_Field = &field;
    const GameEngine::TerrainECS::PlanetSurfacePageProvider surface(
        std::make_shared<const GeneratedHeightPageProvider>(PlanetPageField::MakeLattice()), {},
        GameEngine::Components::TerrainSurfaceRulesEffect{}, kRadius);
    std::mt19937 random(0x51A9u);
    std::uniform_int_distribution<uint32_t> interior(1u, kFaceSamples - 2u);
    GameEngine::TerrainECS::PlanetSurfaceWindow window;
    double worst = 0.0;
    uint32_t compared = 0;
    for (uint32_t face = 0; face < GameEngine::Mathematics::kCubeFaceCount; ++face)
        for (uint32_t i = 0; i < 12u; ++i)
        {
            const uint32_t x = interior(random);
            const uint32_t z = interior(random);
            field.Height(face, x - 1u, z);
            field.Height(face, x + 1u, z);
            field.Height(face, x, z - 1u);
            field.Height(face, x, z + 1u);
            const Shader::CBTPageField shape = FieldShape(field);
            const Double3 d = Direction(face, x, z);
            const double r = kRadius + double(field.Height(face, x, z));
            const Shader::vec3 positionF(float(d.X * r), float(d.Y * r), float(d.Z * r));
            const Shader::CBTPlanetPageSurface paged = Shader::CBT_PlanetPageSurfaceAt(
                positionF, positionF, Shader::vec3(0.0f, 0.0f, 0.0f), Shader::CBT_PLANET_PAGE_NO_ANCHOR, 0, 0.0f, 0.0f,
                0.0f, shape);
            const Shader::vec3 dir = Shader::normalize(positionF);
            const Shader::vec3 gpu = Shader::normalize(dir - paged.Gradient * (1.0f / (float(kRadius) + paged.Height)));

            const uint32_t pageX = std::min(x / kOwned, 8u);
            const uint32_t pageZ = std::min(z / kOwned, 8u);
            ASSERT_TRUE(surface.FillWindow(PageAddress{uint8_t(face), 0u, pageX, pageZ}, window));
            const GameEngine::Mathematics::Vector3 cpu = GameEngine::TerrainECS::PlanetWindowNormal(
                window, x - pageX * kOwned + kPageSurfaceWindowMarginSamples,
                z - pageZ * kOwned + kPageSurfaceWindowMarginSamples, kRadius);
            const double error = std::sqrt(double(cpu.x - gpu.x) * double(cpu.x - gpu.x) +
                                           double(cpu.y - gpu.y) * double(cpu.y - gpu.y) +
                                           double(cpu.z - gpu.z) * double(cpu.z - gpu.z));
            EXPECT_LE(error, kSurfaceNormalTolerance) << "face " << face << " texel (" << x << ", " << z << ")";
            worst = std::max(worst, error);
            ++compared;
        }
    EXPECT_EQ(compared, 72u);
    RecordProperty("WorstSurfaceNormalError", std::to_string(worst));
    s_Field = nullptr;
}

// The fragment decodes a surface page's normal in the frame the provider encodes it in: x along the face's
// X axis made normal to the direction, z = x cross the direction, the rest along the direction.
TEST(PlanetPageNormal, TheFragmentDecodesASurfacePageNormalInTheProvidersFaceFrame)
{
    using GameEngine::Mathematics::Vector3;
    std::mt19937 random(0x7E57u);
    std::uniform_int_distribution<uint32_t> interior(1u, kFaceSamples - 2u);
    std::uniform_real_distribution<float> tilt(-0.4f, 0.4f);
    double worst = 0.0;
    for (uint32_t face = 0; face < GameEngine::Mathematics::kCubeFaceCount; ++face)
        for (uint32_t i = 0; i < 16u; ++i)
        {
            const Double3 d = Direction(face, interior(random), interior(random));
            const Vector3 direction(float(d.X), float(d.Y), float(d.Z));
            const GameEngine::Mathematics::CubeFaceAxes& axes = GameEngine::Mathematics::kCubeFaceAxes[face];
            Vector3 faceX(0.0f, 0.0f, 0.0f);
            faceX[axes.XAxis] = float(axes.XSign);
            const Vector3 tangentX = (faceX - direction * Vector3::Dot(faceX, direction)).Normalize();
            const Vector3 tangentZ = Vector3::Cross(tangentX, direction);
            const Vector3 world = (direction + tangentX * tilt(random) + tangentZ * tilt(random)).Normalize();
            const Shader::vec3 at(direction.x, direction.y, direction.z);
            const Shader::vec3 decoded = Shader::CBT_PlanetSurfaceNormal(
                Vector3::Dot(world, tangentX), Vector3::Dot(world, tangentZ), at, Shader::CBT_PlanetFaceOf(at));
            const double error = std::sqrt(double(decoded.x - world.x) * double(decoded.x - world.x) +
                                           double(decoded.y - world.y) * double(decoded.y - world.y) +
                                           double(decoded.z - world.z) * double(decoded.z - world.z));
            EXPECT_LE(error, 1e-5) << "face " << face;
            worst = std::max(worst, error);
        }
    RecordProperty("WorstDecodeError", std::to_string(worst));
}

// Inside one page the fragment walks the table once: it reads as many table words as one CBT_PageSamplePoint.
TEST(PlanetPageNormal, TheFragmentWalksTheTableOnceInsideAPage)
{
    PlanetPageField field;
    s_Field = &field;
    constexpr uint32_t kFace = 0u;
    constexpr uint32_t kTexel = 4u * kOwned + kOwned / 2u; // the middle of level-0 page 4
    field.Height(kFace, kTexel, kTexel);
    const Shader::CBTPageField shape = FieldShape(field);
    const Double3 d = Direction(kFace, kTexel, kTexel);
    const double r = kRadius + double(field.Height(kFace, kTexel, kTexel));
    const Shader::vec3 position(float(d.X * r), float(d.Y * r), float(d.Z * r));
    const Shader::vec3 eye = position * 1.1f;

    s_TableWordReads = 0;
    const Shader::CBTPlanetPageSurface surface = Shader::CBT_PlanetPageSurfaceAt(
        position, position - eye, eye, Shader::CBT_PLANET_PAGE_NO_ANCHOR, 0, 0.0f, 0.0f, 0.0f, shape);
    const uint32_t fragmentReads = s_TableWordReads;
    s_TableWordReads = 0;
    const float centerHeight = Shader::CBT_PageSamplePoint(FacePoint(kFace, float(kTexel), float(kTexel)), 0.0f, shape);
    const uint32_t oneWalkReads = s_TableWordReads;

    EXPECT_GT(oneWalkReads, 0u);
    EXPECT_EQ(fragmentReads, oneWalkReads) << "the four taps take the center's walk";
    EXPECT_NEAR(surface.Height, centerHeight, 1e-3f);
    s_Field = nullptr;
}

// A +X tap texel 5 m above the provider's height: the fragment normal is the central difference of the page's
// texels, so it tilts away from the bump by the tilt of that difference.
TEST(PlanetPageNormal, TheFragmentNormalFollowsAPageThatDiffersFromTheProvider)
{
    PlanetPageField field;
    s_Field = &field;
    constexpr uint32_t kFace = 0u;
    constexpr uint32_t kTexel = 4u * kOwned + kOwned / 2u; // inside level-0 page 4: the tap has no apron copy
    const Double3 d = Direction(kFace, kTexel, kTexel);
    const double r = kRadius + double(field.Height(kFace, kTexel, kTexel));
    const Shader::vec3 position(float(d.X * r), float(d.Y * r), float(d.Z * r));
    const Shader::vec3 eye = position * 1.1f;
    Double3 normals[2]; // provider texels, bumped texels
    Double3 expected[2];
    for (uint32_t bumped = 0; bumped < 2u; ++bumped)
    {
        field.Height(kFace, kTexel + 1u, kTexel) += bumped != 0u ? 5.0f : 0.0f;
        const Shader::CBTPlanetPageSurface surface = Shader::CBT_PlanetPageSurfaceAt(
            position, position - eye, eye, Shader::CBT_PLANET_PAGE_NO_ANCHOR, 0, 0.0f, 0.0f, 0.0f, FieldShape(field));
        const Shader::vec3 normal = Shader::normalize(Shader::normalize(position) -
                                                      surface.Gradient * (1.0f / (float(kRadius) + surface.Height)));
        normals[bumped] = {normal.x, normal.y, normal.z};
        expected[bumped] = CentralNormal(field, kFace, kTexel, kTexel);
        EXPECT_LE(Distance(normals[bumped], expected[bumped]), kNormalTolerance)
            << (bumped != 0u ? "bumped" : "provider");
    }
    const double tilt = Distance(expected[0], expected[1]);
    EXPECT_GT(tilt, 0.1) << "the bump tilts the central difference";
    EXPECT_NEAR(Distance(normals[0], normals[1]), tilt, 2.0 * kNormalTolerance);
    // Away from the bump: the normal moves against the +X tangent.
    const Double3 tangentX = CentralTangent(field, kFace, kTexel, kTexel, false);
    EXPECT_LT((normals[1].X - normals[0].X) * tangentX.X + (normals[1].Y - normals[0].Y) * tangentX.Y +
                  (normals[1].Z - normals[0].Z) * tangentX.Z,
              0.0);
    s_Field = nullptr;
}

// A height-page overlay with a 5 m bump at the +X tap of one face's texel: the page holds the provider's height
// plus the bump there, and the fragment normal tilts from the provider's normal by the bump's slope.
TEST(PlanetPageNormal, TheFragmentNormalFollowsAnOverlayBumpOnItsFace)
{
    constexpr uint32_t kFace = 2u;
    constexpr uint32_t kTexel = 4u * kOwned + kOwned / 2u;
    const GameEngine::PageStreaming::PageSampleRect bump{kTexel + 1u, kTexel, kTexel + 1u, kTexel};
    GameEngine::PageStreaming::HeightPageLayer overlay(
        GameEngine::PageStreaming::kPageStoreMaxFaces,
        GameEngine::PageStreaming::BuildPageStoreLevels(kFaceSamples, kFaceSamples));
    const float raise = 5.0f;
    overlay.WriteLevel0(kFace, bump, std::span(&raise, 1));
    PlanetPageField formula;
    PlanetPageField overlaid;
    overlaid.Overlay = &overlay;
    EXPECT_FLOAT_EQ(overlaid.Height(kFace, kTexel + 1u, kTexel), formula.Height(kFace, kTexel + 1u, kTexel) + 5.0f);
    EXPECT_FLOAT_EQ(overlaid.Height(0u, kTexel + 1u, kTexel), formula.Height(0u, kTexel + 1u, kTexel))
        << "another face's page at the same place";

    const Double3 d = Direction(kFace, kTexel, kTexel);
    const double r = kRadius + double(formula.Height(kFace, kTexel, kTexel));
    const Shader::vec3 position(float(d.X * r), float(d.Y * r), float(d.Z * r));
    const Shader::vec3 eye = position * 1.1f;
    Double3 normals[2]; // formula, overlaid
    PlanetPageField* fields[2] = {&formula, &overlaid};
    for (uint32_t i = 0; i < 2u; ++i)
    {
        s_Field = fields[i];
        const Shader::CBTPlanetPageSurface surface = Shader::CBT_PlanetPageSurfaceAt(
            position, position - eye, eye, Shader::CBT_PLANET_PAGE_NO_ANCHOR, 0, 0.0f, 0.0f, 0.0f,
            FieldShape(*fields[i]));
        const Shader::vec3 normal = Shader::normalize(Shader::normalize(position) -
                                                      surface.Gradient * (1.0f / (float(kRadius) + surface.Height)));
        normals[i] = {normal.x, normal.y, normal.z};
        EXPECT_LE(Distance(normals[i], CentralNormal(*fields[i], kFace, kTexel, kTexel)), kNormalTolerance) << i;
    }
    const double slopeTilt =
        Distance(CentralNormal(formula, kFace, kTexel, kTexel), CentralNormal(overlaid, kFace, kTexel, kTexel));
    EXPECT_GT(slopeTilt, 0.1) << "the bump's slope tilts the central difference";
    EXPECT_NEAR(Distance(normals[0], normals[1]), slopeTilt, 2.0 * kNormalTolerance);
    s_Field = nullptr;
}

// The five points of a central difference across level-0 page edges, over a level's blend quarter, with slots
// still fading in: each point of CBT_PageSampleCross is its own CBT_PageSamplePoint, bit for bit.
TEST(PlanetPageNormal, TheSharedWalkGivesEachTapItsOwnResolve)
{
    PlanetPageField field;
    s_Field = &field;
    s_FadingSlotModulus = 3u;
    constexpr uint32_t kFace = 2u;
    field.FillCoarseLevels(kFace);
    std::mt19937 random(0x5EEDu);
    std::uniform_int_distribution<uint32_t> page(1u, 7u);
    std::uniform_int_distribution<int> nearEdge(-3, 3);
    std::uniform_real_distribution<float> fraction(0.0f, 1.0f);
    std::uniform_real_distribution<float> level(0.0f, 1.99f);
    uint32_t sharedTaps = 0;
    uint32_t ownWalkTaps = 0;
    for (uint32_t i = 0; i < 400u; ++i)
    {
        const float x = float(int(page(random) * kOwned) + nearEdge(random)) + fraction(random);
        const float z = float(int(page(random) * kOwned) + nearEdge(random)) + fraction(random);
        const float lambda = level(random);
        const float step = Shader::exp2(lambda);
        Shader::CBTPageCross points{};
        points.Center = FacePoint(kFace, x, z);
        points.PlusX = FacePoint(kFace, x + step, z);
        points.MinusX = FacePoint(kFace, x - step, z);
        points.PlusZ = FacePoint(kFace, x, z + step);
        points.MinusZ = FacePoint(kFace, x, z - step);
        for (const Shader::CBTPagePoint& point :
             {points.Center, points.PlusX, points.MinusX, points.PlusZ, points.MinusZ})
            field.Height(kFace, point.TexelX, point.TexelZ);
        const Shader::CBTPageField shape = FieldShape(field);

        const Shader::CBTPageCrossSample cross = Shader::CBT_PageSampleCross(points, lambda, shape);
        EXPECT_EQ(cross.Center, Shader::CBT_PageSamplePoint(points.Center, lambda, shape)) << "point " << i;
        EXPECT_EQ(cross.PlusX, Shader::CBT_PageSamplePoint(points.PlusX, lambda, shape)) << "point " << i;
        EXPECT_EQ(cross.MinusX, Shader::CBT_PageSamplePoint(points.MinusX, lambda, shape)) << "point " << i;
        EXPECT_EQ(cross.PlusZ, Shader::CBT_PageSamplePoint(points.PlusZ, lambda, shape)) << "point " << i;
        EXPECT_EQ(cross.MinusZ, Shader::CBT_PageSamplePoint(points.MinusZ, lambda, shape)) << "point " << i;
        const uint32_t base = uint32_t(lambda);
        for (const Shader::CBTPagePoint& tap : {points.PlusX, points.MinusX, points.PlusZ, points.MinusZ})
        {
            if (Shader::CBT_PageSharesWalk(points.Center, tap, base))
                ++sharedTaps;
            else
                ++ownWalkTaps;
        }
    }
    // Both paths ran: taps inside the center's page and taps past its edge.
    EXPECT_GT(sharedTaps, 100u);
    EXPECT_GT(ownWalkTaps, 100u);
    s_FadingSlotModulus = 0u;
    s_Field = nullptr;
}

// On a face of the P-600 size (2^21 samples a side, R = 600 km), a fragment 500 m from the eye takes its lattice
// coordinate from the face's anchor within 1e-3 texel; the fp32 coordinate of its gnomonic coordinate does not.
TEST(PlanetPageNormal, TheAnchorKeepsTheLatticeCoordinatePreciseOnALargePlanet)
{
    constexpr uint32_t kLargeSamples = 2094397u;
    constexpr double kLargeRadius = 600000.0;
    const double half = 0.5 * double(kLargeSamples - 1u);
    std::mt19937 random(0x600Du);
    std::uniform_real_distribution<double> along(-0.9, 0.9);
    std::uniform_real_distribution<double> step(-350.0, 350.0);
    double worstAnchored = 0.0;
    double worstDirect = 0.0;
    for (uint32_t i = 0; i < 256u; ++i)
    {
        // Face 0 (+X): X along world Y, Z along world Z. The eye 200 m above the surface.
        const Double3 eyeDirection = Normalized(Double3{1.0, along(random), along(random)});
        const Shader::vec3 eye(float(eyeDirection.X * (kLargeRadius + 200.0)),
                               float(eyeDirection.Y * (kLargeRadius + 200.0)),
                               float(eyeDirection.Z * (kLargeRadius + 200.0)));
        const Double3 position{double(eye.x) - 200.0 * eyeDirection.X,
                               double(eye.y) - 200.0 * eyeDirection.Y + step(random),
                               double(eye.z) - 200.0 * eyeDirection.Z + step(random)};
        using GameEngine::PageStreaming::EqualAngleFaceCoordinate;
        const double truth = (EqualAngleFaceCoordinate(position.Y / position.X) + 1.0) * half;
        const double eyeLattice = (EqualAngleFaceCoordinate(double(eye.y) / double(eye.x)) + 1.0) * half;
        const double anchorTexel = std::floor(eyeLattice);
        const float offset = Shader::CBT_PlanetAnchorOffset(
            eye.y, eye.x, float(position.Y - double(eye.y)), float(position.X - double(eye.x)),
            float(eyeLattice - anchorTexel), float(half));
        worstAnchored = std::max(worstAnchored, std::abs(anchorTexel + double(offset) - truth));
        const float direct = Shader::CBT_PlanetLatticeCoordinate(float(position.Y) / float(position.X), float(half));
        worstDirect = std::max(worstDirect, std::abs(double(direct) - truth));
    }
    EXPECT_LE(worstAnchored, 1e-3);
    EXPECT_GT(worstDirect, 10.0 * worstAnchored) << "the anchor is what keeps the coordinate precise";
    RecordProperty("WorstAnchoredTexelError", std::to_string(worstAnchored));
    RecordProperty("WorstDirectTexelError", std::to_string(worstDirect));
}
