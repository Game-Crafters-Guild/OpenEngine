// The night sky's star model (sky_star_model.glsl), evaluated on the device by
// sky_star_model_probe.comp: the flux of a magnitude, the extinction, the contrast visibility,
// the scintillation's growth with airmass and the glare's energy, taken from the core.
//
// Skips ONLY without a Vulkan device. A missing staged probe FAILS: the same target builds it.

#include <gtest/gtest.h>

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include "Platform/Shell.h"

#include "TestDeviceHelper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

using Vec4 = std::array<float, 4>;

// The artistic shape at its physical values: Star core 1, no spikes, Spike length 2, the round point.
constexpr Vec4 kPhysicalShape{1.0f, 0.0f, 2.0f, 0.0f};

// A query to the probe: three input slots; the probe writes three output slots after them.
struct Query
{
    Vec4 In0{};
    Vec4 In1{};
    Vec4 In2 = kPhysicalShape;
};

struct Answer
{
    Vec4 Out0{};
    Vec4 Out1{};
    Vec4 Out2{};
};

constexpr float kPi = 3.14159265f;
// One pixel at 1080 rows and a vertical field of view of 60 degrees, on the view axis.
constexpr float kPixelAngle1080 = 2.0f * 0.57735027f / 1080.0f;
constexpr float kPixelSolidAngle1080 = kPixelAngle1080 * kPixelAngle1080;

std::filesystem::path FindProbeSpv()
{
    namespace fs = std::filesystem;
    const fs::path exe = GameEngine::Platform::GetExecutablePath();
    fs::path dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int up = 0; up < 6; ++up)
    {
        const fs::path candidate = dir / "Shaders" / "sky_star_model_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

class StarModelTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty()) << "sky_star_model_probe.comp.spv was not found above the test executable; "
                                         "the shader target stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty());

        DescriptorBinding data{};
        data.binding = 0;
        data.type = DescriptorType::StorageBuffer;
        data.count = 1;
        data.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "StarModelProbe.Set0";
        m_Layout.bindings = {data};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(float) * 4u;
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "StarModelProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (BufferHandle b : m_Buffers)
            m_Device->DestroyBuffer(b);
        m_Device->Shutdown();
    }

    std::vector<Answer> Run(const std::vector<Query>& queries)
    {
        const size_t bytes = queries.size() * 6u * sizeof(Vec4);
        BufferDesc bd{};
        bd.size = bytes;
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferSrc) |
                   static_cast<uint32_t>(BufferUsage::TransferDst);
        bd.memoryUsage = BufferMemoryUsage::Readback;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "StarModelProbe.Data";
        const BufferHandle buffer = m_Device->CreateBuffer(bd);
        m_Buffers.push_back(buffer);
        // Poison the outputs: a dispatch that never lands is distinguishable from one that wrote zeros.
        std::vector<Vec4> slots(queries.size() * 6u, Vec4{-1.0f, -1.0f, -1.0f, -1.0f});
        for (size_t i = 0; i < queries.size(); ++i)
        {
            slots[i * 6u + 0u] = queries[i].In0;
            slots[i * 6u + 1u] = queries[i].In1;
            slots[i * 6u + 2u] = queries[i].In2;
        }
        m_Device->UpdateBuffer(buffer, 0, bytes, slots.data());

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "StarModelProbe.DS";
        const DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        m_Device->UpdateStorageBufferBinding(ds, 0, buffer, 0, bytes);
        const float pc[4] = {static_cast<float>(queries.size()), 0.0f, 0.0f, 0.0f};
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->SetPushConstants(pc);
        cl->Dispatch(static_cast<uint32_t>((queries.size() + 63u) / 64u), 1u, 1u);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();

        const void* mapped = m_Device->MapBuffer(buffer);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(slots.data(), mapped, bytes);
        m_Device->UnmapBuffer(buffer);
        std::vector<Answer> answers(queries.size());
        for (size_t i = 0; i < queries.size(); ++i)
            answers[i] = {slots[i * 6u + 3u], slots[i * 6u + 4u], slots[i * 6u + 5u]};
        return answers;
    }

    // GE_StarShade for a white star at 1080 rows: magnitude, grey transmittance, sky luminance in
    // nits, limiting magnitude, highlight compression and the artistic glow.
    Answer Shade(float magnitude, float transmittance, float skyNits, float limit, float compression, float glow,
                 float op = 4.0f, Vec4 shape = kPhysicalShape)
    {
        return Run({{{op, magnitude, transmittance, skyNits}, {limit, compression, glow, kPixelSolidAngle1080}, shape}})[0];
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};

} // namespace

// A star's illuminance above the atmosphere follows Pogson's law from V = 0 at 2.54e-6 lx (Allen's
// Astrophysical Quantities): 100 times less every 5 magnitudes.
TEST_F(StarModelTest, AMagnitudeCarriesPogsonsIlluminance)
{
    const std::vector<Answer> a = Run({{{0.0f, 0.0f, 0.0f, 0.0f}, {}}, {{0.0f, 5.0f, 0.0f, 0.0f}, {}},
                                       {{0.0f, 6.5f, 0.0f, 0.0f}, {}}, {{0.0f, -1.46f, 0.0f, 0.0f}, {}}});
    EXPECT_NEAR(a[0].Out0[0] / 2.54e-6f, 1.0f, 1e-4f);
    EXPECT_NEAR(a[1].Out0[0] / 2.54e-8f, 1.0f, 1e-4f);
    EXPECT_NEAR(a[2].Out0[0] / (2.54e-6f * std::pow(10.0f, -0.4f * 6.5f)), 1.0f, 1e-4f);
    EXPECT_NEAR(a[3].Out0[0] / (2.54e-6f * std::pow(10.0f, 0.4f * 1.46f)), 1.0f, 1e-4f);
}

// The star puts exactly its illuminance on screen: its core's energy (the peak times PI r^2 pixels,
// times one pixel's solid angle) and its glare's add up to the illuminance, the glare's share taken out
// of the core. The artistic glow adds Star glow times the star's light on top and leaves the core alone.
TEST_F(StarModelTest, TheCoreAndTheGlareCarryTheStarsIlluminanceAndTheGlowAddsToIt)
{
    for (const float magnitude : {-1.46f, 0.0f, 2.0f, 4.0f, 6.5f})
        for (const float glow : {0.0f, 0.5f})
        {
            const float lux = 2.54e-6f * std::pow(10.0f, -0.4f * magnitude);
            const Answer a = Shade(magnitude, 1.0f, 0.0f, 8.0f, 1.0f, glow);
            EXPECT_NEAR((a.Out1[2] + a.Out1[3]) / lux, 1.0f, 2e-3f) << "magnitude " << magnitude << ", glow " << glow;
            EXPECT_NEAR(a.Out1[3] / lux, a.Out1[0], 2e-3f) << "the stated glare fraction is the drawn glare";
            EXPECT_NEAR(a.Out2[3] / lux, a.Out2[0], 2e-3f) << "the stated glow fraction is the drawn glow";
        }
    EXPECT_EQ(Shade(0.0f, 1.0f, 0.0f, 8.0f, 1.0f, 0.5f).Out0[0], Shade(0.0f, 1.0f, 0.0f, 8.0f, 1.0f, 0.0f).Out0[0])
        << "the glow does not take light from the core";
}

// The eye's glare (CIE 146: L = 10 E / theta_deg^2 for a young observer) around a magnitude 0 star at
// 1080 rows reaches about 20 px before it falls under a tenth of the natural night sky, carrying 4
// to 7 % of the star's light outside the 1 px core; a faint star's stays within its core's quad.
TEST_F(StarModelTest, BrightStarsCarryTheEyesGlareFaintOnesDoNot)
{
    const Answer zero = Shade(0.0f, 1.0f, 0.0f, 8.0f, 1.0f, 0.0f);
    EXPECT_GT(zero.Out0[3], 18.0f) << "cut radius";
    EXPECT_LT(zero.Out0[3], 26.0f) << "cut radius";
    EXPECT_GT(zero.Out1[0], 0.04f) << "glare fraction";
    EXPECT_LT(zero.Out1[0], 0.07f) << "glare fraction";

    const Answer faint = Shade(6.5f, 1.0f, 0.0f, 8.0f, 1.0f, 0.0f);
    EXPECT_LE(faint.Out0[3], 3.0f * faint.Out0[1] + 1e-4f) << "a faint star's glare quad is its core's";
}

// The camera observer draws the same stars without the eye's glare, so the core carries all of
// the star's light and the quad is the core's; the eye observer's glare comes out of that core.
TEST_F(StarModelTest, TheCameraDrawsNoGlare)
{
    const float lux = 2.54e-6f;
    const Answer eye = Shade(0.0f, 1.0f, 0.0f, 8.0f, 1.0f, 0.0f);
    const Answer camera = Shade(0.0f, 1.0f, 0.0f, 8.0f, 1.0f, 0.0f, 13.0f);
    EXPECT_GT(eye.Out1[0], 0.04f) << "the eye's glare fraction";
    EXPECT_EQ(camera.Out0[2], 0.0f) << "the camera's glare scale";
    EXPECT_EQ(camera.Out1[0], 0.0f) << "the camera's glare fraction";
    EXPECT_NEAR(camera.Out1[2] / lux, 1.0f, 2e-3f) << "the core carries the star's light";
    EXPECT_LE(camera.Out0[3], 3.0f * camera.Out0[1] + 1e-4f) << "the quad is the core's";
}

// The artistic glow adds its authored multiple of the brightest stars' light (in full at magnitude 1.5
// and brighter, none from 3), in a (1 + d^2 / R^2)^-2 lobe of R = 7 px at 900 rows (8.4 px at 1080),
// drawn out to 4 R.
TEST_F(StarModelTest, TheGlowAddsLightToTheBrightStarsOnly)
{
    const Answer bright = Shade(0.5f, 1.0f, 0.0f, 8.0f, 1.0f, 0.5f);
    EXPECT_NEAR(bright.Out2[0], 0.5f, 1e-4f);
    EXPECT_NEAR(bright.Out2[2], 7.0f * 1080.0f / 900.0f, 0.01f) << "glow radius in pixels";
    EXPECT_GE(bright.Out0[3], 4.0f * bright.Out2[2] - 1e-3f) << "the quad covers the glow";
    EXPECT_NEAR(Shade(1.5f, 1.0f, 0.0f, 8.0f, 1.0f, 0.5f).Out2[0], 0.5f, 1e-4f) << "a magnitude 1.5 star has the full glow";
    const float partial = Shade(2.25f, 1.0f, 0.0f, 8.0f, 1.0f, 0.5f).Out2[0];
    EXPECT_GT(partial, 0.1f) << "a magnitude 2.25 star has part of the glow";
    EXPECT_LT(partial, 0.4f) << "a magnitude 2.25 star has part of the glow";
    EXPECT_EQ(Shade(3.0f, 1.0f, 0.0f, 8.0f, 1.0f, 0.5f).Out2[0], 0.0f) << "a magnitude 3 star has no glow";
    EXPECT_EQ(Shade(0.5f, 1.0f, 0.0f, 8.0f, 1.0f, 0.0f).Out2[0], 0.0f) << "glow 0 is physical: none";
}

// A brighter star never draws a dimmer peak: the core's peak plus the glow's falls monotonically with
// magnitude for every look the presets ship, so the glow stars stay the brightest points in the sky.
TEST_F(StarModelTest, ABrighterStarAlwaysHasTheHigherPeak)
{
    for (const float compression : {1.0f, 0.775f, 0.55f})
        for (const float glow : {0.0f, 0.8f, 1.75f})
        {
            float previousPeak = 0.0f;
            for (float magnitude = 8.0f; magnitude >= -1.5f; magnitude -= 0.25f)
            {
                const Answer star = Shade(magnitude, 1.0f, 0.0f, 8.0f, compression, glow);
                const float peak = star.Out0[0] + star.Out2[1];
                EXPECT_GT(peak, previousPeak) << "magnitude " << magnitude << ", compression " << compression
                                              << ", glow " << glow;
                previousPeak = peak;
            }
        }
}

// A star only adds light: across its whole quad no channel of its radiance is negative, and it
// fades to under a thousandth of its core's peak at the quad's edge.
TEST_F(StarModelTest, AStarOnlyAddsLightAcrossItsQuad)
{
    for (const float magnitude : {-1.46f, 0.5f, 2.0f, 6.5f})
        for (const float glow : {0.0f, 1.0f})
        {
            const Answer edge = Shade(magnitude, 1.0f, 0.0f, 8.0f, 1.0f, glow, 7.0f);
            const Answer star = Shade(magnitude, 1.0f, 0.0f, 8.0f, 1.0f, glow);
            EXPECT_GE(edge.Out0[0], 0.0f) << "magnitude " << magnitude << ", glow " << glow;
            EXPECT_LT(edge.Out0[1] * 203.0f, 1e-2f * star.Out0[0]) << "magnitude " << magnitude << ", glow " << glow;
        }
}

// The eye sees faint stars grey: the colour fades to white only at the faint end, from 1.0e-7 lx
// (magnitude 3.5) down to 1.6e-8 lx (magnitude 5.5), by the star's physical illuminance, so the bright
// stars keep the catalog's colours and the highlight compression does not change them.
TEST_F(StarModelTest, BrightStarsKeepTheirColourFaintOnesAreWhite)
{
    const std::vector<Answer> s = Run({{{5.0f, 1.5e-8f, 0.0f, 0.0f}, {}},
                                       {{5.0f, 4.0e-8f, 0.0f, 0.0f}, {}},
                                       {{5.0f, 1.1e-7f, 0.0f, 0.0f}, {}}});
    EXPECT_EQ(s[0].Out0[0], 0.0f);
    EXPECT_GT(s[1].Out0[0], 0.0f);
    EXPECT_LT(s[1].Out0[0], 1.0f);
    EXPECT_EQ(s[2].Out0[0], 1.0f);

    const float orangeRB = 1.8f / 0.3f;
    for (const float compression : {1.0f, 0.775f, 0.55f})
        for (const float magnitude : {-1.46f, 0.5f, 2.0f, 3.5f})
        {
            const Answer star = Shade(magnitude, 1.0f, 0.0f, 8.0f, compression, 0.0f, 6.0f);
            EXPECT_NEAR(star.Out0[0] / star.Out0[2] / orangeRB, 1.0f, 0.01f)
                << "magnitude " << magnitude << ", compression " << compression << " keeps its colour";
        }
    const Answer faint = Shade(6.5f, 1.0f, 0.0f, 8.0f, 1.0f, 0.0f, 6.0f);
    EXPECT_NEAR(faint.Out0[0] / faint.Out0[2], 1.0f, 0.02f) << "a magnitude 6.5 orange star reads white";
}

// For the camera observer (the Physical look), every star keeps the catalog's colour at every magnitude.
TEST_F(StarModelTest, ForTheCameraAFaintStarKeepsItsColour)
{
    const float orangeRB = 1.8f / 0.3f;
    for (const float magnitude : {0.5f, 5.0f, 7.0f})
    {
        const Answer star = Shade(magnitude, 1.0f, 0.0f, 8.0f, 1.0f, 0.0f, 12.0f);
        EXPECT_NEAR(star.Out0[0] / star.Out0[2] / orangeRB, 1.0f, 0.01f) << "magnitude " << magnitude;
    }
}

// A faint star is seen by the rods, which see no colour: through the air's reddened transmittance it
// is still the working space's white (R = G = B), not the air's cream. A bright star keeps its colour
// and the air's reddening, as the cones see it.
TEST_F(StarModelTest, AFaintStarIsWhiteEvenThroughTheReddenedAir)
{
    const Answer faint = Shade(5.5f, 1.0f, 0.0f, 8.0f, 1.0f, 0.0f, 8.0f);
    EXPECT_NEAR(std::log2(faint.Out0[0] / faint.Out0[2]), 0.0f, 0.05f) << "R/B of a magnitude 5.5 star";
    EXPECT_NEAR(std::log2(faint.Out0[1] / faint.Out0[2]), 0.0f, 0.05f) << "G/B of a magnitude 5.5 star";

    const Answer bright = Shade(0.0f, 1.0f, 0.0f, 8.0f, 1.0f, 0.0f, 8.0f);
    EXPECT_NEAR(bright.Out0[0] / bright.Out0[2] / (1.8f / 0.3f * 0.95f / 0.74f), 1.0f, 0.01f)
        << "a magnitude 0 orange star keeps its colour and the air's reddening";
}

// Extinction: the light the star delivers scales with the transmittance along its direction, and
// the limiting magnitude applies to the star after extinction, so a star the air dims past the
// limit is hidden. The billboard taps the transmittance LUT along the star's own altitude.
TEST_F(StarModelTest, TheAirsTransmittanceDimsTheStarAndCanHideIt)
{
    const float lux = 2.54e-6f * std::pow(10.0f, -0.4f * 3.0f);
    const Answer clear = Shade(3.0f, 1.0f, 0.0f, 6.5f, 1.0f, 0.0f);
    const Answer hazy = Shade(3.0f, 0.25f, 0.0f, 6.5f, 1.0f, 0.0f);
    EXPECT_NEAR((clear.Out1[2] + clear.Out1[3]) / lux, 1.0f, 2e-3f);
    EXPECT_NEAR((hazy.Out1[2] + hazy.Out1[3]) / (0.25f * lux), 1.0f, 2e-3f);
    EXPECT_EQ(hazy.Out1[1], 1.0f) << "3.0 + 1.5 magnitudes of extinction is within 6.5";

    // 6.0 dimmed by 1.5 magnitudes (transmittance 0.25) is 7.5 after extinction: past 6.5.
    const Answer hidden = Shade(6.0f, 0.25f, 0.0f, 6.5f, 1.0f, 0.0f);
    EXPECT_EQ(hidden.Out1[1], 0.0f);
}

// Where a star sits for the sky (GE_StarGeometry, what the billboard reads its LUTs with): the sky-view
// LUT is read at the star's own planet-local direction, so a star low in a bright twilight band takes
// that band's limit and not the dark zenith's; the transmittance is read at the star's altitude; and the
// pixel's solid angle is the on-axis one, the same for a star at the edge of the frame as at its centre.
TEST_F(StarModelTest, EachStarReadsTheSkyAndTheAirAlongItsOwnDirection)
{
    const float tanHalf = 0.57735027f;
    const float low[3] = {0.0f, 0.0871557f, 0.9961947f}; // 5 degrees up, toward +Z (north)
    const std::vector<Answer> a = Run({{{11.0f, low[0], low[1], low[2]}, {0.0f, 1.0f, 0.0f, tanHalf}, {1080.0f, 0, 0, 0}},
                                       {{11.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f, tanHalf}, {1080.0f, 0, 0, 0}},
                                       {{11.0f, 0.7071068f, 0.0f, 0.7071068f}, {0.0f, 1.0f, 0.0f, tanHalf}, {720.0f, 0, 0, 0}}});
    EXPECT_NEAR(a[0].Out0[1], 0.0871557f, 1e-5f) << "the LUT's up axis is the star's altitude, not the zenith";
    EXPECT_NEAR(a[0].Out0[2], 0.9961947f, 1e-5f) << "the LUT's north axis carries the star's azimuth";
    EXPECT_NEAR(a[0].Out0[3], 0.0871557f, 1e-5f) << "the transmittance is read at the star's altitude";
    EXPECT_NEAR(a[1].Out0[1], 1.0f, 1e-5f);
    EXPECT_NEAR(a[1].Out0[3], 1.0f, 1e-5f);
    EXPECT_NEAR(a[2].Out0[3], 0.0f, 1e-5f) << "a star on the horizon is at altitude 0 whatever the camera faces";
    const float pixel1080 = 2.0f * tanHalf / 1080.0f;
    EXPECT_NEAR(a[0].Out1[0] / (pixel1080 * pixel1080), 1.0f, 1e-4f);
    EXPECT_NEAR(a[1].Out1[0] / (pixel1080 * pixel1080), 1.0f, 1e-4f) << "the edge and the centre of the frame agree";
    const float pixel720 = 2.0f * tanHalf / 720.0f;
    EXPECT_NEAR(a[2].Out1[0] / (pixel720 * pixel720), 1.0f, 1e-4f) << "the solid angle follows the row count";
}

// A star's light on screen: its own light (core and glare), plus the artistic glow's multiple of it;
// the spikes take their share out of the glow, so they never add light. The radiance integrated over
// the quad on a fine grid holds that energy, for the round point and the sparkle alike.
TEST_F(StarModelTest, TheSpikesAndTheSparkleConserveTheStarsLight)
{
    const float glow = 1.0f;
    for (const float shape : {0.0f, 1.0f})
        for (const float spikes : {0.0f, 0.5f})
        {
            const Vec4 dials{1.0f, spikes, 2.0f, shape};
            const Answer star = Shade(0.5f, 1.0f, 0.0f, 8.0f, 1.0f, glow, 9.0f, dials);
            const float lux = 2.54e-6f * std::pow(10.0f, -0.4f * 0.5f);
            const float own = lux / kPixelSolidAngle1080;
            EXPECT_NEAR(star.Out0[0] / (own * (1.0f + glow)), 1.0f, 1e-3f)
                << "shape " << shape << ", spikes " << spikes << ": the total is the star plus its glow";
            EXPECT_NEAR(star.Out0[1] / star.Out0[0], 1.0f, 0.03f)
                << "shape " << shape << ", spikes " << spikes << ": the drawn light is the total";
            EXPECT_NEAR(star.Out0[2], spikes * glow, 1e-3f) << "the spikes carry their share of the glow";
            EXPECT_NEAR(star.Out2[0] + star.Out0[2], glow, 1e-3f) << "the glow keeps the rest";
        }
}

// The spikes are four arms along the screen's axes: around a ring at the spike length, the radiance has
// four maxima, at 0, 90, 180 and 270 degrees; without spikes it has none.
TEST_F(StarModelTest, TheSpikesAreFourArms)
{
    for (const float spikes : {0.0f, 0.5f})
    {
        const Answer star = Shade(0.5f, 1.0f, 0.0f, 8.0f, 1.0f, 1.0f, 9.0f, Vec4{1.0f, 0.5f, 2.0f, 0.0f});
        const float ring = star.Out1[1];
        std::vector<Query> q;
        for (int k = 0; k < 360; ++k)
        {
            const float angle = static_cast<float>(k) * kPi / 180.0f;
            q.push_back({{10.0f, 0.5f, ring * std::cos(angle), ring * std::sin(angle)},
                         {8.0f, 1.0f, 1.0f, kPixelSolidAngle1080},
                         Vec4{1.0f, spikes, 2.0f, 0.0f}});
        }
        const std::vector<Answer> a = Run(q);
        std::vector<float> v;
        for (const Answer& x : a)
            v.push_back(x.Out0[0]);
        std::vector<float> sorted = v;
        std::sort(sorted.begin(), sorted.end());
        const float median = sorted[sorted.size() / 2];
        std::vector<int> peaks;
        for (int k = 0; k < 360; ++k)
            if (v[k] > 3.0f * median && v[k] >= v[(k + 359) % 360] && v[k] > v[(k + 1) % 360])
                peaks.push_back(k);
        if (spikes == 0.0f)
        {
            EXPECT_TRUE(peaks.empty()) << "no spikes: the ring is even";
            continue;
        }
        ASSERT_EQ(peaks.size(), 4u) << "four arms";
        for (int i = 0; i < 4; ++i)
            EXPECT_EQ(peaks[i], 90 * i) << "the arms run along the screen's axes";
    }
}

// The Glow dial acts with either shape: the sparkle carries the star's own light and the glow lobe adds
// the glow's around it, as the round point does.
TEST_F(StarModelTest, TheGlowActsOnTheSparkleToo)
{
    const Answer sparkle = Shade(0.5f, 1.0f, 0.0f, 8.0f, 1.0f, 1.0f, 4.0f, Vec4{1.0f, 0.0f, 2.0f, 1.0f});
    const Answer point = Shade(0.5f, 1.0f, 0.0f, 8.0f, 1.0f, 1.0f, 4.0f, Vec4{1.0f, 0.0f, 2.0f, 0.0f});
    EXPECT_GT(sparkle.Out2[1], 0.0f) << "the sparkle draws the glow lobe";
    EXPECT_NEAR(sparkle.Out2[1] / point.Out2[1], 1.0f, 1e-4f) << "the same glow as the round point";
    const Answer noGlow = Shade(0.5f, 1.0f, 0.0f, 8.0f, 1.0f, 0.0f, 4.0f, Vec4{1.0f, 0.0f, 2.0f, 1.0f});
    EXPECT_EQ(sparkle.Out0[0], noGlow.Out0[0]) << "the glow leaves the sparkle as bright as with no glow";
    EXPECT_EQ(sparkle.Out0[1], noGlow.Out0[1]) << "and as large";
}

// The Star core dial scales every star's core (1 is the physical 1 px); the sparkle's size is its reach.
TEST_F(StarModelTest, TheStarCoreDialScalesTheCore)
{
    EXPECT_NEAR(Shade(0.5f, 1.0f, 0.0f, 8.0f, 0.775f, 0.0f, 4.0f, Vec4{0.7f, 0.0f, 2.0f, 0.0f}).Out0[1], 0.7f, 1e-4f);
    EXPECT_NEAR(Shade(6.5f, 1.0f, 0.0f, 8.0f, 0.775f, 0.0f, 4.0f, Vec4{2.0f, 0.0f, 2.0f, 0.0f}).Out0[1], 2.0f, 1e-4f);
}

// Visibility against the sky (Schaefer 1993, after Hecht and Blackwell): the limiting magnitude is the
// dial's under the natural dark sky and falls as the sky behind the star brightens: to about 4 under a
// full-moon sky (18 mag/arcsec2, 6.8e-3 cd/m2) for a 6.5 dial, and to about -4 in daylight (5,000 cd/m2),
// where Venus shows.
TEST_F(StarModelTest, TheLimitingMagnitudeFallsAsTheSkyBehindTheStarBrightens)
{
    const std::vector<Answer> a = Run({{{2.0f, 6.5f, 0.0f, 0.0f}, {}},
                                       {{2.0f, 6.5f, 6.8e-3f, 0.0f}, {}},
                                       {{2.0f, 6.5f, 5000.0f, 0.0f}, {}},
                                       {{2.0f, 6.5f, 1.0e-3f, 0.0f}, {}},
                                       {{2.0f, 6.5f, 0.1f, 0.0f}, {}}});
    EXPECT_NEAR(a[0].Out0[0], 6.5f, 1e-3f) << "no sky: the dial's limit";
    EXPECT_NEAR(a[1].Out0[0], 4.0f, 0.3f) << "full moon";
    EXPECT_NEAR(a[2].Out0[0], -4.1f, 0.4f) << "daylight";
    EXPECT_GT(a[0].Out0[0], a[3].Out0[0]);
    EXPECT_GT(a[3].Out0[0], a[1].Out0[0]);
    EXPECT_GT(a[1].Out0[0], a[4].Out0[0]);
    EXPECT_GT(a[4].Out0[0], a[2].Out0[0]);
    const Answer moonless = Run({{{2.0f, 6.5f, 1.7e-4f, 0.0f}, {}}})[0];
    EXPECT_NEAR(moonless.Out0[0], 6.5f, 1e-3f) << "the natural moonless sky the LUT carries: the dial's limit";
    EXPECT_NEAR(moonless.Out0[1], 6.6f, 0.1f) << "the eye's own limit under the natural moonless sky (1.7e-4 cd/m2)";
    EXPECT_LT(Run({{{2.0f, 6.5f, 5.1e-4f, 0.0f}, {}}})[0].Out0[0], 6.3f) << "the airglow at the horizon, three times the zenith's";

    // A magnitude 3.5 star shows under the full moon; a magnitude 4.5 star does not; a magnitude 0 star
    // shows at twilight (0.1 cd/m2).
    EXPECT_EQ(Shade(3.5f, 1.0f, 6.8e-3f, 6.5f, 1.0f, 0.0f).Out1[1], 1.0f);
    EXPECT_EQ(Shade(4.5f, 1.0f, 6.8e-3f, 6.5f, 1.0f, 0.0f).Out1[1], 0.0f);
    EXPECT_EQ(Shade(0.0f, 1.0f, 0.1f, 6.5f, 1.0f, 0.0f).Out1[1], 1.0f);
}

// Scintillation: none at the zenith, the authored amount at airmass 10 (5.7 degrees up), a few per
// cent of it 30 degrees up (airmass 2, (2^1.75 - 1) / (10^1.75 - 1) = 0.043).
TEST_F(StarModelTest, ScintillationGrowsFromNoneAtTheZenithToTheAmountAtTheHorizon)
{
    const float sin30 = 0.5f;
    const float sinAirmass10 = 0.0908f;
    const std::vector<Answer> a = Run({{{3.0f, 1.0f, 0.3f, 0.0f}, {}},
                                       {{3.0f, sin30, 0.3f, 0.0f}, {}},
                                       {{3.0f, sinAirmass10, 0.3f, 0.0f}, {}},
                                       {{3.0f, 0.0f, 0.3f, 0.0f}, {}}});
    EXPECT_EQ(a[0].Out0[0], 0.0f) << "zenith";
    EXPECT_NEAR(a[1].Out0[1], 2.0f, 0.02f) << "airmass 30 degrees up";
    EXPECT_NEAR(a[1].Out0[0], 0.3f * 0.043f, 0.002f) << "30 degrees up";
    EXPECT_NEAR(a[2].Out0[0], 0.3f, 0.01f) << "airmass 10";
    EXPECT_EQ(a[3].Out0[0], 0.3f) << "the horizon holds the airmass-10 amount";
}

// Highlight compression is the artistic tone curve on the bright end: at 1 a magnitude -1.5 star
// carries 1,585 times a magnitude 6.5 star's light; at 0.55, 58 times; the reference star is the
// same at every setting.
TEST_F(StarModelTest, HighlightCompressionChangesOnlyTheBrightEnd)
{
    const std::vector<Answer> a = Run({{{1.0f, -1.5f, 1.0f, 0.0f}, {}},
                                       {{1.0f, -1.5f, 0.55f, 0.0f}, {}},
                                       {{1.0f, 6.5f, 0.55f, 0.0f}, {}}});
    EXPECT_NEAR(a[0].Out0[0], 1584.9f, 2.0f);
    EXPECT_NEAR(a[1].Out0[0], 57.5f, 0.2f);
    EXPECT_NEAR(a[2].Out0[0], 1.0f, 1e-4f);
}

// Every star's core is the same 1 px (sigma 0.7 px) point whatever its brightness and the compression:
// a bright star is a brighter point, not a wider disc (the fold-3 sweep: a growing core read as a soft
// disc beside the reference's sharp heroes).
TEST_F(StarModelTest, EveryStarsCoreIsTheSamePoint)
{
    for (const float compression : {1.0f, 0.775f, 0.55f})
        for (const float magnitude : {-1.46f, 0.5f, 3.0f, 6.5f})
            EXPECT_NEAR(Shade(magnitude, 1.0f, 0.0f, 8.0f, compression, 0.8f).Out0[1], 1.0f, 1e-4f)
                << "magnitude " << magnitude << ", compression " << compression;
}
