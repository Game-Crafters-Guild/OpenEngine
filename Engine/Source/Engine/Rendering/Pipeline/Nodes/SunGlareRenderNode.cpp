#include "Engine/Rendering/Pipeline/Nodes/SunGlareRenderNode.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/ViewExposureHistory.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Engine/Rendering/SunGlareRenderFeature.h"
#include "Engine/Rendering/SunScreenProjection.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Sky/SkyRenderer.h"
#include "Rendering/Sky/SkySettings.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;
using namespace ::GameEngine::Rendering;

namespace
{

// Radius of the disc the visibility terms sample, in units of the sun's angular radius: the
// screen probe's ring and the cascade term's taps, so both fade over the same sweep of the sun.
// The ring must not reach much past the disc: at three radii the taps on the occluded side leave
// the sum while the disc is still fully hidden, which showed up as a bright blob sitting on a
// hillside with no sun in it. Just over one radius keeps the probe asking about the sun rather
// than its surroundings, and the fade still spans the disc's own width because the taps are
// area-distributed inside it.
constexpr float kProbeRadiusInSunRadii = 1.4f;



// Night fade at which the sun stops being drawn at all. The sky fades the disc out with
// `1 - nightSkyBlend` as the sun goes below the horizon (SkyEnvironmentSystem computes the
// blend, sky_render.frag multiplies the disc by it), and the halo has to follow the SAME
// curve: a hard elevation cutoff would pop the glare off while the disc was still visibly
// fading. Past this the disc is gone and the pass is not declared.
constexpr float kNightFadeCutoff = 0.98f;

// Size of the cascade term's buffer: one float, padded to the smallest storage-buffer range
// every backend takes.
constexpr uint32_t kCastersBufferBytes = 16u;

// The cascade term's pass: one workgroup (sun_glare_casters.comp) evaluates the fraction of the
// sun's disc whose beam reaches the eye past every shadow caster, into a buffer of one float
// the glare's vertex stage reads. A pass of its own because the term is some 700 independent
// lookups: each of the workgroup's 64 lanes runs one tap of one cascade, so the pass costs the
// latency of one tap rather than of all of them.
RenderGraph::RGBuffer DeclareCastersPass(ViewDeclare& d, const SunGlareRenderFeature& feature,
                                         RenderGraph::RGTexture shadowArray,
                                         const PipelineBufferBindingRG& shadowData,
                                         const SunGlareCastersPushConstants& pc)
{
    BufferDesc desc{};
    desc.size = kCastersBufferBytes;
    desc.stride = sizeof(float);
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    desc.debugName = "SunGlare.Casters";
    const RenderGraph::RGBuffer casters = d.Frame.CreateBuffer(d.PassName("Casters").c_str(), desc);
    if (!casters.IsValid())
        return casters;

    const ComputePipelineId pipe = feature.GetCastersPipelineId();
    // The feature outlives the frame, so the pass reads its layout in place.
    const DescriptorSetLayoutDesc* layout = &feature.GetCastersLayout();
    const SamplerHandle shadowSampler = feature.GetShadowSampler();
    const uint32_t shadowArrayBinding = feature.GetCastersShadowArrayBinding();
    const uint32_t shadowDataBinding = feature.GetCastersShadowDataBinding();
    const uint32_t outputBinding = feature.GetCastersOutputBinding();
    d.Frame.AddPass(
        d.PassName("Casters").c_str(), Rendering::PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(shadowArray, RenderGraph::RGTextureRead::SampledCompute);
            if (shadowData.Graph.IsValid())
                p.Read(shadowData.Graph, RenderGraph::RGBufferRead::Uniform);
            p.Write(casters, RenderGraph::RGBufferWrite::Storage);
        },
        [pipe, layout, shadowSampler, shadowArray, shadowData, casters, shadowArrayBinding,
         shadowDataBinding, outputBinding, pc](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            // These returns leave the buffer unwritten, and the glare reads that frame's float
            // clamped to [0, 1]. They are not reached while the graph and the device work: the
            // pipeline id was valid at declare and GetOrCreatePipelineVariant builds it now (a
            // failure logs), and RGFrame asserts when a declared resource has no physical one.
            const PipelineHandle pso = ctx.GetOrCreatePipelineVariant(pipe);
            const TextureHandle shadowPhys = ctx.GetTexture(shadowArray);
            const BufferHandle castersPhys = ctx.GetBuffer(casters);
            if (!pso.IsValid() || !shadowPhys.IsValid() || !castersPhys.IsValid())
                return;
            DescriptorSetDesc dsDesc{};
            dsDesc.layout = *layout;
            dsDesc.debugName = "SunGlare.Casters.DS";
            dsDesc.transient = true;
            auto ds = dev->CreateDescriptorSet(dsDesc);
            dev->UpdateCombinedImageSamplerBinding(ds, shadowArrayBinding, shadowPhys, shadowSampler);
            dev->UpdateBufferBinding(ds, shadowDataBinding, shadowData.Buffer,
                                     static_cast<size_t>(shadowData.Offset),
                                     static_cast<size_t>(shadowData.Size));
            dev->UpdateStorageBufferBinding(ds, outputBinding, castersPhys, 0, kCastersBufferBytes);
            cl->SetPipeline(pso);
            cl->BindDescriptorSet(0, ds, pso);
            cl->SetPushConstants(pc);
            cl->Dispatch(1u, 1u, 1u);
        });
    return casters;
}

} // namespace

bool SunGlareRenderNode::Initialize(std::string nodeId, std::string nodeJson,
                                    std::string* outError)
{
    m_Id = std::move(nodeId);
    m_OutputRef = Names::View::Resolve;

    try
    {
        const auto j = nlohmann::json::parse(nodeJson);
        if (j.is_object() && j.contains("output") && j["output"].is_string())
            m_OutputRef = j["output"].get<std::string>();
    }
    catch (const std::exception& ex)
    {
        if (outError)
            *outError = std::string("SunGlare node JSON parse failed: ") + ex.what();
        return false;
    }

    return true;
}

void SunGlareRenderNode::DeclareForView(ViewDeclare& d)
{
    auto& rs = d.Services;
    auto* device = rs.GetDevice();
    if (!device)
        return;

    // No sky, no sun, no glare. The same gate the disc is behind: showSunDisk is what the
    // user turns off to hide the sun, and the halo of a hidden sun is a contradiction.
    auto* skyFeat = rs.GetFeature<SkyRenderFeature>();
    if (!skyFeat || !skyFeat->IsInitialized() || !skyFeat->HasActiveSettings())
        return;
    const Rendering::SkySettings& sky = skyFeat->GetSettings();
    if (!sky.showSunDisk || !(sky.primarySunIntensity > 0.0f))
        return;

    const float nightAmount = Math::Clamp01(sky.nightSkyBlend);
    if (nightAmount >= kNightFadeCutoff)
        return;

    const Mathematics::Vector3 sunDir(sky.scatteringSunDir[0], sky.scatteringSunDir[1],
                                      sky.scatteringSunDir[2]);

    auto& renderer = skyFeat->GetRenderer();
    if (!renderer.IsTransmittanceComputed())
        return;

    const CameraData* camera = rs.Views().FindCameraData(d.View.cameraId);
    if (!camera)
        return;

    // Orthographic views have no glare. Every ray is parallel, so the angle to the sun is
    // the same at every pixel and the kernel would evaluate to one constant — a flat wash
    // over the whole viewport rather than a halo. The sky already treats these views
    // separately, drawing a 2D backdrop instead of the atmosphere. A projection matrix's
    // (3,3) element is 0 when it divides by w and 1 when it does not.
    if (std::fabs(camera->proj[15]) > 0.5f)
        return;

    RenderGraph::RGTexture target{};
    if (m_OutputRef == Names::View::Resolve)
        target = d.ViewResolve.IsValid() ? d.ViewResolve : d.ResolveTexture(Names::View::Resolve);
    else if (m_OutputRef == Names::View::Color)
        target = d.ViewColor.IsValid() ? d.ViewColor : d.ResolveTexture(Names::View::Color);
    else
        target = d.ResolveTexture(m_OutputRef);
    if (!target.IsValid() && d.ViewColor.IsValid())
        target = d.ViewColor;
    if (!target.IsValid())
        return;

    const PipelineBufferBindingRG viewParams = d.ResolveBuffer(Names::Res::ViewParams);
    if (!viewParams.IsValid())
        return;

    auto& feature = rs.EnsureFeature<SunGlareRenderFeature>();
    if (!feature.IsInitialized() && !feature.Initialize(device))
        return;

    // The scene depth ATTACHMENT, not a resolve of it. The probe reads sixteen texels, which
    // does not justify a resolve pass of its own — and a resolve is a pass that has to be
    // scheduled and paid for every frame whether the sun is up or not.
    //
    // The attachment also holds every depth writer's depth, a forward draw that writes its own
    // in the World pass included (ForwardDrawDepth::ColourPass), which the PREPASS resolve does
    // not: a probe reading that resolve would let the sun glare through such a surface. This
    // pass declares at kPostProcess, after every depth writer, so the read-after-write edge the
    // graph derives from it is what orders the two — no explicit pass ordering is needed or wanted.
    //
    // Multisampled whenever MSAA is on, which is why there are two pipelines: a sampler2D
    // and a sampler2DMS variant, chosen from the attachment's own sample count below.
    RenderGraph::RGTexture depth = d.ViewDepth;
    if (!depth.IsValid())
        depth = d.ResolveTexture(Names::View::Depth);

    const RenderGraph::RGTexture trans = d.Frame.ImportExternalTexture(
        "Sky_Transmittance_LUT", renderer.GetTransmittanceLutTexture(),
        Rendering::ResourceState::ShaderResource);
    if (!trans.IsValid())
        return;

    // Shadow cascades: the array the shadow pass rendered plus the block carrying its
    // per-cascade matrices. Both absent (no directional shadows this frame) simply disables
    // the cascade term — the pass still runs on the screen probe alone.
    const RenderGraph::RGTexture shadowArray = rs.GetShadowMapArrayRG(d.Frame, d.View.id);
    const PipelineBufferBindingRG shadowData = d.ResolveBuffer(Names::Res::ShadowData);
    const bool cascadesReady = shadowArray.IsValid() && shadowData.IsValid();

    SunGlarePushConstants pc{};
    pc.SunDirRadius[0] = sunDir.x;
    pc.SunDirRadius[1] = sunDir.y;
    pc.SunDirRadius[2] = sunDir.z;
    // The width of the SOURCE, which sets the glare kernel's core: scattered light carries no
    // structure finer than the thing that made it, so the halo is flat across the sun instead of
    // adding a second spike on top of the drawn disc.
    //
    // Deliberately the physical radius rather than the radius the sky DRAWS the disc at: the two
    // part company once SkyEnvironment::SunSize is moved off 1, and it is the source that
    // scatters. A stylistically enlarged disc therefore keeps a halo sized for the real sun,
    // which is the intent — but it also means the halo stops spreading visibly outside a disc a
    // few times oversized.
    pc.SunDirRadius[3] = kSunAngularRadiusRad;
    pc.SunColorIrradiance[0] = sky.primarySunColor[0];
    pc.SunColorIrradiance[1] = sky.primarySunColor[1];
    pc.SunColorIrradiance[2] = sky.primarySunColor[2];
    pc.SunColorIrradiance[3] = sky.primarySunIntensity;

    // Everything the sky scales the drawn sun by, so the halo tracks the disc exactly: the
    // exposure trim it applies to the whole dome, times the day-to-night fade that takes the
    // disc out as the sun sets.
    pc.Params[0] = std::exp2(sky.exposureEV) * (1.0f - nightAmount);

    // The ray to the sun as the transmittance LUT lookup takes it: its zenith cosine and the
    // observer's normalised altitude, with the shell's radii in Horizon. All are one value per
    // frame, so the shader takes them as scalars and does the single tap in its vertex stage.
    Rendering::AtmosphereParametersGPU atmo{};
    Rendering::SkyRenderer::FillDefaultAtmosphere(atmo);
    const float planetCenterY = -atmo.planetRadius;
    const Mathematics::Vector3 fromCenter(camera->cameraPos[0], camera->cameraPos[1] - planetCenterY,
                                          camera->cameraPos[2]);
    const float r = std::max(std::sqrt(fromCenter.LengthSquared()), 1e-5f);
    const Mathematics::Vector3 up = fromCenter / r;
    pc.Params[1] = std::clamp(Mathematics::Vector3::Dot(sunDir, up), -1.0f, 1.0f);
    pc.Params[2] = Math::Clamp01((r - atmo.planetRadius) / std::max(atmo.atmosphereRadius - atmo.planetRadius, 1e-3f));

    // Where the sun lands on screen, for the occlusion probe. Off-screen the probe has
    // nothing to ask about, so it is skipped and the halo continues from beyond the frame
    // edge — which is what a bright source just outside the field really does. The kernel's
    // own falloff is the only thing that fades it; there is no separate edge dial.
    const std::optional<Mathematics::Vector3> ndc = ProjectSunDirectionToNdc(*camera, sunDir);
    const Mathematics::Vector3 sunNdc = ndc.value_or(Mathematics::Vector3());
    const bool onScreen =
        ndc && std::fabs(sunNdc.x) <= 1.0f && std::fabs(sunNdc.y) <= 1.0f && depth.IsValid();
    pc.Probe[0] = sunNdc.x * 0.5f + 0.5f;
    pc.Probe[1] = 0.5f - sunNdc.y * 0.5f;
    // Ring radius PER AXIS. The projection's [0][0] and [1][1] terms are the reciprocals of
    // the frustum's horizontal and vertical half-extents, so scaling the same angular radius
    // by each gives a ring that is a circle in PIXELS. One scalar on both axes stretches it
    // by the viewport aspect — on a 3.4:1 view the horizontal taps land three times further
    // from the sun than the vertical ones, and anything narrower can never occlude it.
    const float projXX = std::fabs(camera->proj[0]);
    const float projYY = std::fabs(camera->proj[5]);
    const float visibilityDiscRadius = kProbeRadiusInSunRadii * kSunAngularRadiusRad;
    pc.Probe[2] = visibilityDiscRadius * projXX * 0.5f;
    pc.Probe[3] = visibilityDiscRadius * projYY * 0.5f;

    // The depth attachment's sample count decides the variant, and it also IS the probe's
    // enable flag: the shader reads params.w as the count and treats 0 as "no probe this
    // frame", so a sun off screen and a missing depth texture take the same branch.
    const uint32_t depthSamples =
        depth.IsValid() ? std::max(d.Frame.Graph().ResourceDesc(depth.Id).SampleCount, 1u) : 1u;
    pc.Params[3] = onScreen ? static_cast<float>(depthSamples) : 0.0f;

    // The terrain skyline along the sun's own azimuth, on the CPU, from the heightfield.
    //
    // This is the term that does not care where the camera points. The screen probe is gated on
    // the sun projecting inside the viewport, so a sun one degree outside the frame edge cannot
    // be occluded by anything at all -- the halo's edge glow and far veil stayed at full
    // strength behind a hill and then switched off the frame after the sun's centre crossed in.
    // A skyline is a property of the terrain and the eye, so it holds on- and off-frame, and
    // past whatever distance the shadow cascades reach.
    //
    // Compared as TANGENTS, and faded over the sun's own angular radius rather than a chosen
    // width: the disc is hidden when the skyline is one radius above its centre and clear when
    // it is one radius below, which is exactly the geometry of a disc setting behind a ridge.
    float horizonVisibility = 1.0f;
    {
        // Marched on the terrain side (TerrainExtractionSystem), which is the only layer that
        // can see both the heightfield and the renderer. Absent means "no terrain to report
        // on", which must read as fully visible rather than as flat ground.
        const float horizonTangent = feature.GetSkylineTangent(d.View.id);
        if (horizonTangent > SunGlareRenderFeature::kNoSkyline)
        {
            // Both sides as tangents of the elevation above the horizontal. sunDir is a unit
            // vector, so its Y IS the sine of the sun's elevation and the horizontal component
            // is the cosine.
            const float sunHoriz =
                std::sqrt(std::max(sunDir.x * sunDir.x + sunDir.z * sunDir.z, 1e-12f));
            const float sunTangent = sunDir.y / sunHoriz;
            const float radiusTangent = kSunAngularRadiusRad; // tan(r) == r at this size
            const float t = Math::Clamp01((sunTangent - (horizonTangent - radiusTangent)) / (2.0f * radiusTangent));
            horizonVisibility = t * t * (3.0f - 2.0f * t); // smoothstep over the disc's diameter
        }
    }
    pc.Horizon[0] = horizonVisibility;
    pc.Horizon[1] = atmo.planetRadius;
    pc.Horizon[2] = atmo.atmosphereRadius;

    // The cascade term needs the eye, the sun and the disc; the shadow data block carries the
    // matrices and the cascade COUNT (ge_shadowParams.z), which is not passed again because a
    // second copy could disagree with it. No cascades this frame leaves the screen probe and
    // the skyline as the only gates.
    RenderGraph::RGBuffer casters{};
    if (cascadesReady && feature.GetCastersPipelineId().IsValid())
    {
        SunGlareCastersPushConstants castersPc{};
        castersPc.Eye = Mathematics::Vector4(camera->cameraPos[0], camera->cameraPos[1],
                                             camera->cameraPos[2], 0.0f);
        castersPc.SunDirRadius = Mathematics::Vector4(sunDir.x, sunDir.y, sunDir.z,
                                                      visibilityDiscRadius);
        casters = DeclareCastersPass(d, feature, shadowArray, shadowData, castersPc);
    }
    pc.Horizon[3] = casters.IsValid() ? 1.0f : 0.0f;

    // Draw into the view's letterboxed region, the way the sky pass does. A fullscreen draw
    // over the whole target would tint the bars the sky deliberately left cleared, and the
    // NDC the vertex stage emits is mapped onto whatever this viewport is — so the halo
    // lands where the frame is either way.
    uint32_t vpX = 0, vpY = 0, vpW = d.RenderWidth, vpH = d.RenderHeight;
    const ViewLetterbox letterbox = rs.Views().GetViewLetterbox(d.View.id);
    if (letterbox.active)
    {
        vpX = letterbox.x;
        vpY = letterbox.y;
        vpW = letterbox.width;
        vpH = letterbox.height;
    }
    // One of the two variants, by the attachment's sample count. An MSAA view whose
    // multisampled variant failed to load gets no glare rather than a multisampled image bound
    // to a sampler2D, which is undefined — the feature logs that case at initialization.
    const GraphicsPipelineId pipe = feature.GetPipelineId(depthSamples);
    if (!pipe.IsValid())
        return;
    const DescriptorSetLayoutDesc* layout = &feature.GetLayout(depthSamples);
    const SamplerHandle sampler = feature.GetSampler();
    const uint32_t transBinding = feature.GetTransmittanceBinding();
    const uint32_t depthBinding = feature.GetDepthBinding();
    const uint32_t viewParamsBinding = feature.GetViewParamsBinding();
    const uint32_t castersBinding = feature.GetCastersBinding();
    const uint32_t exposureBinding = feature.GetExposureBinding();
    // The glare lands in scene colour at the frame's pre-exposure P.
    const PipelineBufferBindingRG exposure = ResolveViewExposureHistory(d);
    if (!exposure.IsValid())
        return;

    d.Frame.AddPass(
        d.PassName().c_str(), Rendering::PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Load; // additive over the scene
            ops.Store = RenderGraph::RGStoreOp::Store;
            p.AttachColor(0, target, ops);
            p.Read(trans, RenderGraph::RGTextureRead::Sampled);
            if (depth.IsValid())
                p.Read(depth, RenderGraph::RGTextureRead::Sampled);
            // A blueprint buffer backed by upload memory dissolves into AllocUpload and
            // carries no graph id: the binding is valid while the edge is not. Declaring an
            // invalid edge asserts, so gate on the graph handle the way every other
            // buffer-reading node does.
            if (viewParams.Graph.IsValid())
                p.Read(viewParams.Graph, RenderGraph::RGBufferRead::Uniform);
            if (casters.IsValid())
                p.Read(casters, RenderGraph::RGBufferRead::Storage);
            p.Read(exposure.Graph, RenderGraph::RGBufferRead::Storage);
        },
        [pipe, layout, sampler, pc, trans, depth, viewParams, casters, vpX, vpY, vpW, vpH,
         transBinding, depthBinding, viewParamsBinding, castersBinding, exposureBinding,
         exposure](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto pso = ctx.GetOrCreatePipelineVariant(pipe);
            if (!pso.IsValid())
                return;

            const TextureHandle transPhys = ctx.GetTexture(trans);
            if (!transPhys.IsValid())
                return;
            // The probe is disabled in the push constants when depth is unavailable, so a
            // type-compatible dummy is safe here (the LUT is the one texture always bound).
            // The dummy is single-sample, and it can only be reached on the single-sample
            // pipeline: the multisampled variant is selected from a VALID depth attachment.
            const TextureHandle depthPhys = depth.IsValid() ? ctx.GetTexture(depth) : TextureHandle{};

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = *layout;
            dsDesc.debugName = "SunGlare.DS";
            dsDesc.transient = true;
            auto ds = dev->CreateDescriptorSet(dsDesc);
            dev->UpdateCombinedImageSamplerBinding(ds, transBinding, transPhys, sampler);
            dev->UpdateCombinedImageSamplerBinding(
                ds, depthBinding, depthPhys.IsValid() ? depthPhys : transPhys, sampler);
            dev->UpdateBufferBinding(ds, viewParamsBinding, viewParams.Buffer,
                                     static_cast<size_t>(viewParams.Offset),
                                     static_cast<size_t>(viewParams.Size));
            // The cascade term is disabled through the push constants when its buffer is
            // absent, but the set still has to be complete: the exposure buffer, a storage
            // buffer of more than one float, stands in.
            const BufferHandle castersPhys =
                casters.IsValid() ? ctx.GetBuffer(casters) : BufferHandle{};
            if (castersPhys.IsValid())
                dev->UpdateStorageBufferBinding(ds, castersBinding, castersPhys, 0,
                                                kCastersBufferBytes);
            else
                dev->UpdateStorageBufferBinding(ds, castersBinding, exposure.Buffer,
                                                exposure.Offset, exposure.Size);
            dev->UpdateStorageBufferBinding(ds, exposureBinding, exposure.Buffer, exposure.Offset,
                                            exposure.Size);

            cl->SetPipeline(pso);
            cl->SetViewport(static_cast<float>(vpX), static_cast<float>(vpY),
                            static_cast<float>(vpW), static_cast<float>(vpH));
            cl->SetScissor(vpX, vpY, vpW, vpH);
            cl->BindDescriptorSet(0, ds, pso);
            cl->SetPushConstants(pc);
            cl->Draw(3, 1);
        });
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
