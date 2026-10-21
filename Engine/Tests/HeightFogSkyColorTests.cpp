// The height fog's sky horizon colour mode: the haze takes the colour of the sky it meets.
//
// The GPU arm runs the real SkyEnvironmentSystem at two times of day, resolves the fog's UBO
// through the production FillHeightFogSkyColor, and evaluates on the device both the fog's colour
// (height_fog_sky_horizon_probe.comp, the shared GE_SkyHorizonRadiance) and the sky backdrop the
// sky pass draws at the horizon of the same azimuth, over a stand-in sky-view LUT that varies with
// azimuth and elevation, so a fog that read the wrong row, the wrong azimuth, or the wrong exposure
// or night terms disagrees with the sky.
//
// Skips ONLY without a Vulkan device. A missing staged probe shader FAILS: the same build compiles
// and stages it, so its absence is a build defect, not an environment gap.

#include <gtest/gtest.h>

#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffectPresets.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h" // World::AddComponentImmediate/GetComponent definitions
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "Engine/Rendering/HeightFogParamsUBO.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Platform/Shell.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Sky/SkySettings.h"

#include "TestDeviceHelper.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using Engine::Renderer::HeightFogColorSource;
using Engine::Renderer::HeightFogParamsUBO;
using Engine::Renderer::PostProcessSettings;

namespace
{

constexpr uint32_t kDirections = 128u;
constexpr uint32_t kLutWidth = 64u;
constexpr uint32_t kLutHeight = 32u;

// Mirrors height_fog_sky_horizon_probe.comp's ProbePC.
struct ProbePC
{
    float fogScaleNightOffsetCount[4];
    float fogHorizonColor[4];
    float skyExposureNight[4];
    float skyNightHorizonColor[4];
};

PostProcessSettings SkyHorizonFog()
{
    PostProcessSettings settings{};
    settings.HeightFogColorMode = static_cast<int32>(Components::HeightFogColorMode::SkyHorizon);
    return settings;
}

// The sky settings SkyEnvironmentSystem produces for the default sky at `hours`, with an exposure
// trim off zero so a fog that dropped the sky's exposure multiply disagrees with it.
Rendering::SkySettings SkySettingsAt(float hours)
{
    ECS::World world;
    Engine::Renderer::RenderServices services;
    Engine::Renderer::SkyEnvironmentSystem system{&services};
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::SkyEnvironment sky{};
    sky.TimeOfDayHours = hours;
    sky.SkyExposureTrim = 1.25f;
    world.AddComponentImmediate(entity, sky);
    system.Update(world, 0.0f);

    const auto* feature = services.GetFeature<Engine::Renderer::SkyRenderFeature>();
    EXPECT_NE(feature, nullptr);
    EXPECT_TRUE(feature && feature->HasActiveSettings());
    return feature ? feature->GetSettings() : Rendering::SkySettings{};
}

// Resolve the staged probe SPIR-V by walking up from the executable, as the other probes do.
std::filesystem::path FindProbeSpv()
{
    namespace fs = std::filesystem;
    const fs::path exe = GameEngine::Platform::GetExecutablePath();
    fs::path dir = exe.empty() ? fs::current_path() : exe.parent_path();
    for (int up = 0; up < 6; ++up)
    {
        const fs::path candidate = dir / "Shaders" / "height_fog_sky_horizon_probe.comp.spv";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return {};
}

class HeightFogSkyHorizonTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";

        const std::filesystem::path spvPath = FindProbeSpv();
        ASSERT_FALSE(spvPath.empty())
            << "height_fog_sky_horizon_probe.comp.spv was not found above the test executable ("
            << GameEngine::Platform::GetExecutablePath().string()
            << ") -- the shader build stages it, so its absence is a build defect";
        const std::vector<uint8_t> spv = Utils::ReadFile(spvPath.string());
        ASSERT_FALSE(spv.empty()) << "staged probe SPIR-V is empty: " << spvPath.string();

        DescriptorBinding lut{};
        lut.binding = 0;
        lut.type = DescriptorType::CombinedImageSampler;
        lut.count = 1;
        lut.shaderStages = kShaderStageCompute;
        DescriptorBinding out{};
        out.binding = 1;
        out.type = DescriptorType::StorageBuffer;
        out.count = 1;
        out.shaderStages = kShaderStageCompute;
        m_Layout.debugName = "HeightFogSkyHorizonProbe.Set0";
        m_Layout.bindings = {lut, out};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spv);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        cd.PushConstants.Size = sizeof(ProbePC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "HeightFogSkyHorizonProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
        ASSERT_TRUE(m_Pipeline.IsValid());

        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearRepeat("HeightFogSkyHorizonProbeSampler"));
        ASSERT_TRUE(m_Sampler.IsValid());
    }

    void TearDown() override
    {
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        for (TextureHandle& t : m_Textures)
            if (t.IsValid())
                m_Device->DestroyTexture(t);
        for (BufferHandle& b : m_Buffers)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
        if (m_Device)
            m_Device->Shutdown();
    }

    // Stand-in sky-view LUT: every texel distinct along both axes (u is the azimuth, v the
    // elevation with the horizon at v = 0.5), scaled by `brightness` so each time of day has its own.
    TextureHandle MakeSkyViewLut(float brightness)
    {
        std::vector<float> texels(static_cast<size_t>(kLutWidth) * kLutHeight * 4u);
        for (uint32_t y = 0; y < kLutHeight; ++y)
        {
            for (uint32_t x = 0; x < kLutWidth; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) / kLutWidth;
                const float v = (static_cast<float>(y) + 0.5f) / kLutHeight;
                float* t = &texels[(static_cast<size_t>(y) * kLutWidth + x) * 4u];
                t[0] = brightness * (0.2f + u);
                t[1] = brightness * (0.1f + 2.0f * v);
                t[2] = brightness * (1.3f - u + v);
                t[3] = 1.0f;
            }
        }
        return MakeTexture(kLutWidth, kLutHeight, texels);
    }

    // Stand-in HDRI with one mip and the worst horizon detail there is: every column alternates
    // between black and white, as a fence or a tree line on the horizon row would.
    TextureHandle MakeStripedHdri()
    {
        constexpr uint32_t kWidth = 256u;
        constexpr uint32_t kHeight = 8u;
        std::vector<float> texels(static_cast<size_t>(kWidth) * kHeight * 4u);
        for (uint32_t y = 0; y < kHeight; ++y)
        {
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                float* t = &texels[(static_cast<size_t>(y) * kWidth + x) * 4u];
                const float value = (x % 2u) == 0u ? 1.0f : 0.0f;
                t[0] = t[1] = t[2] = value;
                t[3] = 1.0f;
            }
        }
        return MakeTexture(kWidth, kHeight, texels);
    }

    TextureHandle MakeTexture(uint32_t width, uint32_t height, const std::vector<float>& texels)
    {
        TextureDesc td{};
        td.width = width;
        td.height = height;
        td.depth = 1u;
        td.mipLevels = 1u;
        td.arrayLayers = 1u;
        td.sampleCount = 1u;
        td.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferDst);
        td.debugName = "HeightFogSkyHorizonProbe.Backdrop";
        TextureHandle tex = m_Device->CreateTexture(td);
        m_Textures.push_back(tex);

        BufferDesc ud{};
        ud.size = texels.size() * sizeof(float);
        ud.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        ud.memoryUsage = BufferMemoryUsage::Upload;
        ud.debugName = "HeightFogSkyHorizonProbe.LutUpload";
        BufferHandle upload = m_Device->CreateBuffer(ud);
        m_Buffers.push_back(upload);
        void* mapped = m_Device->MapBuffer(upload);
        std::memcpy(mapped, texels.data(), ud.size);
        m_Device->UnmapBuffer(upload);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(upload, tex, 0u, 0u, width, height);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
                                                          ResourceState::ShaderResource));
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        return tex;
    }

    // Two vec4 per direction: the fog's colour, then the sky's.
    std::vector<float> Probe(TextureHandle lut, const HeightFogParamsUBO& fog, const Rendering::SkySettings& sky,
                             bool hdriSweep = false)
    {
        ProbePC pc{};
        pc.fogScaleNightOffsetCount[0] = fog.skyHorizonScale;
        pc.fogScaleNightOffsetCount[1] = fog.skyNightAmount;
        pc.fogScaleNightOffsetCount[2] = fog.skyAzimuthOffset;
        pc.fogScaleNightOffsetCount[3] = static_cast<float>(kDirections);
        pc.fogHorizonColor[0] = fog.skyHorizonColorR;
        pc.fogHorizonColor[1] = fog.skyHorizonColorG;
        pc.fogHorizonColor[2] = fog.skyHorizonColorB;
        pc.fogHorizonColor[3] = static_cast<float>(fog.colorSource);
        pc.skyExposureNight[0] = sky.exposureEV;
        pc.skyExposureNight[1] = sky.nightSkyBlend;
        pc.skyExposureNight[2] = hdriSweep ? 1.0f : 0.0f;
        for (int c = 0; c < 3; ++c)
            pc.skyNightHorizonColor[c] = sky.nightSkyHorizonColor[c];

        const size_t bytes = static_cast<size_t>(kDirections) * 8u * sizeof(float);
        BufferDesc od{};
        od.size = bytes;
        od.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                   static_cast<uint32_t>(BufferUsage::TransferSrc) |
                   static_cast<uint32_t>(BufferUsage::TransferDst);
        od.memoryUsage = BufferMemoryUsage::Readback;
        od.flags = BufferCreateFlags::PersistentlyMapped;
        od.debugName = "HeightFogSkyHorizonProbe.Out";
        BufferHandle outBuf = m_Device->CreateBuffer(od);
        m_Buffers.push_back(outBuf);
        const std::vector<float> poison(static_cast<size_t>(kDirections) * 8u, -1.0f);
        m_Device->UpdateBuffer(outBuf, 0, bytes, poison.data());

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "HeightFogSkyHorizonProbe.DS";
        DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        m_Device->UpdateCombinedImageSamplerBinding(ds, 0, lut, m_Sampler);
        m_Device->UpdateStorageBufferBinding(ds, 1, outBuf, 0, bytes);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->SetPushConstants(pc);
        cl->Dispatch((kDirections + 63u) / 64u, 1u, 1u);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();

        std::vector<float> out(static_cast<size_t>(kDirections) * 8u);
        void* mapped = m_Device->MapBuffer(outBuf);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(out.data(), mapped, bytes);
        m_Device->UnmapBuffer(outBuf);
        return out;
    }

    void ExpectFogMatchesSkyAt(float hours, float lutBrightness)
    {
        const Rendering::SkySettings sky = SkySettingsAt(hours);
        HeightFogParamsUBO fog{};
        Engine::Renderer::FillHeightFogSkyColor(SkyHorizonFog(), &sky, fog);
        ASSERT_EQ(fog.colorSource, static_cast<int32>(HeightFogColorSource::SkyViewLut))
            << "the default physical sky must hand the fog its backdrop at " << hours << " h";

        const std::vector<float> got = Probe(MakeSkyViewLut(lutBrightness), fog, sky);
        ASSERT_EQ(got.size(), static_cast<size_t>(kDirections) * 8u);
        EXPECT_NE(got[3], -1.0f) << "the probe never wrote: the dispatch did not reach the buffer";

        size_t mismatched = 0;
        for (uint32_t i = 0; i < kDirections; ++i)
        {
            const float* fogColor = &got[i * 8u];
            const float* skyColor = &got[i * 8u + 4u];
            for (int c = 0; c < 3; ++c)
            {
                const float tolerance = std::max(std::abs(skyColor[c]) * 1e-3f, 1e-6f);
                if (std::abs(fogColor[c] - skyColor[c]) > tolerance)
                {
                    if (mismatched < 4)
                    {
                        ADD_FAILURE() << hours << " h, direction " << i << " (elevation "
                                      << fogColor[3] << " rad, azimuth " << skyColor[3]
                                      << " rad), channel " << c << ": fog " << fogColor[c]
                                      << ", sky at the horizon " << skyColor[c];
                    }
                    ++mismatched;
                }
            }
        }
        EXPECT_EQ(mismatched, 0u) << mismatched << " channels of " << kDirections
                                  << " directions differ from the sky at " << hours << " h";
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    SamplerHandle m_Sampler{};
    std::vector<TextureHandle> m_Textures;
    std::vector<BufferHandle> m_Buffers;
};

} // namespace

// Mid-morning: no night composite, so the LUT's horizon row and the exposure carry the colour.
TEST_F(HeightFogSkyHorizonTest, FogColorIsTheSkyAtTheHorizonAtTenInTheMorning)
{
    ASSERT_EQ(SkySettingsAt(10.0f).nightSkyBlend, 0.0f) << "10 h must be full day for this arm";
    ExpectFogMatchesSkyAt(10.0f, 1.0f);
}

// Late evening: the night composite owns the horizon, so the fog must carry its blend and colour.
TEST_F(HeightFogSkyHorizonTest, FogColorIsTheSkyAtTheHorizonAtTenAtNight)
{
    ASSERT_GT(SkySettingsAt(22.0f).nightSkyBlend, 0.5f) << "22 h must be night for this arm";
    ExpectFogMatchesSkyAt(22.0f, 0.02f);
}

// An HDRI's horizon row carries the scene's detail. The fog must take the row's average around each
// compass direction, not one texel, or a fence on the horizon becomes vertical streaks in the haze.
TEST_F(HeightFogSkyHorizonTest, HdriFogAveragesTheHorizonRowsDetail)
{
    Rendering::SkySettings hdri{};
    hdri.environmentTexture = TextureHandle{1u, 1u};
    HeightFogParamsUBO fog{};
    Engine::Renderer::FillHeightFogSkyColor(SkyHorizonFog(), &hdri, fog);

    const std::vector<float> got = Probe(MakeStripedHdri(), fog, hdri, /*hdriSweep=*/true);
    ASSERT_EQ(got.size(), static_cast<size_t>(kDirections) * 8u);
    float largestStep = 0.0f;
    float sum = 0.0f;
    for (uint32_t i = 0; i < kDirections; ++i)
    {
        sum += got[i * 8u];
        if (i > 0)
            largestStep = std::max(largestStep, std::abs(got[i * 8u] - got[(i - 1u) * 8u]));
    }
    EXPECT_NEAR(sum / kDirections, 0.5f, 0.05f) << "the fog must carry the row's mean";
    EXPECT_LT(largestStep, 0.1f) << "neighbouring compass directions half a texel apart differ by "
                                 << largestStep << " on a row whose columns alternate 0 and 1";
}

// The other backdrops: an HDRI is sampled with its rotation and intensity, a gradient and the
// stylized ground's rim are a single colour at the horizon, and without a sky the fog keeps its
// fixed colour.
TEST(HeightFogSkyColor, FollowsEachSkyBackdropAndFallsBackToTheFixedColorWithoutOne)
{
    HeightFogParamsUBO fog{};
    Engine::Renderer::FillHeightFogSkyColor(SkyHorizonFog(), nullptr, fog);
    EXPECT_EQ(fog.colorSource, static_cast<int32>(HeightFogColorSource::Fixed));

    Rendering::SkySettings sky{};
    sky.exposureEV = 1.0f;
    sky.nightSkyBlend = 0.25f;

    PostProcessSettings fixedFog{};
    fixedFog.HeightFogColorMode = static_cast<int32>(Components::HeightFogColorMode::FixedColor);
    Engine::Renderer::FillHeightFogSkyColor(fixedFog, &sky, fog);
    EXPECT_EQ(fog.colorSource, static_cast<int32>(HeightFogColorSource::Fixed));

    Rendering::SkySettings hdri = sky;
    hdri.environmentTexture = TextureHandle{1u, 1u};
    hdri.hdriIntensity = 3.0f;
    hdri.skyboxRotationRadians = 0.7f;
    fog = {};
    Engine::Renderer::FillHeightFogSkyColor(SkyHorizonFog(), &hdri, fog);
    EXPECT_EQ(fog.colorSource, static_cast<int32>(HeightFogColorSource::SkyHdri));
    EXPECT_FLOAT_EQ(fog.skyHorizonScale, 6.0f);
    EXPECT_FLOAT_EQ(fog.skyAzimuthOffset, 0.7f);
    EXPECT_FLOAT_EQ(fog.skyNightAmount, 0.0f);

    Rendering::SkySettings gradient = sky;
    gradient.skyMode = static_cast<uint32_t>(Components::SkyMode::Gradient);
    gradient.gradientSkyHorizonColor[0] = 0.4f;
    gradient.gradientSkyHorizonColor[1] = 0.5f;
    gradient.gradientSkyHorizonColor[2] = 0.6f;
    fog = {};
    Engine::Renderer::FillHeightFogSkyColor(SkyHorizonFog(), &gradient, fog);
    EXPECT_EQ(fog.colorSource, static_cast<int32>(HeightFogColorSource::SkyConstant));
    EXPECT_FLOAT_EQ(fog.skyHorizonScale, 2.0f);
    EXPECT_FLOAT_EQ(fog.skyHorizonColorG, 0.5f);

    Rendering::SkySettings stylized = sky;
    stylized.belowHorizonMode = static_cast<uint32_t>(Rendering::SkyBelowHorizonMode::StylizedGround);
    stylized.groundHorizonColor[0] = 0.8f;
    stylized.groundHorizonNightColor[0] = 0.0f;
    fog = {};
    Engine::Renderer::FillHeightFogSkyColor(SkyHorizonFog(), &stylized, fog);
    EXPECT_EQ(fog.colorSource, static_cast<int32>(HeightFogColorSource::SkyConstant));
    EXPECT_FLOAT_EQ(fog.skyHorizonColorR, 0.6f) << "the rim at a quarter night: 0.8 toward 0";
}

// A new fog takes its colour from the sky, and so does the frame no volume covers.
TEST(HeightFogSkyColor, ANewFogTakesItsColorFromTheSky)
{
    EXPECT_EQ(Components::HeightFogEffect{}.ColorMode, Components::HeightFogColorMode::SkyHorizon);
    EXPECT_EQ(PostProcessSettings{}.HeightFogColorMode, static_cast<int32>(Components::HeightFogColorMode::SkyHorizon));
}

// A preset and the time-of-day keyframes replace the authored shape, but not where the colour comes
// from: the colour mode is the user's choice either way, including the non-default one.
TEST(HeightFogSkyColor, PresetsAndTimeOfDayKeyframesKeepTheColorMode)
{
    Components::HeightFogEffect keyframed{};
    keyframed.ColorMode = Components::HeightFogColorMode::FixedColor;
    Components::ApplyHeightFogTimeOfDay(keyframed, 19.0f);
    EXPECT_EQ(keyframed.ColorMode, Components::HeightFogColorMode::FixedColor);

    Components::HeightFogEffect preset{};
    preset.ColorMode = Components::HeightFogColorMode::FixedColor;
    Components::ApplyHeightFogPreset(preset, Components::HeightFogPreset::GroundMist);
    EXPECT_EQ(preset.ColorMode, Components::HeightFogColorMode::FixedColor);
    EXPECT_EQ(preset.Preset, Components::HeightFogPreset::GroundMist);
}
