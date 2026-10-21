// HDR highlight-extension contract for tonemap.frag, measured on the real GPU path.
//
// THE REGRESSION THIS FILE EXISTS FOR
// -----------------------------------
// A tone mapper does two jobs: it compresses brightness AND it renders colour
// (desaturates highlights, holds hue, compresses out-of-gamut). Under HDR output
// the pass must extend the SELECTED operator's own display-referred result into
// the panel's headroom. It must never crossfade OUT of the operator toward a
// generic scene-derived roll, because that keeps the brightness compression and
// discards the colour rendering exactly where saturated sources live — and it
// makes every SDR-fit operator produce byte-identical pixels above the crossfade
// threshold, so the operator choice stops meaning anything.
//
// `OperatorsRemainDistinctAboveHeadroomThreshold` is that gate. It is the first
// test in the file because it is the reason the file exists.
//
// WHY A GPU TEST AND NOT A CPU MODEL
// ----------------------------------
// The claims are about what tonemap.frag computes, so the instrument is the
// compiled SPIR-V running through MaterialBuilder's pipeline, driven by the same
// push constants the render passes write. A CPU reimplementation would be a model
// of the shader and could agree with a broken shader. The render target is
// R32G32B32A32_FLOAT so values above paper white survive readback exactly; an
// 8-bit or FP16 target cannot represent what is being asserted.
//
// INSTRUMENT VERIFICATION (this suite is built to be able to fail)
// ---------------------------------------------------------------
//  * The push-constant writer is BY NAME off SPIR-V reflection and ASSERTs that
//    every name it needs was found. A renamed field fails loudly instead of
//    silently writing zeros into a field the shader then reads as "SDR".
//  * `HdrHeadroomIsActuallyAvailable` runs first and asserts the fixture's own
//    headroom premise, so a paper-white/max-nits plumbing change cannot make the
//    rest of the suite vacuously green.
//  * Every distinctness assertion carries a margin far above FP32 noise and is
//    stated pairwise with the operator names in the failure message.
//
// MUTATION TABLE — MEASURED, not predicted. Each mutation was applied to
// tonemap.frag, the .spv and the test exe deleted and confirmed absent, the
// shader recompiled, the .spv hash checked to differ from baseline (so a
// mutation that never reached the GPU could not masquerade as a weak test), and
// the suite re-run. Columns are the tests below, in file order:
//
//   0 HdrHeadroomIsActuallyAvailable        4 OperatorCeilingsMapOntoTheDisplay
//   1 OperatorsRemainDistinct...            5 DiffuseWhiteStaysAtOrBelowPaperWhite
//   2 SaturatedHighlightKeepsOperator...    6 HighlightsAboveKneeAreExtended
//   3 NeutralGainsHdrHeadroom               7 UnchangedPathsStayUnchanged
//
//   M1  restore the old scene-derived crossfade      RED: 1, 2, 4
//   M2  feed the extension `sceneLinear`             RED: 1, 2, 4, 5, 7
//   M3  route Neutral back to `return sdrTonemapped` RED: 1, 3, 4
//   M4  clamp the extended result to 1.0             RED: 1, 2, 3, 4
//   M5  knee 0.75 -> 0.70                            RED: 4, 5
//   M6  knee 0.75 -> 0.95                            RED: 1, 3, 4, 6
//   M7  drop the outEncoding guard (extend SDR too)  RED: 0, 3, 4, 6, 7
//
// Every test in the file goes RED under at least one mutation, including the
// instrument-premise test (M7 breaks it by extending Photographic's SDR
// output). M1 does NOT turn 3 red, because the historical defect was two
// separate faults — the crossfade AND Neutral's hard clamp — and M3 is the
// second one.
//
// M2 to M7 were measured on the three looks: each mutant applied to
// tonemap.frag, the .spv deleted, rebuilt (its hash differing from the
// baseline's) and the suite re-run, and each matched its row above. M1's row was
// measured on the eight-operator shader the looks replaced; the crossfade it
// restores is not reproduced on the looks. Filmic (ACES 2) is excluded from
// tests 2 and 7(b) by recorded decision (at those tests) and has its own
// contract test (8, outside the table).
//
// Numbers in the failure messages are shader units where 1.0 == paper white.
// At the fixture's 203-nit paper white / 1000-nit panel, outputMax == 4.9261.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <Rendering/Common/Utils.h>
#include <Rendering/Core/CommandList.h>
#include <Rendering/Core/Device.h>
#include <Rendering/Materials/MaterialBuilder.h>
#include <Rendering/Materials/ShaderReflection.h>

#include <Rendering/Utils/CubeLutFileParser.h>
#include <Rendering/Utils/CubeLutGpuUpload.h>

#include "Source/Passes/TonemapAces2Tables.h"

using namespace GameEngine::Rendering;

namespace
{

// tonemap.frag's `look` push constant: the three looks (Components::Look) and
// the no-look debug arm a view selects (Passes::kTonemapNoLookArm).
enum class Mode : int
{
    Neutral = 0,
    Filmic = 1,       // ACES 2
    Photographic = 2, // AgX
    NoLook = 3,
};

const char* ModeName(Mode m)
{
    switch (m)
    {
        case Mode::Neutral: return "Neutral";
        case Mode::Filmic: return "Filmic";
        case Mode::Photographic: return "Photographic";
        case Mode::NoLook: return "NoLook";
    }
    return "?";
}

// `outEncoding` push constant. 4 = HDR display-referred linear (headroom).
//
// The SDR arm is 1, NOT 0. Both take the same branch out of
// ApplyOutputRangeTonemap, but 0 is the hardware-sRGB swapchain path and falls
// through to this pass's Bayer dither, which biases a 1x1 probe by a fixed
// -0.492 LSB (measured: 0.001183 at ACES 1/scene-0.18, exactly
// 0.4921875 * lsbInLinear at Bayer8x8(0,0)). Comparing that against an
// undithered HDR result makes every "unchanged" assertion fail on a difference
// this suite is not about. Encoding 1 emits undithered display-referred linear
// and is what the main SDR chain actually uses (TonemapPass.h defaults to it).
constexpr int kEncodingHdrLinear = 4;
constexpr int kEncodingSdrLinear = 1;

// The fixture's display. 1.0 in shader units is paper white by construction
// (encode_srgb.frag multiplies by paperWhiteNits before PQ), so outputMax is
// 1000/203 == 4.9261 and "nits" below == value * 203.
constexpr float kPaperWhiteNits = 203.0f;
constexpr float kPanelPeakNits = 1000.0f;
constexpr float kOutputMax = kPanelPeakNits / kPaperWhiteNits;

struct Rgb
{
    float R = 0.0f, G = 0.0f, B = 0.0f;

    float Peak() const { return std::max(R, std::max(G, B)); }
};

std::string Fmt(const Rgb& c)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(%.6f, %.6f, %.6f)", c.R, c.G, c.B);
    return buf;
}

// The Cube LUT the pass samples.
enum class RigLut
{
    None,             // the identity tables at intensity 0: no LUT effect on the volume
    Half,             // a 2x2x2 table mapping every colour to half of itself
    Square,           // a 33^3 table mapping each channel x to x * x: a tone curve, not a scale
    IdentityFallback, // what a LUT slot holds when its file is missing or fails to load, at intensity 1
};

// A 33^3 .cube mapping each channel x to x * x over [0, 1] (red fastest, as the format orders it).
// Trilinear interpolation between its knots is within 1/4096 of x * x.
constexpr uint32_t kSquareLutSize = 33;

std::string SquareCubeLutText()
{
    std::string text = "LUT_3D_SIZE " + std::to_string(kSquareLutSize) + "\n";
    const float step = 1.0f / static_cast<float>(kSquareLutSize - 1);
    char line[96];
    for (uint32_t b = 0; b < kSquareLutSize; ++b)
        for (uint32_t g = 0; g < kSquareLutSize; ++g)
            for (uint32_t r = 0; r < kSquareLutSize; ++r)
            {
                const float rv = r * step, gv = g * step, bv = b * step;
                std::snprintf(line, sizeof(line), "%.7f %.7f %.7f\n", rv * rv, gv * gv, bv * bv);
                text += line;
            }
    return text;
}

struct TonemapParams
{
    float Exposure = 1.0f;
    Mode Look = Mode::Neutral;
    int DitherMode = 0;
    int OutEncoding = kEncodingHdrLinear;
    float PaperWhiteNits = kPaperWhiteNits;
    float MaxOutputNits = kPanelPeakNits;
    int PreserveAlpha = 0;
    int UseAutoExposure = 0;
    RigLut Lut = RigLut::None;
};

// Writes the tonemap push-constant block BY NAME from the reflected layout.
// Returns false if any field the suite drives is missing, so a rename fails the
// test rather than leaving a zero the shader reads as a different mode.
bool BuildPushConstants(const PushConstantRangeMeta& pcr, const TonemapParams& p,
                        const CubeLutGpuBindingState& lut, float lutIntensity,
                        std::vector<uint8_t>& out, std::string& missing)
{
    out.assign(pcr.Size, 0);

    struct FloatField { const char* Name; float Value; bool Seen; };
    struct IntField { const char* Name; int32_t Value; bool Seen; };

    FloatField floats[] = {
        {"exposure", p.Exposure, false},
        {"paperWhiteNits", p.PaperWhiteNits, false},
        {"maxOutputNits", p.MaxOutputNits, false},
        {"lutIntensity", lutIntensity, false},
        {"lut3dInMin", lut.In3DMin, false},
        {"lut3dInMax", lut.In3DMax, false},
        {"lut1dInMin", lut.In1DMin, false},
        {"lut1dInMax", lut.In1DMax, false},
    };
    IntField ints[] = {
        {"look", static_cast<int32_t>(p.Look), false},
        {"ditherMode", p.DitherMode, false},
        {"outEncoding", p.OutEncoding, false},
        {"preserveAlpha", p.PreserveAlpha, false},
        {"useAutoExposure", p.UseAutoExposure, false},
        {"lutInputEncoding", lut.InputEncoding, false},
        {"lutSize3d", lut.Size3D, false},
        {"lutSize1d", lut.Size1D, false},
        {"lutFlags", lut.Flags, false},
    };

    for (const auto& m : pcr.Block.Members)
    {
        for (auto& f : floats)
            if (m.Name == f.Name)
            {
                if (m.Offset + sizeof(float) > out.size())
                    return false;
                std::memcpy(out.data() + m.Offset, &f.Value, sizeof(float));
                f.Seen = true;
            }
        for (auto& i : ints)
            if (m.Name == i.Name)
            {
                if (m.Offset + sizeof(int32_t) > out.size())
                    return false;
                std::memcpy(out.data() + m.Offset, &i.Value, sizeof(int32_t));
                i.Seen = true;
            }
    }

    missing.clear();
    for (const auto& f : floats)
        if (!f.Seen)
            missing += std::string(f.Name) + " ";
    for (const auto& i : ints)
        if (!i.Seen)
            missing += std::string(i.Name) + " ";
    return missing.empty();
}

int OrdinalOf(const ShaderMeta& meta, uint32_t setIndex)
{
    std::vector<uint32_t> ord;
    ord.reserve(meta.Sets.size());
    for (const auto& s : meta.Sets)
        ord.push_back(s.Set);
    std::sort(ord.begin(), ord.end());
    ord.erase(std::unique(ord.begin(), ord.end()), ord.end());
    for (size_t i = 0; i < ord.size(); ++i)
        if (ord[i] == setIndex)
            return static_cast<int>(i);
    return -1;
}

// One device + one pipeline for the whole suite; every case is a 1x1 draw with a
// different scene colour and push-constant block.
class TonemapRig
{
public:
    // `fatal` distinguishes "this machine has no GPU" (a legitimate skip) from
    // "the thing under test is missing or broken" (never a skip — a filter or a
    // missing shader that exits PASSED is exactly how this suite would lie).
    bool Init(std::string& why, bool& fatal)
    {
        fatal = true;
#ifdef _WIN32
        _putenv_s("GE_HEADLESS_TEST", "1");
#else
        setenv("GE_HEADLESS_TEST", "1", 1);
#endif

#ifdef RENDERING_SHADER_OUTPUT_DIR
        Utils::SetShaderPathResolver(
            +[](const std::filesystem::path& rel) -> std::filesystem::path
            {
                const std::filesystem::path outDir(RENDERING_SHADER_OUTPUT_DIR);
                std::error_code ec;
                std::filesystem::path direct = outDir / rel;
                if (std::filesystem::exists(direct, ec))
                    return direct;
                return outDir.parent_path() / rel;
            });
#endif

        DeviceDesc dd{};
        dd.preferredAPI = GraphicsAPI::Vulkan;
        dd.enableDynamicRendering = true;
        m_Device = DeviceFactory::CreateDevice(dd);
        if (!m_Device || !m_Device->Initialize(dd))
        {
            why = "Vulkan device init failed";
            fatal = false; // no GPU on this machine
            return false;
        }

        std::vector<uint8_t> vs, fs;
        try
        {
            vs = Utils::LoadShaderFile("fullscreen_noinput.vert.spv");
            fs = Utils::LoadShaderFile("tonemap.frag.spv");
        }
        catch (const std::exception& e)
        {
            why = std::string("shader load threw: ") + e.what();
            return false;
        }
        if (vs.empty() || fs.empty())
        {
            why = "fullscreen_noinput.vert.spv / tonemap.frag.spv not found — shaders not compiled";
            return false;
        }

        StageReflectionResult rvs{}, rfs{};
        ReflectionOptions ro{};
        if (!ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vs.data()),
                          vs.size() / 4, ro, rvs, nullptr) ||
            !ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fs.data()),
                          fs.size() / 4, ro, rfs, nullptr))
        {
            why = "SPIR-V reflection failed";
            return false;
        }
        m_Meta = MergeStages({rvs, rfs});

        if (m_Meta.PushConstants.empty() || m_Meta.PushConstants[0].Block.Members.empty())
        {
            why = "tonemap.frag exposes no reflected push-constant block";
            return false;
        }
        m_Pcr = m_Meta.PushConstants[0];

        PipelineDesc pd{};
        pd.type = PipelineType::Graphics;
        pd.vertexShader = vs;
        pd.pixelShader = fs;
        pd.debugName = "TonemapHdrHeadroom";
        MaterialBuilder::FormatsHint fh{};
        fh.ColorFormats = {static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT)};
        std::string err;
        if (!MaterialBuilder::BuildPipelineDescFromMeta(m_Meta, pd, fh, MaterialBuilder::MergeMode::Auto,
                                                        MaterialBuilder::PushConstantPolicy{}, &err))
        {
            why = "BuildPipelineDescFromMeta: " + err;
            return false;
        }
        m_Pipeline = m_Device->CreatePipeline(pd);
        if (m_Pipeline == INVALID_HANDLE)
        {
            why = "CreatePipeline failed (R32G32B32A32_FLOAT colour target unsupported?)";
            return false;
        }

        TextureDesc srcD{};
        srcD.width = srcD.height = srcD.depth = 1;
        srcD.mipLevels = srcD.arrayLayers = srcD.sampleCount = 1;
        srcD.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        srcD.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        srcD.debugName = "TonemapHdrHeadroom.Scene";
        m_Src = m_Device->CreateTexture(srcD);

        TextureDesc dstD{};
        dstD.width = dstD.height = dstD.depth = 1;
        dstD.mipLevels = dstD.arrayLayers = dstD.sampleCount = 1;
        dstD.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        dstD.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        dstD.debugName = "TonemapHdrHeadroom.Out";
        m_Dst = m_Device->CreateTexture(dstD);

        m_Upload = m_Device->CreateUploadBuffer(16, "TonemapHdrHeadroom.Upload");
        m_Readback = m_Device->CreateReadbackBuffer(16, "TonemapHdrHeadroom.Readback");
        if (!m_Src.IsValid() || !m_Dst.IsValid() || !m_Upload.IsValid() || !m_Readback.IsValid())
        {
            why = "resource creation failed";
            return false;
        }

        SamplerDesc sd{};
        m_Sampler = m_Device->CreateSampler(sd);
        if (!m_Sampler.IsValid())
        {
            why = "CreateSampler failed";
            return false;
        }

        SamplerDesc lsd{};
        lsd.minFilter = lsd.magFilter = 1; // linear: the LUT is interpolated between its knots
        lsd.addressModeU = lsd.addressModeV = lsd.addressModeW = 2; // clamp
        m_LutSampler = m_Device->CreateSampler(lsd);
        m_IdentityLut = CreateIdentityCubeLutTextures(m_Device.get());
        CubeLutParseResult half{};
        if (!ParseCubeLutFromText("LUT_3D_SIZE 2\n"
                                  "0 0 0\n0.5 0 0\n0 0.5 0\n0.5 0.5 0\n"
                                  "0 0 0.5\n0.5 0 0.5\n0 0.5 0.5\n0.5 0.5 0.5\n",
                                  half) ||
            !half.Ok ||
            !CreateCubeLutGpuTextures(m_Device.get(), half, TextureFormat::R16G16B16A16_FLOAT, m_HalfLut, &why) ||
            !m_IdentityLut.Lut3D.IsValid() || !m_IdentityLut.Lut1DStrip.IsValid())
        {
            why = "LUT tables failed: " + why;
            return false;
        }
        CubeLutParseResult square{};
        if (!ParseCubeLutFromText(SquareCubeLutText(), square) || !square.Ok ||
            !CreateCubeLutGpuTextures(m_Device.get(), square, TextureFormat::R32G32B32A32_FLOAT, m_SquareLut, &why))
        {
            why = "square LUT failed: " + why + " " + square.Error;
            return false;
        }

        if (!BindDescriptors(pd, why))
            return false;

        return true;
    }

    // Runs tonemap.frag once on a 1x1 scene-linear colour and returns its RGB output.
    Rgb Run(const Rgb& scene, const TonemapParams& params)
    {
        const float texel[4] = {scene.R, scene.G, scene.B, 1.0f};
        m_Device->UpdateBuffer(m_Upload, 0, sizeof(texel), texel);

        float lutIntensity = 0.0f;
        const CubeLutGpuBindingState lut = LutBinding(params.Lut, lutIntensity);
        std::vector<uint8_t> pc;
        std::string missing;
        if (!BuildPushConstants(m_Pcr, params, lut, lutIntensity, pc, missing))
        {
            ADD_FAILURE() << "push-constant block missing reflected fields: " << missing;
            return {};
        }

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_Src, m_SrcState, ResourceState::CopyDest));
        cl->CopyBufferToTextureSubresource(m_Upload, m_Src, 0, 0, 1, 1, 0, sizeof(texel));
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_Src, ResourceState::CopyDest,
                                                          ResourceState::ShaderResource));
        m_SrcState = ResourceState::ShaderResource;

        cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_Dst, m_DstState, ResourceState::RenderTarget));
        RenderPassDesc rp{};
        rp.colorTargets[0] = m_Dst;
        rp.colorTargetCount = 1;
        rp.clearColor[0] = true;
        rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        cl->BeginRenderPass(rp);
        cl->SetViewport(0, 0, 1, 1);
        cl->SetScissor(0, 0, 1, 1);
        m_Device->UpdateCombinedImageSamplerBinding(m_DsTex, m_Lut3dBinding, lut.Lut3D, m_LutSampler);
        m_Device->UpdateCombinedImageSamplerBinding(m_DsTex, m_Lut1dBinding, lut.Lut1DStrip, m_LutSampler);
        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(m_TexSet, m_DsTex, m_Pipeline);
        if (m_SamplerSet != m_TexSet && m_DsSampler.IsValid())
            cl->BindDescriptorSet(m_SamplerSet, m_DsSampler, m_Pipeline);
        cl->SetPushConstantsById(m_PcRangeId, pc.data(), pc.size(), 0);
        cl->Draw(3, 1);
        cl->EndRenderPass();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_Dst, ResourceState::RenderTarget,
                                                          ResourceState::CopySource));
        m_DstState = ResourceState::CopySource;
        cl->CopyTextureToBuffer(m_Dst, m_Readback, 1, 1, 0, 0, 0, 0);
        cl->End();

        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();

        Rgb out{};
        if (const void* p = m_Device->MapBuffer(m_Readback))
        {
            float v[4] = {0, 0, 0, 0};
            std::memcpy(v, p, sizeof(v));
            out = Rgb{v[0], v[1], v[2]};
            m_Device->UnmapBuffer(m_Readback);
        }
        else
        {
            ADD_FAILURE() << "readback map failed";
        }
        return out;
    }

    IDevice* Device() const { return m_Device.get(); }

private:
    bool BindDescriptors(const PipelineDesc& pd, std::string& why)
    {
        bool hasCombined = false, hasImage = false, hasSampler = false;
        uint32_t combinedSet = 0, combinedBinding = 0, imageSet = 0, imageBinding = 0;
        uint32_t samplerSet = 0, samplerBinding = 0, ssboSet = 0, ssboBinding = 0;
        bool hasSsbo = false;
        bool hasTables = false;
        uint32_t tablesSet = 0, tablesBinding = 0;
        bool hasLut3d = false, hasLut1d = false;
        uint32_t lutSet = 0;
        for (const auto& s : m_Meta.Sets)
            for (const auto& b : s.Bindings)
            {
                if (b.Type == ShaderMetaBindingType::kCombinedImageSampler && b.Name == "uLut3d")
                { hasLut3d = true; lutSet = s.Set; m_Lut3dBinding = b.Binding; }
                else if (b.Type == ShaderMetaBindingType::kCombinedImageSampler && b.Name == "uLut1d")
                { hasLut1d = true; m_Lut1dBinding = b.Binding; }
                else if (!hasCombined && b.Type == ShaderMetaBindingType::kCombinedImageSampler)
                { hasCombined = true; combinedSet = s.Set; combinedBinding = b.Binding; }
                if (!hasImage && b.Type == ShaderMetaBindingType::kSampledImage)
                { hasImage = true; imageSet = s.Set; imageBinding = b.Binding; }
                if (!hasSampler && b.Type == ShaderMetaBindingType::kSampler)
                { hasSampler = true; samplerSet = s.Set; samplerBinding = b.Binding; }
                if (b.Type == ShaderMetaBindingType::kStorageBuffer && b.Name == "Aces2Tables")
                { hasTables = true; tablesSet = s.Set; tablesBinding = b.Binding; }
                else if (!hasSsbo && b.Type == ShaderMetaBindingType::kStorageBuffer)
                { hasSsbo = true; ssboSet = s.Set; ssboBinding = b.Binding; }
            }

        auto makeSet = [&](uint32_t set, const char* name) -> DescriptorSetHandle
        {
            const int ord = OrdinalOf(m_Meta, set);
            if (ord < 0 || static_cast<size_t>(ord) >= pd.descriptorSetLayouts.size())
                return {};
            DescriptorSetDesc d{};
            d.layout = pd.descriptorSetLayouts[static_cast<size_t>(ord)];
            d.transient = false;
            d.debugName = name;
            return m_Device->CreateDescriptorSet(d);
        };

        if (hasCombined)
        {
            m_TexSet = combinedSet;
            m_SamplerSet = combinedSet;
            m_DsTex = makeSet(combinedSet, "TonemapHdrHeadroom.Combined");
            if (!m_DsTex.IsValid()) { why = "combined descriptor set failed"; return false; }
            m_Device->UpdateCombinedImageSamplerBinding(m_DsTex, combinedBinding, m_Src, m_Sampler);
        }
        else if (hasImage && hasSampler)
        {
            m_TexSet = imageSet;
            m_SamplerSet = samplerSet;
            m_DsTex = makeSet(imageSet, "TonemapHdrHeadroom.Image");
            if (!m_DsTex.IsValid()) { why = "image descriptor set failed"; return false; }
            m_Device->UpdateImageBinding(m_DsTex, imageBinding, m_Src);
            if (samplerSet == imageSet)
                m_DsSampler = m_DsTex;
            else
            {
                m_DsSampler = makeSet(samplerSet, "TonemapHdrHeadroom.Sampler");
                if (!m_DsSampler.IsValid()) { why = "sampler descriptor set failed"; return false; }
            }
            m_Device->UpdateSamplerBinding(m_DsSampler, samplerBinding, m_Sampler);
        }
        else
        {
            why = "tonemap.frag exposes neither a combined nor a separate image+sampler binding";
            return false;
        }

        // The Cube LUT tables: Run binds the identity or the half LUT per draw.
        if (!hasLut3d || !hasLut1d || lutSet != m_TexSet)
        {
            why = "tonemap.frag's uLut3d / uLut1d are missing or outside the scene color's set";
            return false;
        }

        // tonemap.frag statically references the auto-exposure SSBO, so it must be bound
        // even with useAutoExposure == 0. exposureScale is left at 1.0 and unused.
        if (hasSsbo)
        {
            DescriptorSetHandle target =
                (ssboSet == m_TexSet) ? m_DsTex : ((ssboSet == m_SamplerSet) ? m_DsSampler : DescriptorSetHandle{});
            if (!target.IsValid())
            {
                why = "uExposure lives in a set the rig does not bind";
                return false;
            }
            BufferDesc sbd{};
            sbd.size = 16;
            sbd.usage = static_cast<uint32_t>(BufferUsage::Storage);
            sbd.memoryUsage = BufferMemoryUsage::Upload;
            sbd.flags = BufferCreateFlags::PersistentlyMapped;
            m_Exposure = m_Device->CreateBuffer(sbd);
            if (!m_Exposure.IsValid()) { why = "exposure SSBO failed"; return false; }
            if (void* p = m_Device->MapBuffer(m_Exposure))
            {
                const float scale = 1.0f;
                const uint32_t valid = 1u;
                std::memcpy(p, &scale, 4);
                std::memcpy(static_cast<uint8_t*>(p) + 4, &valid, 4);
                m_Device->UnmapBuffer(m_Exposure);
            }
            m_Device->UpdateStorageBufferBinding(target, ssboBinding, m_Exposure, 0, 16);
        }

        // tonemap.frag's ACES 2 tier tables (Aces2Tables SSBO): upload the
        // generated payload the engine's TonemapPass uploads in production —
        // an unbound/zeroed block makes ACES2 render black in the fixture.
        if (hasTables)
        {
            DescriptorSetHandle target =
                (tablesSet == m_TexSet) ? m_DsTex : ((tablesSet == m_SamplerSet) ? m_DsSampler : DescriptorSetHandle{});
            if (!target.IsValid())
            {
                why = "Aces2Tables lives in a set the rig does not bind";
                return false;
            }
            BufferDesc tbd{};
            tbd.size = Passes::kAces2TablesBytes;
            tbd.usage = static_cast<uint32_t>(BufferUsage::Storage);
            tbd.memoryUsage = BufferMemoryUsage::Upload;
            tbd.flags = BufferCreateFlags::PersistentlyMapped;
            m_Aces2Tables = m_Device->CreateBuffer(tbd);
            if (!m_Aces2Tables.IsValid()) { why = "Aces2Tables SSBO failed"; return false; }
            if (void* p = m_Device->MapBuffer(m_Aces2Tables))
            {
                std::memcpy(p, Passes::kAces2Tables, Passes::kAces2TablesBytes);
                m_Device->UnmapBuffer(m_Aces2Tables);
            }
            m_Device->UpdateStorageBufferBinding(target, tablesBinding, m_Aces2Tables, 0,
                                                 Passes::kAces2TablesBytes);
        }

        m_PcRangeId = 0;
        const uint32_t rangeCount = m_Device->GetPipelinePushConstantRangeCount(m_Pipeline);
        for (uint32_t rid = 0; rid < rangeCount; ++rid)
        {
            PushConstantRangeInfo info{};
            if (m_Device->GetPipelinePushConstantRangeInfo(m_Pipeline, rid, info) && info.size >= m_Pcr.Size)
            {
                m_PcRangeId = rid;
                break;
            }
        }
        return true;
    }

    std::unique_ptr<IDevice> m_Device;
    ShaderMeta m_Meta{};
    PushConstantRangeMeta m_Pcr{};
    PipelineHandle m_Pipeline = INVALID_HANDLE;
    TextureHandle m_Src{}, m_Dst{};
    BufferHandle m_Upload{}, m_Readback{}, m_Exposure{}, m_Aces2Tables{};
    SamplerHandle m_Sampler{}, m_LutSampler{};
    DescriptorSetHandle m_DsTex{}, m_DsSampler{};
    // The binding and intensity the pass is given for each rig LUT. Tables a LUT does not use are
    // the identity tables, as in production.
    CubeLutGpuBindingState LutBinding(RigLut which, float& intensity) const
    {
        intensity = which == RigLut::None ? 0.0f : 1.0f;
        if (which == RigLut::None || which == RigLut::IdentityFallback)
            return IdentityCubeLutBinding(m_IdentityLut);
        CubeLutGpuBindingState lut{};
        lut.Lut3D = which == RigLut::Half ? m_HalfLut.Lut3D : m_SquareLut.Lut3D;
        lut.Lut1DStrip = m_IdentityLut.Lut1DStrip;
        lut.Size3D = which == RigLut::Half ? 2 : static_cast<int32_t>(kSquareLutSize);
        lut.Flags = 1;
        return lut;
    }

    CubeLutGpuTextures m_IdentityLut{}, m_HalfLut{}, m_SquareLut{};
    uint32_t m_Lut3dBinding = 0, m_Lut1dBinding = 0;
    uint32_t m_TexSet = 0, m_SamplerSet = 0, m_PcRangeId = 0;
    ResourceState m_SrcState = ResourceState::Undefined;
    ResourceState m_DstState = ResourceState::Undefined;
};

TonemapRig& Rig()
{
    static TonemapRig rig;
    return rig;
}

bool& RigReady()
{
    static bool ready = false;
    return ready;
}

std::string& RigWhy()
{
    static std::string why;
    return why;
}

bool& RigFatal()
{
    static bool fatal = true;
    return fatal;
}

class TonemapHdrHeadroom : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        static bool attempted = false;
        if (!attempted)
        {
            attempted = true;
            RigReady() = Rig().Init(RigWhy(), RigFatal());
        }
    }

    void SetUp() override
    {
        if (RigReady())
            return;
        // Only "no GPU here" skips. Anything else — missing SPIR-V, reflection
        // failure, unsupported target format — FAILS, because a suite that
        // reports PASSED without running is the failure mode this file is
        // written against.
        if (RigFatal())
            FAIL() << "tonemap rig could not be built: " << RigWhy();
        GTEST_SKIP() << "no usable Vulkan device on this machine: " << RigWhy();
    }

    static Rgb Hdr(const Rgb& scene, Mode mode)
    {
        TonemapParams p{};
        p.Look = mode;
        p.OutEncoding = kEncodingHdrLinear;
        return Rig().Run(scene, p);
    }

    static Rgb Sdr(const Rgb& scene, Mode mode)
    {
        TonemapParams p{};
        p.Look = mode;
        p.OutEncoding = kEncodingSdrLinear;
        return Rig().Run(scene, p);
    }
};

// The looks: every arm that renders colour. The no-look arm is excluded by
// design: it performs no colour rendering, so it keeps the scene-derived
// shoulder. Neutral was the opposite failure of the removed crossfade (hard
// clamp at paper white) and now extends like Photographic.
constexpr Mode kColourRenderingModes[] = {Mode::Neutral, Mode::Filmic, Mode::Photographic};

// --- 0. Instrument premise -------------------------------------------------
//
// Everything below is a claim about behaviour ABOVE paper white. If the fixture
// has no headroom, every one of those claims is vacuous. Assert the premise
// before trusting any reading taken with it.
TEST_F(TonemapHdrHeadroom, HdrHeadroomIsActuallyAvailable)
{
    ASSERT_GT(kOutputMax, 2.0f) << "fixture headroom too small to distinguish the shaders";

    // No look under HDR is the pass's own report of the headroom: it rolls the raw
    // scene value to at most outputMax. Its shoulder only APPROACHES outputMax, so
    // the probe has to be far above it.
    const Rgb veryBright = Hdr({4096.0f, 4096.0f, 4096.0f}, Mode::NoLook);
    EXPECT_NEAR(veryBright.Peak(), kOutputMax, 0.05f)
        << "NoLook/HDR should saturate at outputMax " << kOutputMax << ", got " << Fmt(veryBright)
        << " — the push constants are not reaching the shader, or paper-white plumbing changed";

    // And the SDR path must NOT have headroom, or the HDR assertions prove nothing.
    // Photographic, not Filmic: Filmic bypasses the highlight extension, so only an
    // extended look can show the SDR arm gaining headroom (mutant M7).
    const Rgb sdr = Sdr({64.0f, 64.0f, 64.0f}, Mode::Photographic);
    EXPECT_LE(sdr.Peak(), 1.0001f) << "SDR Photographic exceeded paper white: " << Fmt(sdr);
}

// --- 1. THE REGRESSION GATE ------------------------------------------------
//
// Above the old crossfade's upper edge (peak >= min(outputMax, 4)) the result was
// 100% a scene-derived shoulder and 0% the selected operator, so every operator
// returned the same pixel (3.515329 on every channel at scene 8.0, measured on
// the eight-operator shader the looks replaced).
TEST_F(TonemapHdrHeadroom, OperatorsRemainDistinctAboveHeadroomThreshold)
{
    const Rgb scene{8.0f, 8.0f, 8.0f};

    std::vector<std::pair<Mode, Rgb>> results;
    for (Mode m : kColourRenderingModes)
        results.emplace_back(m, Hdr(scene, m));

    for (const auto& [m, c] : results)
        EXPECT_GT(c.Peak(), 1.0f) << ModeName(m) << " produced no HDR highlight at scene 8.0: " << Fmt(c);

    // Pairwise distinctness. The collapse this gates makes the separation exactly
    // zero, so the margin only has to clear readback noise — the target is
    // R32G32B32A32_FLOAT, so that is ~1e-7. 0.002 (0.4 nits) is three orders above
    // it and far below the smallest true separation between the looks at this probe.
    constexpr float kMinSeparation = 0.002f;
    int collapsed = 0;
    for (size_t i = 0; i < results.size(); ++i)
        for (size_t j = i + 1; j < results.size(); ++j)
        {
            const float d = std::fabs(results[i].second.Peak() - results[j].second.Peak());
            if (d < kMinSeparation)
            {
                ++collapsed;
                ADD_FAILURE() << "operators collapsed onto one curve above the headroom threshold: "
                              << ModeName(results[i].first) << " " << Fmt(results[i].second) << " vs "
                              << ModeName(results[j].first) << " " << Fmt(results[j].second)
                              << " (separation " << d << " < " << kMinSeparation << ")";
            }
        }
    EXPECT_EQ(collapsed, 0) << "the HDR path is discarding the selected operator again";

    // Spread, stated separately from the pairwise check: a partial regression that
    // collapsed only some modes would still leave a spread, but a full one leaves none.
    float lo = results.front().second.Peak(), hi = lo;
    for (const auto& [m, c] : results)
    {
        lo = std::min(lo, c.Peak());
        hi = std::max(hi, c.Peak());
    }
    EXPECT_GT(hi - lo, 1.0f) << "the operators span only " << (hi - lo)
                             << " at scene 8.0 — they are rendering one curve, not three";
}

// --- 2. The colour rendering survives --------------------------------------
//
// The extension is a positive scalar on the operator's OWN output, so the HDR
// pixel's chromaticity must equal the operator's SDR chromaticity — the hue and
// highlight desaturation the operator computed. The old blend replaced it with
// the raw scene chromaticity instead. Probe: a saturated blue highlight, where
// the two differ most.
TEST_F(TonemapHdrHeadroom, SaturatedHighlightKeepsOperatorColourRendering)
{
    const Rgb scene{0.8f, 1.6f, 8.0f}; // saturated blue, bright enough to clear the old threshold

    auto normalize = [](const Rgb& c) -> Rgb
    {
        const float s = c.R + c.G + c.B;
        return (s > 1e-6f) ? Rgb{c.R / s, c.G / s, c.B / s} : Rgb{};
    };
    const Rgb sceneChroma = normalize(scene);

    // Filmic (ACES 2) is deliberately absent — RECORDED DECISION (2026-08-04): it
    // is peak-parameterised and bypasses the extension. Its HDR arm runs the
    // Academy output transform matched to the panel's headroom, which performs
    // its own colour rendering at that peak; the SDR arm's chromaticity is a
    // different peak's answer, not this contract. Its HDR behaviour is pinned
    // in Aces2PeakParameterisedContract instead.
    for (Mode m : {Mode::Photographic, Mode::Neutral})
    {
        const Rgb sdr = Sdr(scene, m);
        const Rgb hdr = Hdr(scene, m);
        const Rgb sdrChroma = normalize(sdr);
        const Rgb hdrChroma = normalize(hdr);

        const float dOperator = std::fabs(hdrChroma.R - sdrChroma.R) +
                                std::fabs(hdrChroma.G - sdrChroma.G) +
                                std::fabs(hdrChroma.B - sdrChroma.B);
        const float dScene = std::fabs(hdrChroma.R - sceneChroma.R) +
                             std::fabs(hdrChroma.G - sceneChroma.G) +
                             std::fabs(hdrChroma.B - sceneChroma.B);

        EXPECT_LT(dOperator, 0.02f)
            << ModeName(m) << ": HDR chromaticity " << Fmt(hdrChroma) << " drifted from the operator's own "
            << Fmt(sdrChroma) << " — the colour rendering was replaced, not extended";
        EXPECT_GT(dScene, dOperator)
            << ModeName(m) << ": HDR chromaticity " << Fmt(hdrChroma) << " is closer to the RAW SCENE "
            << Fmt(sceneChroma) << " than to the operator's " << Fmt(sdrChroma)
            << " — the operator's gamut/desaturation work was discarded";
    }
}

// --- 3. Neutral is no longer the opposite failure --------------------------
//
// Neutral took `default: return sdrTonemapped` and hard-clamped at paper white
// (~203 nits) on a 1000-nit panel. Pre-registered failure against the unmodified
// shader: peak == 0.869 at scene 8.0, i.e. below 1.0.
TEST_F(TonemapHdrHeadroom, NeutralGainsHdrHeadroom)
{
    const Rgb hdr = Hdr({8.0f, 8.0f, 8.0f}, Mode::Neutral);
    EXPECT_GT(hdr.Peak(), 1.25f)
        << "Neutral under HDR is still clamped at paper white: " << Fmt(hdr)
        << " (" << hdr.Peak() * kPaperWhiteNits << " nits on a " << kPanelPeakNits << "-nit panel)";

    const Rgb sdr = Sdr({8.0f, 8.0f, 8.0f}, Mode::Neutral);
    EXPECT_LE(sdr.Peak(), 1.0001f) << "Neutral must still clamp on SDR output: " << Fmt(sdr);
}

// --- 4. Each operator's own ceiling maps onto the display -------------------
//
// The extension maps operator output 1.0 exactly onto outputMax, so an operator
// that CLAMPS at 1.0 reaches the panel's peak on a saturating source — the "real
// HDR highlights" the removed blend existed to produce. An operator that only
// approaches 1.0 lands short, which is its own curve's answer and the correct one.
//
// Which looks are in which group is MEASURED here, not hardcoded: the SDR arm
// at the same scene value IS the look's ceiling, and the HDR arm must be that
// ceiling put through the extension (Filmic's tier ladder lands at the same
// place for a ceiling of 1.0). Measured ceilings at scene 4096:
//
//   Filmic (ACES 2)     1.000000 -> 4.9261 (1000 nits) — clamps (min(vec3(1.)))
//   Photographic (AgX)  0.997119 -> 4.2443 ( 862 nits) — does NOT reach 1.0; its
//            6th-order polynomial sigmoid tops out ~0.14% short, and near the top
//            the extension's slope is ((outputMax-knee)/(1-knee))^2 ~ 279, so that
//            0.3% shortfall costs 14% of peak luminance. Inherent to the
//            published fit, not an engine defect — but it is why Photographic's
//            brightest possible pixel is dimmer than Filmic's.
//   Neutral — asymptotic by construction, lands short by design.
TEST_F(TonemapHdrHeadroom, OperatorCeilingsMapOntoTheDisplay)
{
    // The contract the shader implements, stated independently here. This is not
    // a reimplementation of the operator — the operator's output is measured from
    // the shader — only of the scalar the extension is defined to apply to it.
    auto extensionRef = [](float ceiling, float outputMax)
    {
        constexpr float kKnee = 0.75f;
        constexpr float kOperatorHeadroom = 1.0f - kKnee;
        if (ceiling <= kKnee)
            return ceiling;
        const float excess = std::min(ceiling - kKnee, kOperatorHeadroom);
        const float c = 1.0f / kOperatorHeadroom - 1.0f / (outputMax - kKnee);
        return kKnee + excess / (1.0f - c * excess);
    };

    const Rgb scene{4096.0f, 4096.0f, 4096.0f};
    int reachedPanelPeak = 0;
    for (Mode m : kColourRenderingModes)
    {
        const float ceiling = Sdr(scene, m).Peak();
        const Rgb hdr = Hdr(scene, m);

        ASSERT_GT(ceiling, 0.75f) << ModeName(m) << ": saturating source did not clear the knee ("
                                  << ceiling << ") — this probe proves nothing";
        EXPECT_LE(hdr.Peak(), kOutputMax + 1e-3f)
            << ModeName(m) << " exceeded the display's max linear value: " << Fmt(hdr);
        EXPECT_NEAR(hdr.Peak(), extensionRef(ceiling, kOutputMax), 3e-3f)
            << ModeName(m) << ": ceiling " << ceiling << " landed at " << hdr.Peak() << " ("
            << hdr.Peak() * kPaperWhiteNits << " nits), not where the extension puts it";

        if (ceiling >= 0.9999f)
        {
            ++reachedPanelPeak;
            EXPECT_NEAR(hdr.Peak(), kOutputMax, 5e-3f)
                << ModeName(m) << " clamps at 1.0 but did not land on panel peak: " << Fmt(hdr)
                << " (" << hdr.Peak() * kPaperWhiteNits << " nits, expected " << kPanelPeakNits << ")";
        }
    }
    // Without this the clause above is vacuous if every look stopped clamping.
    EXPECT_GE(reachedPanelPeak, 1) << "no look reached the panel's peak luminance — Filmic "
                                   << "clamps at 1.0 and must land on it";
}

// --- 5. Knee pinned from below ---------------------------------------------
//
// The knee is the operator-output level where the extension starts. Set it too
// low and diffuse white — the operator's own rendering of scene 1.0 — is pushed
// above BT.2408 reference white, so the whole image glares. Neutral is the binding
// look: it renders scene 1.0 at 0.869, the highest of the looks (no look maps 1.0
// to 1.0). At knee 0.75 that lands at 196 nits; at knee 0.70 it lands at 214 nits
// and this test goes RED.
TEST_F(TonemapHdrHeadroom, DiffuseWhiteStaysAtOrBelowPaperWhite)
{
    for (Mode m : kColourRenderingModes)
    {
        const Rgb hdr = Hdr({1.0f, 1.0f, 1.0f}, m);
        EXPECT_LE(hdr.Peak(), 1.0f + 1e-3f)
            << ModeName(m) << " pushed diffuse white above paper white: " << Fmt(hdr) << " ("
            << hdr.Peak() * kPaperWhiteNits << " nits) — the extension knee is too low";
    }
}

// --- 6. Knee pinned from above ---------------------------------------------
//
// Set the knee too high and the extension only fires on pixels that already
// saturate the operator, so ordinary specular highlights get no headroom at all
// and the HDR display shows an SDR image. Photographic renders scene 4.0 above a
// 0.75 knee and below a 0.95 one (asserted below); Neutral renders it above 0.95,
// and Filmic bypasses the extension.
TEST_F(TonemapHdrHeadroom, HighlightsAboveKneeAreExtended)
{
    for (Mode m : {Mode::Photographic})
    {
        const Rgb sdr = Sdr({4.0f, 4.0f, 4.0f}, m);
        const Rgb hdr = Hdr({4.0f, 4.0f, 4.0f}, m);
        // Probe validity: this scene must land the operator strictly BETWEEN the
        // knee under test (0.75) and the mutation's knee (0.95), or the test cannot
        // tell the two apart and its green result would mean nothing.
        ASSERT_GT(sdr.Peak(), 0.75f) << ModeName(m) << ": probe is below the knee, "
                                     << Fmt(sdr) << " — it cannot discriminate";
        ASSERT_LT(sdr.Peak(), 0.95f) << ModeName(m) << ": probe is above a 0.95 knee, "
                                     << Fmt(sdr) << " — it cannot discriminate";
        EXPECT_GT(hdr.Peak(), sdr.Peak() * 1.05f)
            << ModeName(m) << ": a scene-4.0 highlight got no headroom — SDR " << Fmt(sdr) << " vs HDR "
            << Fmt(hdr) << " (" << hdr.Peak() * kPaperWhiteNits << " nits); the extension knee is too high";
        EXPECT_LE(hdr.Peak(), kOutputMax + 1e-3f)
            << ModeName(m) << " exceeded the display's max linear value: " << Fmt(hdr);
    }
}

// --- 7. Paths that must not move -------------------------------------------
//
// SDR output is byte-identical to the operator (no extension, no clamp change),
// no look keeps its scene-derived shoulder, and an HDR surface with no headroom
// (max nits == paper white) collapses to the SDR result.
TEST_F(TonemapHdrHeadroom, UnchangedPathsStayUnchanged)
{
    // (a) SDR: below the operator ceiling nothing may move, and nothing may exceed
    //     paper white regardless of how bright the source is.
    for (Mode m : kColourRenderingModes)
    {
        const Rgb dim = Sdr({0.18f, 0.18f, 0.18f}, m);
        EXPECT_GT(dim.Peak(), 0.0f) << ModeName(m) << ": SDR mid-grey was black";
        EXPECT_LT(dim.Peak(), 0.75f)
            << ModeName(m) << ": SDR mid-grey " << Fmt(dim) << " is at or above the extension knee — "
            << "this probe can no longer tell the SDR path apart from an extended one";
        const Rgb bright = Sdr({64.0f, 64.0f, 64.0f}, m);
        EXPECT_LE(bright.Peak(), 1.0001f)
            << ModeName(m) << ": the SDR path gained headroom: " << Fmt(bright);
    }

    // (b) Midtones below the knee are identical on both paths for the EXTENDED
    //     looks — the extension is strictly a highlight operation. Filmic (ACES 2)
    //     is deliberately absent — RECORDED DECISION (2026-08-04): peak-matched
    //     ACES 2 output transforms lift mid-grey by Academy design, and the
    //     paper-white-relative ladder
    //     gives this fixture's 203-nit-paper-white/1000-nit panel the ~493-nit
    //     curve, so its midtones legitimately move under HDR. The lift is
    //     bounded by Aces2PeakParameterisedContract.
    for (Mode m : kColourRenderingModes)
    {
        if (m == Mode::Filmic)
            continue;
        const Rgb sdr = Sdr({0.18f, 0.18f, 0.18f}, m);
        const Rgb hdr = Hdr({0.18f, 0.18f, 0.18f}, m);
        EXPECT_NEAR(hdr.R, sdr.R, 1e-5f) << ModeName(m) << ": midtone moved under HDR, " << Fmt(sdr)
                                         << " -> " << Fmt(hdr);
        EXPECT_NEAR(hdr.G, sdr.G, 1e-5f) << ModeName(m);
        EXPECT_NEAR(hdr.B, sdr.B, 1e-5f) << ModeName(m);
    }

    // (c) No look keeps the scene-derived shoulder: unlike the looks it never
    //     compressed anything, so there is nothing to preserve. Its scene-1.0
    //     output stays at 1.0 (the shoulder's start), which no extended look does.
    const Rgb noLook1 = Hdr({1.0f, 1.0f, 1.0f}, Mode::NoLook);
    EXPECT_NEAR(noLook1.Peak(), 1.0f, 1e-3f) << "NoLook/HDR at scene 1.0: " << Fmt(noLook1);
    const Rgb noLook4 = Hdr({4.0f, 4.0f, 4.0f}, Mode::NoLook);
    EXPECT_GT(noLook4.Peak(), 2.0f) << "NoLook/HDR lost its shoulder: " << Fmt(noLook4);

    // (d) An HDR surface with no headroom must equal the SDR result. Filmic is
    //     included: outputMax == 1.0 selects tier 0 at t = 0 exactly.
    for (Mode m : {Mode::Neutral, Mode::Photographic, Mode::Filmic})
    {
        TonemapParams p{};
        p.Look = m;
        p.OutEncoding = kEncodingHdrLinear;
        p.MaxOutputNits = kPaperWhiteNits; // outputMax == 1.0
        const Rgb flat = Rig().Run({8.0f, 8.0f, 8.0f}, p);
        EXPECT_LE(flat.Peak(), 1.0001f)
            << ModeName(m) << ": zero-headroom HDR surface produced " << Fmt(flat);
    }
}

// --- 8. ACES 2's peak-parameterised contract --------------------------------
//
// Filmic (ACES 2) bypasses the highlight extension: its tier ladder produces
// display-referred output for the panel's headroom directly, and the routing
// only clamps it. RECORDED DECISION (2026-08-04): mid-grey moves with peak by
// Academy design — the paper-white-relative ladder gives this fixture the
// ~493-nit curve — so
// the HDR arm is pinned against the OCIO-derived expectation at that effective
// peak, never against the SDR arm.
TEST_F(TonemapHdrHeadroom, Aces2PeakParameterisedContract)
{
    // SDR: tier 0 exactly — clamped at paper white, deterministic to the byte.
    const Rgb sdrMidA = Sdr({0.18f, 0.18f, 0.18f}, Mode::Filmic);
    const Rgb sdrMidB = Sdr({0.18f, 0.18f, 0.18f}, Mode::Filmic);
    EXPECT_EQ(sdrMidA.R, sdrMidB.R) << "SDR ACES2 is not deterministic";
    EXPECT_EQ(sdrMidA.G, sdrMidB.G) << "SDR ACES2 is not deterministic";
    EXPECT_EQ(sdrMidA.B, sdrMidB.B) << "SDR ACES2 is not deterministic";
    // Tier-0 mid-grey: the 100-nit transform maps scene 0.18 to 10 nits
    // (0.100 paper-white-relative) — the Academy's stated anchor.
    EXPECT_NEAR(sdrMidA.Peak(), 0.1000f, 2e-3f)
        << "SDR ACES2 mid-grey left the 100-nit tier: " << Fmt(sdrMidA);
    const Rgb sdrBright = Sdr({4096.0f, 4096.0f, 4096.0f}, Mode::Filmic);
    EXPECT_LE(sdrBright.Peak(), 1.0001f) << "SDR ACES2 gained headroom: " << Fmt(sdrBright);

    // HDR: output never exceeds the display's max linear value.
    const Rgb hdrBright = Hdr({4096.0f, 4096.0f, 4096.0f}, Mode::Filmic);
    EXPECT_LE(hdrBright.Peak(), kOutputMax + 1e-3f)
        << "HDR ACES2 exceeded outputMax: " << Fmt(hdrBright);

    // Mid-grey lift is BOUNDED and matches the peak-matched transform, not SDR.
    // Expectation derived from the OCIO reference via the tier validator
    // (tier_at(log2(4.9261)) at scene 0.18 -> 0.131735, +31.7% over tier 0;
    // Tools/ShaderGen/validate_aces2_tiers.py). GPU-vs-validator agreement
    // measured at <= 5e-5; the 2e-3 tolerance covers driver transcendental
    // variance without admitting the SDR value (0.100) or a runaway tier.
    constexpr float kMidGreyAtFixturePeak = 0.131735f;
    const Rgb hdrMid = Hdr({0.18f, 0.18f, 0.18f}, Mode::Filmic);
    EXPECT_NEAR(hdrMid.Peak(), kMidGreyAtFixturePeak, 2e-3f)
        << "ACES2 mid-grey is not the peak-matched transform's value: " << Fmt(hdrMid)
        << " (tier 0 would be 0.100; the " << kPanelPeakNits << "-nit fixture expects "
        << kMidGreyAtFixturePeak << ")";
    EXPECT_LT(hdrMid.Peak(), 0.75f)
        << "ACES2 mid-grey reached the extension knee — the tier selection is runaway: " << Fmt(hdrMid);

    // Zero headroom == the SDR result to the byte (tier 0, t = 0, same clamp).
    TonemapParams p{};
    p.Look = Mode::Filmic;
    p.OutEncoding = kEncodingHdrLinear;
    p.MaxOutputNits = kPaperWhiteNits;
    const Rgb flat = Rig().Run({0.18f, 0.18f, 0.18f}, p);
    EXPECT_EQ(flat.R, sdrMidA.R) << "zero-headroom HDR ACES2 != SDR: " << Fmt(flat);
    EXPECT_EQ(flat.G, sdrMidA.G);
    EXPECT_EQ(flat.B, sdrMidA.B);
}

// --- 9. The Cube LUT grades the look's output, before the HDR extension -----
//
// A LUT is authored for the display range [0, 1]. The pass samples it on the
// look's output and extends the result into the panel's headroom afterwards, so
// the LUT sees the range it was authored for and the headroom survives. The
// rig's half LUT maps every colour to half of itself: under HDR a scene-8.0
// highlight that Neutral renders at 0.97 becomes 0.48, below the knee, so the
// result equals the SDR answer; applying the LUT after the extension would give
// half of the extended 3.47 instead. Filmic's grey highlight above 1.0 is looked
// up divided by its own largest channel and scaled back: half of its HDR output.
TEST_F(TonemapHdrHeadroom, CubeLutGradesTheLooksOutputBeforeTheHdrExtension)
{
    const Rgb scene{8.0f, 8.0f, 8.0f};
    for (Mode m : {Mode::Neutral, Mode::Photographic})
    {
        TonemapParams sdr{}; sdr.Look = m; sdr.OutEncoding = kEncodingSdrLinear;
        TonemapParams sdrLut = sdr; sdrLut.Lut = RigLut::Half;
        TonemapParams hdrLut{}; hdrLut.Look = m; hdrLut.OutEncoding = kEncodingHdrLinear; hdrLut.Lut = RigLut::Half;
        const Rgb plain = Rig().Run(scene, sdr);
        const Rgb gradedSdr = Rig().Run(scene, sdrLut);
        const Rgb gradedHdr = Rig().Run(scene, hdrLut);
        EXPECT_NEAR(gradedSdr.Peak(), 0.5f * plain.Peak(), 2e-3f)
            << ModeName(m) << ": SDR " << Fmt(gradedSdr) << " is not half of " << Fmt(plain);
        EXPECT_NEAR(gradedHdr.Peak(), gradedSdr.Peak(), 2e-3f)
            << ModeName(m) << ": the LUT ran after the HDR extension: " << Fmt(gradedHdr) << " against SDR "
            << Fmt(gradedSdr);
    }

    TonemapParams filmic{}; filmic.Look = Mode::Filmic; filmic.OutEncoding = kEncodingHdrLinear;
    TonemapParams filmicLut = filmic; filmicLut.Lut = RigLut::Half;
    const Rgb hdr = Rig().Run(scene, filmic);
    const Rgb gradedFilmic = Rig().Run(scene, filmicLut);
    ASSERT_GT(hdr.Peak(), 1.5f) << "Filmic did not use the headroom: " << Fmt(hdr);
    EXPECT_NEAR(gradedFilmic.Peak(), 0.5f * hdr.Peak(), 5e-3f)
        << "Filmic's headroom did not survive the LUT: " << Fmt(gradedFilmic) << " against " << Fmt(hdr);

    // The no-look view shows the image without any grade.
    TonemapParams noLook{}; noLook.Look = Mode::NoLook; noLook.OutEncoding = kEncodingSdrLinear;
    TonemapParams noLookLut = noLook; noLookLut.Lut = RigLut::Half;
    const Rgb half = {0.5f, 0.5f, 0.5f};
    EXPECT_NEAR(Rig().Run(half, noLookLut).Peak(), Rig().Run(half, noLook).Peak(), 1e-5f);
}

// A LUT effect whose slot holds no usable LUT (none picked yet, or a file that failed to load)
// leaves the image exactly as no LUT effect would. The slot keeps the identity tables bound for
// valid descriptors, and the pass must not sample them: a 2-texel table filtered at 8-bit
// precision quantises dark values (0.0009 linear rendered as 0).
TEST_F(TonemapHdrHeadroom, ALutSlotWithNoUsableLutLeavesTheImageUntouched)
{
    std::vector<float> levels = {0.0009086f, 0.0020875f, 0.0031641f};
    for (int i = 0; i <= 96; ++i)
        levels.push_back(std::exp2(-12.0f + 14.0f * static_cast<float>(i) / 96.0f));
    for (Mode m : {Mode::Neutral, Mode::Filmic, Mode::Photographic})
    {
        TonemapParams plain{}; plain.Look = m; plain.OutEncoding = kEncodingSdrLinear;
        TonemapParams slot = plain; slot.Lut = RigLut::IdentityFallback;
        float worst = 0.0f;
        std::string where;
        for (float v : levels)
        {
            const Rgb scene{v, v, v};
            const Rgb a = Rig().Run(scene, plain);
            const Rgb b = Rig().Run(scene, slot);
            const float d = std::max({std::fabs(a.R - b.R), std::fabs(a.G - b.G), std::fabs(a.B - b.B)});
            if (d > worst)
            {
                worst = d;
                where = std::to_string(v) + ": " + Fmt(b) + " against " + Fmt(a);
            }
        }
        EXPECT_LE(worst, 1e-6f) << ModeName(m) << ": the empty LUT slot changed the image at scene " << where;
    }
}

// A LUT is authored for display values in [0, 1], so it grades each pixel at the value it shows:
// a pixel inside [0, 1] is looked up as it is, under SDR and under HDR alike, and a pixel above 1.0
// (Filmic's HDR highlights) is divided by its own largest channel for the lookup and scaled back.
// The square LUT therefore maps Filmic's mid-grey and 0.5 to the square of the look's output under
// both encodings, at any panel peak, and leaves a grey highlight above 1.0 as it is: above 1.0 the
// LUT grades a highlight's color, not its tone.
TEST_F(TonemapHdrHeadroom, FilmicUnderHdrGradesMidtonesAsInSdr)
{
    for (const int encoding : {kEncodingSdrLinear, kEncodingHdrLinear})
    {
        for (const float v : {0.18f, 0.5f})
        {
            const Rgb scene{v, v, v};
            TonemapParams filmic{}; filmic.Look = Mode::Filmic; filmic.OutEncoding = encoding;
            TonemapParams graded = filmic; graded.Lut = RigLut::Square;
            const Rgb out = Rig().Run(scene, filmic);
            const Rgb g = Rig().Run(scene, graded);
            ASSERT_LT(out.Peak(), 1.0f) << "scene " << v << " is not a midtone here: " << Fmt(out);
            EXPECT_NEAR(g.Peak(), out.Peak() * out.Peak(), 2e-3f)
                << (encoding == kEncodingHdrLinear ? "HDR" : "SDR") << " scene " << v << ": Filmic " << Fmt(out)
                << " graded to " << Fmt(g) << ", expected its square";
        }
    }

    const Rgb highlight{2.0f, 2.0f, 2.0f};
    TonemapParams filmic{}; filmic.Look = Mode::Filmic; filmic.OutEncoding = kEncodingHdrLinear;
    TonemapParams graded = filmic; graded.Lut = RigLut::Square;
    const Rgb out = Rig().Run(highlight, filmic);
    ASSERT_GT(out.Peak(), 1.0f) << "scene 2.0 does not reach the headroom: " << Fmt(out);
    EXPECT_NEAR(Rig().Run(highlight, graded).Peak(), out.Peak(), 5e-3f)
        << "a grey highlight above 1.0 keeps its tone: " << Fmt(out);
}

} // namespace
