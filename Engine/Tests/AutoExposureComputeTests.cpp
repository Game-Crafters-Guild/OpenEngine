// Execute the shipping auto-exposure shaders (auto_exposure_histogram.comp, auto_exposure_resolve.comp,
// pre_exposure_snapshot.comp) on synthetic frames: the histogram pass's split of the frame into
// geometry and sky by depth and its read of colour stored at the pre-exposure, the resolve's metering
// rule (the geometry is metered; the sky holds the exposure back only when it is a large share of the
// frame and more than two stops over the key; an all-sky frame meters the sky), the lights meter (the
// resolve takes the incident reading in place of the histograms, under the same clamp, compensation
// and smoothing), and the snapshot's choice of the pre-exposure. The rules are stated in the shaders;
// the expectations below restate them numerically.
#include "Components/Rendering/ExposureCompensationCurve.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/IncidentLightMeter.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestDeviceHelper.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kBinCount = 256u;
constexpr uint32_t kSkyOffset = kBinCount;
constexpr uint32_t kHistogramWords = 2u * kBinCount;
constexpr size_t kHistogramBytes = kHistogramWords * sizeof(uint32_t);
constexpr size_t kStateBytes = 16;
constexpr float kMinLogLum = -24.0f; // auto_exposure_histogram.comp
constexpr float kLogLumRange = 38.0f;
constexpr float kKey = 0.18f;

// log2 luminance at the centre of histogram bin `bin` (1..255), as the resolve reads it.
float BinLog(uint32_t bin)
{
    return kMinLogLum + (float(bin - 1u) / float(kBinCount - 2u)) * kLogLumRange;
}

// The ExposureHistory state the snapshot reads and writes (ExposureReadbackFeature::State).
struct SnapshotState
{
    float ExposureScale = 0.0f;
    uint32_t Valid = 0u;
    float PreExposure = 0.0f;
    float PreviousPreExposure = 0.0f;
};
static_assert(sizeof(SnapshotState) == kStateBytes);

struct Program
{
    ShaderMeta Meta;
    DescriptorSetLayoutDesc Layout;
    PipelineHandle Pipeline;
};

class AutoExposureComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            GTEST_SKIP() << "Vulkan device unavailable";
        Sampler = Device->CreateSampler(SamplerDesc::PointClamp("AutoExposure.Test"));
        Histogram = DeviceBuffer(kHistogramBytes);
        State = DeviceBuffer(kStateBytes);
        Compensation = DeviceBuffer(kStateBytes);
        HistogramExposure = DeviceBuffer(kStateBytes);
    }
    void TearDown() override
    {
        if (!Device)
            return;
        Device->WaitForIdle();
        for (auto t : Textures)
            Device->DestroyTexture(t);
        for (auto b : Buffers)
            Device->DestroyBuffer(b);
        Device->Shutdown();
    }

    BufferHandle DeviceBuffer(size_t bytes)
    {
        BufferDesc d{};
        d.size = bytes;
        d.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferSrc) |
                  static_cast<uint32_t>(BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::DeviceLocal;
        auto b = Device->CreateBuffer(d);
        EXPECT_TRUE(b.IsValid());
        Buffers.push_back(b);
        return b;
    }
    BufferHandle StagingWith(const void* data, size_t bytes)
    {
        BufferDesc d{};
        d.size = bytes;
        d.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        d.memoryUsage = BufferMemoryUsage::Upload;
        auto b = Device->CreateBuffer(d);
        Buffers.push_back(b);
        void* p = Device->MapBuffer(b);
        EXPECT_NE(p, nullptr);
        if (p)
        {
            std::memcpy(p, data, bytes);
            Device->UnmapBuffer(b);
        }
        return b;
    }
    void Submit(CommandList* cl)
    {
        cl->End();
        Device->ExecuteCommandLists({cl});
        Device->WaitForIdle();
    }
    void WriteBuffer(BufferHandle dst, const void* data, size_t bytes)
    {
        auto staging = StagingWith(data, bytes);
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBuffer(staging, dst, bytes);
        cl->Barrier(ResourceBarrier::CreateBufferBarrier(dst, ResourceState::CopyDest, ResourceState::UnorderedAccess));
        Submit(cl.get());
    }
    std::vector<uint32_t> ReadWords(BufferHandle src, size_t bytes)
    {
        auto readback = Device->CreateReadbackBuffer(bytes);
        Buffers.push_back(readback);
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateBufferBarrier(src, ResourceState::UnorderedAccess, ResourceState::CopySource));
        cl->CopyBuffer(src, readback, bytes);
        Submit(cl.get());
        std::vector<uint32_t> words(bytes / 4);
        const void* p = Device->MapBuffer(readback);
        EXPECT_NE(p, nullptr);
        if (p)
        {
            std::memcpy(words.data(), p, bytes);
            Device->UnmapBuffer(readback);
        }
        return words;
    }
    TextureHandle Texture(uint32_t w, uint32_t h, TextureFormat format, const std::vector<float>& texels)
    {
        TextureDesc d{};
        d.width = w;
        d.height = h;
        d.depth = d.arrayLayers = d.sampleCount = d.mipLevels = 1;
        d.format = static_cast<uint32_t>(format);
        d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        auto t = Device->CreateTexture(d);
        EXPECT_TRUE(t.IsValid());
        Textures.push_back(t);
        auto staging = StagingWith(texels.data(), texels.size() * sizeof(float));
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(t, ResourceState::Undefined, ResourceState::CopyDest));
        cl->CopyBufferToTextureSubresource(staging, t, 0, 0, w, h);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(t, ResourceState::CopyDest, ResourceState::ShaderResource));
        Submit(cl.get());
        return t;
    }
    Program& Load(const char* name)
    {
        auto& program = Programs[name];
        if (program.Pipeline.IsValid())
            return program;
        ShaderPackage pkg{};
        std::string err;
        EXPECT_TRUE(LoadShaderPkg(std::string("Shaders/") + name + ".shaderpkg", Device->PreferredShaderSource(), pkg, &err))
            << err;
        program.Meta = std::move(pkg.meta);
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(pkg.stageBytes.at("cs"));
        auto patch = [&](uint32_t set, DescriptorSetLayoutDesc& layout)
        { if (set == 0) program.Layout = layout; };
        MaterialHelper::ApplyShaderMetaToComputeDesc(*Device, program.Meta, cd, MaterialBuilder::MergeMode::Auto,
                                                     {true, 128}, patch, &err);
        program.Pipeline = Device->GetOrCreateComputePipeline(Device->InternComputePipeline(std::move(cd)));
        EXPECT_TRUE(program.Pipeline.IsValid()) << name << ": " << err;
        return program;
    }

    // One histogram pass over an RGBA32F colour frame and an R32F depth frame (0 = far plane).
    // Returns both histograms: [0, 256) geometry, [256, 512) sky.
    std::vector<uint32_t> RunHistogram(uint32_t w, uint32_t h, const std::vector<float>& rgba,
                                       const std::vector<float>& depth)
    {
        return RunHistogram(w, h, rgba, w, h, depth);
    }
    // The depth may be smaller than the colour (auto exposure meters after the upscale while the
    // depth stays at the internal resolution); pixels map by normalized position.
    std::vector<uint32_t> RunHistogram(uint32_t w, uint32_t h, const std::vector<float>& rgba,
                                       uint32_t depthW, uint32_t depthH, const std::vector<float>& depth)
    {
        auto colour = Texture(w, h, TextureFormat::R32G32B32A32_FLOAT, rgba);
        auto depthTexture = Texture(depthW, depthH, TextureFormat::R32_FLOAT, depth);
        const std::vector<uint32_t> zeros(kHistogramWords, 0u);
        WriteBuffer(Histogram, zeros.data(), kHistogramBytes);
        const float exposureState[4] = {0.0f, 0.0f, HistogramPreExposure, HistogramPreExposure};
        WriteBuffer(HistogramExposure, exposureState, kStateBytes);

        Program& program = Load("auto_exposure_histogram");
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        DescriptorSetDesc dd{};
        dd.layout = program.Layout;
        dd.transient = true;
        auto ds = Device->CreateDescriptorSet(dd);
        NamedDescriptorWriter writer(Device.get(), ds, program.Meta, 0);
        writer.AddCombinedImageSampler("uHDRColor", colour, Sampler);
        writer.AddCombinedImageSampler("uSceneDepth", depthTexture, Sampler);
        writer.AddStorageBuffer("HistogramBuffer", Histogram, 0, kHistogramBytes);
        writer.AddStorageBuffer("uExposure", HistogramExposure, 0, kStateBytes);
        writer.Flush();
        cl->SetPipeline(program.Pipeline);
        NamedPushConstantWriter pc(program.Meta, program.Meta.PushConstants[0].Name);
        pc.Add("pixelCountX", w);
        pc.Add("pixelCountY", h);
        pc.Flush(cl.get());
        cl->BindDescriptorSet(0, ds, program.Pipeline);
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Submit(cl.get());
        return ReadWords(Histogram, kHistogramBytes);
    }

    // One resolve pass over the current histogram buffer from a first-frame state (snaps to the
    // target), with a key of 0.18 and clamps wide enough never to bind. Returns the LINEAR scale.
    float RunResolve()
    {
        return RunResolveWith([](NamedPushConstantWriter& pc)
                              {
                                  pc.Add("deltaTime", 0.0f);
                                  pc.Add("speedUp", 1.0f);
                                  pc.Add("speedDown", 3.0f);
                                  pc.Add("exposureKey", kKey);
                                  pc.Add("minExposure", 1.0e-12f);
                                  pc.Add("maxExposure", 1.0e12f);
                              });
    }

    // One resolve pass with the push constants AutoExposureNode writes: the settings' key, clamps
    // and speeds (PostProcessSettings::TryWriteField), the meter, the lights reading and dt. Starts
    // from `previousScale` as an adapted state, or from a first frame (a snap) when it is 0.
    float RunNodeResolve(const GameEngine::Engine::Renderer::PostProcessSettings& settings,
                         float lightsLogLuminance, float deltaTime, float previousScale = 0.0f)
    {
        return RunResolveWith(
            [&](NamedPushConstantWriter& pc)
            {
                for (const auto& member : Programs["auto_exposure_resolve"].Meta.PushConstants[0].Block.Members)
                {
                    if (member.Name == "deltaTime")
                        pc.Add("deltaTime", deltaTime);
                    else if (member.Name == "exposureMeter")
                        pc.Add("exposureMeter", static_cast<uint32_t>(settings.AutoExposureMeter));
                    else if (member.Name == "lightsLogLuminance")
                        pc.Add("lightsLogLuminance", lightsLogLuminance);
                    else
                        EXPECT_TRUE(settings.TryWriteField(member.Name, pc)) << member.Name;
                }
            },
            previousScale);
    }

    template <typename WritePushConstants>
    float RunResolveWith(WritePushConstants&& writePushConstants, float previousScale = 0.0f)
    {
        uint32_t state[4] = {0u, previousScale > 0.0f ? 1u : 0u, 0u, 0u};
        std::memcpy(&state[0], &previousScale, sizeof(float));
        WriteBuffer(State, state, kStateBytes);
        Program& program = Load("auto_exposure_resolve");
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        DescriptorSetDesc dd{};
        dd.layout = program.Layout;
        dd.transient = true;
        auto ds = Device->CreateDescriptorSet(dd);
        NamedDescriptorWriter writer(Device.get(), ds, program.Meta, 0);
        writer.AddStorageBuffer("HistogramBuffer", Histogram, 0, kHistogramBytes);
        writer.AddStorageBuffer("state", State, 0, kStateBytes);
        writer.AddStorageBuffer("uCompensation", Compensation, 0, kStateBytes);
        writer.Flush();
        cl->SetPipeline(program.Pipeline);
        NamedPushConstantWriter pc(program.Meta, program.Meta.PushConstants[0].Name);
        writePushConstants(pc);
        pc.Flush(cl.get());
        cl->BindDescriptorSet(0, ds, program.Pipeline);
        cl->Dispatch(1, 1, 1);
        Submit(cl.get());
        const auto words = ReadWords(State, kStateBytes);
        float scale = 0.0f;
        std::memcpy(&scale, words.data(), sizeof(float));
        return scale;
    }

    // Resolve a histogram given as {bin, count} entries for geometry and sky. Returns the metered
    // log2 luminance: log2(key / scale).
    float MeterBins(const std::vector<std::pair<uint32_t, uint32_t>>& geometry,
                    const std::vector<std::pair<uint32_t, uint32_t>>& sky)
    {
        std::vector<uint32_t> words(kHistogramWords, 0u);
        for (const auto& [bin, count] : geometry)
            words[bin] += count;
        for (const auto& [bin, count] : sky)
            words[kSkyOffset + bin] += count;
        WriteBuffer(Histogram, words.data(), kHistogramBytes);
        const float scale = RunResolve();
        EXPECT_GT(scale, 0.0f);
        return std::log2(kKey / scale);
    }

    // A moonless night, metered: a 20x20 frame whose first 40 % of texels are ground (geometry) at 2^-22
    // and the rest sky (the far plane) at 2^-20, with every 25th sky texel a star at 2^-8 when
    // `withStars`. Returns the metered log2 luminance.
    float MeterMoonlessNight(bool withStars)
    {
        constexpr uint32_t w = 20, h = 20;
        std::vector<float> rgba(size_t(w) * h * 4), depth(size_t(w) * h);
        for (uint32_t i = 0; i < w * h; ++i)
        {
            const bool isGround = i < w * h * 4 / 10;
            const bool isStar = withStars && !isGround && i % 25 == 0;
            const float lum = isGround ? std::exp2(-22.0f) : (isStar ? std::exp2(-8.0f) : std::exp2(-20.0f));
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = lum;
            rgba[i * 4 + 3] = 1.0f;
            depth[i] = isGround ? 0.5f : 0.0f;
        }
        RunHistogram(w, h, rgba, depth);
        return std::log2(kKey / RunResolve());
    }

    // One snapshot dispatch over `state` with the view's static scale and auto-exposure flag.
    // Returns the state it leaves.
    SnapshotState RunSnapshot(const SnapshotState& state, float staticScale, int useAutoExposure)
    {
        WriteBuffer(State, &state, kStateBytes);
        Program& program = Load("pre_exposure_snapshot");
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        DescriptorSetDesc dd{};
        dd.layout = program.Layout;
        dd.transient = true;
        auto ds = Device->CreateDescriptorSet(dd);
        NamedDescriptorWriter writer(Device.get(), ds, program.Meta, 0);
        writer.AddStorageBuffer("state", State, 0, kStateBytes);
        writer.Flush();
        cl->SetPipeline(program.Pipeline);
        NamedPushConstantWriter pc(program.Meta, program.Meta.PushConstants[0].Name);
        pc.Add("exposure", staticScale);
        pc.Add("useAutoExposure", useAutoExposure);
        pc.Flush(cl.get());
        cl->BindDescriptorSet(0, ds, program.Pipeline);
        cl->Dispatch(1, 1, 1);
        Submit(cl.get());
        const auto out = ReadWords(State, kStateBytes);
        SnapshotState result{};
        std::memcpy(&result, out.data(), kStateBytes);
        return result;
    }

    std::unique_ptr<IDevice> Device;
    SamplerHandle Sampler;
    BufferHandle Histogram;
    BufferHandle State;
    BufferHandle Compensation;
    // The ExposureHistory the histogram pass reads its colour's pre-exposure from.
    BufferHandle HistogramExposure;
    float HistogramPreExposure = 0.0f; // 0: no snapshot, read as 1
    std::vector<BufferHandle> Buffers;
    std::vector<TextureHandle> Textures;
    std::map<std::string, Program> Programs;
};

// A geometry bin in the middle of the range (log2 luminance about -2.2); sky bins sit a whole number
// of bins above it, about 0.15 stops per bin.
constexpr uint32_t kGeometryBin = 147u;

uint32_t BinsForStops(float stops)
{
    return static_cast<uint32_t>(std::lround(stops * float(kBinCount - 2u) / kLogLumRange));
}
} // namespace

TEST_F(AutoExposureComputeTest, HistogramSplitsFarPlanePixelsIntoTheSkyHistogram)
{
    // 16x16: the top 10 rows are sky (depth 0, luminance 8), the rest geometry (depth 0.25,
    // luminance 0.5). Both are grey, so Rec.709 luminance equals the channel value.
    constexpr uint32_t w = 16, h = 16, skyRows = 10;
    std::vector<float> rgba(size_t(w) * h * 4), depth(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            const bool sky = y < skyRows;
            const float lum = sky ? 8.0f : 0.5f;
            const size_t i = size_t(y) * w + x;
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = lum;
            rgba[i * 4 + 3] = 1.0f;
            depth[i] = sky ? 0.0f : 0.25f;
        }
    const auto words = RunHistogram(w, h, rgba, depth);
    ASSERT_EQ(words.size(), size_t(kHistogramWords));

    uint64_t geometry = 0, sky = 0;
    uint32_t geometryPeak = 0, skyPeak = 0;
    for (uint32_t b = 0; b < kBinCount; ++b)
    {
        geometry += words[b];
        sky += words[kSkyOffset + b];
        if (words[b] > words[geometryPeak])
            geometryPeak = b;
        if (words[kSkyOffset + b] > words[kSkyOffset + skyPeak])
            skyPeak = b;
    }
    EXPECT_EQ(geometry, uint64_t(w) * (h - skyRows)) << "every depth > 0 pixel is geometry";
    EXPECT_EQ(sky, uint64_t(w) * skyRows) << "every far-plane pixel is sky";
    EXPECT_NEAR(BinLog(geometryPeak), -1.0f, kLogLumRange / 254.0f);
    EXPECT_NEAR(BinLog(skyPeak), 3.0f, kLogLumRange / 254.0f);
}

TEST_F(AutoExposureComputeTest, HalfSizeDepthMapsByNormalizedPosition)
{
    // Colour 16x16, depth 8x8 (half size): the top half of the frame is sky in both. Mapping the
    // colour's texel coordinates straight into the depth would read depth rows 4..7 (geometry) for
    // colour rows 4..7 (sky) and miscount a quarter of the frame.
    constexpr uint32_t w = 16, h = 16, dw = 8, dh = 8;
    std::vector<float> rgba(size_t(w) * h * 4), depth(size_t(dw) * dh);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * w + x;
            const float lum = y < h / 2 ? 8.0f : 0.5f;
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = lum;
            rgba[i * 4 + 3] = 1.0f;
        }
    for (uint32_t y = 0; y < dh; ++y)
        for (uint32_t x = 0; x < dw; ++x)
            depth[size_t(y) * dw + x] = y < dh / 2 ? 0.0f : 0.25f;
    const auto words = RunHistogram(w, h, rgba, dw, dh, depth);
    uint64_t geometry = 0, sky = 0;
    for (uint32_t b = 0; b < kBinCount; ++b)
    {
        geometry += words[b];
        sky += words[kSkyOffset + b];
    }
    EXPECT_EQ(sky, uint64_t(w) * h / 2);
    EXPECT_EQ(geometry, uint64_t(w) * h / 2);
}

TEST_F(AutoExposureComputeTest, SkyFillingMostOfTheFrameNoLongerDarkensTheScene)
{
    // The viewer's frame: 90 % sky one and a half stops brighter than the scene. Under the two-stop
    // headroom the sky does not move the meter: the scene is keyed to mid-grey.
    constexpr uint32_t w = 20, h = 20;
    const float scene = 0.25f, skyLum = 0.25f * std::exp2(1.5f);
    std::vector<float> rgba(size_t(w) * h * 4), depth(size_t(w) * h);
    for (uint32_t i = 0; i < w * h; ++i)
    {
        const bool sky = i >= w * h / 10; // first 10 % geometry
        const float lum = sky ? skyLum : scene;
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = lum;
        rgba[i * 4 + 3] = 1.0f;
        depth[i] = sky ? 0.0f : 0.5f;
    }
    RunHistogram(w, h, rgba, depth);
    const float metered = std::log2(kKey / RunResolve());
    EXPECT_NEAR(metered, std::log2(scene), 0.2f) << "the scene is metered, not the sky";
}

TEST_F(AutoExposureComputeTest, SmallSkyShareDoesNotPullTheMeter)
{
    // A window of daylight in a dark room: 8 % of the frame, six stops over the room. Under the 10 %
    // share threshold the sky never holds the exposure back.
    const float metered = MeterBins({{kGeometryBin, 920}}, {{kGeometryBin + BinsForStops(6.0f), 80}});
    EXPECT_NEAR(metered, BinLog(kGeometryBin), 1e-3f);
}

TEST_F(AutoExposureComputeTest, LargeBrightSkySitsTwoStopsOverTheKey)
{
    // Half the frame is sky five stops over the scene: the full pull leaves the sky's average exactly
    // two stops over the metered key.
    const uint32_t skyBin = kGeometryBin + BinsForStops(5.0f);
    const float metered = MeterBins({{kGeometryBin, 500}}, {{skyBin, 500}});
    EXPECT_NEAR(metered, BinLog(skyBin) - 2.0f, 1e-3f);
}

TEST_F(AutoExposureComputeTest, SkyPullScalesWithItsShareOfTheFrame)
{
    // 20 % sky sits halfway up the 10-30 % ramp: half the pull.
    const uint32_t skyBin = kGeometryBin + BinsForStops(5.0f);
    const float g = BinLog(kGeometryBin), s = BinLog(skyBin);
    const float metered = MeterBins({{kGeometryBin, 800}}, {{skyBin, 200}});
    EXPECT_NEAR(metered, g + 0.5f * (s - 2.0f - g), 1e-3f);
}

TEST_F(AutoExposureComputeTest, SkyWithinTheHeadroomDoesNotPull)
{
    // Half the frame is sky 1.9 stops over the scene: inside the two-stop headroom, no pull.
    const float metered = MeterBins({{kGeometryBin, 500}}, {{kGeometryBin + BinsForStops(1.9f), 500}});
    EXPECT_NEAR(metered, BinLog(kGeometryBin), 1e-3f);
}

TEST_F(AutoExposureComputeTest, AllSkyFrameMetersTheSky)
{
    // Looking up into open sky: no geometry, the sky is metered as before the split.
    const uint32_t skyBin = kGeometryBin + BinsForStops(4.0f);
    EXPECT_NEAR(MeterBins({}, {{skyBin, 1000}}), BinLog(skyBin), 1e-3f);
}

TEST_F(AutoExposureComputeTest, GeometryUnderFivePercentBlendsTowardTheSky)
{
    // 2.5 % geometry is halfway up the 0-5 % smoothstep: the geometry estimate is halfway to the sky.
    // The sky is one stop over the scene, inside the headroom, so the ceiling does not act.
    const uint32_t skyBin = kGeometryBin + BinsForStops(1.0f);
    const float g = BinLog(kGeometryBin), s = BinLog(skyBin);
    const float metered = MeterBins({{kGeometryBin, 25}}, {{skyBin, 975}});
    EXPECT_NEAR(metered, s + 0.5f * (g - s), 1e-3f);
}

namespace
{
// A camera in Auto with the lights meter and the given Min/Max EV, compensation 0.
GameEngine::Engine::Renderer::PostProcessSettings LightsMeterSettings(float minEv, float maxEv)
{
    GameEngine::Engine::Renderer::PostProcessSettings settings{};
    settings.AutoExposureActive = true;
    settings.AutoExposureMeter = static_cast<GameEngine::int32>(GameEngine::Components::ExposureMeter::Lights);
    settings.AutoExposureMinEv = minEv;
    settings.AutoExposureMaxEv = maxEv;
    return settings;
}

float LightsLogLuminance(float ev100)
{
    return GameEngine::Rendering::MeteredLogLuminance(ev100);
}

// Relative agreement of two linear scales, in stops.
float StopsApart(float a, float b)
{
    return std::fabs(std::log2(a) - std::log2(b));
}
} // namespace

// The lights meter ignores the frame: a histogram full of bright geometry does not move it, and the
// resolve lands exactly the exposure a Manual camera at the reading's EV gives.
TEST_F(AutoExposureComputeTest, TheLightsMeterIgnoresTheHistogramAndMetersTheReading)
{
    std::vector<uint32_t> words(kHistogramWords, 0u);
    words[kGeometryBin + BinsForStops(6.0f)] = 1000u;
    WriteBuffer(Histogram, words.data(), kHistogramBytes);
    const float scale = RunNodeResolve(LightsMeterSettings(-5.0f, 20.0f), LightsLogLuminance(12.0f), 0.0f);
    EXPECT_LT(StopsApart(scale, EvToLinearExposure(12.0f)), 1e-3f);
}

// The picture meter, given the same settings and the same reading, meters the histogram instead.
TEST_F(AutoExposureComputeTest, ThePictureMeterIgnoresTheLightsReading)
{
    std::vector<uint32_t> words(kHistogramWords, 0u);
    words[kGeometryBin] = 1000u;
    WriteBuffer(Histogram, words.data(), kHistogramBytes);
    auto settings = LightsMeterSettings(-5.0f, 20.0f);
    settings.AutoExposureMeter = static_cast<GameEngine::int32>(GameEngine::Components::ExposureMeter::Picture);
    const float scale = RunNodeResolve(settings, LightsLogLuminance(12.0f), 0.0f);
    EXPECT_NEAR(std::log2(kKey / scale), BinLog(kGeometryBin), 1e-3f);
}

// A moonlit reading (EV100 -3) under a floor of 4 is held at EV 4; under a floor of -5 it is exposed as
// read.
TEST_F(AutoExposureComputeTest, TheLightsReadingIsClampedByMinAndMaxEv)
{
    const float night = LightsLogLuminance(-3.0f);
    EXPECT_LT(StopsApart(RunNodeResolve(LightsMeterSettings(4.0f, 18.0f), night, 0.0f), EvToLinearExposure(4.0f)),
              1e-3f);
    EXPECT_LT(StopsApart(RunNodeResolve(LightsMeterSettings(-5.0f, 20.0f), night, 0.0f), EvToLinearExposure(-3.0f)),
              1e-3f);
    // A reading brighter than the ceiling is held at the ceiling.
    EXPECT_LT(StopsApart(RunNodeResolve(LightsMeterSettings(4.0f, 14.0f), LightsLogLuminance(15.3f), 0.0f),
                         EvToLinearExposure(14.0f)),
              1e-3f);
}

// A moonless night under the lights meter (EV100 -12) is exposed as read under a floor of -14: the
// resolve's divide-by-zero guard sits below every reading the window can hold.
TEST_F(AutoExposureComputeTest, TheLightsMeterExposesAMoonlessNightUnderALowFloor)
{
    EXPECT_LT(StopsApart(RunNodeResolve(LightsMeterSettings(-14.0f, 18.0f), LightsLogLuminance(-12.0f), 0.0f),
                         EvToLinearExposure(-12.0f)),
              1e-3f);
}

// Compensation applies on top of the lights reading as on the picture: +1 stop doubles the scale.
TEST_F(AutoExposureComputeTest, CompensationAppliesOnTopOfTheLightsReading)
{
    auto settings = LightsMeterSettings(-5.0f, 20.0f);
    settings.AutoExposureBiasEv = 1.0f;
    const float scale = RunNodeResolve(settings, LightsLogLuminance(12.0f), 0.0f);
    EXPECT_LT(StopsApart(scale, 2.0f * EvToLinearExposure(12.0f)), 1e-3f);
}

// The adaptation speeds smooth the lights meter toward a new reading exactly as they smooth the
// picture: one 0.1 s frame at Speed Down 3 covers 1 - 2^-0.3 of the way in stops.
TEST_F(AutoExposureComputeTest, TheSpeedsSmoothTheLightsMeterTowardANewReading)
{
    const auto settings = LightsMeterSettings(-5.0f, 20.0f);
    const float before = EvToLinearExposure(12.0f);
    const float scale = RunNodeResolve(settings, LightsLogLuminance(14.0f), 0.1f, before);
    const float expectedStops = -2.0f * (1.0f - std::exp2(-0.1f * settings.AutoExposureSpeedDown));
    EXPECT_NEAR(std::log2(scale / before), expectedStops, 1e-3f);
}

TEST_F(AutoExposureComputeTest, EmptyFrameMetersTheWindowFloor)
{
    // Only near-black pixels (bin 0, excluded from metering): the meter reads the window floor.
    EXPECT_NEAR(MeterBins({{0u, 1000}}, {{0u, 1000}}), kMinLogLum, 1e-3f);
}

// A moonless night is metered from its sky and its ground, not from its stars: the ground sits near
// 2^-22 and the sky near 2^-20 (0.0002 nits), and both land inside the window.
TEST_F(AutoExposureComputeTest, AMoonlessNightMetersItsSkyAndGroundNotItsStars)
{
    EXPECT_NEAR(MeterMoonlessNight(true), -22.0f, 0.5f)
        << "a moonless night must meter its ground (2^-22), not its stars (2^-8)";
}

// The stars do not move the meter: they draw no depth (SkyRenderer's sky and star pipelines), so they
// fall in the sky histogram, whose top tenth the meter ignores. A moonless night meters the same with
// and without them.
TEST_F(AutoExposureComputeTest, StarsAloneDoNotMoveTheMeter)
{
    EXPECT_NEAR(MeterMoonlessNight(true), MeterMoonlessNight(false), 0.15f)
        << "the stars set the reading of a moonless night";
}

TEST_F(AutoExposureComputeTest, HistogramBinsAbsoluteLuminanceFromPreExposedColour)
{
    // The same scene stored at P = 1 and at P = 16 (four stops up, as the forward passes store it):
    // the histogram divides by P, so both land in the same absolute bins.
    constexpr uint32_t w = 8, h = 8;
    const float lum = 0.5f;
    auto frame = [&](float preExposure)
    {
        std::vector<float> rgba(size_t(w) * h * 4), depth(size_t(w) * h, 0.25f);
        for (uint32_t i = 0; i < w * h; ++i)
        {
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = lum * preExposure;
            rgba[i * 4 + 3] = 1.0f;
        }
        HistogramPreExposure = preExposure;
        return RunHistogram(w, h, rgba, depth);
    };
    const auto absolute = frame(1.0f);
    const auto preExposed = frame(16.0f);
    uint32_t peak = 0;
    for (uint32_t b = 0; b < kBinCount; ++b)
        if (absolute[b] > absolute[peak])
            peak = b;
    EXPECT_NEAR(BinLog(peak), std::log2(lum), kLogLumRange / 254.0f);
    EXPECT_EQ(preExposed, absolute) << "colour stored at P = 16 must meter as the same absolute scene";
}

// The snapshot's rule (pre_exposure_snapshot.comp): P is the view's exposure rounded to a power of two.
TEST_F(AutoExposureComputeTest, SnapshotFirstFrameSnapsToTheRoundedStaticScale)
{
    // A zero-filled buffer (the first frame): no valid history, so the static scale, log2(3) = 1.58
    // rounded to 2: P = 4. The previous P equals it, so the history ratio is 1.
    const auto out = RunSnapshot({0.0f, 0u, 0.0f, 0.0f}, 3.0f, /*useAutoExposure=*/1);
    EXPECT_EQ(out.PreExposure, 4.0f);
    EXPECT_EQ(out.PreviousPreExposure, 4.0f);
    EXPECT_EQ(out.ExposureScale, 0.0f) << "the snapshot never writes the adapted exposure";
}

TEST_F(AutoExposureComputeTest, SnapshotUnderManualExposureUsesTheStaticScaleEveryFrame)
{
    // A valid adapted history is ignored under manual exposure: log2(0.3) = -1.74 rounds to -2.
    const auto out = RunSnapshot({100.0f, 1u, 8.0f, 8.0f}, 0.3f, /*useAutoExposure=*/0);
    EXPECT_EQ(out.PreExposure, 0.25f);
    EXPECT_EQ(out.PreviousPreExposure, 8.0f) << "last frame's P carries into previousPreExposure";
}

TEST_F(AutoExposureComputeTest, SnapshotUnderAutoExposureUsesLastFramesAdaptedScale)
{
    // log2(100) = 6.64 rounds to 7: P = 128.
    const auto out = RunSnapshot({100.0f, 1u, 4.0f, 2.0f}, 1.0f, /*useAutoExposure=*/1);
    EXPECT_EQ(out.PreExposure, 128.0f);
    EXPECT_EQ(out.PreviousPreExposure, 4.0f);
}

TEST_F(AutoExposureComputeTest, SnapshotSnapsOnAnUnusableHistoryLikeTheResolve)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(RunSnapshot({nan, 1u, 4.0f, 4.0f}, 2.0f, 1).PreExposure, 2.0f) << "a NaN history snaps";
    EXPECT_EQ(RunSnapshot({-1.0f, 1u, 4.0f, 4.0f}, 2.0f, 1).PreExposure, 2.0f) << "a negative history snaps";
    const auto badPrevious = RunSnapshot({8.0f, 1u, nan, 0.0f}, 1.0f, 1);
    EXPECT_EQ(badPrevious.PreExposure, 8.0f);
    EXPECT_EQ(badPrevious.PreviousPreExposure, 8.0f) << "an unusable previous P starts the history ratio at 1";
}

TEST_F(AutoExposureComputeTest, SnapshotWithoutAStaticScaleStoresAbsoluteColour)
{
    EXPECT_EQ(RunSnapshot({0.0f, 0u, 0.0f, 0.0f}, 0.0f, 0).PreExposure, 1.0f);
}

TEST_F(AutoExposureComputeTest, SnapshotWithTheShippedPinIsTheIdentity)
{
    // The shipped graphs' PreExposureSnapshot pass passes exposure 1 and auto exposure off, whatever
    // the view meters: P stays 1, so every multiply and divide by it is exact.
    const auto out = RunSnapshot({0.02f, 1u, 1.0f, 1.0f}, 1.0f, 0);
    EXPECT_EQ(out.PreExposure, 1.0f);
    EXPECT_EQ(out.PreviousPreExposure, 1.0f);
}

namespace
{
// A camera in Auto with the picture meter, an open envelope and the given compensation curve.
GameEngine::Engine::Renderer::PostProcessSettings PictureMeterSettings(const GameEngine::Math::Curve& curve)
{
    GameEngine::Engine::Renderer::PostProcessSettings settings{};
    settings.AutoExposureActive = true;
    settings.AutoExposureMinEv = -14.0f;
    settings.AutoExposureMaxEv = 20.0f;
    settings.AutoExposureCompensation = GameEngine::Rendering::SampleExposureCompensation(curve);
    return settings;
}

// The scene EV100 the resolve meters from a log2 luminance (the inverse of MeteredLogLuminance).
float MeteredEv(float logLuminance)
{
    return logLuminance - GameEngine::Rendering::MeteredLogLuminance(0.0f);
}
} // namespace

// A dark frame (a moonlit ground) meters at its reading and the default curve darkens it by the
// curve's value there: the exposure is the Manual exposure at the reading, compensated.
TEST_F(AutoExposureComputeTest, TheCompensationCurveDarkensADarkHistogramByItsValueAtTheReading)
{
    const uint32_t nightBin = kGeometryBin - BinsForStops(11.0f);
    std::vector<uint32_t> words(kHistogramWords, 0u);
    words[nightBin] = 1000u;
    WriteBuffer(Histogram, words.data(), kHistogramBytes);
    const GameEngine::Math::Curve curve = GameEngine::Components::DefaultExposureCompensationCurve();
    const float ev = MeteredEv(BinLog(nightBin));
    ASSERT_LT(ev, 0.0f) << "the frame must meter as a night";
    const float scale = RunNodeResolve(PictureMeterSettings(curve), 0.0f, 0.0f);
    const float expectedStops = std::log2(EvToLinearExposure(ev)) + curve.Evaluate(ev);
    EXPECT_NEAR(std::log2(scale), expectedStops, 0.05f)
        << "metered EV100 " << ev << ", the curve gives " << curve.Evaluate(ev) << " stops";
    EXPECT_LT(curve.Evaluate(ev), -1.0f) << "the default curve darkens a night";
}

// A daylight frame is left as metered by the default curve.
TEST_F(AutoExposureComputeTest, TheDefaultCurveLeavesADaylightHistogramAsMetered)
{
    const uint32_t dayBin = kGeometryBin + BinsForStops(4.0f);
    std::vector<uint32_t> words(kHistogramWords, 0u);
    words[dayBin] = 1000u;
    WriteBuffer(Histogram, words.data(), kHistogramBytes);
    const float ev = MeteredEv(BinLog(dayBin));
    ASSERT_GT(ev, 10.0f) << "the frame must meter as a day";
    const float scale =
        RunNodeResolve(PictureMeterSettings(GameEngine::Components::DefaultExposureCompensationCurve()), 0.0f, 0.0f);
    EXPECT_LT(StopsApart(scale, EvToLinearExposure(ev)), 1e-3f);
}

// The curve reads the meter's reading before the floor and applies after it: a moonless reading held
// at a floor of -5 is darkened by the curve's value at the reading, not at the floor.
TEST_F(AutoExposureComputeTest, TheCompensationAppliesAtTheFloorFromTheUnclampedReading)
{
    GameEngine::Math::Curve curve{};
    curve.TryInsert(GameEngine::Math::CurveKey{-12.0f, -4.0f});
    curve.TryInsert(GameEngine::Math::CurveKey{0.0f, 0.0f});
    auto settings = LightsMeterSettings(-5.0f, 20.0f);
    settings.AutoExposureCompensation = GameEngine::Rendering::SampleExposureCompensation(curve);
    const float scale = RunNodeResolve(settings, LightsLogLuminance(-12.0f), 0.0f);
    EXPECT_LT(StopsApart(scale, EvToLinearExposure(-5.0f) * std::exp2(-4.0f)), 1e-3f);
}

// An adapted exposure that sits outside the clamp range because of the curve stays where it is: the
// smoothing holds the previous exposure inside every exposure the target can take, not inside the
// clamp range, so a compensated target past the floor's gain is not cut back each frame.
TEST_F(AutoExposureComputeTest, AnAdaptedCompensatedExposurePastTheClampHolds)
{
    GameEngine::Math::Curve brighten{};
    brighten.TryInsert(GameEngine::Math::CurveKey{0.0f, 2.0f});
    auto settings = LightsMeterSettings(4.0f, 18.0f);
    settings.AutoExposureCompensation = GameEngine::Rendering::SampleExposureCompensation(brighten);
    const float target = EvToLinearExposure(4.0f) * 4.0f;
    const float scale = RunNodeResolve(settings, LightsLogLuminance(-3.0f), 0.1f, target);
    EXPECT_LT(StopsApart(scale, target), 1e-3f);
}

// The resolve records the stops it took from the curve at the reading, with its valid word, for the
// passes that undo the compensation (the stars).
TEST_F(AutoExposureComputeTest, TheResolveRecordsTheCompensationItApplied)
{
    GameEngine::Math::Curve curve{};
    curve.TryInsert(GameEngine::Math::CurveKey{-12.0f, -4.0f});
    curve.TryInsert(GameEngine::Math::CurveKey{0.0f, 0.0f});
    auto settings = LightsMeterSettings(-14.0f, 20.0f);
    settings.AutoExposureCompensation = GameEngine::Rendering::SampleExposureCompensation(curve);
    const uint32_t zeros[4] = {0u, 0u, 0u, 0u};
    WriteBuffer(Compensation, zeros, kStateBytes);
    (void)RunNodeResolve(settings, LightsLogLuminance(-6.0f), 0.0f);
    const auto words = ReadWords(Compensation, kStateBytes);
    float stops = 0.0f;
    std::memcpy(&stops, words.data(), sizeof(float));
    EXPECT_NEAR(stops, -2.0f, 1e-3f);
    EXPECT_EQ(words[1], 1u);
}

// The resolve records the meter's reading before the clamps, for the night tint's gate: a full moon
// (EV100 -4.3) held at a floor of -2 is recorded as -4.3, not as the floor.
TEST_F(AutoExposureComputeTest, TheResolveRecordsTheReadingBeforeTheClamps)
{
    constexpr float kFullMoonEv = -4.3f;
    const auto settings = LightsMeterSettings(-2.0f, 20.0f);
    const uint32_t zeros[4] = {0u, 0u, 0u, 0u};
    WriteBuffer(Compensation, zeros, kStateBytes);
    (void)RunNodeResolve(settings, LightsLogLuminance(kFullMoonEv), 0.0f);
    const auto words = ReadWords(Compensation, kStateBytes);
    float reading = 0.0f;
    std::memcpy(&reading, &words[2], sizeof(float));
    EXPECT_NEAR(reading, LightsLogLuminance(kFullMoonEv), 1e-4f);
    EXPECT_EQ(words[1], 1u);
}
