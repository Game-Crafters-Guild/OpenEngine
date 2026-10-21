// The height fog is evaluated where each surface is shaded, by the surface's own distance, and the
// sky pass applies the fog's sky fill where it draws the sky. These tests run the shipped shaders on
// single pixels and read back what they write:
// - the sky pass's height fog variant (sky_render_fog.frag) over a gradient sky, for the sky fill;
// - forward_fog.glsl, the fog adapter_forward.glsl applies, through test probes that call it for one
//   surface at a chosen place (Shaders/forward_fog_probe*.frag), with the blend keywords;
// - an anti-aliased edge drawn the way the pipeline draws it under 4x MSAA: the sky over every sample,
//   then a surface over the samples its primitive covers, then the hardware resolve.
//
// The camera stands at the origin looking down +z with the identity view, so a surface on +z at
// distance d is at view distance d. With the fog colour green, a red surface and no sun, the output is
// (T, 1 - T, 0) for the fog's transmittance T, so the green channel reads the fog's opacity directly.
//
// Skips ONLY without a Vulkan device. A missing shader FAILS: the same target builds it.
#include <gtest/gtest.h>

#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/HeightFogParamsUBO.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Sky/SkyRenderer.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;
using GameEngine::Engine::Renderer::HeightFogColorSource;
using GameEngine::Engine::Renderer::HeightFogFlagDistanceEnabled;
using GameEngine::Engine::Renderer::HeightFogFlagSkyEnabled;
using GameEngine::Engine::Renderer::HeightFogParamsUBO;

namespace
{
using Pixel = std::array<float, 4>;

constexpr float kNear = 0.5f;
constexpr float kFar = 1800.0f;
constexpr uint32_t kEdgeSamples = 4;
const Pixel kRed{1.0f, 0.0f, 0.0f, 1.0f};
const Pixel kBlue{0.0f, 0.0f, 1.0f, 1.0f};

void Require(bool condition, const std::string& why)
{
    if (!condition)
        throw std::runtime_error(why);
}

GameEngine::Mathematics::Matrix4x4 Projection()
{
    return GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0f, 1.0f, kNear, kFar);
}

// One shader pair, its reflected set 0 and the pipeline for one sample count and blend.
struct Program
{
    PipelineHandle Handle{};
    PipelineDesc Desc{};
    ShaderMeta Meta{};
};

// What a draw covers: the whole pixel, or the edge probe's triangle over the half of a pixel that
// holds samples 2 and 3 of the standard 4x pattern (msaa_edge_cover_probe.vert).
enum class Cover
{
    Fullscreen,
    HalfPixel
};

// One draw: the fragment shader, what it covers, the probe's push constants and whether it blends
// straight alpha over what is there.
struct DrawStep
{
    std::string Fragment;
    Cover Covers = Cover::Fullscreen;
    std::array<float, 8> Push{};
    bool AlphaBlend = false;
};

// The sky pass's height fog variant, over the whole pixel.
DrawStep Sky()
{
    return {"sky_render_fog"};
}

// A surface of `color` (rgb, a = alpha) on +z at `distance`, shaded through the forward fog with the
// blend keyword the probe `probe` defines.
DrawStep Surface(float distance, Pixel color, Cover cover = Cover::Fullscreen,
                 const std::string& probe = "forward_fog_probe", bool alphaBlend = false)
{
    return {probe, cover, {color[0], color[1], color[2], color[3], 0.0f, 0.0f, distance, 1.0f}, alphaBlend};
}

void ExpectPixelNear(const Pixel& actual, const Pixel& expected, float tolerance, const std::string& what)
{
    for (int c = 0; c < 3; ++c)
        EXPECT_NEAR(actual[c], expected[c], tolerance) << what << ", channel " << c;
}

class HeightFogRendering : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No usable Vulkan device";
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("HeightFogTests"));
        ASSERT_TRUE(m_Sampler.IsValid());

        m_View = CreateBuffer(sizeof(ViewParamsUBO), BufferUsage::Uniform);
        m_Fog = CreateBuffer(sizeof(HeightFogParamsUBO), BufferUsage::Uniform);
        m_Sky = CreateBuffer(sizeof(SkyUBOGPU), BufferUsage::Uniform);
        m_Atmosphere = CreateBuffer(sizeof(AtmosphereParametersGPU), BufferUsage::Uniform);
        // The view's ExposureHistory, as the pipeline binds it: scene colour stored at pre-exposure 1.
        m_Exposure = CreateBuffer(GameEngine::Engine::Renderer::ExposureReadbackFeature::kStateBytes,
                                  BufferUsage::Storage);
        GameEngine::Engine::Renderer::ExposureReadbackFeature::State exposure{};
        exposure.Scale = 1.0f;
        exposure.Valid = 1u;
        exposure.PreExposure = 1.0f;
        exposure.PreviousPreExposure = 1.0f;
        m_Device->UpdateBuffer(m_Exposure, 0, sizeof(exposure), &exposure);

        ViewParamsUBO view{};
        const auto proj = Projection();
        const auto invProj = GameEngine::Mathematics::Inverse(proj);
        std::memcpy(view.ge_invProj, invProj.Data(), sizeof(view.ge_invProj));
        std::memcpy(view.ge_proj, proj.Data(), sizeof(view.ge_proj));
        for (int i = 0; i < 4; ++i)
        {
            view.ge_view[i * 5] = 1.0f;
            view.ge_invView[i * 5] = 1.0f;
        }
        view.ge_nearFar[0] = kNear;
        view.ge_nearFar[1] = kFar;
        view.ge_cameraPosWS[3] = 1.0f;
        for (float& f : view.ge_screenSize)
            f = 1.0f;
        view.ge_exposureParams[0] = 1.0f;
        m_Device->UpdateBuffer(m_View, 0, sizeof(view), &view);

        // A gradient sky that is red in every direction, seen by the same camera: the sky's basis is
        // the identity view's, a 90 degree square frustum.
        SkyUBOGPU sky{};
        sky.cameraRight[0] = 1.0f;
        sky.cameraUp[1] = 1.0f;
        sky.cameraForward[2] = 1.0f;
        sky.viewportAspect = 1.0f;
        sky.tanHalfFovY = 1.0f;
        sky.viewportResolution[0] = sky.viewportResolution[1] = 1.0f;
        sky.skyboxParams[1] = -1.0f;
        for (float* gradient : {sky.gradientSkyTop, sky.gradientSkyHorizon, sky.gradientSkyBottom})
            gradient[0] = 1.0f;
        sky.gradientSkyTop[3] = 1.0f;
        m_Device->UpdateBuffer(m_Sky, 0, sizeof(sky), &sky);
        const AtmosphereParametersGPU atmosphere{};
        m_Device->UpdateBuffer(m_Atmosphere, 0, sizeof(atmosphere), &atmosphere);

        m_Black = Upload({0, 0, 0, 1});
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (auto image : m_Images)
            m_Device->DestroyTexture(image);
        for (auto& [key, program] : m_Programs)
            m_Device->DestroyPipeline(program.Handle);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        for (auto buffer : {m_View, m_Fog, m_Sky, m_Atmosphere, m_Exposure})
            if (buffer.IsValid())
                m_Device->DestroyBuffer(buffer);
        m_Device->Shutdown();
    }

    BufferHandle CreateBuffer(size_t size, BufferUsage usage)
    {
        BufferDesc bd{};
        bd.size = size;
        bd.usage = static_cast<uint32_t>(usage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        auto buffer = m_Device->CreateBuffer(bd);
        Require(buffer.IsValid(), "buffer allocation failed");
        return buffer;
    }

    // Distance fog only, green, reaching the far plane: a fourth of the way to opaque per fourth of
    // the far distance, with no sun and no sky fill unless a test turns it on.
    static HeightFogParamsUBO DistanceFog()
    {
        HeightFogParamsUBO fog{};
        fog.intensity = 1.0f;
        fog.density = 1.0f;
        fog.maxOpacity = 1.0f;
        fog.minDistance = 0.0f;
        fog.smoothLength = 1.0f;
        fog.maxDistance = kFar;
        fog.flags = HeightFogFlagDistanceEnabled;
        fog.emissiveR = 0.0f;
        fog.emissiveG = 1.0f;
        fog.emissiveB = 0.0f;
        fog.sunIntensity = 0.0f;
        fog.colorSource = static_cast<int32_t>(HeightFogColorSource::Fixed);
        return fog;
    }

    void SetFog(const HeightFogParamsUBO& fog) { m_Device->UpdateBuffer(m_Fog, 0, sizeof(fog), &fog); }

    TextureHandle Allocate(uint32_t samples = 1)
    {
        TextureDesc td{};
        td.width = td.height = td.depth = 1;
        td.mipLevels = td.arrayLayers = 1;
        td.sampleCount = samples;
        td.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget |
                                         TextureUsage::TransferSrc | TextureUsage::TransferDst);
        td.debugName = "HeightFogTests.Image";
        auto image = m_Device->CreateTexture(td);
        Require(image.IsValid(), "image allocation failed");
        m_Images.push_back(image);
        return image;
    }

    TextureHandle Upload(Pixel pixel)
    {
        auto image = Allocate();
        auto upload = m_Device->CreateUploadBuffer(sizeof(Pixel), "HeightFogTests.Upload");
        Require(upload.IsValid(), "upload allocation failed");
        m_Device->UpdateBuffer(upload, 0, sizeof(Pixel), pixel.data());
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image, ResourceState::Undefined, ResourceState::CopyDest));
        cl->CopyBufferToTextureSubresource(upload, image, 0, 0, 1, 1, 0, sizeof(Pixel));
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image, ResourceState::CopyDest, ResourceState::ShaderResource));
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(upload);
        return image;
    }

    const Program& GetProgram(const DrawStep& step, uint32_t samples)
    {
        const std::string key = step.Fragment + (step.Covers == Cover::HalfPixel ? "|half|" : "|full|") +
                                std::to_string(samples) + (step.AlphaBlend ? "|blend" : "");
        auto [it, added] = m_Programs.try_emplace(key);
        Program& p = it->second;
        if (!added)
            return p;
        const char* vertex = step.Covers == Cover::HalfPixel ? "Shaders/msaa_edge_cover_probe.vert.spv"
                                                             : "Shaders/fullscreen_noinput.vert.spv";
        auto vs = Utils::LoadShaderFile(vertex);
        auto fs = Utils::LoadShaderFile(("Shaders/" + step.Fragment + ".frag.spv").c_str());
        Require(!vs.empty() && !fs.empty(), "missing shader: " + step.Fragment);
        StageReflectionResult rvs{}, rfs{};
        ReflectionOptions opts{};
        Require(ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vs.data()), vs.size() / 4,
                             opts, rvs, nullptr),
                "vertex reflection failed");
        Require(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fs.data()), fs.size() / 4,
                             opts, rfs, nullptr),
                "fragment reflection failed: " + step.Fragment);
        p.Meta = MergeStages({rvs, rfs});
        Require(p.Meta.Sets.size() == 1 && p.Meta.Sets[0].Set == 0, "unexpected descriptor sets in " + step.Fragment);
        p.Desc.type = PipelineType::Graphics;
        p.Desc.vertexShader = vs;
        p.Desc.pixelShader = fs;
        MaterialBuilder::FormatsHint formats{};
        formats.ColorFormats = {static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT)};
        std::string why;
        Require(MaterialBuilder::BuildPipelineDescFromMeta(p.Meta, p.Desc, formats, MaterialBuilder::MergeMode::Auto,
                                                           MaterialBuilder::PushConstantPolicy{}, &why),
                why);
        p.Desc.rasterizationSamples = samples;
        p.Desc.rasterizationState.cullMode = CullModeFlagBits::None;
        if (step.AlphaBlend)
            p.Desc.EnableBlending(true, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);
        p.Handle = m_Device->CreatePipeline(p.Desc);
        Require(p.Handle.IsValid(), "pipeline creation failed: " + step.Fragment);
        return p;
    }

    // A program's set 0, bound by name: the view, the fog, the sky's own blocks, the exposure state
    // and, for every texture m_Textures does not name, black (the gradient sky and the fixed fog
    // colour sample none of them).
    DescriptorSetHandle BindSet(const Program& p)
    {
        DescriptorSetDesc sd{};
        sd.layout = p.Desc.descriptorSetLayouts[0];
        sd.transient = false;
        auto ds = m_Device->CreateDescriptorSet(sd);
        Require(ds.IsValid(), "descriptor allocation failed");
        for (const auto& binding : p.Meta.Sets[0].Bindings)
        {
            const std::string& name = binding.Name;
            if (binding.Type == ShaderMetaBindingType::kCombinedImageSampler)
            {
                const auto texture = m_Textures.find(name);
                m_Device->UpdateCombinedImageSamplerBinding(
                    ds, binding.Binding, texture == m_Textures.end() ? m_Black : texture->second, m_Sampler);
            }
            else if (name == "uExposure" || name == "ExposureHistory")
                m_Device->UpdateStorageBufferBinding(ds, binding.Binding, m_Exposure, 0,
                                                     GameEngine::Engine::Renderer::ExposureReadbackFeature::kStateBytes);
            else if (name.find("HeightFog") != std::string::npos)
                m_Device->UpdateBufferBinding(ds, binding.Binding, m_Fog, 0, sizeof(HeightFogParamsUBO));
            else if (name.find("ViewParams") != std::string::npos)
                m_Device->UpdateBufferBinding(ds, binding.Binding, m_View, 0, sizeof(ViewParamsUBO));
            else if (name == "uSky")
                m_Device->UpdateBufferBinding(ds, binding.Binding, m_Sky, 0, sizeof(SkyUBOGPU));
            else if (name == "uAtmos")
                m_Device->UpdateBufferBinding(ds, binding.Binding, m_Atmosphere, 0, sizeof(AtmosphereParametersGPU));
            else
                throw std::runtime_error("unexpected binding " + name);
        }
        return ds;
    }

    // Draws `steps` in order into one pixel of `samples` samples, resolves a multisampled pixel with
    // the render pass's resolve, as the world pass resolves scene colour, and reads it back.
    Pixel Render(const std::vector<DrawStep>& steps, uint32_t samples = 1)
    {
        std::vector<std::pair<const Program*, DescriptorSetHandle>> bound;
        for (const DrawStep& step : steps)
        {
            const Program& p = GetProgram(step, samples);
            bound.push_back({&p, BindSet(p)});
        }
        const TextureHandle target = Allocate(samples);
        const TextureHandle resolved = samples > 1 ? Allocate() : target;
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::Undefined, ResourceState::RenderTarget));
        if (samples > 1)
            cl->Barrier(
                ResourceBarrier::CreateTextureBarrier(resolved, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc rp{};
        rp.colorTargets[0] = target;
        rp.colorTargetCount = 1;
        rp.clearColor[0] = true;
        rp.clearColorValue[0][0] = rp.clearColorValue[0][1] = rp.clearColorValue[0][2] = 0.0f;
        rp.clearColorValue[0][3] = 1.0f;
        rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        if (samples > 1)
            rp.resolveColorTargets[0] = resolved;
        cl->BeginRenderPass(rp);
        cl->SetViewport(0.0f, 0.0f, 1.0f, 1.0f);
        cl->SetScissor(0, 0, 1, 1);
        for (size_t i = 0; i < steps.size(); ++i)
        {
            const Program& p = *bound[i].first;
            cl->SetPipeline(p.Handle);
            cl->BindDescriptorSet(0, bound[i].second, p.Handle);
            if (!p.Meta.PushConstants.empty())
                cl->SetPushConstantsById(0, steps[i].Push.data(),
                                         std::min<size_t>(p.Meta.PushConstants[0].Size, sizeof(steps[i].Push)), 0);
            cl->Draw(3, 1);
        }
        cl->EndRenderPass();
        cl->Barrier(
            ResourceBarrier::CreateTextureBarrier(resolved, ResourceState::RenderTarget, ResourceState::CopySource));
        auto rb = m_Device->CreateReadbackBuffer(sizeof(Pixel), "HeightFogTests.Readback");
        Require(rb.IsValid(), "readback allocation failed");
        cl->CopyTextureToBuffer(resolved, rb, 1, 1, 0, 0, 0, 0);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        for (auto& [program, ds] : bound)
            m_Device->DestroyDescriptorSet(ds);

        Pixel px{};
        auto* data = m_Device->MapBuffer(rb);
        Require(data != nullptr, "readback map failed");
        std::memcpy(px.data(), data, sizeof(px));
        m_Device->UnmapBuffer(rb);
        m_Device->DestroyBuffer(rb);
        for (float channel : px)
            Require(std::isfinite(channel), "nonfinite output");
        return px;
    }

    std::unique_ptr<IDevice> m_Device;
    SamplerHandle m_Sampler{};
    BufferHandle m_View{}, m_Fog{}, m_Sky{}, m_Atmosphere{}, m_Exposure{};
    TextureHandle m_Black{};
    // Textures bound by name instead of black.
    std::map<std::string, TextureHandle> m_Textures;
    std::map<std::string, Program> m_Programs;
    std::vector<TextureHandle> m_Images;
};

TEST_F(HeightFogRendering, FogThickensRightUpToTheFarPlane)
{
    // A surface shaded in the last metres before the far plane takes its fog like any other: the fog
    // is evaluated where the surface is shaded, so no depth test can take it for sky.
    SetFog(DistanceFog());
    float previous = 0.0f;
    for (float distance : {1500.0f, 1700.0f, 1790.0f, 1795.0f, 1799.0f, 1799.9f})
    {
        const Pixel px = Render({Surface(distance, kRed)});
        const float opacity = px[1];
        EXPECT_GE(opacity, previous - 1e-5f) << "at " << distance;
        EXPECT_NEAR(px[0] + px[1], 1.0f, 1e-4f) << "at " << distance;
        previous = opacity;
    }
    // Optical depth 4 at the far plane leaves 1 - e^-4 of the surface behind the fog.
    EXPECT_GT(previous, 0.97f);
}

TEST_F(HeightFogRendering, ClearedDepthIsSkyAndKeepsTheSceneWithoutSkyFog)
{
    // The sky pass with the sky fill off leaves the sky as it draws it.
    SetFog(DistanceFog());
    const Pixel px = Render({Sky()});
    EXPECT_EQ(px[0], 1.0f);
    EXPECT_EQ(px[1], 0.0f);
    EXPECT_EQ(px[2], 0.0f);
    EXPECT_EQ(px[3], 1.0f);
}

TEST_F(HeightFogRendering, ClearedDepthTakesSkyFogWhenItIsOn)
{
    // Looking along the horizon with the sky fill raised by half: the sky takes half its full amount,
    // 0.5 * (1 - e^-4) at full density, the fill the fog pass gave the cleared depth. A surface at the
    // far plane takes the whole distance fog instead (above 0.97), so the two read apart.
    HeightFogParamsUBO fog = DistanceFog();
    fog.flags |= HeightFogFlagSkyEnabled;
    fog.skyHorizonOffset = 0.5f;
    SetFog(fog);
    const Pixel px = Render({Sky()});
    const float skyAmount = 0.5f * (1.0f - std::exp(-4.0f));
    EXPECT_NEAR(px[1], skyAmount, 1e-4f);
    EXPECT_NEAR(px[0], 1.0f - skyAmount, 1e-4f);
}

TEST_F(HeightFogRendering, IntensityBlendsTheFogTowardTheScene)
{
    // Half the intensity lands halfway between the unfogged colour and the full fog, on a surface and
    // on the sky fill alike. No intensity, or no density, leaves the colour exactly.
    HeightFogParamsUBO fog = DistanceFog();
    fog.flags |= HeightFogFlagSkyEnabled;
    fog.skyHorizonOffset = 0.5f;
    for (const DrawStep& step : {Surface(900.0f, kRed), Sky()})
    {
        SCOPED_TRACE(step.Fragment);
        fog.intensity = 1.0f;
        SetFog(fog);
        const Pixel full = Render({step});
        ASSERT_GT(full[1], 0.1f) << "the full fog must move the pixel for the blend to be measurable";
        fog.intensity = 0.5f;
        SetFog(fog);
        const Pixel half = Render({step});
        for (int channel = 0; channel < 4; ++channel)
            EXPECT_NEAR(half[channel], 0.5f * (kRed[channel] + full[channel]), 1e-4f) << "channel " << channel;
        fog.intensity = 0.0f;
        SetFog(fog);
        EXPECT_EQ(Render({step}), kRed) << "at intensity 0";
        fog.intensity = 1.0f;
        fog.density = 0.0f;
        SetFog(fog);
        EXPECT_EQ(Render({step}), kRed) << "at density 0";
        fog.density = 1.0f;
    }
}

TEST_F(HeightFogRendering, HalfCoveredEdgePixelTakesItsSurfacesFog)
{
    // An anti-aliased silhouette under 4x MSAA: a surface covers samples 2 and 3 of a pixel, and the
    // sky, or a fogged far surface, the other two. The surface's fragment carries its own fog onto the
    // samples it covers and the far side's stays on the others, so the resolved pixel is exactly the
    // average of the two sides' own fog: the coverage-exact edge. A fog applied once per pixel by one
    // depth (sample 0, which lies on the far side here) gives the whole pixel the far side's fog.
    HeightFogParamsUBO fog = DistanceFog();
    fog.minDistance = 150.0f;
    fog.smoothLength = 150.0f;
    fog.flags |= HeightFogFlagSkyEnabled;
    SetFog(fog);

    // The probe triangle covers half the pixel's samples: a surface drawn over the black clear
    // resolves to half of its colour.
    const Pixel half = Render({Surface(60.0f, kRed, Cover::HalfPixel)}, kEdgeSamples);
    ASSERT_NEAR(half[0], 0.5f, 1e-4f) << "the edge probe no longer covers two of the four samples";

    for (const bool againstSky : {true, false})
    {
        SCOPED_TRACE(againstSky ? "against the sky" : "against a fogged far surface");
        const DrawStep far = againstSky ? Sky() : Surface(1500.0f, kBlue);
        for (float nearDistance : {60.0f, 400.0f})
        {
            SCOPED_TRACE("surface at " + std::to_string(nearDistance) + " m");
            const Pixel farAlone = Render({far});
            const Pixel nearAlone = Render({Surface(nearDistance, kRed)});
            ASSERT_GT(std::abs(farAlone[1] - nearAlone[1]), 0.05f)
                << "the two sides take the same fog, so the fixture cannot tell the rules apart";
            const Pixel edge = Render({far, Surface(nearDistance, kRed, Cover::HalfPixel)}, kEdgeSamples);
            Pixel exact{};
            for (int c = 0; c < 4; ++c)
                exact[c] = 0.5f * (farAlone[c] + nearAlone[c]);
            ExpectPixelNear(edge, exact, 1e-4f, "the edge pixel");
        }
    }
}

TEST_F(HeightFogRendering, BlendedSurfaceInFrontOfAFarSurfaceKeepsItsOwnFog)
{
    // A straight-alpha quad 4 m away in front of a surface 1,500 m away, blended over it as the
    // transparent passes blend: the quad takes the fog of 4 m, almost none, and the far surface its
    // own. A fog applied to the pixel by the far surface's depth fogs the quad as the far surface.
    const Pixel quad{1.0f, 0.0f, 0.0f, 0.5f};
    const auto blendQuad = [&](float intensity, Pixel& farOut)
    {
        HeightFogParamsUBO fog = DistanceFog();
        fog.intensity = intensity;
        SetFog(fog);
        farOut = Render({Surface(1500.0f, kBlue)});
        return Render({Surface(1500.0f, kBlue), Surface(4.0f, quad, Cover::Fullscreen, "forward_fog_probe", true)});
    };
    Pixel far{}, farOff{};
    const Pixel blended = blendQuad(1.0f, far);
    const Pixel quadAlone = Render({Surface(4.0f, quad)});
    for (int c = 0; c < 3; ++c)
        EXPECT_NEAR(blended[c], quadAlone[c] * quad[3] + far[c] * (1.0f - quad[3]), 1e-4f) << "channel " << c;
    // The quad's red excess over the far surface stays within 5 percent of the unfogged quad's.
    const Pixel blendedOff = blendQuad(0.0f, farOff);
    const float excessOff = blendedOff[0] - farOff[0];
    const float excessOn = blended[0] - far[0];
    ASSERT_GT(excessOff, 0.4f);
    EXPECT_GT(excessOn / excessOff, 0.95f) << "the quad keeps " << 100.0f * excessOn / excessOff << " percent";
}

TEST_F(HeightFogRendering, EveryBlendFadesItsSurfaceTowardItsIdentity)
{
    // At 900 m the fog has transmittance T and in-scatter S, read from an opaque surface of the colour
    // and of black. Each blend keyword fades the surface toward the colour that leaves what is behind
    // it unchanged: straight alpha c T + S, premultiplied c T + S a, additive c T, multiply
    // mix(1, c, T), and a blend with no identity colour keeps c.
    SetFog(DistanceFog());
    const Pixel color{0.9f, 0.35f, 0.1f, 0.4f};
    const Pixel opaque = Render({Surface(900.0f, color)});
    const Pixel inScatter = Render({Surface(900.0f, {0.0f, 0.0f, 0.0f, 0.4f})});
    const float transmittance = (opaque[0] - inScatter[0]) / color[0];
    ASSERT_GT(transmittance, 0.05f);
    ASSERT_LT(transmittance, 0.95f);
    Pixel premultiplied{}, additive{}, multiply{};
    for (int c = 0; c < 3; ++c)
    {
        premultiplied[c] = color[c] * transmittance + inScatter[c] * color[3];
        additive[c] = color[c] * transmittance;
        multiply[c] = 1.0f + (color[c] - 1.0f) * transmittance;
    }
    ExpectPixelNear(Render({Surface(900.0f, color, Cover::Fullscreen, "forward_fog_probe_premultiplied")}),
                    premultiplied, 1e-4f, "premultiplied");
    ExpectPixelNear(Render({Surface(900.0f, color, Cover::Fullscreen, "forward_fog_probe_additive")}), additive,
                    1e-4f, "additive");
    ExpectPixelNear(Render({Surface(900.0f, color, Cover::Fullscreen, "forward_fog_probe_multiply")}), multiply,
                    1e-4f, "multiply");
    ExpectPixelNear(Render({Surface(900.0f, color, Cover::Fullscreen, "forward_fog_probe_unfogged")}), color, 1e-6f,
                    "no identity colour");
}

TEST_F(HeightFogRendering, ABlendedSurfaceBeyondTheCloudsTakesTheirTransmittance)
{
    // The late transparents draw after the clouds composite. A blended surface nearer than the clouds'
    // mean distance at its pixel is drawn over them unchanged; one beyond it takes the clouds'
    // transmittance as its blend composites it: straight alpha on its alpha, additive on its colour.
    HeightFogParamsUBO off = DistanceFog();
    off.intensity = 0.0f;
    SetFog(off);
    constexpr float kCloudTransmittance = 0.3f;
    constexpr float kCloudDistance = 500.0f;
    m_Textures["ge_volumetricClouds"] = Upload({0.0f, kCloudTransmittance, kCloudDistance, 0.0f});
    const Pixel sprite{0.8f, 0.4f, 0.2f, 0.5f};
    const Pixel near = Render({Surface(400.0f, sprite, Cover::Fullscreen, "forward_fog_probe_clouds")});
    ExpectPixelNear(near, sprite, 1e-6f, "in front of the clouds");
    EXPECT_FLOAT_EQ(near[3], sprite[3]);
    const Pixel far = Render({Surface(800.0f, sprite, Cover::Fullscreen, "forward_fog_probe_clouds")});
    ExpectPixelNear(far, sprite, 1e-6f, "the colour of a straight-alpha surface beyond the clouds");
    EXPECT_NEAR(far[3], sprite[3] * kCloudTransmittance, 1e-6f);
    const Pixel farAdditive = Render({Surface(800.0f, sprite, Cover::Fullscreen, "forward_fog_probe_clouds_additive")});
    ExpectPixelNear(farAdditive,
                    {sprite[0] * kCloudTransmittance, sprite[1] * kCloudTransmittance, sprite[2] * kCloudTransmittance, 1},
                    1e-6f, "an additive surface beyond the clouds");
}

} // namespace
