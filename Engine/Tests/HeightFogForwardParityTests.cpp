// A transparent fragment drawn after the HeightFog post-process fogs itself (GE_HEIGHT_FOG,
// forward_height_fog.glsl). The claim that makes that correct: a fragment at distance d along a
// direction takes the same fog as an opaque surface at distance d along it. Both sides run as they
// ship: height_fog.frag over a 1x1 scene whose depth puts the opaque surface at the fragment's
// place, and forward_height_fog_probe.frag, which calls GE_ApplyHeightFog for the fragment. The
// CPU only places the camera, writes the two parameter blocks and compares the pixels; it does not
// evaluate the fog.
//
// The scatter weight is pinned against the same draws: a fragment that hides nothing behind it
// (additive, weight 0) takes the post-process's result less its in-scatter, and a premultiplied one
// of alpha a takes it less (1 - a) of the in-scatter.
//
// Skips only without a Vulkan device; a missing shader fails.
#include <gtest/gtest.h>

#include "Engine/Rendering/HeightFogParamsUBO.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;
using GameEngine::Engine::Renderer::HeightFogColorSource;
using GameEngine::Engine::Renderer::HeightFogFlagDistanceEnabled;
using GameEngine::Engine::Renderer::HeightFogFlagHeightEnabled;
using GameEngine::Engine::Renderer::HeightFogFlagNoiseEnabled;
using GameEngine::Engine::Renderer::HeightFogParamsUBO;

namespace
{
using Pixel = std::array<float, 4>;
using Vec3 = std::array<float, 3>;

void Require(bool condition, const std::string& why)
{
    if (!condition)
        throw std::runtime_error(why);
}

// Where the camera stands and looks: yaw about +Y, then pitch (positive looks up), looking down
// view -Z.
struct Pose
{
    Vec3 Camera{};
    float YawDeg = 0, PitchDeg = 0;
    float Distance = 0;
};

constexpr float kNear = 0.5f;
constexpr float kFar = 20000.0f;

// The view's rotation (world from view), column-major 3x3 as R[col][row].
std::array<Vec3, 3> Rotation(const Pose& pose)
{
    const float y = pose.YawDeg * std::numbers::pi_v<float> / 180.0f;
    const float p = pose.PitchDeg * std::numbers::pi_v<float> / 180.0f;
    const float cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p);
    // Ry(yaw) * Rx(pitch), by columns.
    return {Vec3{cy, 0.0f, -sy}, Vec3{sy * sp, cp, cy * sp}, Vec3{sy * cp, -sp, cy * cp}};
}

// The fragment's place: the camera plus Distance along view -Z.
Vec3 Target(const Pose& pose)
{
    const auto r = Rotation(pose);
    Vec3 out{};
    for (int i = 0; i < 3; ++i)
        out[i] = pose.Camera[i] - r[2][i] * pose.Distance;
    return out;
}

// A 1x1 view looking at the target through its centre. The projection is an infinite reversed-Z
// perspective, so the surface Distance ahead has depth kNear / Distance; the sky is depth 0.
ViewParamsUBO View(const Pose& pose)
{
    ViewParamsUBO v{};
    const auto r = Rotation(pose);
    // view = [R^T | -R^T * camera], column-major.
    float view[16] = {};
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row)
            view[c * 4 + row] = r[row][c];
    for (int row = 0; row < 3; ++row)
    {
        float t = 0;
        for (int k = 0; k < 3; ++k)
            t -= r[row][k] * pose.Camera[k];
        view[12 + row] = t;
    }
    view[15] = 1;
    std::memcpy(v.ge_view, view, sizeof(view));
    // invView = [R | camera].
    float invView[16] = {};
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row)
            invView[c * 4 + row] = r[c][row];
    for (int row = 0; row < 3; ++row)
        invView[12 + row] = pose.Camera[row];
    invView[15] = 1;
    std::memcpy(v.ge_invView, invView, sizeof(invView));
    // P rows: [s 0 0 0], [0 s 0 0], [0 0 0 n], [0 0 -1 0]; its inverse rows:
    // [1/s 0 0 0], [0 1/s 0 0], [0 0 0 -1], [0 0 1/n 0].
    const float s = 1.0f;
    float proj[16] = {}, invProj[16] = {};
    proj[0] = s;
    proj[5] = s;
    proj[14] = kNear;
    proj[11] = -1.0f;
    invProj[0] = 1.0f / s;
    invProj[5] = 1.0f / s;
    invProj[14] = -1.0f;
    invProj[11] = 1.0f / kNear;
    std::memcpy(v.ge_proj, proj, sizeof(proj));
    std::memcpy(v.ge_invProj, invProj, sizeof(invProj));
    v.ge_nearFar[0] = kNear;
    v.ge_nearFar[1] = kFar;
    for (int i = 0; i < 3; ++i)
        v.ge_cameraPosWS[i] = pose.Camera[i];
    v.ge_cameraPosWS[3] = 1;
    for (float& f : v.ge_screenSize)
        f = 1.0f;
    return v;
}

HeightFogParamsUBO BaseFog()
{
    HeightFogParamsUBO f{};
    f.intensity = 1.0f;
    f.density = 0.6f;
    f.minDistance = 30.0f;
    f.smoothLength = 200.0f;
    f.baseHeight = 0.0f;
    f.transitionLength = 150.0f;
    f.maxDistance = 3000.0f;
    f.maxOpacity = 0.95f;
    f.sunIntensity = 1.5f;
    f.flags = HeightFogFlagDistanceEnabled | HeightFogFlagHeightEnabled;
    return f;
}

struct Image
{
    TextureHandle Handle{};
    uint32_t Width = 0, Height = 0;
};

struct Pipeline
{
    PipelineHandle Handle{};
    PipelineDesc Desc{};
    ShaderMeta Meta{};
};

class HeightFogForwardParity : public ::testing::Test
{
  protected:
    void SetUp() override
    {
#ifdef _WIN32
        _putenv_s("GE_HEADLESS_TEST", "1");
#else
        setenv("GE_HEADLESS_TEST", "1", 1);
#endif
        Utils::SetShaderPathResolver(+[](const std::filesystem::path& rel)
                                     { return std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR) / rel; });
        DeviceDesc dd{};
        dd.preferredAPI = GraphicsAPI::Vulkan;
        dd.enableDynamicRendering = true;
        m_Device = DeviceFactory::CreateDevice(dd);
        if (!m_Device || !m_Device->Initialize(dd))
            GTEST_SKIP() << "No usable Vulkan device";
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("HeightFogParity"));
        ASSERT_TRUE(m_Sampler.IsValid());
        BufferDesc bd{};
        bd.size = 512;
        bd.usage = static_cast<uint32_t>(BufferUsage::Uniform);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        m_ViewBuffer = m_Device->CreateBuffer(bd);
        m_FogBuffer = m_Device->CreateBuffer(bd);
        ASSERT_TRUE(m_ViewBuffer.IsValid() && m_FogBuffer.IsValid());
        // A backdrop that changes with direction, for the sky colour sources.
        std::vector<Pixel> backdrop(64 * 32);
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 64; ++x)
                backdrop[y * 64 + x] = {0.2f + 0.6f * x / 63.0f, 0.3f + 0.5f * y / 31.0f, 0.9f - 0.4f * x / 63.0f, 1};
        m_Backdrop = Upload(64, 32, backdrop);
        m_Depth = Allocate(1, 1);
        m_Scene = Allocate(1, 1);
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (auto image : m_Images)
            m_Device->DestroyTexture(image.Handle);
        for (auto& [name, pipeline] : m_Pipelines)
            m_Device->DestroyPipeline(pipeline.Handle);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        if (m_ViewBuffer.IsValid())
            m_Device->DestroyBuffer(m_ViewBuffer);
        if (m_FogBuffer.IsValid())
            m_Device->DestroyBuffer(m_FogBuffer);
        m_Device->Shutdown();
    }

    Image Allocate(uint32_t w, uint32_t h)
    {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        td.depth = 1;
        td.mipLevels = td.arrayLayers = td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget |
                                         TextureUsage::TransferSrc | TextureUsage::TransferDst);
        td.debugName = "HeightFogParity.Image";
        Image image{m_Device->CreateTexture(td), w, h};
        Require(image.Handle.IsValid(), "image allocation failed");
        m_Images.push_back(image);
        return image;
    }

    void Write(Image image, const std::vector<Pixel>& pixels)
    {
        auto upload = m_Device->CreateUploadBuffer(pixels.size() * sizeof(Pixel), "HeightFogParity.Upload");
        Require(upload.IsValid(), "upload allocation failed");
        m_Device->UpdateBuffer(upload, 0, pixels.size() * sizeof(Pixel), pixels.data());
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image.Handle, ResourceState::Undefined, ResourceState::CopyDest));
        cl->CopyBufferToTextureSubresource(upload, image.Handle, 0, 0, image.Width, image.Height, 0,
                                          image.Width * sizeof(Pixel));
        cl->Barrier(
            ResourceBarrier::CreateTextureBarrier(image.Handle, ResourceState::CopyDest, ResourceState::ShaderResource));
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(upload);
    }

    Image Upload(uint32_t w, uint32_t h, const std::vector<Pixel>& pixels)
    {
        auto image = Allocate(w, h);
        Write(image, pixels);
        return image;
    }

    Pipeline& GetPipeline(const std::string& shader)
    {
        auto [it, added] = m_Pipelines.try_emplace(shader);
        if (!added)
            return it->second;
        auto& p = it->second;
        auto vs = Utils::LoadShaderFile("fullscreen_noinput.vert.spv");
        auto fs = Utils::LoadShaderFile((shader + ".frag.spv").c_str());
        Require(!vs.empty() && !fs.empty(), "missing shader: " + shader);
        StageReflectionResult rvs{}, rfs{};
        ReflectionOptions opts{};
        Require(ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vs.data()), vs.size() / 4,
                             opts, rvs, nullptr),
                "vertex reflection failed");
        Require(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fs.data()), fs.size() / 4,
                             opts, rfs, nullptr),
                "fragment reflection failed: " + shader);
        p.Meta = MergeStages({rvs, rfs});
        p.Desc.type = PipelineType::Graphics;
        p.Desc.vertexShader = vs;
        p.Desc.pixelShader = fs;
        MaterialBuilder::FormatsHint formats{};
        formats.ColorFormats = {static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT)};
        std::string why;
        Require(MaterialBuilder::BuildPipelineDescFromMeta(p.Meta, p.Desc, formats, MaterialBuilder::MergeMode::Auto,
                                                           MaterialBuilder::PushConstantPolicy{}, &why),
                why);
        p.Handle = m_Device->CreatePipeline(p.Desc);
        Require(p.Handle.IsValid(), "pipeline creation failed: " + shader);
        return p;
    }

    // One fullscreen draw of `shader` into a 1x1 target, its set 0 bound by name.
    Pixel Draw(const std::string& shader, const std::vector<uint8_t>& pushConstants = {})
    {
        auto& p = GetPipeline(shader);
        Require(p.Meta.Sets.size() == 1 && p.Meta.Sets[0].Set == 0, "unexpected descriptor sets in " + shader);
        DescriptorSetDesc sd{};
        sd.layout = p.Desc.descriptorSetLayouts[0];
        sd.transient = false;
        auto ds = m_Device->CreateDescriptorSet(sd);
        Require(ds.IsValid(), "descriptor allocation failed");
        for (const auto& binding : p.Meta.Sets[0].Bindings)
        {
            if (binding.Type == ShaderMetaBindingType::kCombinedImageSampler)
            {
                Image image{};
                if (binding.Name == "uSceneColor")
                    image = m_Scene;
                else if (binding.Name == "uDepth")
                    image = m_Depth;
                else if (binding.Name == "uSkyBackdrop" || binding.Name == "ge_skyBackdrop")
                    image = m_Backdrop;
                else
                    throw std::runtime_error("unexpected texture " + binding.Name + " in " + shader);
                m_Device->UpdateCombinedImageSamplerBinding(ds, binding.Binding, image.Handle, m_Sampler);
            }
            else if (binding.Type == ShaderMetaBindingType::kUniformBuffer)
            {
                const bool fog = binding.Name.find("HeightFog") != std::string::npos;
                Require(fog || binding.Name.find("ViewParams") != std::string::npos,
                        "unexpected uniform block " + binding.Name + " in " + shader);
                m_Device->UpdateBufferBinding(ds, binding.Binding, fog ? m_FogBuffer : m_ViewBuffer, 0,
                                              fog ? sizeof(HeightFogParamsUBO) : sizeof(ViewParamsUBO));
            }
            else
                throw std::runtime_error("unexpected binding " + binding.Name + " in " + shader);
        }
        auto target = Allocate(1, 1);
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(
            ResourceBarrier::CreateTextureBarrier(target.Handle, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc rp{};
        rp.colorTargets[0] = target.Handle;
        rp.colorTargetCount = 1;
        rp.clearColor[0] = true;
        rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        cl->BeginRenderPass(rp);
        cl->SetViewport(0.0f, 0.0f, 1.0f, 1.0f);
        cl->SetScissor(0, 0, 1, 1);
        cl->SetPipeline(p.Handle);
        cl->BindDescriptorSet(0, ds, p.Handle);
        if (!p.Meta.PushConstants.empty())
        {
            std::vector<uint8_t> bytes(p.Meta.PushConstants[0].Size, 0);
            std::memcpy(bytes.data(), pushConstants.data(), std::min(bytes.size(), pushConstants.size()));
            cl->SetPushConstantsById(0, bytes.data(), bytes.size(), 0);
        }
        cl->Draw(3, 1);
        cl->EndRenderPass();
        cl->Barrier(
            ResourceBarrier::CreateTextureBarrier(target.Handle, ResourceState::RenderTarget, ResourceState::CopySource));
        auto rb = m_Device->CreateReadbackBuffer(sizeof(Pixel), "HeightFogParity.Readback");
        Require(rb.IsValid(), "readback allocation failed");
        cl->CopyTextureToBuffer(target.Handle, rb, 1, 1, 0, 0, 0, 0);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        Pixel px{};
        auto* data = m_Device->MapBuffer(rb);
        Require(data != nullptr, "readback map failed");
        std::memcpy(px.data(), data, sizeof(px));
        m_Device->UnmapBuffer(rb);
        m_Device->DestroyBuffer(rb);
        m_Device->DestroyDescriptorSet(ds);
        m_Device->DestroyTexture(target.Handle);
        m_Images.pop_back();
        for (float channel : px)
            Require(std::isfinite(channel), "nonfinite output from " + shader);
        return px;
    }

    void SetView(const Pose& pose)
    {
        const ViewParamsUBO v = View(pose);
        m_Device->UpdateBuffer(m_ViewBuffer, 0, sizeof(v), &v);
    }

    void SetFog(const HeightFogParamsUBO& fog) { m_Device->UpdateBuffer(m_FogBuffer, 0, sizeof(fog), &fog); }

    // The HeightFog post-process over an opaque surface of `color` at the pose's distance.
    Pixel PostProcess(const Pose& pose, const Pixel& color)
    {
        SetView(pose);
        Write(m_Scene, {color});
        Write(m_Depth, {Pixel{kNear / pose.Distance, 0, 0, 0}});
        return Draw("height_fog");
    }

    // A forward fragment of `color` at the same place, fogging itself.
    Pixel Forward(const Pose& pose, const Pixel& color, float scatterWeight)
    {
        SetView(pose);
        const Vec3 at = Target(pose);
        const float pc[8] = {color[0], color[1], color[2], color[3], at[0], at[1], at[2], scatterWeight};
        std::vector<uint8_t> bytes(sizeof(pc));
        std::memcpy(bytes.data(), pc, sizeof(pc));
        return Draw("forward_height_fog_probe", bytes);
    }

    static std::vector<Pose> Poses()
    {
        std::vector<Pose> poses;
        for (float height : {15.0f, 350.0f})
            for (float yaw : {0.0f, 128.0f})
                for (float pitch : {-35.0f, -8.0f, 0.0f, 6.0f, 30.0f})
                    for (float distance : {20.0f, 150.0f, 600.0f, 1400.0f, 2800.0f})
                        poses.push_back(Pose{{40.0f, height, -25.0f}, yaw, pitch, distance});
        return poses;
    }

    static std::string Describe(const Pose& pose)
    {
        return "camera height " + std::to_string(pose.Camera[1]) + ", yaw " + std::to_string(pose.YawDeg) +
               ", pitch " + std::to_string(pose.PitchDeg) + ", distance " + std::to_string(pose.Distance);
    }

    static void ExpectNear(const Pixel& expected, const Pixel& actual, const std::string& what)
    {
        for (int c = 0; c < 3; ++c)
            EXPECT_NEAR(actual[c], expected[c], 2e-5f + 2e-4f * std::abs(expected[c])) << what << ", channel " << c;
    }

    // Every pose under `fog`: the opaque surface and the fragment at its place agree, and the
    // scatter weight takes off exactly its share of the in-scatter.
    void CheckParity(const HeightFogParamsUBO& fog, const std::string& name)
    {
        SetFog(fog);
        const Pixel color{0.9f, 0.35f, 0.1f, 1.0f};
        const Pixel black{0, 0, 0, 1};
        float strongest = 0;
        for (const Pose& pose : Poses())
        {
            const Pixel opaque = PostProcess(pose, color);
            const Pixel inScatter = PostProcess(pose, black);
            const std::string what = name + ", " + Describe(pose);
            ExpectNear(opaque, Forward(pose, color, 1.0f), what + ", straight alpha");
            Pixel additive{}, premultiplied{};
            for (int c = 0; c < 3; ++c)
            {
                additive[c] = opaque[c] - inScatter[c];
                premultiplied[c] = opaque[c] - 0.6f * inScatter[c];
                strongest = std::max(strongest, std::abs(opaque[c] - color[c]));
            }
            ExpectNear(additive, Forward(pose, color, 0.0f), what + ", additive");
            ExpectNear(premultiplied, Forward(pose, color, 0.4f), what + ", premultiplied at alpha 0.4");
            if (HasFatalFailure() || HasNonfatalFailure())
                return;
        }
        // The fog must matter in these poses, or agreement proves nothing.
        EXPECT_GT(strongest, 0.3f) << name << ": the fog barely changes the scene in any pose";
    }

    std::unique_ptr<IDevice> m_Device;
    SamplerHandle m_Sampler{};
    BufferHandle m_ViewBuffer{}, m_FogBuffer{};
    Image m_Backdrop{}, m_Depth{}, m_Scene{};
    std::vector<Image> m_Images;
    std::map<std::string, Pipeline> m_Pipelines;
};
} // namespace

TEST_F(HeightFogForwardParity, FixedColourFogAgreesWithThePostProcessAtTheSameDistance)
{
    CheckParity(BaseFog(), "fixed colour");
}

TEST_F(HeightFogForwardParity, PartialIntensityAgreesWithThePostProcess)
{
    HeightFogParamsUBO fog = BaseFog();
    fog.intensity = 0.65f;
    CheckParity(fog, "intensity 0.65");
}

TEST_F(HeightFogForwardParity, DistanceOnlyAndHeightOnlyFogAgreeWithThePostProcess)
{
    HeightFogParamsUBO fog = BaseFog();
    fog.flags = HeightFogFlagDistanceEnabled;
    CheckParity(fog, "distance only");
    fog.flags = HeightFogFlagHeightEnabled;
    fog.density = 2.0f;
    CheckParity(fog, "height only");
}

TEST_F(HeightFogForwardParity, SkyColourSourcesAgreeWithThePostProcess)
{
    HeightFogParamsUBO fog = BaseFog();
    fog.colorSource = static_cast<int32_t>(HeightFogColorSource::SkyConstant);
    fog.skyHorizonColorR = 0.55f;
    fog.skyHorizonColorG = 0.7f;
    fog.skyHorizonColorB = 0.95f;
    CheckParity(fog, "sky constant");
    // The backdrop read by direction, so a wrong direction on either side shows.
    fog.colorSource = static_cast<int32_t>(HeightFogColorSource::SkyHdri);
    fog.skyHorizonScale = 1.0f;
    CheckParity(fog, "sky HDRI");
}

TEST_F(HeightFogForwardParity, NoisyFogAgreesWithThePostProcess)
{
    HeightFogParamsUBO fog = BaseFog();
    fog.flags |= HeightFogFlagNoiseEnabled;
    fog.noiseStrength = 0.6f;
    fog.noiseScale = 60.0f;
    fog.noiseTime = 3.25f;
    fog.noiseVelX = 4.0f;
    CheckParity(fog, "noise");
}
