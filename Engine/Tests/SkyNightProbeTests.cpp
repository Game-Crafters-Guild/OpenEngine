// The night sky's shipped GLSL, evaluated on the device (Engine/Tests/Shaders/sky_night_probe.comp,
// which calls Includes/moon_disc.glsl and Includes/sky_composite.glsl and composes nothing itself):
//
//   1. The moon disc. The disc is a magnified image of the real moon: its surface has the real moon's
//      radiance at any drawn size (the illuminance over the real moon's solid angle), so the art's moon
//      drawn at the real moon's size gives the moonlight back. It is lit on the side that faces the
//      sun, over the share of its face the phase angle says, and its night side shows earthshine from
//      the Earth's phase, neutral to bluish. The probe samples the real moon texture, so the
//      normalisation and the lit share are checked against the art that ships (the art's moon covers
//      0.889 of the drawn radius).
//   2. The sky under the disc. The air's in-scatter lies in front of the moon, so the disc does not
//      hide it; where the disc draws, the sky is the mean of four taps on its rim, weighted by the
//      disc's cover and the view ray's transmittance. The disc hides the falling stars behind it.
//   3. The night sky's own glow. GE_ApplySkyGroundAndNight adds airglow and starlight to the LUT's
//      sky, at about 1.7e-4 cd/m2 at the zenith (V 22 mag per square arcsecond) and three times that
//      at the horizon, rather than replacing the sky with an authored night.
//
// A CPU copy of these formulas would only prove that it agrees with itself. Skips only without a
// Vulkan device; a missing staged probe or moon texture fails.

#include <gtest/gtest.h>

#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Platform/Shell.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Sky/SkyRenderer.h"
#include "Rendering/Sky/SkySystemConfig.h"

#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numbers>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Mirrors sky_night_probe.comp's ProbePC.
struct ProbePC
{
    float modeCountRadiusIlluminance[4];
    float trimNight[4];
    float sunDir[4];
    float moonDir[4];
    float skyColor[4];
    float nightSkyHorizonColor[4];
};

constexpr uint32_t kDiscMode = 0u;
constexpr uint32_t kGlowMode = 1u;
constexpr uint32_t kRimMode = 2u;
// Disc samples per side: 512 x 512 over the drawn disc, about half the texture's resolution.
constexpr uint32_t kDiscGrid = 512u;
// Glow directions from the horizon (0) to the zenith (kGlowDirections - 1), one per degree.
constexpr uint32_t kGlowDirections = 91u;
// Mode 2's fixed table: the four rim taps, three values of the sky under the disk and the stars' cover.
constexpr uint32_t kRimTableSlots = 8u;
// The real moon's angular radius (moon_disc.glsl's GE_MOON_ANGULAR_RADIUS: 0.2595 degrees), and the
// drawn radius at which the art's moon (GE_MOON_TEXTURE_RADIUS of the drawn disc) has that size.
constexpr double kRealMoonRadius = 0.004529;
constexpr double kRealSizeDrawnRadius = kRealMoonRadius / 0.889;
// The default drawn moon radius: SkyEnvironmentSystem's 0.026 rad times the default MoonSize.
const float kDefaultMoonRadius = 0.026f * Components::SkyEnvironment{}.MoonSize;
// Earthshine at a full Earth (a new moon) as a fraction of the lit side's radiance
// (GE_MOON_EARTHSHINE_AT_FULL_EARTH); the Earth's lit fraction scales it.
constexpr double kEarthshineAtFullEarth = 1.1e-4;

using Vec3 = std::array<double, 3>;
using Rgb = Vec3;
using Rgba = std::array<double, 4>;

double Luminance(const Rgb& c)
{
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

double Dot(const Vec3& a, const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 Normalized(const Vec3& v)
{
    const double length = std::sqrt(Dot(v, v));
    return {v[0] / length, v[1] / length, v[2] / length};
}

// A direction `angle` radians from `from` toward `toward` (any direction not parallel to `from`).
Vec3 RotatedToward(const Vec3& from, const Vec3& toward, double angle)
{
    const double along = Dot(toward, from);
    const Vec3 tangent = Normalized({toward[0] - along * from[0], toward[1] - along * from[1], toward[2] - along * from[2]});
    return {from[0] * std::cos(angle) + tangent[0] * std::sin(angle),
            from[1] * std::cos(angle) + tangent[1] * std::sin(angle),
            from[2] * std::cos(angle) + tangent[2] * std::sin(angle)};
}

// The moon used by the disc tests: 35 degrees up in the south-east.
const Vec3 kMoon = Normalized({0.5, 0.57, -0.65});

// What the drawn disc gives for one sun: its radiance summed over its drawn solid angle, the phase
// mask summed over the art's moon (alpha-weighted, in texels), the lit texels' centroid as a direction
// on the sky (unit, in the tangent plane at the moon), and every texel for pairwise comparisons.
struct DiscSums
{
    Rgb Moonlight{};
    double LitTexels = 0.0;
    Vec3 LitCentroidOnSky{};
    std::vector<Rgba> Texels;
};

std::filesystem::path FindProbeSpv()
{
    namespace fs = std::filesystem;
    const fs::path exe = GameEngine::Platform::GetExecutablePath();
    fs::path dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int up = 0; up < 6; ++up)
    {
        const fs::path candidate = dir / "Shaders" / "sky_night_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

class SkyNightProbeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty()) << "sky_night_probe.comp.spv was not found above the test executable ("
                                      << GameEngine::Platform::GetExecutablePath().string()
                                      << "): the shader build stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty()) << "staged probe SPIR-V is empty: " << spvPath.string();

        // The moon texture through the sky's own loader, from the staged engine assets.
        SkyRenderer::Config config{};
        config.installAssetsRoot = GameEngine::TestPaths::StagedRoot() / "Assets";
        ASSERT_TRUE(std::filesystem::exists(config.installAssetsRoot / kSkyMoonFullTextureRelativePath))
            << "the staged moon texture is missing under " << config.installAssetsRoot.string()
            << ": build StageTestAssets";
        m_Sky = std::make_unique<SkyRenderer>();
        ASSERT_TRUE(m_Sky->Initialize(m_Device.get(), config));
        m_MoonTexture = m_Sky->EnsureMoonFullTexture();
        ASSERT_TRUE(m_MoonTexture.IsValid());

        DescriptorBinding texture{};
        texture.binding = 0;
        texture.type = DescriptorType::CombinedImageSampler;
        texture.count = 1;
        texture.shaderStages = kShaderStageCompute;
        DescriptorBinding out{};
        out.binding = 1;
        out.type = DescriptorType::StorageBuffer;
        out.count = 1;
        out.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "SkyNightProbe.Set0";
        m_Layout.bindings = {texture, out};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(ProbePC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "SkyNightProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        m_Sky.reset();
        m_Device->Shutdown();
    }

    std::vector<Rgba> Run(const ProbePC& pc, uint32_t slots)
    {
        const size_t bytes = static_cast<size_t>(slots) * 4u * sizeof(float);
        BufferDesc od{};
        od.size = bytes;
        od.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferSrc) |
                   static_cast<uint32_t>(BufferUsage::TransferDst);
        od.memoryUsage = BufferMemoryUsage::Readback;
        od.flags = BufferCreateFlags::PersistentlyMapped;
        od.debugName = "SkyNightProbe.Out";
        const BufferHandle outBuf = m_Device->CreateBuffer(od);
        m_Buffers.push_back(outBuf);
        // Poison: a dispatch that never lands is distinguishable from one that wrote zeros.
        const std::vector<float> poison(static_cast<size_t>(slots) * 4u, -1.0f);
        m_Device->UpdateBuffer(outBuf, 0, bytes, poison.data());

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "SkyNightProbe.DS";
        const DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        m_Device->UpdateCombinedImageSamplerBinding(ds, 0, m_MoonTexture, m_Sky->GetLinearSampler());
        m_Device->UpdateStorageBufferBinding(ds, 1, outBuf, 0, bytes);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->SetPushConstants(pc);
        cl->Dispatch((slots + 63u) / 64u, 1u, 1u);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();

        std::vector<float> raw(static_cast<size_t>(slots) * 4u);
        void* mapped = m_Device->MapBuffer(outBuf);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(raw.data(), mapped, bytes);
        m_Device->UnmapBuffer(outBuf);

        std::vector<Rgba> out(slots);
        for (uint32_t i = 0; i < slots; ++i)
            out[i] = {raw[i * 4u + 0], raw[i * 4u + 1], raw[i * 4u + 2], raw[i * 4u + 3]};
        return out;
    }

    // The disc's sums for the sun at `sun` (small-angle: each grid cell spans (2 r / grid)^2 sr).
    DiscSums Disc(const Vec3& sun, float drawnRadius, float illuminance)
    {
        ProbePC pc{};
        pc.modeCountRadiusIlluminance[0] = static_cast<float>(kDiscMode);
        pc.modeCountRadiusIlluminance[1] = static_cast<float>(kDiscGrid);
        pc.modeCountRadiusIlluminance[2] = drawnRadius;
        pc.modeCountRadiusIlluminance[3] = illuminance;
        for (int c = 0; c < 3; ++c)
        {
            pc.sunDir[c] = static_cast<float>(sun[c]);
            pc.moonDir[c] = static_cast<float>(kMoon[c]);
        }
        const uint32_t samples = kDiscGrid * kDiscGrid;
        std::vector<Rgba> out = Run(pc, samples + 2u);
        const Vec3 right = {out[samples][0], out[samples][1], out[samples][2]};
        const Vec3 up = {out[samples + 1u][0], out[samples + 1u][1], out[samples + 1u][2]};
        out.resize(samples);

        const double cell = 2.0 * drawnRadius / kDiscGrid;
        DiscSums sums;
        double cx = 0.0;
        double cy = 0.0;
        for (uint32_t i = 0; i < samples; ++i)
        {
            const Rgba& sample = out[i];
            for (int c = 0; c < 3; ++c)
                sums.Moonlight[c] += sample[c] * cell * cell;
            sums.LitTexels += sample[3];
            // The texel's place on the disc, x along right and y down up (moon UV).
            const double px = ((i % kDiscGrid) + 0.5) / kDiscGrid * 2.0 - 1.0;
            const double py = ((i / kDiscGrid) + 0.5) / kDiscGrid * 2.0 - 1.0;
            cx += sample[3] * px;
            cy += sample[3] * py;
        }
        sums.LitCentroidOnSky =
            Normalized({cx * right[0] - cy * up[0], cx * right[1] - cy * up[1], cx * right[2] - cy * up[2]});
        sums.Texels = std::move(out);
        return sums;
    }

    // The composite's sky at full night at each glow direction, horizon first, over `lutSky`.
    std::vector<Rgb> NightSky(const Rgb& lutSky, const float nightHorizonColor[3])
    {
        ProbePC pc{};
        pc.modeCountRadiusIlluminance[0] = static_cast<float>(kGlowMode);
        pc.modeCountRadiusIlluminance[1] = static_cast<float>(kGlowDirections);
        pc.trimNight[1] = 1.0f;
        pc.moonDir[1] = 1.0f;
        for (int c = 0; c < 3; ++c)
        {
            pc.skyColor[c] = static_cast<float>(lutSky[c]);
            pc.nightSkyHorizonColor[c] = nightHorizonColor[c];
        }
        std::vector<Rgb> sky;
        for (const Rgba& sample : Run(pc, kGlowDirections))
            sky.push_back({sample[0], sample[1], sample[2]});
        return sky;
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<SkyRenderer> m_Sky;
    TextureHandle m_MoonTexture{};
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};

// The sun on the far side of the sky from the moon: a full moon.
Vec3 FullMoonSun()
{
    return {-kMoon[0], -kMoon[1], -kMoon[2]};
}

} // namespace

// A full moon whose art is drawn at the real moon's size gives back the illuminance it was handed, in
// the moon's color; drawn at the default size (about 12 times larger) its surface is just as bright,
// so the light it would carry grows with its area. A magnified image keeps its radiance.
TEST_F(SkyNightProbeTest, TheDiscHasTheRealMoonsSurfaceBrightnessAtAnyDrawnSize)
{
    constexpr float kIlluminance = 1.0f;
    const SkySystemConfig config{};
    const float moonColor[3] = {config.moonColor.r, config.moonColor.g, config.moonColor.b};
    for (const float radius : {static_cast<float>(kRealSizeDrawnRadius), kDefaultMoonRadius})
    {
        const double areaRatio = (radius / kRealSizeDrawnRadius) * (radius / kRealSizeDrawnRadius);
        const Rgb delivered = Disc(FullMoonSun(), radius, kIlluminance).Moonlight;
        EXPECT_NEAR(Luminance(delivered) / areaRatio, kIlluminance, 0.02 * kIlluminance)
            << "drawn radius " << radius << " rad: the surface's radiance is the real moon's";
        for (int c = 0; c < 3; ++c)
            EXPECT_NEAR(delivered[c] / Luminance(delivered), moonColor[c], 0.02 * moonColor[c])
                << "channel " << c << ": the disc's color is the moon's tint";
    }
}

// A quarter moon is lit on the side that faces the sun, wherever the sun is around it: the lit
// texels' centroid points, on the sky, at the sun's direction from the moon.
TEST_F(SkyNightProbeTest, TheSunLightsTheSideOfTheMoonThatFacesIt)
{
    const Vec3 zenith = {0.0, 1.0, 0.0};
    const Vec3 east = {1.0, 0.0, 0.0};
    const Vec3 west = {-1.0, 0.0, 0.0};
    for (const Vec3& toward : {zenith, east, west})
    {
        const Vec3 sun = RotatedToward(kMoon, toward, 0.5 * std::numbers::pi);
        // The sun's direction from the moon on the sky: the sun's part in the moon's tangent plane.
        const double along = Dot(sun, kMoon);
        const Vec3 sunTangent = Normalized({sun[0] - along * kMoon[0], sun[1] - along * kMoon[1], sun[2] - along * kMoon[2]});
        const DiscSums disc = Disc(sun, kDefaultMoonRadius, 1.0f);
        const double cosine = Dot(disc.LitCentroidOnSky, sunTangent);
        EXPECT_GT(cosine, std::cos(5.0 * std::numbers::pi / 180.0))
            << "sun toward (" << toward[0] << ", " << toward[1] << ", " << toward[2]
            << "): the lit side points " << std::acos(std::min(1.0, cosine)) * 180.0 / std::numbers::pi
            << " degrees away from the sun";
    }
}

// The sun lights the share of the art's moon that the phase angle a says, (1 + cos a) / 2, from a
// two-day crescent to a gibbous moon, on both sides.
TEST_F(SkyNightProbeTest, ThePhaseLightsTheShareOfTheMoonThePhaseAngleSays)
{
    const double fullTexels = Disc(FullMoonSun(), kDefaultMoonRadius, 1.0f).LitTexels;
    ASSERT_GT(fullTexels, 0.0) << "the moon texture is blank: the staged art did not load";
    const Vec3 zenith = {0.0, 1.0, 0.0};
    const Vec3 nadir = {0.0, -1.0, 0.0};
    for (const double elongationDegrees : {18.0, 54.0, 126.0, 144.0})
        for (const Vec3& toward : {zenith, nadir})
        {
            const double elongation = elongationDegrees * std::numbers::pi / 180.0;
            const Vec3 sun = RotatedToward(kMoon, toward, elongation);
            const double litFraction = 0.5 * (1.0 - std::cos(elongation));
            EXPECT_NEAR(Disc(sun, kDefaultMoonRadius, 1.0f).LitTexels / fullTexels, litFraction, 0.01)
                << "elongation " << elongationDegrees << " degrees";
        }
}

// The two quarters together give back their light: each alone carries its half of the art's albedo,
// the maria-dark half or the bright highlands, so one is brighter than the other; their mean is the
// disc's mean. Drawn at the real moon's size, so the sum is the moonlight itself.
TEST_F(SkyNightProbeTest, TheQuartersTogetherDeliverTheirMoonlight)
{
    constexpr float kIlluminance = 1.0f;
    const Vec3 east = {1.0, 0.0, 0.0};
    const Vec3 west = {-1.0, 0.0, 0.0};
    const float radius = static_cast<float>(kRealSizeDrawnRadius);
    const double oneSide = Luminance(Disc(RotatedToward(kMoon, east, 0.5 * std::numbers::pi), radius, kIlluminance).Moonlight);
    const double otherSide = Luminance(Disc(RotatedToward(kMoon, west, 0.5 * std::numbers::pi), radius, kIlluminance).Moonlight);
    EXPECT_NEAR(0.5 * (oneSide + otherSide), kIlluminance, 0.02 * kIlluminance)
        << "one quarter " << oneSide << ", the other " << otherSide;
}

// The night side shows earthshine from the Earth's phase: a texel deep in the dark half for one sun is
// the same texel lit by the mirrored sun (the same share of the face lit, so the same lit radiance),
// times the earthshine fraction: about 1e-4 at a thin crescent, half of it at a quarter, near nothing
// close to full. Its color is the earthlight's: its red over blue is at most 0.9 of the lit side's (the
// shipped tints put it near 0.52), so a night side in the lit side's color fails by a tenth.
TEST_F(SkyNightProbeTest, TheNightSideShowsEarthshineFromTheEarthsPhase)
{
    const Vec3 zenith = {0.0, 1.0, 0.0};
    const Vec3 nadir = {0.0, -1.0, 0.0};
    for (const double elongationDegrees : {30.0, 90.0, 150.0})
    {
        const double elongation = elongationDegrees * std::numbers::pi / 180.0;
        const DiscSums lit = Disc(RotatedToward(kMoon, zenith, elongation), kDefaultMoonRadius, 1.0f);
        const DiscSums dark = Disc(RotatedToward(kMoon, nadir, elongation), kDefaultMoonRadius, 1.0f);
        // The Earth's lit fraction seen from the moon, (1 + cos elongation) / 2.
        const double expected = kEarthshineAtFullEarth * 0.5 * (1.0 + std::cos(elongation));
        int compared = 0;
        for (size_t i = 0; i < lit.Texels.size(); ++i)
        {
            const Rgb litTexel = {lit.Texels[i][0], lit.Texels[i][1], lit.Texels[i][2]};
            const Rgb darkTexel = {dark.Texels[i][0], dark.Texels[i][1], dark.Texels[i][2]};
            if (lit.Texels[i][3] < 0.99 || dark.Texels[i][3] > 1e-6 || Luminance(litTexel) <= 0.0)
                continue;
            EXPECT_NEAR(Luminance(darkTexel) / Luminance(litTexel), expected, 1e-3 * expected)
                << "elongation " << elongationDegrees << " degrees, texel " << i;
            EXPECT_LE(darkTexel[0] / darkTexel[2], 0.9 * (litTexel[0] / litTexel[2]))
                << "elongation " << elongationDegrees
                << ": earthshine is bluer than the lit side (earthlight, not the moon's warm reflectance)";
            if (++compared > 2000)
                break;
        }
        EXPECT_GT(compared, 100) << "elongation " << elongationDegrees << ": too few texels to compare";
    }
}

// The sky under the disc is the sky around it: four rim taps at the drawn radius, one on each side of
// the moon (so their mean has no point at the disc's centre), weighted by the disc's cover and the
// view ray's transmittance: a moon the planet hides leaves the sky as it is.
TEST_F(SkyNightProbeTest, TheSkyUnderTheDiscIsTheSkyAroundIt)
{
    ProbePC pc{};
    pc.modeCountRadiusIlluminance[0] = static_cast<float>(kRimMode);
    pc.modeCountRadiusIlluminance[1] = static_cast<float>(kRimTableSlots);
    pc.modeCountRadiusIlluminance[2] = kDefaultMoonRadius;
    for (int c = 0; c < 3; ++c)
        pc.moonDir[c] = static_cast<float>(kMoon[c]);
    const std::vector<Rgba> out = Run(pc, kRimTableSlots);
    Vec3 tangentSum{};
    for (uint32_t k = 0; k < 4u; ++k)
    {
        EXPECT_NEAR(out[k][3], kDefaultMoonRadius, 1e-5) << "rim tap " << k << " sits on the drawn rim";
        const Vec3 rim = {out[k][0], out[k][1], out[k][2]};
        const double along = Dot(rim, kMoon);
        for (int c = 0; c < 3; ++c)
            tangentSum[c] += rim[c] - along * kMoon[c];
    }
    EXPECT_LT(std::sqrt(Dot(tangentSum, tangentSum)), 1e-5) << "the four taps sit on opposite sides of the moon";
    EXPECT_NEAR(out[4][0], 0.0, 1e-6) << "a moon the planet hides leaves the sky as it is";
    EXPECT_NEAR(out[5][0], 1.0, 1e-6) << "where the disc covers the sky in clear air, the sky is the rim's";
    EXPECT_NEAR(out[6][0], 0.5, 1e-6) << "the disc's soft edge blends the two";
}

// The moon's disk hides a star behind it and never a falling star, which burns in front of the moon;
// a star off the disk is not hidden.
TEST_F(SkyNightProbeTest, TheMoonHidesAStarButNotAFallingStar)
{
    ProbePC pc{};
    pc.modeCountRadiusIlluminance[0] = static_cast<float>(kRimMode);
    pc.modeCountRadiusIlluminance[1] = static_cast<float>(kRimTableSlots);
    pc.modeCountRadiusIlluminance[2] = kDefaultMoonRadius;
    pc.moonDir[1] = 1.0f;
    const std::vector<Rgba> out = Run(pc, kRimTableSlots);
    EXPECT_NEAR(out[7][0], 1.0, 1e-6) << "a star behind the disk's center";
    EXPECT_EQ(out[7][1], 0.0f) << "a falling star in front of the disk's center";
    EXPECT_NEAR(out[7][2], 0.0, 1e-6) << "a star at twice the drawn radius";
}

// The default night glow with nothing under it: about 1.7e-4 cd/m2 at the zenith and three times
// that at the horizon (scene-linear on the 203-nit anchor), within the composite's 2.5 % noise.
TEST_F(SkyNightProbeTest, TheNightGlowIsTheNaturalNightSkysLevel)
{
    const Components::SkyEnvironment sky{};
    const std::vector<Rgb> night = NightSky(Rgb{}, sky.NightSkyHorizonColorKeys.Midnight);
    const double zenithNits = Luminance(night.back()) * Components::kReferenceWhiteNits;
    const double horizonNits = Luminance(night.front()) * Components::kReferenceWhiteNits;
    EXPECT_NEAR(zenithNits, 1.7e-4, 0.03 * 1.7e-4) << "the zenith's airglow and starlight";
    EXPECT_NEAR(horizonNits / zenithNits, 3.0, 0.15) << "the horizon brightens by van Rhijn's longer path";
}

// The glow adds to the LUT's sky, which carries the twilight and the moonlit sky: a moonlit sky
// under it comes through whole, with the same glow on top.
TEST_F(SkyNightProbeTest, TheNightGlowAddsToTheMoonlitSky)
{
    const Components::SkyEnvironment sky{};
    // A full moon's sky 30 degrees up, about 0.02 cd/m2.
    const Rgb moonlitSky = {0.8e-4, 1.0e-4, 1.3e-4};
    const std::vector<Rgb> glowOnly = NightSky(Rgb{}, sky.NightSkyHorizonColorKeys.Midnight);
    const std::vector<Rgb> night = NightSky(moonlitSky, sky.NightSkyHorizonColorKeys.Midnight);
    for (uint32_t i = 0; i < kGlowDirections; i += 15u)
        for (int c = 0; c < 3; ++c)
            EXPECT_NEAR(night[i][c] - moonlitSky[c], glowOnly[i][c], 0.01 * glowOnly[i][c])
                << "elevation " << i << " degrees, channel " << c;
}
