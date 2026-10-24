// The IBL irradiance on an up-facing surface, read back from the irradiance cube after each bake
// (IrradianceReadbackFeature). Two halves:
//
//   * On the device: a copy of the cube's +Y centre texels lands, after its frame's submission,
//     as pi x luminance x 203 lux (the cube stores E / pi in the scene-linear unit), carries the
//     digest of the bake it was read from, holds between copies and moves with the next one; the
//     rest of the face holds another value, so only the centre gives the expected reading.
//   * At declaration, against the production SkyEnvironmentSource and
//     SceneReflectionProbeEnvironmentSource, which bake into the same cube: the copy is declared in
//     the bake frame, after the diffuse convolve and reading the cube it writes; a steady
//     environment declares none; a changed sky declares it again; and a readback created after
//     the last bake gets one copy of the resting cube without a bake.
//
// The sky's own LUTs are computed by the pipeline's sky node, which this file does not run, so
// the bake's numbers are not asserted here; the device half asserts the numbers on known texels.

#include <gtest/gtest.h>

#include "Components/Rendering/LightPhotometry.h"
#include "EngineLogCapture.h"
#include "Engine/Rendering/IEnvironmentSource.h" // complete type for ImageBasedLightingFeature's unique_ptr
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"
#include "Engine/Rendering/SkyEnvironmentSource.h"
#include "Engine/Rendering/IrradianceReadbackFeature.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Sky/SkySettings.h"
#include "Rendering/Sky/SkySystem.h"
#include "TestDeviceHelper.h"
#include "Types/ColorUtils.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using Engine::Renderer::ImageBasedLightingFeature;
using Engine::Renderer::RenderServices;
using Engine::Renderer::SkyEnvironmentSource;
using Engine::Renderer::IrradianceReadbackFeature;
using Engine::Renderer::SkyRenderFeature;

namespace
{
constexpr std::string_view kCopyPass = "IBLGen.IrradianceReadback";
constexpr std::string_view kConvolvePass = "IBLGen.DiffuseConvolve";
// ImageBasedLightingFeature::Initialize seeds the irradiance cube with this ambient.
constexpr float kSeedAmbient = 0.1f;

struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 262144) {}
};

float ExpectedLux(const float linearRgb[3])
{
    return std::numbers::pi_v<float> * ColorUtils::LinearRec709Luminance(linearRgb) *
           Components::kReferenceWhiteNits;
}

// The value a float takes after a round trip through RGBA16F.
float AsHalf(float value)
{
    return std::ldexp(std::round(std::ldexp(value, 10 - std::ilogb(value))), std::ilogb(value) - 10);
}

RenderGraph::RGPassId FindPass(const RenderGraph::RGGraph& graph, std::string_view name)
{
    for (uint32_t p = 0; p < graph.PassCount(); ++p)
        if (graph.PassName(p) && std::string_view(graph.PassName(p)) == name)
            return p;
    return RenderGraph::kInvalidId;
}

RenderGraph::RGResourceId FindResource(const RenderGraph::RGGraph& graph, std::string_view name)
{
    for (uint32_t r = 0; r < graph.ResourceCount(); ++r)
        if (graph.ResourceName(r) && std::string_view(graph.ResourceName(r)) == name)
            return r;
    return RenderGraph::kInvalidId;
}

class IrradianceReadbackTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Services = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_Services->Initialize(m_Device.get()));
        ASSERT_TRUE(m_Services->EnsureFeature<ImageBasedLightingFeature>().Initialize(m_Device.get()));
        m_Pools = std::make_unique<FramePools>(m_Device.get());
        // One frame stream for the whole test, as a window has: a submission's token belongs to
        // its frame, so a frame destroyed before the read leaves a token nothing can query.
        m_Frame = std::make_unique<RenderGraph::RGFrame>(m_Device.get(), &m_Pools->Persistent,
                                                         &m_Pools->Transient, &m_Pools->Ring);
    }

    void TearDown() override
    {
        m_Frame.reset();
        m_Pools.reset();
        if (m_Services)
            m_Services->Shutdown();
        m_Services.reset();
        if (m_Device)
            m_Device->Shutdown();
    }

    ImageBasedLightingFeature& Ibl() { return *m_Services->GetFeature<ImageBasedLightingFeature>(); }

    // Fill the cube's +Y face with `face` and the 2x2 block at its centre with `centre` (RGBA16F
    // bit patterns), leaving it at rest (ShaderResource). Two values, so a copy taken anywhere but
    // the centre of the face reads the wrong one.
    void FillUpFace(const float face[4], const uint16_t centre[4])
    {
        constexpr uint32_t kSpan = IrradianceReadbackFeature::kTexelSpan;
        constexpr uint32_t kPitch = IrradianceReadbackFeature::kRowPitchBytes;
        std::vector<uint8_t> block(static_cast<size_t>(kPitch) * kSpan, 0);
        for (uint32_t y = 0; y < kSpan; ++y)
            for (uint32_t x = 0; x < kSpan; ++x)
                std::memcpy(block.data() + y * kPitch + x * 4 * sizeof(uint16_t), centre, 4 * sizeof(uint16_t));
        const BufferHandle upload = m_Device->CreateUploadBuffer(block.size(), "IrradianceReadbackTest.Centre");
        ASSERT_TRUE(upload.IsValid());
        m_Device->UpdateBuffer(upload, 0, block.size(), block.data());

        const TextureHandle cube = Ibl().GetIrradianceCube();
        const uint32_t origin = (ImageBasedLightingFeature::kIrradianceSize - kSpan) / 2u;
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->ClearColorImageSubresource(cube, 0, IrradianceReadbackFeature::kUpFaceLayer, face);
        cl->CopyBufferToTextureSubresource(upload, cube, 0, IrradianceReadbackFeature::kUpFaceLayer, kSpan, kSpan, 0,
                                           kPitch, 1, 0, origin, origin, ResourceState::CopyDest);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(cube, ResourceState::CopyDest, ResourceState::ShaderResource,
                                                          0, 1, IrradianceReadbackFeature::kUpFaceLayer, 1));
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(upload);
    }

    // One frame that copies the cube as it rests, submitted and completed.
    void CopyRestingCube(IrradianceReadbackFeature& readback, uint64_t frameIndex, uint64_t bakeDigest)
    {
        RenderGraph::RGFrame& frame = *m_Frame;
        frame.BeginFrame(frameIndex);
        const RenderGraph::RGTexture cube = frame.ImportExternalTexture(
            "IBL_Irradiance", Ibl().GetIrradianceCube(), ResourceState::ShaderResource,
            TextureFormat::R16G16B16A16_FLOAT, 1, ImageBasedLightingFeature::kNumCaptureFaces);
        readback.DeclareCopy(*m_Device, frame, cube, ImageBasedLightingFeature::kIrradianceSize, bakeDigest);
        frame.Execute();
        readback.OnFrameSubmittedRG(frame, frame.SubmissionToken());
        m_Device->WaitForIdle();
    }

    // Schedules and executes one frame of the sky's bake (the schedule ticks the frames since the
    // last bake); returns whether a bake was declared.
    bool RunSkyBakeFrame(SkyEnvironmentSource& source, uint64_t frameIndex)
    {
        RenderGraph::RGFrame& frame = *m_Frame;
        frame.BeginFrame(frameIndex);
        Engine::Renderer::EnvironmentBakeContext ctx{};
        ctx.Frame = &frame;
        ctx.Device = m_Device.get();
        ctx.Feature = &Ibl();
        source.ScheduleBake(ctx);
        const bool bakes = FindPass(frame.Graph(), kConvolvePass) != RenderGraph::kInvalidId;
        frame.Execute();
        m_Device->WaitForIdle();
        return bakes;
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
    std::unique_ptr<FramePools> m_Pools;
    std::unique_ptr<RenderGraph::RGFrame> m_Frame;
};
} // namespace

TEST_F(IrradianceReadbackTest, ACopyLandsInLuxAndMovesWithTheNextBake)
{
    IrradianceReadbackFeature readback;
    float lux = -1.0f;
    uint64_t digest = 0;
    EXPECT_FALSE(readback.TryGetUpIrradianceLux(lux, &digest)) << "nothing was copied yet";

    CopyRestingCube(readback, 0, 7);
    ASSERT_TRUE(readback.TryGetUpIrradianceLux(lux, &digest));
    const float seed[3] = {AsHalf(kSeedAmbient), AsHalf(kSeedAmbient), AsHalf(kSeedAmbient)};
    EXPECT_NEAR(lux, ExpectedLux(seed), 1e-3f) << "pi x the texel luminance x 203";
    EXPECT_EQ(digest, 7u) << "the reading carries the digest of the bake it was copied from";

    // Between copies the reading holds (a steady sky declares no copy: the test below).
    float held = -1.0f;
    uint64_t heldDigest = 0;
    ASSERT_TRUE(readback.TryGetUpIrradianceLux(held, &heldDigest));
    EXPECT_EQ(held, lux);
    EXPECT_EQ(heldDigest, 7u);

    // The face around the centre holds another value, so only the centre texels give this reading.
    const float face[4] = {2.0f, 1.0f, 0.5f, 1.0f};
    const uint16_t centreHalves[4] = {0x3C00u, 0x3800u, 0x3400u, 0x3C00u}; // 1, 0.5, 0.25, 1
    const float centre[3] = {1.0f, 0.5f, 0.25f};
    FillUpFace(face, centreHalves);
    CopyRestingCube(readback, 1, 8);
    ASSERT_TRUE(readback.TryGetUpIrradianceLux(lux, &digest));
    EXPECT_NEAR(lux, ExpectedLux(centre), 1e-2f)
        << "the reading is the face's centre, read by its luminance (the rest of the face reads "
        << ExpectedLux(face) << " lx)";
    EXPECT_EQ(digest, 8u);
}

TEST_F(IrradianceReadbackTest, TheSkyDeclaresTheCopyOnlyWithItsBake)
{
    auto& sky = m_Services->EnsureFeature<SkyRenderFeature>();
    ASSERT_TRUE(sky.Initialize(m_Device.get()));
    SkySettings settings{};
    sky.SetSettings(settings, SkySunState{});

    SkyEnvironmentSource source(*m_Services);
    // Declares this frame's sky bake and returns the graph. A bake's exec latches its digest;
    // these frames are declared, not executed, so the test latches it the way the exec does.
    struct Declared
    {
        bool Copies = false;
        bool Bakes = false;
        bool CopyReadsTheConvolvedCube = false;
    };
    auto declareFrame = [&](uint64_t index)
    {
        RenderGraph::RGFrame& frame = *m_Frame;
        frame.BeginFrame(index);
        Engine::Renderer::EnvironmentBakeContext ctx{};
        ctx.Frame = &frame;
        ctx.Device = m_Device.get();
        ctx.Feature = &Ibl();
        source.ScheduleBake(ctx);
        const auto& graph = frame.Graph();
        const RenderGraph::RGPassId copy = FindPass(graph, kCopyPass);
        const RenderGraph::RGPassId convolve = FindPass(graph, kConvolvePass);
        const RenderGraph::RGResourceId cube = FindResource(graph, "IBL_Irradiance");
        Declared out;
        out.Copies = copy != RenderGraph::kInvalidId;
        out.Bakes = convolve != RenderGraph::kInvalidId;
        out.CopyReadsTheConvolvedCube = out.Copies && out.Bakes && cube != RenderGraph::kInvalidId &&
                                        graph.HasWriteAccess(convolve, cube) && graph.HasReadAccess(copy, cube) &&
                                        convolve < copy;
        if (out.Bakes)
        {
            Ibl().SetLastBakedDigest(source.InputDigest());
            Ibl().MarkBaked();
        }
        return out;
    };

    // No readback: the bake declares no copy.
    Declared first = declareFrame(0);
    ASSERT_TRUE(first.Bakes) << "the first frame with a sky must bake; the check is void";
    EXPECT_FALSE(first.Copies) << "without the readback feature the sky pays nothing";

    // A readback created after that bake gets one copy of the resting cube, with no bake.
    m_Services->EnsureFeature<IrradianceReadbackFeature>();
    Declared late = declareFrame(1);
    EXPECT_FALSE(late.Bakes);
    EXPECT_TRUE(late.Copies) << "a readback that arrives on a steady sky must get a reading";

    // A steady sky declares neither, frame after frame.
    for (uint64_t i = 2; i < 2 + ImageBasedLightingFeature::kIblRebakeIntervalFrames; ++i)
    {
        const Declared steady = declareFrame(i);
        EXPECT_FALSE(steady.Bakes) << "frame " << i;
        EXPECT_FALSE(steady.Copies) << "frame " << i << ": the reading is re-derived only when the sky bakes";
    }

    // The sky changes: the bake reruns and the copy follows its convolve.
    settings.primarySunIntensity *= 0.5f;
    sky.SetSettings(settings, SkySunState{});
    const Declared changed = declareFrame(2 + ImageBasedLightingFeature::kIblRebakeIntervalFrames);
    ASSERT_TRUE(changed.Bakes) << "a changed sky must bake again; the check is void";
    EXPECT_TRUE(changed.Copies);
    EXPECT_TRUE(changed.CopyReadsTheConvolvedCube) << "the copy must read the cube the convolve writes, after it";
}

// get_render_stats reports the bake count and the frames since the last bake, and a capture waits
// until the count stops rising. An executed sky bake counts once and restarts the frame count; a
// steady sky bakes nothing. (A reflection probe's bake ends in the same MarkBaked, and needs the
// capture views' culling and extraction, which a bare RenderServices does not run.)
TEST_F(IrradianceReadbackTest, AnExecutedSkyBakeCountsOnceAndRestartsTheFramesSinceBake)
{
    auto& sky = m_Services->EnsureFeature<SkyRenderFeature>();
    ASSERT_TRUE(sky.Initialize(m_Device.get()));
    sky.SetSettings(SkySettings{}, SkySunState{});
    SkyEnvironmentSource source(*m_Services);
    EXPECT_EQ(Ibl().GetBakeCount(), 0u);

    ASSERT_TRUE(RunSkyBakeFrame(source, 0)) << "the first frame with a sky must bake; the check is void";
    EXPECT_EQ(Ibl().GetBakeCount(), 1u) << "one executed bake counts once";
    EXPECT_EQ(Ibl().GetFramesSinceBake(), 0u) << "the bake restarted the frame count";

    ASSERT_FALSE(RunSkyBakeFrame(source, 1)) << "a steady sky must not bake again";
    EXPECT_EQ(Ibl().GetBakeCount(), 1u);
    EXPECT_EQ(Ibl().GetFramesSinceBake(), 1u);
}

// With async compute on, the sky's convolve writes the cube on the compute queue and the copy
// reads its up face on graphics, so the graph records the face's transition into the copy source
// on the compute list. The copy must still read from that layout, never report it unknown.
TEST_F(IrradianceReadbackTest, ACopyAfterAnAsyncComputeBakeReadsTheFaceInItsGraphState)
{
    if (m_Device->GetComputeQueueFamilyIndex() == m_Device->GetGraphicsQueueFamilyIndex())
        GTEST_SKIP() << "No separate compute queue family: the graph records every transition on one list";
    auto& sky = m_Services->EnsureFeature<SkyRenderFeature>();
    ASSERT_TRUE(sky.Initialize(m_Device.get()));
    sky.SetSettings(SkySettings{}, SkySunState{});
    SkyEnvironmentSource source(*m_Services);
    IrradianceReadbackFeature& readback = m_Services->EnsureFeature<IrradianceReadbackFeature>();

    RenderGraph::RGFrame& frame = *m_Frame;
    frame.SetPhysicalQueueMap(RenderGraph::RGFrame::kIdentityQueueMap);
    std::vector<std::string> log;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&log, Logger::LogLevel::Warning);
        frame.BeginFrame(0);
        Engine::Renderer::EnvironmentBakeContext ctx{};
        ctx.Frame = &frame;
        ctx.Device = m_Device.get();
        ctx.Feature = &Ibl();
        source.ScheduleBake(ctx);
        ASSERT_NE(FindPass(frame.Graph(), kConvolvePass), RenderGraph::kInvalidId)
            << "the first frame with a sky must bake; the check is void";
        ASSERT_NE(FindPass(frame.Graph(), kCopyPass), RenderGraph::kInvalidId)
            << "a bake with the readback present must declare the copy; the check is void";
        frame.Execute();
        readback.OnFrameSubmittedRG(frame, frame.SubmissionToken());
        m_Device->WaitForIdle();
        Logger::Log::Flush();
    }

    const RenderGraph::RGResourceId cube = FindResource(frame.Graph(), "IBL_Irradiance");
    ASSERT_NE(cube, RenderGraph::kInvalidId);
    const auto& barriers = frame.Graph().Barriers();
    const auto& placement = frame.Graph().BarrierSubmissions();
    ASSERT_EQ(placement.size(), barriers.size());
    bool relocated = false;
    for (size_t i = 0; i < barriers.size(); ++i)
    {
        const RenderGraph::RGBarrier& b = barriers[i];
        relocated |= b.Resource == cube && b.NewLayout == RenderGraph::RGImageLayout::TransferSrc &&
                     placement[i] != RenderGraph::kInvalidId;
    }
    ASSERT_TRUE(relocated)
        << "the face's transition into the copy source must record on the compute tail; the check is void";

    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(log, "no recorded layout"), 0u)
        << GameEngine::TestLog::FirstLineContaining(log, "no recorded layout");
    float lux = -1.0f;
    EXPECT_TRUE(readback.TryGetUpIrradianceLux(lux)) << "the copy must land";
}

// A reflection probe the camera stands in replaces the sky as the scene's environment and bakes
// into the same irradiance cube, so the readback follows it. A readback that arrives while the
// probe is idle gets one copy of the cube as it rests, and later frames copy nothing until a bake.
// (The probe's bake itself, which declares the copy after its own diffuse convolve, needs the
// capture views' culling and extraction, which a bare RenderServices does not run.)
TEST_F(IrradianceReadbackTest, AReflectionProbeCopiesTheRestingCubeOnce)
{
    Engine::Renderer::SceneReflectionProbeEnvironmentSource probe(*m_Services);
    Engine::Renderer::SceneReflectionProbeEnvironmentDesc desc{};
    desc.SourceKey = 1;
    desc.UpdateMode = Engine::Renderer::SceneReflectionProbeUpdateMode::Once;
    probe.SetProbe(desc);

    auto declaresCopy = [&](uint64_t index)
    {
        RenderGraph::RGFrame& frame = *m_Frame;
        frame.BeginFrame(index);
        Engine::Renderer::EnvironmentBakeContext ctx{};
        ctx.Frame = &frame;
        ctx.Device = m_Device.get();
        ctx.Feature = &Ibl();
        ctx.DeltaTimeSeconds = 1.0f / 60.0f;
        probe.ScheduleBake(ctx);
        return FindPass(frame.Graph(), kCopyPass) != RenderGraph::kInvalidId;
    };

    // A cube an earlier bake left behind, and a readback that arrives now.
    Ibl().SetLastBakedDigest(5);
    m_Services->EnsureFeature<IrradianceReadbackFeature>();
    EXPECT_TRUE(declaresCopy(0)) << "a readback that arrives between bakes must get a reading";
    for (uint64_t i = 1; i < 4; ++i)
        EXPECT_FALSE(declaresCopy(i)) << "frame " << i << ": the resting cube is copied once";
}
