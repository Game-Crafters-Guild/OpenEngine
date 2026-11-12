#include "Mathematics/Interpolation.h"
#include "UI/UIManager.h"

#include "Logger/Logger.h"
#include "Platform/SystemMetrics.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineDescTranslator.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/ReverseZ.h"
#include "../UIOpaqueInteriors.h"
#include "UI/CaretBlink.h"
#include "UI/UIFrameBufferRing.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
namespace
{

// The opaque pre-pass's D16 depth target, at the UI target's size. Invalid
// when that size is not known at declaration or the target is multisampled.
RenderGraph::RGTexture CreateOverlayDepth(RenderGraph::RGFrame& frame, RenderGraph::RGTexture target,
                                          IDevice& device)
{
    const RenderGraph::RGResourceDesc& targetDesc = frame.Graph().ResourceDesc(target.Id);
    uint32_t width = targetDesc.Width;
    uint32_t height = targetDesc.Height;
    if ((width == 0 || height == 0) && frame.IsBackbuffer(target))
        device.GetSwapchainSize(width, height);
    if (width == 0 || height == 0 || targetDesc.SampleCount != 1)
        return {};

    TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::D16_UNORM);
    desc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil);
    desc.debugName = "UI Overlay Depth";
    return frame.CreateTexture("UI Overlay Depth", desc);
}

// The main draw over the pre-pass depth: it writes no depth, and Greater is strict, so a
// primitive's own interior, which the pre-pass painted at the equal depth, fails.
Rendering::PipelineDesc OverPrePassDepth(Rendering::PipelineDesc desc)
{
    desc.depthStencilState.depthTestEnable = true;
    desc.depthStencilState.depthWriteEnable = false;
    desc.depthStencilState.depthCompareOp = CompareOp::Greater;
    return desc;
}

} // namespace
#endif

// RenderGraph pass of the SDF UI renderer — the only render path since the
// retained twin was deleted. Declared fresh every frame: no retained pass
// state, no bindingsHash/targetHash recreate machinery. The viewport is the
// engine's one orientation — a positive-height rect, clip +Y on its top row —
// and ui_sdf.vert maps its top-left-origin pixels into that. SdfPushConstants
// block: the UI shader outputs LINEAR, the terminal encode owns the only OETF.
int UIManager::ResolveOutputEncoding(UI::UITargetSpace targetSpace)
{
    static_assert(UI::UITargetSpace::kKindCount == 5,
                  "A new UITargetSpace needs an outputEncoding arm here.");
    switch (targetSpace.GetKind())
    {
    case UI::UITargetSpace::Kind::EncodedSrgb:
        return 1;
    case UI::UITargetSpace::Kind::HdrPq:
        return 2;
    case UI::UITargetSpace::Kind::Hlg:
        return 3;
    case UI::UITargetSpace::Kind::ScRgb:
        return 4;
    case UI::UITargetSpace::Kind::LinearSdr:
    case UI::UITargetSpace::Kind::Count: // size sentinel; no factory produces it
        break;
    }
    return 0;
}

UIManager::TextCoverageConstants UIManager::ResolveTextCoverageConstants(
    float contrast, float blendGamma, UI::UITargetSpace targetSpace)
{
    static_assert(UI::UITargetSpace::kKindCount == 5,
                  "A new UITargetSpace needs a blend-space arm here: a target that "
                  "interpolates raw sRGB-encoded bytes must resolve BlendSpaceEncoded = 1 "
                  "(the shader's Skia-direction retarget) — and its pins in "
                  "TextCoverageCorrectionTests must be updated with it.");
    // EncodedSrgb is the one attachment that interpolates raw sRGB-encoded
    // bytes; it selects the shader's Skia-direction retarget arm. Every other
    // space — LinearSdr and the three HDR flavours — is a linear-light
    // attachment: the ROP interpolates linear values and the terminal encode
    // owns the OETF. blendGamma names the TARGET space in both arms, so it
    // passes through un-gated: at its 0 default the linear arm is the
    // identity retarget by mathematics, while the encoded arm selects the
    // full Skia-direction correction (area-exact ink out of an encoded
    // source-over — Chrome's SK_GAMMA_SRGB configuration).
    const int blendSpaceEncoded =
        targetSpace.GetKind() == UI::UITargetSpace::Kind::EncodedSrgb ? 1 : 0;
    return TextCoverageConstants{Math::Clamp01(contrast),
                                 std::clamp(blendGamma, 0.0f, 4.0f), blendSpaceEncoded};
}

bool UIManager::RenderRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture target,
                         UI::UITargetSpace targetSpace)
{
    // Default arm: the UI owns the target and clears it (editor chrome path).
    return RenderRG(frame, target, targetSpace, RenderGraph::RGLoadOp::Clear);
}

bool UIManager::RenderRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture target,
                         UI::UITargetSpace targetSpace, RenderGraph::RGLoadOp loadOp)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!target.IsValid() || !m_Device)
        return false;

    // Per-frame prep (init, styles, primitives, ring uploads). The pass is
    // declared even on zero-draw or
    // unready (!fd.ready: no root / shaders missing) frames — the UI pass is
    // the target's CLEARER (it absorbed the old Editor.ClearPresentationTarget),
    // and a presented target must never hold unwritten memory. The exec body
    // keeps every draw early-out; the attachment ClearOps run regardless.
    const SdfFrameData fd = PrepareSdfFrameData();

    // Resolve each deferred binding against this frame's publish map; a stale
    // publish is skipped and its slot keeps its last texture. The execute step
    // writes through the reservation, so a slot released before it runs stays as is.
    struct ResolvedBinding
    {
        ::GenerationalVector::Handle Reservation;
        RenderGraph::RGTexture Tex{};
    };
    std::vector<ResolvedBinding> resolved;
    resolved.reserve(m_SdfDeferredRGBindings.size());
    for (const auto& binding : m_SdfDeferredRGBindings)
    {
        if (binding.ExternalName.empty())
            continue; // old-arm-only binding (rgRef) — not consumable here
        const auto itPub = m_ExternalRGPublished.find(binding.ExternalName);
        if (itPub == m_ExternalRGPublished.end())
            continue;
        const PublishedRGTexture& pub = itPub->second;
        if (!pub.For.IsFor(frame) || pub.TexId == RenderGraph::kInvalidId)
            continue; // stale publish — never consume a foreign frame's id
        resolved.push_back({binding.Reservation, RenderGraph::RGTexture{pub.TexId}});
    }

    const BufferHandle primBuf      = m_SdfPrimRing ? m_SdfPrimRing->GetBuffer() : BufferHandle{};
    const BufferHandle clipBuf      = m_SdfClipRing ? m_SdfClipRing->GetBuffer() : BufferHandle{};
    const BufferHandle drawOrderBuf = m_SdfDrawOrderRing ? m_SdfDrawOrderRing->GetBuffer() : BufferHandle{};
    const BufferHandle transformBuf = m_SdfTransformRing ? m_SdfTransformRing->GetBuffer() : BufferHandle{};

    const bool uploadsOk = fd.UploadsOk;

    // Output encoding resolves before pipeline selection: it feeds both the
    // push constants and the subpixel activation gate. Both derive from the
    // HOST-declared target space — never from the display (#784): an SDR
    // offscreen attachment stays unlifted (and subpixel-eligible) under an
    // HDR display, and an HDR-bound attachment takes its lift under an SDR
    // display.
    const int outputEncoding = ResolveOutputEncoding(targetSpace);

    // The encoded-blend target selects the UI_BLEND_SPACE_ENCODED shader
    // variants: paint adapters pass CSS bytes raw and encode sampled linear
    // sources, so the ROP interpolates encoded values (#767). A build staged
    // without the variant renders NOTHING into an encoded declaration (the
    // clear still runs — the pass owns the target) rather than silently
    // blending in the wrong space.
    const bool blendEncoded =
        targetSpace.GetKind() == UI::UITargetSpace::Kind::EncodedSrgb;
#if defined(GE_DEV_DIAG)
    if (blendEncoded && !m_SdfEncodedVariantAvailable)
    {
        static bool s_WarnedEncodedVariantMissing = false;
        if (!s_WarnedEncodedVariantMissing)
        {
            s_WarnedEncodedVariantMissing = true;
            Logger::Log::Warning(
                "[UI] target declared EncodedSrgb but the encoded shader variant is not "
                "staged — UI draws are skipped (clear-only frames)");
        }
    }
#endif

    // Subpixel RGB text AA swaps in the dual-source pipeline twin — the twin
    // of whichever blend-space variant the target selected. Every gate miss
    // (setting off, HDR-space target, no dualSrcBlend, no staged variant)
    // lands on the matching grayscale pipeline — same draws, never dropped
    // content and never a wrong-space pipeline.
    const bool subpixelActive = ResolveTextSubpixelActive(
        m_TextSubpixelAA, outputEncoding,
        m_Device->GetCapabilities().supportsDualSourceBlending,
        blendEncoded ? m_SdfSubpixelEncodedVariantAvailable : m_SdfSubpixelVariantAvailable);
    m_LastResolvedOutputEncoding = outputEncoding;
    m_LastTextSubpixelActive = subpixelActive;

    // Text-correction constants resolve at declare time with the rest of the
    // target-space translation: the attachment's blend space selects the
    // retarget arm, in this one place.
    const TextCoverageConstants textCoverage =
        ResolveTextCoverageConstants(m_TextContrast, m_TextBlendGamma, targetSpace);

    // The opaque pre-pass runs when any interior is listed and its pipeline
    // for this target space is staged; its depth target lives this frame only.
    const BufferHandle opaqueBuf = m_SdfOpaqueRing ? m_SdfOpaqueRing->GetBuffer() : BufferHandle{};
    const uint32_t opaqueCount = static_cast<uint32_t>(m_SdfOpaqueInteriors->Interiors().size());
    const bool opaqueStaged =
        m_OpaquePrePassEnabled && (blendEncoded ? m_SdfOpaqueEncodedAvailable : m_SdfOpaqueAvailable);
    const RenderGraph::RGTexture opaqueDepth =
        uploadsOk && opaqueStaged && opaqueCount > 0 && opaqueBuf.IsValid()
            ? CreateOverlayDepth(frame, target, *m_Device)
            : RenderGraph::RGTexture{};

    frame.AddPass(
        "UI Overlay", static_cast<int32_t>(PassPhase::kUI),
        [&](RenderGraph::RGPassBuilder& p)
        {
            // Load-op is caller-chosen. Clear when the UI owns/initializes the
            // target (backbuffer direct-attach, or the FinalLinear transient —
            // the editor chrome path; Clear is the import contract there). Load
            // when compositing the UI OVER existing content (a game HUD over the
            // scene's FinalColor). The clear color below is ignored under Load.
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = loadOp;
            ops.Store = RenderGraph::RGStoreOp::Store;
            ops.Clear.Color[0] = 0.0f;
            ops.Clear.Color[1] = 0.0f;
            ops.Clear.Color[2] = 0.0f;
            ops.Clear.Color[3] = 1.0f;
            p.AttachColor(0, target, ops);

            if (opaqueDepth.IsValid())
            {
                RenderGraph::RGAttachmentOps depthOps{};
                depthOps.Load = RenderGraph::RGLoadOp::Clear;
                depthOps.Store = RenderGraph::RGStoreOp::DontCare;
                depthOps.Clear.Depth = ReverseZ::kClearFar;
                p.AttachDepth(opaqueDepth, depthOps);
                p.Read(frame.ImportExternalBuffer("UISdfOpaque", opaqueBuf),
                       RenderGraph::RGBufferRead::Storage);
            }

            // SSBO reads (ring buffers rotate per device frame; the import
            // dedups by physical so repeat declares are id-stable).
            if (primBuf.IsValid())
                p.Read(frame.ImportExternalBuffer("UISdfPrims", primBuf),
                       RenderGraph::RGBufferRead::Storage);
            if (clipBuf.IsValid())
                p.Read(frame.ImportExternalBuffer("UISdfClips", clipBuf),
                       RenderGraph::RGBufferRead::Storage);
            if (drawOrderBuf.IsValid())
                p.Read(frame.ImportExternalBuffer("UISdfDrawOrder", drawOrderBuf),
                       RenderGraph::RGBufferRead::Storage);
            if (transformBuf.IsValid())
                p.Read(frame.ImportExternalBuffer("UISdfTransforms", transformBuf),
                       RenderGraph::RGBufferRead::Storage);

            // One sampled read per resolved deferred binding — this IS the
            // barrier that orders the producers (scene/game pipelines) before
            // the UI samples them; in the hybrid era that ordering came from
            // executing the RenderGraph frame before the old graph compiled.
            for (const ResolvedBinding& rb : resolved)
                p.Read(rb.Tex, RenderGraph::RGTextureRead::Sampled);
        },
        [this, resolved, primBuf, clipBuf, drawOrderBuf, transformBuf, uploadsOk, outputEncoding,
         subpixelActive, blendEncoded, textCoverage, opaqueBuf, opaqueCount,
         opaquePrePass = opaqueDepth.IsValid()](RenderGraph::RGContext& ctx)
        {
            // Slot updates run even on draw-skipped frames: the published
            // producers rendered this frame, and the registry slots must
            // track the current physicals for the NEXT drawn frame.
            if (m_SdfTextureRegistry)
            {
                for (const ResolvedBinding& rb : resolved)
                {
                    const TextureHandle physical = ctx.GetTexture(rb.Tex);
                    if (physical.IsValid())
                        m_SdfTextureRegistry->UpdateSlot(rb.Reservation, physical);
                }
            }
            const size_t drawCount = m_DrawOrder.size();
            if (!uploadsOk || drawCount == 0)
                return;
            const size_t totalSlots = m_PrimitiveAllocator.GetTotalSlots();
            if (totalSlots == 0)
                return;

            const uint32_t frameIdx = m_Device->GetFrameIndex();

            // The paper-white / black-lift constants stay device-derived: they
            // parameterize the lift's STRENGTH for a target whose declared
            // space enables it; the target space alone decides WHETHER the
            // lift runs.
            const auto hdrState = m_Device->GetHdrOutputState();

            // Encoded declaration without the staged variant: skip the draws
            // (the attachment clear above already ran). Falling back to the
            // linear pipeline here would be a wrong-SPACE render, which is
            // strictly worse than missing content.
            if (blendEncoded && !m_SdfEncodedVariantAvailable)
                return;

            // A pass with no depth attachment takes a pipeline with no depth state.
            Rendering::GraphicsPipelineId& pipelineId =
                opaquePrePass
                    ? m_SdfPipelineIdOverPrePass[(blendEncoded ? 2u : 0u) + (subpixelActive ? 1u : 0u)]
                : blendEncoded
                    ? (subpixelActive ? m_SdfPipelineIdSubpixelEncoded : m_SdfPipelineIdEncoded)
                    : (subpixelActive ? m_SdfPipelineIdSubpixel : m_SdfPipelineId);
            if (!pipelineId.IsValid())
            {
                Rendering::PipelineDesc& pipelineDesc =
                    blendEncoded ? (subpixelActive ? m_SdfPipelineDescSubpixelEncoded
                                                   : m_SdfPipelineDescEncoded)
                                 : (subpixelActive ? m_SdfPipelineDescSubpixel : m_SdfPipelineDesc);
                pipelineDesc.debugName =
                    blendEncoded ? (subpixelActive ? "UI_SDF_Subpixel_Encoded" : "UI_SDF_Encoded")
                                 : (subpixelActive ? "UI_SDF_Subpixel" : "UI_SDF");
                pipelineId = Rendering::PipelineDescTranslator::InternGraphics(
                    *m_Device, opaquePrePass ? OverPrePassDepth(pipelineDesc) : pipelineDesc);
            }
            const PipelineHandle pipeline = ctx.GetOrCreatePipelineVariant(pipelineId);
            if (!pipeline.IsValid())
                return;
            ctx.Cmd->SetPipeline(pipeline);

            uint32_t w = m_LastLayoutWidth;
            uint32_t h = m_LastLayoutHeight;
            if (w == 0 || h == 0)
                m_Device->GetSwapchainSize(w, h);

            ctx.Cmd->SetViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
            ctx.Cmd->SetScissor(0, 0, w, h);

            // Verbatim twin of the old arm's push-constant block — the
            // single-OETF invariant lives here (outputEncoding/paperWhite/
            // blackLift drive LINEAR-space compositing; no encode).
            SdfPushConstants pc{};
            pc.TargetSize[0] = static_cast<float>(w);
            pc.TargetSize[1] = static_cast<float>(h);
            pc.TimeSeconds = m_Time;
            // Read per frame rather than cached, so changing the OS caret
            // setting takes effect without restarting the editor. This is a
            // win32k transition, not a shared-memory read like GetTickCount —
            // roughly two orders of magnitude dearer — but at sub-microsecond
            // cost once per frame it sits far below a frame's noise floor.
            pc.CaretPhaseTogglesPerSecond =
                UI::CaretPhaseTogglesPerSecond(Platform::GetCaretBlinkHalfPeriod());
            pc.TextContrast = textCoverage.Contrast;
            pc.TextBlendGamma = textCoverage.BlendGamma;
            pc.TextBlendSpaceEncoded = textCoverage.BlendSpaceEncoded;
            pc.EdgeSoftness = std::max(0.01f, m_TextSmoothingGamma);
            pc.OutputEncoding = outputEncoding;
            const float deviceWhite =
                std::clamp(hdrState.staticMetadata.paperWhiteNits, kHdrPaperWhiteFloorNits, 1000.0f);
            pc.PaperWhiteNits = m_HdrUiPaperWhiteNits > 0.0f
                ? std::clamp(m_HdrUiPaperWhiteNits, 40.0f, 350.0f)
                : deviceWhite;
            pc.DeviceWhiteNits = deviceWhite;
            // Auto (< 0): in HDR modes, lift by the display's black floor so
            // dark chrome sits at the darkest level the panel can actually
            // show — effectively 0 on OLED, the backlight floor on LCD.
            pc.BlackLiftNits = m_HdrUiBlackLiftNits >= 0.0f
                ? m_HdrUiBlackLiftNits
                : (pc.OutputEncoding >= 2
                       ? std::clamp(hdrState.staticMetadata.minMasteringLuminance,
                                    kHdrUiBlackLiftMinNits, kHdrUiBlackLiftMaxNits)
                       : 0.0f);
            if (!primBuf.IsValid() || !drawOrderBuf.IsValid() || !transformBuf.IsValid())
                return;

            if (opaquePrePass)
            {
                RecordSdfOpaquePrePass(ctx, pc, opaqueBuf, opaqueCount, blendEncoded);
                ctx.Cmd->SetPipeline(pipeline);
            }

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_SdfSsboSetLayout;
            dsDesc.debugName = "UISdf_SSBO_Set";
            dsDesc.transient = true;
            DescriptorSetHandle ssboSet = m_Device->CreateDescriptorSet(dsDesc);
            if (!ssboSet.IsValid())
                return;

            const size_t primBytes  = totalSlots * sizeof(UI::UIPrimitive);
            const size_t primOffset = m_SdfPrimRing->GetFrameOffset(frameIdx);
            m_Device->UpdateStorageBufferBinding(ssboSet, 0, primBuf, primOffset, primBytes);

            if (clipBuf.IsValid())
            {
                const size_t clipSlots = m_ClipAllocator.GetTotalSlots();
                const size_t clipBytes = std::max(clipSlots, size_t(1)) * sizeof(UI::UIClipRect);
                const size_t clipOffset = m_SdfClipRing->GetFrameOffset(frameIdx);
                m_Device->UpdateStorageBufferBinding(ssboSet, 1, clipBuf, clipOffset, clipBytes);
            }

            const size_t drawOrderBytes  = drawCount * sizeof(uint32_t);
            const size_t drawOrderOffset = m_SdfDrawOrderRing->GetFrameOffset(frameIdx);
            m_Device->UpdateStorageBufferBinding(ssboSet, 2, drawOrderBuf, drawOrderOffset,
                                                 drawOrderBytes);

            const size_t transformBytes =
                std::max<size_t>(1, m_TransformAllocator.GetTotalSlots()) * sizeof(UI::UITransformSlot);
            const size_t transformOffset = m_SdfTransformRing->GetFrameOffset(frameIdx);
            m_Device->UpdateStorageBufferBinding(ssboSet, 3, transformBuf, transformOffset, transformBytes);

            ctx.Cmd->BindDescriptorSet(0, ssboSet, pipeline);

            // Compat profile: one draw per run, each against the bind group
            // holding exactly that run's textures, with the run's first
            // draw-order index on a push constant. Painter's order is intact —
            // runs are consecutive spans of the same list, never a reordering.
            if (m_SdfCompatProfile)
            {
                if (!m_SdfTextureRegistry)
                    return;
                for (const UI::UICompatDrawRun& run : m_SdfCompatRuns.Runs())
                {
                    if (run.Count == 0)
                        continue;
                    const DescriptorSetHandle texSet = m_SdfTextureRegistry->GetDrawRunSet(run);
                    if (!texSet.IsValid())
                        continue;
                    ctx.Cmd->BindDescriptorSet(1, texSet, pipeline);
                    pc.DrawBase = run.First;
                    ctx.Cmd->SetConstants(0, sizeof(pc), &pc);
                    ctx.Cmd->Draw(/*vertexCount=*/6u,
                                  /*instanceCount=*/run.Count,
                                  /*firstVertex=*/0u,
                                  /*firstInstance=*/0u);
                }
                return;
            }

            ctx.Cmd->SetConstants(0, sizeof(pc), &pc);

            if (m_SdfTextureRegistry)
            {
                DescriptorSetHandle texSet = m_SdfTextureRegistry->GetFrameSet();
                if (texSet.IsValid())
                    ctx.Cmd->BindDescriptorSet(1, texSet, pipeline);
            }

            ctx.Cmd->Draw(/*vertexCount=*/6u,
                          /*instanceCount=*/static_cast<uint32_t>(drawCount),
                          /*firstVertex=*/0u,
                          /*firstInstance=*/0u);
        });
    return true;
#else
    (void)frame;
    (void)target;
    (void)targetSpace;
    return false;
#endif
}

void UIManager::RecordSdfOpaquePrePass(RenderGraph::RGContext& ctx, const SdfPushConstants& pc,
                                       BufferHandle opaqueBuf, uint32_t opaqueCount, bool blendEncoded)
{
    Rendering::GraphicsPipelineId& pipelineId =
        blendEncoded ? m_SdfOpaquePipelineIdEncoded : m_SdfOpaquePipelineId;
    if (!pipelineId.IsValid())
        pipelineId = Rendering::PipelineDescTranslator::InternGraphics(
            *m_Device, blendEncoded ? m_SdfOpaquePipelineDescEncoded : m_SdfOpaquePipelineDesc);
    // A missing pipeline leaves the depth at its clear, where the main draw
    // passes everywhere: the frame is the painter's-order frame.
    const PipelineHandle pipeline = ctx.GetOrCreatePipelineVariant(pipelineId);
    if (!pipeline.IsValid())
        return;

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_SdfOpaqueSetLayout;
    dsDesc.debugName = "UISdf_Opaque_Set";
    dsDesc.transient = true;
    const DescriptorSetHandle opaqueSet = m_Device->CreateDescriptorSet(dsDesc);
    if (!opaqueSet.IsValid())
        return;
    m_Device->UpdateStorageBufferBinding(opaqueSet, 0, opaqueBuf,
                                         m_SdfOpaqueRing->GetFrameOffset(m_Device->GetFrameIndex()),
                                         opaqueCount * sizeof(UI::UIOpaqueInterior));

    ctx.Cmd->SetPipeline(pipeline);
    ctx.Cmd->BindDescriptorSet(0, opaqueSet, pipeline);
    ctx.Cmd->SetConstants(0, sizeof(pc), &pc);
    ctx.Cmd->Draw(/*vertexCount=*/6u, /*instanceCount=*/opaqueCount, /*firstVertex=*/0u,
                  /*firstInstance=*/0u);
}
