// The night tint's rule (Includes/night_tint.glsl), evaluated on the device by night_tint_probe.comp:
// which pixels of a night image take the tint and which keep their colour.
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
#include <cstring>
#include <filesystem>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

using Vec4 = std::array<float, 4>;

// A dark night: the exposure adapts to 0.001 cd/m2, and the pixel is the dark sky's scene luminance.
constexpr float kNightAdaptationNits = 0.001f;
constexpr float kDarkSceneNits = 0.01f;

// Rec. 709 luminance of a display colour.
float Luminance(const Vec4& c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

std::filesystem::path FindProbeSpv()
{
    namespace fs = std::filesystem;
    const fs::path exe = GameEngine::Platform::GetExecutablePath();
    fs::path dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int up = 0; up < 6; ++up)
    {
        const fs::path candidate = dir / "Shaders" / "night_tint_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

class NightTintTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty()) << "night_tint_probe.comp.spv was not found above the test executable; "
                                         "the shader target stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty());
        DescriptorBinding data{};
        data.binding = 0;
        data.type = DescriptorType::StorageBuffer;
        data.count = 1;
        data.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "NightTintProbe.Set0";
        m_Layout.bindings = {data};
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(float) * 4u;
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "NightTintProbe";
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

    // Each query: (op, a, b, c) and the second slot's input (its x is overwritten); returns the second
    // slot after the dispatch.
    std::vector<Vec4> Run(const std::vector<std::pair<Vec4, Vec4>>& queries)
    {
        const size_t bytes = queries.size() * 2u * sizeof(Vec4);
        BufferDesc bd{};
        bd.size = bytes;
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferSrc) |
                   static_cast<uint32_t>(BufferUsage::TransferDst);
        bd.memoryUsage = BufferMemoryUsage::Readback;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "NightTintProbe.Data";
        const BufferHandle buffer = m_Device->CreateBuffer(bd);
        m_Buffers.push_back(buffer);
        std::vector<Vec4> slots(queries.size() * 2u, Vec4{-1.0f, -1.0f, -1.0f, -1.0f});
        for (size_t i = 0; i < queries.size(); ++i)
        {
            slots[i * 2u] = queries[i].first;
            slots[i * 2u + 1u] = queries[i].second;
        }
        m_Device->UpdateBuffer(buffer, 0, bytes, slots.data());
        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "NightTintProbe.DS";
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
        std::vector<Vec4> out(queries.size());
        for (size_t i = 0; i < queries.size(); ++i)
            out[i] = slots[i * 2u + 1u];
        return out;
    }

    float Amount(float strength, float adaptationNits, float sceneNits, float contrast)
    {
        return Run({{{0.0f, strength, adaptationNits, sceneNits}, {-1.0f, contrast, 0.0f, 0.0f}}})[0][0];
    }

    // A pixel of the scene: its stored luminance, the star light in it and its depth (0 = sky).
    float StarContrast(float storedY, float starY, float depth)
    {
        return Run({{{1.0f, storedY, starY, depth}, {-1.0f, 0.0f, 0.0f, 0.0f}}})[0][0];
    }

    float SceneNits(float storedY, float preExposure)
    {
        return Run({{{2.0f, storedY, preExposure, 0.0f}, {-1.0f, 0.0f, 0.0f, 0.0f}}})[0][0];
    }

    float AdaptationNits(float meteredNits, float appliedExposure)
    {
        return Run({{{3.0f, meteredNits, appliedExposure, 0.0f}, {-1.0f, 0.0f, 0.0f, 0.0f}}})[0][0];
    }

    // A display colour tinted in full by tint.
    Vec4 Tinted(const Vec4& display, const Vec4& tint)
    {
        return Run({{{4.0f, display[0], display[1], display[2]}, {-1.0f, tint[0], tint[1], tint[2]}}})[0];
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};

} // namespace

// How far a pixel stands above the sky without its star light decides the tint: 1.6 times (a faint
// star's core) takes none of it, 1.3 times part of it, and the sky itself (1.0 to 1.05 times: dither and
// gradient) all of it.
TEST_F(NightTintTest, AFaintStarOverTheTintedSkyKeepsItsColour)
{
    EXPECT_NEAR(Amount(1.0f, kNightAdaptationNits, kDarkSceneNits, 1.02f), 1.0f, 1e-3f) << "the sky takes the tint";
    EXPECT_NEAR(Amount(1.0f, kNightAdaptationNits, kDarkSceneNits, 1.05f), 1.0f, 1e-3f) << "a sky gradient takes it";
    EXPECT_EQ(Amount(1.0f, kNightAdaptationNits, kDarkSceneNits, 1.6f), 0.0f) << "a faint star keeps its colour";
    EXPECT_EQ(Amount(1.0f, kNightAdaptationNits, kDarkSceneNits, 3.0f), 0.0f) << "a bright star keeps its colour";
    const float between = Amount(1.0f, kNightAdaptationNits, kDarkSceneNits, 1.3f);
    EXPECT_GT(between, 0.0f);
    EXPECT_LT(between, 1.0f);
}

// The sky above a dark skyline and a lit edge beside dark geometry take the tint: neither holds star
// light, however much darker the geometry beside them is.
TEST_F(NightTintTest, TheSkyAndALitEdgeBesideDarkGeometryTakeTheTint)
{
    const float sky = StarContrast(1.0f, 0.0f, 0.0f);
    const float litEdge = StarContrast(2.0f, 0.0f, 0.4f);
    EXPECT_NEAR(Amount(1.0f, kNightAdaptationNits, kDarkSceneNits, sky), 1.0f, 1e-3f) << "contrast " << sky;
    EXPECT_NEAR(Amount(1.0f, kNightAdaptationNits, kDarkSceneNits, litEdge), 1.0f, 1e-3f) << "contrast " << litEdge;
}

// A star's core and its glow keep the star's colour (the glow adds 0.6 of the sky's light to it), and
// geometry in front of a star hides its light, so the geometry takes the tint.
TEST_F(NightTintTest, AStarsGlowKeepsItsColourAndGeometryInFrontOfAStarTakesTheTint)
{
    const float core = StarContrast(4.0f, 3.0f, 0.0f);
    const float glow = StarContrast(1.6f, 0.6f, 0.0f);
    const float occluder = StarContrast(0.5f, 3.0f, 0.2f);
    EXPECT_EQ(Amount(1.0f, kNightAdaptationNits, kDarkSceneNits, core), 0.0f) << "contrast " << core;
    EXPECT_EQ(Amount(1.0f, kNightAdaptationNits, kDarkSceneNits, glow), 0.0f) << "contrast " << glow;
    EXPECT_NEAR(Amount(1.0f, kNightAdaptationNits, kDarkSceneNits, occluder), 1.0f, 1e-3f) << "contrast " << occluder;
}

// A day image and the bright parts of a night image are untouched; strength 0 is physical.
TEST_F(NightTintTest, DayImagesBrightPixelsAndStrengthZeroAreUntouched)
{
    EXPECT_EQ(Amount(1.0f, 10.0f, kDarkSceneNits, 1.0f), 0.0f) << "a day view";
    EXPECT_EQ(Amount(1.0f, kNightAdaptationNits, 2500.0f, 1.0f), 0.0f) << "the moon";
    EXPECT_EQ(Amount(1.0f, kNightAdaptationNits, 5.0f, 1.0f), 0.0f) << "a lit window";
    EXPECT_EQ(Amount(0.0f, kNightAdaptationNits, kDarkSceneNits, 1.0f), 0.0f) << "strength 0";
    EXPECT_NEAR(Amount(0.5f, kNightAdaptationNits, kDarkSceneNits, 1.0f), 0.5f, 1e-3f) << "strength scales it";
}

// The gate reads what the view meters, before the clamps and the curve: a room lit by one 60 W lamp
// (the interior fixture meters 0.31 cd/m2, EV100 0.8) takes none of the tint, its dark corners included,
// and a full moon (0.009 cd/m2, EV100 -4.3) takes most of it.
TEST_F(NightTintTest, ALampLitRoomTakesNoTintAndAFullMoonTakesIt)
{
    constexpr float kLampLitRoomNits = 0.31f;
    constexpr float kFullMoonNits = 0.009f;
    EXPECT_EQ(Amount(1.0f, kLampLitRoomNits, kDarkSceneNits, 1.0f), 0.0f) << "a lamp-lit room's dark corner";
    EXPECT_EQ(Amount(1.0f, 0.05f, kDarkSceneNits, 1.0f), 0.0f) << "the gate's edge";
    EXPECT_GT(Amount(1.0f, kFullMoonNits, kDarkSceneNits, 1.0f), 0.5f) << "a full moon";
    EXPECT_NEAR(Amount(1.0f, 0.005f, kDarkSceneNits, 1.0f), 1.0f, 1e-3f) << "the full tint's edge";
}

// The pixel's scene luminance divides out the pre-exposure: the same moonlit surface (0.06 cd/m2)
// stored at two pre-exposures reads the same, so the field band does not move with the pre-exposure.
TEST_F(NightTintTest, ThePixelsSceneLuminanceDoesNotDependOnThePreExposure)
{
    constexpr float kMoonlitFaceNits = 0.06f;
    for (const float preExposure : {1.0f, 4096.0f})
    {
        const float storedY = kMoonlitFaceNits / 203.0f * preExposure;
        EXPECT_NEAR(SceneNits(storedY, preExposure), kMoonlitFaceNits, kMoonlitFaceNits * 1e-4f) << preExposure;
    }
}

// The gate takes the meter's reading when the view meters, and the luminance the exposure maps to
// mid-grey (0.18 x 203 cd/m2 over the applied exposure) when it does not.
TEST_F(NightTintTest, TheGateReadsTheMeterAndElseTheExposure)
{
    EXPECT_NEAR(AdaptationNits(0.31f, 4096.0f), 0.31f, 1e-6f) << "a metered view";
    EXPECT_NEAR(AdaptationNits(-1.0f, 4096.0f), 0.18f * 203.0f / 4096.0f, 1e-6f) << "a Manual exposure";
}

// The tint keeps a pixel's luminance and stays inside its display range: a bright twilight sky (a pale
// blue at display luminance 0.6) takes the default hue as far as it fits, with no channel past 1, and
// a dark moonlit pixel takes it in full.
TEST_F(NightTintTest, TheTintKeepsLuminanceAndDoesNotClipABrightSky)
{
    const Vec4 tint{0.155f, 0.249f, 1.0f, 0.0f};
    const Vec4 sky{0.5f, 0.6f, 0.9f, 0.0f};
    const Vec4 brightSky = Tinted(sky, tint);
    EXPECT_LE(std::max({brightSky[0], brightSky[1], brightSky[2]}), 1.0f + 1e-5f) << "a channel clips";
    EXPECT_NEAR(Luminance(brightSky), Luminance(sky), 1e-4f);
    EXPECT_GT(brightSky[2] - brightSky[0], sky[2] - sky[0]) << "the sky takes some of the hue";
    const Vec4 ground{0.04f, 0.035f, 0.03f, 0.0f};
    const Vec4 moonlit = Tinted(ground, tint);
    const float tintY = Luminance(tint);
    for (int c = 0; c < 3; ++c)
        EXPECT_NEAR(moonlit[c], Luminance(ground) * tint[c] / tintY, 1e-5f) << "channel " << c;
}

// The window is scene-referred: a moonlit surface (0.25 lux on a pale face, about 0.06 cd/m2) takes the
// full tint whatever exposure the curve and the floor give the night, and the spare starts at 0.5 cd/m2.
TEST_F(NightTintTest, AMoonlitSurfaceTakesTheTintAtEveryExposure)
{
    constexpr float kMoonlitFaceNits = 0.06f;
    for (const float adaptationNits : {0.0005f, 0.001f, 0.003f})
        EXPECT_NEAR(Amount(1.0f, adaptationNits, kMoonlitFaceNits, 1.0f), 1.0f, 1e-3f) << adaptationNits;
    EXPECT_NEAR(Amount(1.0f, kNightAdaptationNits, 0.5f, 1.0f), 1.0f, 1e-3f) << "the field's edge";
    const float between = Amount(1.0f, kNightAdaptationNits, 1.0f, 1.0f);
    EXPECT_GT(between, 0.0f);
    EXPECT_LT(between, 1.0f);
    EXPECT_EQ(Amount(1.0f, kNightAdaptationNits, 2.0f, 1.0f), 0.0f) << "the spare's edge";
}
