// Every pass that reads scene colour stored at the pre-exposure P keeps its meaning when P changes.
// Each shipping shader runs twice on the same scene: once with the colour stored at P = 1, once with
// it stored at P = 4 (and its temporal history at a previous P of 2). A reader whose output stays in
// stored units must then produce exactly 4 times its P = 1 output, the tonemap exactly its P = 1
// output, and a writer exactly 4 times its P = 1 output: P is a power of two, so every multiply and
// divide by it is exact. A reader that multiplies where it should divide, or forgets P, fails.
#include "Mathematics/HalfFloat.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/CubeLutGpuUpload.h"
#include "TestDeviceHelper.h"
#include <gtest/gtest.h>

#include <bit>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
using Pixel = std::array<float, 4>;
using Image = std::vector<Pixel>;
using PushValue = std::variant<float, int32_t, uint32_t, std::array<float, 4>>;

constexpr uint32_t kSize = 8;       // every rig image is kSize x kSize
constexpr float kPreExposure = 4.0f; // the stored scale of the second run
constexpr float kPreviousPreExposure = 2.0f;

// One run of one shader: its sampled inputs by name, its uniform-buffer members by name, its push
// constants by name, and the view's ExposureHistory {scale 1, valid 1, P, previous P}.
struct RigRun
{
    std::map<std::string, Image> Textures;
    std::map<std::string, std::vector<float>> UniformMembers;
    std::map<std::string, PushValue> Push;
    std::map<std::string, std::vector<uint8_t>> StorageBuffers;
    float PreExposure = 1.0f;
    float PreviousPreExposure = 1.0f;
    uint32_t VertexCount = 3;
    uint32_t InstanceCount = 1;
    // A graphics shader's colour target; scene colour is RGBA16F.
    TextureFormat ColorFormat = TextureFormat::R32G32B32A32_FLOAT;
};

struct RigProgram
{
    ShaderMeta Meta;
    DescriptorSetLayoutDesc Layout;
    PipelineHandle Pipeline;
    bool Compute = false;
};

class PreExposureReader : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            GTEST_SKIP() << "Vulkan device unavailable";
        Sampler = Device->CreateSampler(SamplerDesc::PointClamp("PreExposureReader"));
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

    RigProgram& Load(const std::string& name, TextureFormat colorFormat)
    {
        auto& program = Programs[name + "#" + std::to_string(static_cast<uint32_t>(colorFormat))];
        if (program.Pipeline.IsValid())
            return program;
        ShaderPackage pkg{};
        std::string err;
        EXPECT_TRUE(LoadShaderPkg("Shaders/" + name + ".shaderpkg", Device->PreferredShaderSource(), pkg, &err))
            << name << ": " << err;
        program.Meta = pkg.meta;
        program.Compute = pkg.stageBytes.count("cs") != 0;
        if (program.Compute)
        {
            ComputePipelineDesc cd{};
            cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(pkg.stageBytes.at("cs"));
            auto patch = [&](uint32_t set, DescriptorSetLayoutDesc& layout)
            { if (set == 0) program.Layout = layout; };
            MaterialHelper::ApplyShaderMetaToComputeDesc(*Device, program.Meta, cd, MaterialBuilder::MergeMode::Auto,
                                                         {true, 128}, patch, &err);
            program.Pipeline = Device->GetOrCreateComputePipeline(Device->InternComputePipeline(std::move(cd)));
        }
        else
        {
            PipelineDesc desc{};
            desc.type = PipelineType::Graphics;
            desc.vertexShader = pkg.stageBytes.at("vs");
            desc.pixelShader = pkg.stageBytes.at("fs");
            desc.EnableDepthTest(false);
            desc.SetCullingMode(CullModeFlagBits::None);
            MaterialBuilder::FormatsHint formats{};
            formats.ColorFormats = {static_cast<uint32_t>(colorFormat)};
            EXPECT_TRUE(MaterialBuilder::BuildPipelineDescFromMeta(program.Meta, desc, formats,
                                                                   MaterialBuilder::MergeMode::Auto,
                                                                   MaterialBuilder::PushConstantPolicy{}, &err))
                << name << ": " << err;
            program.Layout = desc.descriptorSetLayouts.empty() ? DescriptorSetLayoutDesc{} : desc.descriptorSetLayouts[0];
            program.Pipeline = Device->CreatePipeline(desc);
        }
        EXPECT_TRUE(program.Pipeline.IsValid()) << name << ": " << err;
        return program;
    }

    TextureHandle Texture(const Image& pixels, TextureFormat format, uint32_t usage)
    {
        TextureDesc d{};
        d.width = d.height = kSize;
        d.depth = d.arrayLayers = d.sampleCount = d.mipLevels = 1;
        d.format = static_cast<uint32_t>(format);
        d.usage = usage | static_cast<uint32_t>(TextureUsage::TransferDst | TextureUsage::TransferSrc);
        const TextureHandle t = Device->CreateTexture(d);
        EXPECT_TRUE(t.IsValid());
        Textures.push_back(t);
        if (!pixels.empty())
        {
            const BufferHandle upload = Device->CreateUploadBuffer(pixels.size() * sizeof(Pixel), "PreExposureReader");
            Buffers.push_back(upload);
            Device->UpdateBuffer(upload, 0, pixels.size() * sizeof(Pixel), pixels.data());
            auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
            cl->Begin();
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(t, ResourceState::Undefined, ResourceState::CopyDest));
            cl->CopyBufferToTextureSubresource(upload, t, 0, 0, kSize, kSize, 0, kSize * sizeof(Pixel));
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(t, ResourceState::CopyDest, ResourceState::ShaderResource));
            Submit(cl.get());
        }
        return t;
    }

    BufferHandle Buffer(const void* data, size_t bytes, BufferUsage usage)
    {
        BufferDesc d{};
        d.size = std::max<size_t>(bytes, 16);
        d.usage = static_cast<uint32_t>(usage);
        d.memoryUsage = BufferMemoryUsage::Upload;
        const BufferHandle b = Device->CreateBuffer(d);
        EXPECT_TRUE(b.IsValid());
        Buffers.push_back(b);
        std::vector<uint8_t> zeros(d.size, 0);
        Device->UpdateBuffer(b, 0, zeros.size(), zeros.data());
        if (data && bytes)
            Device->UpdateBuffer(b, 0, bytes, data);
        return b;
    }

    void Submit(CommandList* cl)
    {
        cl->End();
        Device->ExecuteCommandLists({cl});
        Device->WaitForIdle();
    }

    Image ReadBack(TextureHandle t, TextureFormat format, ResourceState from)
    {
        const bool half = format == TextureFormat::R16G16B16A16_FLOAT;
        const size_t texel = half ? 8 : 16;
        const BufferHandle readback = Device->CreateReadbackBuffer(kSize * kSize * texel);
        Buffers.push_back(readback);
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(t, from, ResourceState::CopySource));
        cl->CopyTextureToBuffer(t, readback, kSize, kSize);
        Submit(cl.get());
        Image out(kSize * kSize);
        if (const auto* bytes = static_cast<const uint8_t*>(Device->MapBuffer(readback)))
        {
            for (size_t i = 0; i < out.size(); ++i)
                for (size_t c = 0; c < 4; ++c)
                {
                    if (half)
                    {
                        uint16_t h = 0;
                        std::memcpy(&h, bytes + i * texel + c * 2, 2);
                        out[i][c] = GameEngine::Mathematics::HalfToFloat(h);
                    }
                    else
                        std::memcpy(&out[i][c], bytes + i * texel + c * 4, 4);
                }
            Device->UnmapBuffer(readback);
        }
        return out;
    }

    // A kSize x kSize D32 texture, array-shaped when `arrayed`, in the shader-resource state. Its
    // contents are undefined: it fills a shadow binding a run does not sample.
    TextureHandle ShadowTexture(bool arrayed)
    {
        TextureDesc d{};
        d.width = d.height = kSize;
        d.depth = d.arrayLayers = d.sampleCount = d.mipLevels = 1;
        d.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
        d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::DepthStencil);
        d.flags = arrayed ? TextureCreateFlags::ForceArrayView : TextureCreateFlags::None;
        const TextureHandle t = Device->CreateTexture(d);
        EXPECT_TRUE(t.IsValid());
        Textures.push_back(t);
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(t, ResourceState::Undefined, ResourceState::ShaderResource));
        Submit(cl.get());
        return t;
    }

    SamplerHandle ShadowSampler()
    {
        if (!m_ShadowSampler.IsValid())
        {
            SamplerDesc desc = SamplerDesc::PointClamp("PreExposureReader.Shadow");
            desc.compareEnable = true;
            desc.compareOp = CompareOp::GreaterOrEqual;
            m_ShadowSampler = Device->CreateSampler(desc);
        }
        return m_ShadowSampler;
    }

    // Runs `name` once. Graphics shaders draw into one RGBA32F target ("oColor"); compute shaders
    // write their reflected storage images, each RGBA16F. Returns every output by name.
    std::map<std::string, Image> Run(const std::string& name, const RigRun& run)
    {
        RigProgram& program = Load(name, run.ColorFormat);
        std::map<std::string, Image> outputs;
        if (!program.Pipeline.IsValid())
            return outputs;
        const float exposure[4] = {1.0f, 0.0f, run.PreExposure, run.PreviousPreExposure};
        uint32_t valid = 1u;
        std::array<uint8_t, 16> exposureBytes{};
        std::memcpy(exposureBytes.data(), exposure, 16);
        std::memcpy(exposureBytes.data() + 4, &valid, 4);
        const TextureHandle fallback = Texture(Image(kSize * kSize, Pixel{0.5f, 0.5f, 0.5f, 1.0f}),
                                               TextureFormat::R32G32B32A32_FLOAT,
                                               static_cast<uint32_t>(TextureUsage::ShaderResource));

        DescriptorSetDesc dd{};
        dd.layout = program.Layout;
        dd.transient = true;
        const DescriptorSetHandle ds = Device->CreateDescriptorSet(dd);
        std::map<std::string, TextureHandle> storageImages;
        for (const DescriptorSetMeta& set : program.Meta.Sets)
        {
            if (set.Set != 0)
                continue;
            for (const DescriptorBindingMeta& b : set.Bindings)
            {
                if (b.Type == ShaderMetaBindingType::kCombinedImageSampler && b.Image && b.Image->Depth)
                {
                    // A shadow sampler gets a depth texture of its shape and a comparison sampler,
                    // so the descriptor is valid; the runs that bind one leave it unread.
                    Device->UpdateCombinedImageSamplerBinding(ds, b.Binding, ShadowTexture(b.Image->Arrayed),
                                                              ShadowSampler());
                }
                else if (b.Type == ShaderMetaBindingType::kCombinedImageSampler && b.Image && b.Image->Dim == 3)
                {
                    // A 3D sampler (hdr_color_fx.frag's Cube LUT table) gets the identity table, so
                    // the descriptor's view type matches; the runs that bind one leave it unread.
                    const CubeLutGpuTextures identity = CreateIdentityCubeLutTextures(Device.get());
                    Textures.push_back(identity.Lut3D);
                    Textures.push_back(identity.Lut1DStrip);
                    Device->UpdateCombinedImageSamplerBinding(ds, b.Binding, identity.Lut3D, Sampler);
                }
                else if (b.Type == ShaderMetaBindingType::kCombinedImageSampler)
                {
                    const auto it = run.Textures.find(b.Name);
                    const TextureHandle t = it == run.Textures.end()
                                                ? fallback
                                                : Texture(it->second, TextureFormat::R32G32B32A32_FLOAT,
                                                          static_cast<uint32_t>(TextureUsage::ShaderResource));
                    Device->UpdateCombinedImageSamplerBinding(ds, b.Binding, t, Sampler);
                }
                else if (b.Type == ShaderMetaBindingType::kStorageImage)
                {
                    const TextureHandle t = Texture({}, TextureFormat::R16G16B16A16_FLOAT,
                                                    static_cast<uint32_t>(TextureUsage::UnorderedAccess));
                    storageImages[b.Name] = t;
                    Device->UpdateStorageImageBinding(ds, b.Binding, t);
                }
                else if (b.Type == ShaderMetaBindingType::kStorageBuffer)
                {
                    if (b.Name == "uExposure")
                    {
                        Device->UpdateStorageBufferBinding(ds, b.Binding, Buffer(exposureBytes.data(), 16, BufferUsage::Storage), 0, 16);
                        continue;
                    }
                    const auto it = run.StorageBuffers.find(b.Name);
                    const std::vector<uint8_t> data = it == run.StorageBuffers.end() ? std::vector<uint8_t>(65536, 0) : it->second;
                    Device->UpdateStorageBufferBinding(ds, b.Binding, Buffer(data.data(), data.size(), BufferUsage::Storage), 0,
                                                       data.size());
                }
                else if (b.Type == ShaderMetaBindingType::kUniformBuffer)
                {
                    const uint32_t size = b.Block ? std::max<uint32_t>(b.Block->Size, 16u) : 4096u;
                    std::vector<uint8_t> bytes(size, 0);
                    if (b.Block)
                        for (const auto& member : b.Block->Members)
                            if (auto it = run.UniformMembers.find(member.Name); it != run.UniformMembers.end())
                                std::memcpy(bytes.data() + member.Offset, it->second.data(),
                                            std::min<size_t>(it->second.size() * 4, size - member.Offset));
                    Device->UpdateBufferBinding(ds, b.Binding, Buffer(bytes.data(), bytes.size(), BufferUsage::Uniform), 0, size);
                }
            }
        }

        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        TextureHandle colorTarget{};
        if (program.Compute)
        {
            for (const auto& [n, t] : storageImages)
                cl->Barrier(ResourceBarrier::CreateTextureBarrier(t, ResourceState::Undefined, ResourceState::UnorderedAccess));
            cl->SetPipeline(program.Pipeline);
        }
        else
        {
            colorTarget = Texture({}, run.ColorFormat,
                                  static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource));
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(colorTarget, ResourceState::Undefined, ResourceState::RenderTarget));
            RenderPassDesc rp{};
            rp.colorTargets[0] = colorTarget;
            rp.colorTargetCount = 1;
            rp.clearColor[0] = true;
            rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
            cl->BeginRenderPass(rp);
            cl->SetViewport(0.0f, 0.0f, float(kSize), float(kSize));
            cl->SetScissor(0, 0, kSize, kSize);
            cl->SetPipeline(program.Pipeline);
        }
        if (!program.Meta.PushConstants.empty())
        {
            NamedPushConstantWriter pc(program.Meta, program.Meta.PushConstants[0].Name);
            for (const auto& member : program.Meta.PushConstants[0].Block.Members)
                if (auto it = run.Push.find(member.Name); it != run.Push.end())
                    std::visit([&](auto v) { pc.Add(member.Name, v); }, it->second);
            pc.Flush(cl.get());
        }
        cl->BindDescriptorSet(0, ds, program.Pipeline);
        if (program.Compute)
        {
            cl->Dispatch(1, 1, 1); // 8x8 groups cover the kSize x kSize image
            Submit(cl.get());
            for (const auto& [n, t] : storageImages)
                outputs[n] = ReadBack(t, TextureFormat::R16G16B16A16_FLOAT, ResourceState::UnorderedAccess);
        }
        else
        {
            cl->Draw(run.VertexCount, run.InstanceCount);
            cl->EndRenderPass();
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(colorTarget, ResourceState::RenderTarget, ResourceState::ShaderResource));
            Submit(cl.get());
            outputs["oColor"] = ReadBack(colorTarget, run.ColorFormat, ResourceState::ShaderResource);
        }
        return outputs;
    }

    std::unique_ptr<IDevice> Device;
    SamplerHandle Sampler;
    SamplerHandle m_ShadowSampler;
    std::vector<TextureHandle> Textures;
    std::vector<BufferHandle> Buffers;
    std::map<std::string, RigProgram> Programs;
};

// A scene with values on both sides of every absolute threshold the readers use (bloom and halation
// whites, the fog glow threshold, the DoF highlight band, TAA's Karis knee): 0.05 to 24.
Image Scene(float alpha = 1.0f)
{
    Image image(kSize * kSize);
    for (uint32_t i = 0; i < kSize * kSize; ++i)
    {
        const float v = 0.05f * std::exp2(float(i % 16) * 0.6f);
        image[i] = Pixel{v, v * 0.7f, v * 0.4f + 0.01f, alpha};
    }
    return image;
}

Image Scaled(const Image& image, float scale, bool alphaToo = false)
{
    Image out = image;
    for (Pixel& p : out)
        for (int c = 0; c < (alphaToo ? 4 : 3); ++c)
            p[c] *= scale;
    return out;
}

Image Filled(Pixel value)
{
    return Image(kSize * kSize, value);
}

// Every texel of `actual` equals `scale` x `reference` (relative 1e-5: exact up to the rounding a
// shader's fp16 output adds), and the reference is not all zero (a pass that wrote nothing would
// compare equal).
void ExpectScaled(const Image& reference, const Image& actual, float scale, int channels, const char* what)
{
    ASSERT_EQ(reference.size(), actual.size()) << what;
    double energy = 0.0;
    int mismatches = 0;
    for (size_t i = 0; i < reference.size(); ++i)
        for (int c = 0; c < channels; ++c)
        {
            const float expected = reference[i][c] * scale;
            energy += std::abs(reference[i][c]);
            if (std::abs(actual[i][c] - expected) > 1e-5f * std::max(1.0f, std::abs(expected)) + 1e-6f && mismatches++ < 4)
                ADD_FAILURE() << what << " texel " << i << " channel " << c << ": " << actual[i][c] << " vs "
                              << expected;
        }
    EXPECT_EQ(mismatches, 0) << what;
    EXPECT_GT(energy, 0.0) << what << " wrote nothing: the comparison could not fail";
}

RigRun AtPreExposure(RigRun run, float p, float previous)
{
    run.PreExposure = p;
    run.PreviousPreExposure = previous;
    return run;
}
} // namespace

TEST_F(PreExposureReader, TonemapShowsTheSameImageWhateverThePreExposure)
{
    RigRun run;
    run.Push = {{"exposure", 0.5f}, {"look", int32_t(0)}, {"outEncoding", int32_t(1)}, {"useAutoExposure", int32_t(0)},
                {"paperWhiteNits", 203.0f}, {"maxOutputNits", 1000.0f}};
    run.Textures["uHDRColor"] = Scene();
    const Image at1 = Run("tonemap", run)["oColor"];
    run.Textures["uHDRColor"] = Scaled(Scene(), kPreExposure);
    const Image atP = Run("tonemap", AtPreExposure(run, kPreExposure, kPreExposure))["oColor"];
    ExpectScaled(at1, atP, 1.0f, 3, "tonemap");
}

TEST_F(PreExposureReader, BloomAndScatteringThresholdsCompareAbsoluteBrightness)
{
    RigRun run;
    run.Push = {{"threshold", 1.0f}, {"knee", 0.5f}, {"exposure", 1.0f}, {"useAutoExposure", int32_t(0)},
                {"bloomAntiFlicker", int32_t(1)}, {"bloomDepthVeilEnabled", int32_t(1)},
                {"bloomDepthVeilIntensity", 0.5f}, {"bloomDepthVeilStart", 0.0f}, {"bloomDepthVeilEnd", 2.0f},
                {"thresholdFree", int32_t(0)}};
    run.UniformMembers["ge_nearFar"] = {0.1f, 1000.0f, 0.0f, 0.0f};
    run.Textures["uDepth"] = Filled({0.01f, 0.0f, 0.0f, 0.0f});
    run.Textures["uHDR"] = Scene();
    const Image at1 = Run("bloom_threshold", run)["oColor"];
    run.Textures["uHDR"] = Scaled(Scene(), kPreExposure);
    const Image atP = Run("bloom_threshold", AtPreExposure(run, kPreExposure, kPreExposure))["oColor"];
    ExpectScaled(at1, atP, kPreExposure, 4, "bloom_threshold (rgb and the depth veil in alpha)");
}

TEST_F(PreExposureReader, HalationExtractsAbsoluteHighlights)
{
    RigRun run;
    run.Push = {{"exposure", 1.0f}, {"useAutoExposure", int32_t(0)}};
    run.Textures["uHDR"] = Scene();
    const Image at1 = Run("halation_prefilter", run)["oColor"];
    run.Textures["uHDR"] = Scaled(Scene(), kPreExposure);
    const Image atP = Run("halation_prefilter", AtPreExposure(run, kPreExposure, kPreExposure))["oColor"];
    ExpectScaled(at1, atP, kPreExposure, 3, "halation_prefilter");
}

TEST_F(PreExposureReader, ColourFilterComposesAgainstExposedValues)
{
    // Add (mode 1) is not scale-invariant: it composes against the exposed value, so P must be
    // divided out of the exposure the block anchors at.
    RigRun run;
    run.Push = {{"colorFilterR", 0.2f}, {"colorFilterG", 0.1f}, {"colorFilterB", 0.05f}, {"colorFilterIntensity", 1.0f},
                {"colorFilterBlendMode", int32_t(1)}, {"exposure", 0.5f}, {"useAutoExposure", int32_t(0)}};
    run.Textures["uSceneColor"] = Scene();
    const Image at1 = Run("hdr_color_fx", run)["oColor"];
    run.Textures["uSceneColor"] = Scaled(Scene(), kPreExposure);
    const Image atP = Run("hdr_color_fx", AtPreExposure(run, kPreExposure, kPreExposure))["oColor"];
    ExpectScaled(at1, atP, kPreExposure, 3, "hdr_color_fx");
}

TEST_F(PreExposureReader, FogGlowThresholdsAndKarisWeightsAreAbsolute)
{
    // The glow's source is the fog's own light, evaluated from the view depth (GE_FogGlowLayer): a
    // distance fog of a bright emissive colour over depths that vary across the rig, so the in-scatter
    // crosses the threshold on some pixels and not on others. The view's projection is the identity,
    // so a pixel's distance is the length of (ndc, depth).
    std::vector<float> identity(16, 0.0f);
    identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
    Image depth(kSize * kSize);
    for (uint32_t i = 0; i < kSize * kSize; ++i)
        depth[i] = Pixel{0.05f + 0.9f * float(i % 16) / 15.0f, 0.0f, 0.0f, 0.0f};
    RigRun run;
    run.Push = {{"fogGlowIntensity", 1.0f}, {"fogGlowRadius", 1.0f}, {"fogGlowThreshold", 1.0f}, {"fogGlowKnee", 0.5f},
                {"fogGlowFadeStart", 0.0f}, {"fogGlowFadeEnd", 0.2f}, {"fogGlowTintR", 1.0f}, {"fogGlowTintG", 1.0f},
                {"fogGlowTintB", 1.0f}, {"fogGlowAntiFlicker", int32_t(1)}, {"antiFlickerStage", int32_t(1)}};
    run.UniformMembers = {{"ge_invProj", identity},
                          {"ge_view", identity},
                          {"intensity", {1.0f}},
                          {"density", {1.0f}},
                          {"maxOpacity", {1.0f}},
                          {"smoothLength", {0.25f}},
                          {"maxDistance", {2.0f}},
                          {"emissiveR", {6.0f}},
                          {"emissiveG", {5.0f}},
                          {"emissiveB", {4.0f}},
                          {"flags", {std::bit_cast<float>(int32_t(1))}}};
    run.Textures["uDepth"] = depth;
    for (const char* shader : {"fog_glow_fast", "fog_glow_prefilter"})
    {
        run.Textures["uFogged"] = Scene();
        const Image at1 = Run(shader, run)["oColor"];
        run.Textures["uFogged"] = Scaled(Scene(), kPreExposure);
        const Image atP = Run(shader, AtPreExposure(run, kPreExposure, kPreExposure))["oColor"];
        ExpectScaled(at1, atP, kPreExposure, 3, shader);
    }
    run.Textures = {{"uSrc", Scene()}};
    const Image at1 = Run("fog_glow_downsample", run)["oColor"];
    run.Textures = {{"uSrc", Scaled(Scene(), kPreExposure)}};
    const Image atP = Run("fog_glow_downsample", AtPreExposure(run, kPreExposure, kPreExposure))["oColor"];
    ExpectScaled(at1, atP, kPreExposure, 3, "fog_glow_downsample (Karis stage)");
}

namespace
{
// TAA inputs at a stored scale: the current frame at `current`, the history at `history`.
RigRun TaaRun(float current, float historyScale, bool upscale)
{
    RigRun run;
    const std::vector<float> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    run.UniformMembers["uInvViewProj"] = identity;
    run.UniformMembers["uPrevViewProj"] = identity;
    run.UniformMembers["uExtent"] = {float(kSize), float(kSize), 1.0f / kSize, 1.0f / kSize};
    if (upscale)
        run.UniformMembers["uOutExtent"] = {float(kSize), float(kSize), 1.0f / kSize, 1.0f / kSize};
    run.UniformMembers["uParams0"] = {0.9f, 1.0f, 0.0f, 0.0f};  // history weight, history valid
    run.UniformMembers["uParams1"] = {1.0f, 0.01f, 0.0f, 0.0f}; // clip gamma, depth reject
    run.UniformMembers["uParams2"] = {0.0f, 0.0f, 0.0f, 1.0f};  // changed content: clip, Karis weights
    run.Textures["uSceneColor"] = Scaled(Scene(), current);
    Image history = Scaled(Scene(), historyScale * 0.8f);
    for (Pixel& p : history)
        p[3] = 0.5f; // the history's alpha is its depth
    run.Textures["uHistory"] = history;
    run.Textures["uDepth"] = Filled({0.5f, 0.0f, 0.0f, 0.0f});
    run.Textures["uMV"] = Filled({0.0f, 0.0f, 0.5f, 1.0f});
    return run;
}
} // namespace

TEST_F(PreExposureReader, TemporalAntiAliasingRescalesItsHistoryAndWeighsAbsoluteLuma)
{
    for (const auto& [shader, upscale] : {std::pair{"taa_resolve", false}, std::pair{"taa_resolve_upscale", true}})
    {
        auto at1 = Run(shader, TaaRun(1.0f, 1.0f, upscale));
        auto atP = Run(shader, AtPreExposure(TaaRun(kPreExposure, kPreviousPreExposure, upscale), kPreExposure,
                                             kPreviousPreExposure));
        ExpectScaled(at1["uOutColor"], atP["uOutColor"], kPreExposure, 3, shader);
        ExpectScaled(at1["uOutHistory"], atP["uOutHistory"], kPreExposure, 3, shader);
    }
}

TEST_F(PreExposureReader, ReflectionTemporalResolveScalesItsAbsoluteFloors)
{
    // `current` and `history` are absolute radiance; the run stores them at P.
    auto run = [](float p, const Image& current, const Image& history)
    {
        RigRun r;
        r.Push = {{"historyValid", uint32_t(1)}};
        r.Textures["uCurrent"] = Scaled(current, p);
        r.Textures["uReprojected"] = Scaled(history, p);
        Image moments(kSize * kSize);
        for (size_t i = 0; i < moments.size(); ++i)
        {
            const float l = 0.3f * current[i][0] * p;
            moments[i] = Pixel{l, l * l, 1.0f, 0.5f};
        }
        r.Textures["uReprojectedMoments"] = moments;
        r.Textures["uDepth"] = Filled({0.5f, 0.0f, 0.0f, 0.0f});
        r.Textures["uNormalRoughness"] = Filled({0.5f, 0.5f, 0.2f, 0.0f});
        r.Textures["uHitData"] = Filled({0.5f, 0.5f, 1.0f, 1.0f});
        return AtPreExposure(r, p, p);
    };
    auto expectScaled = [&](const Image& current, const Image& history, const char* what)
    {
        auto at1 = Run("sssr_temporal", run(1.0f, current, history));
        auto atP = Run("sssr_temporal", run(kPreExposure, current, history));
        ExpectScaled(at1["uHistoryOut"], atP["uHistoryOut"], kPreExposure, 3, what);
        ExpectScaled(at1["uMomentsOut"], atP["uMomentsOut"], kPreExposure, 1, what);
    };
    // A lit frame: the clip radius is relative to the neighbourhood mean.
    expectScaled(Scene(), Scaled(Scene(), 0.9f), "sssr_temporal, lit");
    // A dim frame (luminance 0.01): the radius is the absolute floor, and the history alternates
    // between a value past it (clipped) and one below the luminance worth clipping (kept).
    Image dimHistory(kSize * kSize);
    for (size_t i = 0; i < dimHistory.size(); ++i)
        dimHistory[i] = (i % 2) ? Pixel{0.03f, 0.03f, 0.03f, 1.0f} : Pixel{5e-6f, 5e-6f, 5e-6f, 1.0f};
    expectScaled(Filled({0.01f, 0.01f, 0.01f, 1.0f}), dimHistory, "sssr_temporal, dim");
}

TEST_F(PreExposureReader, DepthOfFieldSoftensAbsoluteHighlights)
{
    auto run = [](float p)
    {
        RigRun r;
        r.Push = {{"dofIntensity", 1.0f}, {"dofMaxRadius", 6.0f}, {"dofApertureBladeCount", int32_t(6)},
                  {"dofApertureRoundness", 1.0f}, {"dofAnamorphicSqueeze", 1.0f}, {"dofSamplingQuality", int32_t(1)}};
        Image color = Scaled(Scene(), p);
        for (size_t i = 0; i < color.size(); ++i)
            color[i][3] = (i % 2 == 0) ? 4.0f : -4.0f; // far and near CoC, in pixels
        r.Textures["uHalfColorCoc"] = color;
        r.Textures["uDilatedCoc"] = Filled({4.0f, 4.0f, 0.0f, 0.0f});
        return r;
    };
    auto at1 = Run("ffx_dof_blur", run(1.0f));
    auto atP = Run("ffx_dof_blur", AtPreExposure(run(kPreExposure), kPreExposure, kPreExposure));
    ExpectScaled(at1["uNearField"], atP["uNearField"], kPreExposure, 3, "ffx_dof_blur near field");
    ExpectScaled(at1["uFarField"], atP["uFarField"], kPreExposure, 3, "ffx_dof_blur far field");
}

TEST_F(PreExposureReader, LensFlareStoresItsLightAtThePreExposure)
{
    // One full-screen flare quad, occlusion probe off (source depth < 0).
    const float instance[16] = {0.0f, 0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.5f, 0.5f,
                                2.0f, 1.0f, 0.5f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f};
    RigRun run;
    run.StorageBuffers["Instances"] = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(instance),
                                                            reinterpret_cast<const uint8_t*>(instance) + sizeof(instance));
    run.Textures["uAtlas"] = Scene();
    run.Push = {{"atlasHasAuthoredAlpha", int32_t(0)}};
    run.VertexCount = 6;
    const Image at1 = Run("lens_flare", run)["oColor"];
    const Image atP = Run("lens_flare", AtPreExposure(run, kPreExposure, kPreExposure))["oColor"];
    ExpectScaled(at1, atP, kPreExposure, 3, "lens_flare");
}

namespace
{
// The SSR passes' viewport inside the target, in target UV: the whole target.
constexpr std::array<float, 4> kFullViewport = {0.0f, 0.0f, 1.0f, 1.0f};

// A view whose matrices are all identity: positions reproject onto themselves, so the SSR history
// lookups land on the same pixel.
void IdentityView(RigRun& run)
{
    const std::vector<float> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (const char* m : {"ge_view", "ge_invView", "ge_proj", "ge_invProj", "ge_viewProj", "ge_prevViewProj",
                          "prevInvViewProj"})
        run.UniformMembers[m] = identity;
    run.UniformMembers["ge_nearFar"] = {0.1f, 100.0f, 0.0f, 0.0f};
    run.UniformMembers["ge_screenSize"] = {float(kSize), float(kSize), 1.0f / kSize, 1.0f / kSize};
}
} // namespace

TEST_F(PreExposureReader, ReflectionReprojectionRescalesItsHistoryAndMoments)
{
    // The surface history the reprojection validates against, written by the temporal resolve.
    RigRun surface;
    IdentityView(surface);
    surface.Push = {{"historyValid", uint32_t(1)}, {"viewportRect", kFullViewport}};
    surface.Textures["uDepth"] = Filled({0.5f, 0.0f, 0.0f, 0.0f});
    surface.Textures["uNormalRoughness"] = Filled({0.5f, 0.5f, 0.2f, 0.0f});
    const Image surfaceHistory = Run("sssr_temporal", surface)["uSurfaceOut"];

    auto run = [&](float p, float previous)
    {
        RigRun r;
        IdentityView(r);
        r.Push = {{"historyValid", uint32_t(1)}, {"viewportRect", kFullViewport}};
        r.Textures["uCurrent"] = Scaled(Scene(), p);
        r.Textures["uHitData"] = Filled({0.0f, 0.0f, 0.0f, 0.0f}); // a miss: the receiver is the target
        r.Textures["uDepth"] = Filled({0.5f, 0.0f, 0.0f, 0.0f});
        r.Textures["uNormalRoughness"] = Filled({0.5f, 0.5f, 0.2f, 0.0f});
        r.Textures["uHistory"] = Scaled(Scene(), previous * 0.9f);
        Image moments(kSize * kSize);
        for (size_t i = 0; i < moments.size(); ++i)
        {
            const float l = 0.3f * Scene()[i][0] * previous;
            moments[i] = Pixel{l, l * l, 1.0f, 0.5f};
        }
        r.Textures["uMoments"] = moments;
        r.Textures["uSurfaceHistory"] = surfaceHistory;
        r.Textures["uMotionVectors"] = Filled({100.0f, 100.0f, 0.0f, 0.0f}); // no exact motion: analytic
        return AtPreExposure(r, p, previous);
    };
    auto at1 = Run("sssr_reproject", run(1.0f, 1.0f));
    auto atP = Run("sssr_reproject", run(kPreExposure, kPreviousPreExposure));
    ExpectScaled(at1["uReprojected"], atP["uReprojected"], kPreExposure, 3, "sssr_reproject history");
    ExpectScaled(at1["uReprojectedMoments"], atP["uReprojectedMoments"], kPreExposure, 1,
                 "sssr_reproject first moment");
}

TEST_F(PreExposureReader, SunGlareWritesItsRadianceAtThePreExposure)
{
    // The glare's analytic radiance, with every occlusion term off: no cascades, the sun off
    // screen for the depth probe, the skyline clear. The transmittance LUT is the rig's grey.
    RigRun run;
    IdentityView(run);
    run.Push = {{"sunDirRadius", std::array<float, 4>{0.0f, 0.0f, 1.0f, 0.0047f}},
                {"sunColorIrradiance", std::array<float, 4>{1.0f, 0.9f, 0.8f, 1.0f}},
                {"probe", std::array<float, 4>{0.5f, 0.5f, 0.01f, 0.01f}},
                {"params", std::array<float, 4>{1.0f, 1.0f, 0.0f, 0.0f}},
                {"cameraShadow", std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}},
                {"horizon", std::array<float, 4>{1.0f, 6360e3f, 6460e3f, 0.0f}}};
    const Image at1 = Run("sun_glare", run)["oColor"];
    const Image atP = Run("sun_glare", AtPreExposure(run, kPreExposure, kPreExposure))["oColor"];
    ExpectScaled(at1, atP, kPreExposure, 3, "sun_glare");
}

namespace
{
// GE_OctEncode (octahedral_normal.glsl) of a unit vector.
std::array<float, 2> OctEncode(float x, float y, float z)
{
    const float sum = std::abs(x) + std::abs(y) + std::abs(z);
    x /= sum;
    y /= sum;
    if (z < 0.0f)
    {
        const float ex = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float ey = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ex;
        y = ey;
    }
    return {x * 0.5f + 0.5f, y * 0.5f + 0.5f};
}
} // namespace

TEST_F(PreExposureReader, ReflectionRaysCompressFirefliesInAbsoluteLuminance)
{
    // Identity projection: a texel's view position is (ndc.xy, raw depth). The left half is a far
    // surface (depth 0.3) whose mirror rays run along +x and slightly toward the camera; the right
    // half is a nearer wall (depth 0.7) facing them, so the rays from the two leftmost columns hit
    // it and read the scene colour there, part of it past the firefly knee.
    const float len = std::sqrt(1.0f + 0.25f * 0.25f);
    const float rx = 1.0f / len, rz = 0.25f / len;
    const std::array<float, 2> mirror = OctEncode(rx, 0.0f, rz);
    const std::array<float, 2> wall = OctEncode(-rx, 0.0f, -rz);
    Image depth(kSize * kSize), normals(kSize * kSize), specular(kSize * kSize), weight(kSize * kSize);
    std::vector<uint32_t> rays;
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x)
        {
            const size_t i = y * kSize + x;
            const bool left = x < kSize / 2;
            depth[i] = Pixel{left ? 0.3f : 0.7f, 0.0f, 0.0f, 0.0f};
            normals[i] = left ? Pixel{mirror[0], mirror[1], 0.02f, 0.0f} : Pixel{wall[0], wall[1], 0.02f, 0.0f};
            specular[i] = Pixel{0.1f, 0.1f, 0.1f, mirror[1]}; // rgb: the probe estimate a miss writes
            weight[i] = Pixel{0.0f, 0.0f, 0.0f, mirror[0]};
            if (left)
                rays.push_back(x | (y << 16));
        }
    std::vector<uint8_t> rayBytes(rays.size() * 4);
    std::memcpy(rayBytes.data(), rays.data(), rayBytes.size());
    const uint32_t args[8] = {1, 1, 1, static_cast<uint32_t>(rays.size()), 1, 1, 1, 0};
    std::vector<uint8_t> argBytes(sizeof(args));
    std::memcpy(argBytes.data(), args, sizeof(args));

    auto run = [&](float p)
    {
        RigRun r;
        IdentityView(r);
        r.Push = {{"sssrMaxDistance", 2.0f}, {"sssrThickness", 1.0f}, {"sssrMaxSteps", int32_t(64)},
                  {"historyValid", uint32_t(0)}, {"multiBounce", int32_t(0)}, {"viewportRect", kFullViewport}};
        r.Textures["uSceneColor"] = Scaled(Scene(), p);
        r.Textures["uDepth"] = depth;
        r.Textures["uHZB"] = depth;
        r.Textures["uNormalRoughness"] = normals;
        r.Textures["uSpecularRadiance"] = Scaled(specular, p);
        r.Textures["uSpecularWeight"] = weight;
        r.StorageBuffers["RayList"] = rayBytes;
        r.StorageBuffers["Args"] = argBytes;
        return AtPreExposure(r, p, p);
    };
    auto at1 = Run("sssr_intersect", run(1.0f));
    auto atP = Run("sssr_intersect", run(kPreExposure));
    ExpectScaled(at1["uRadiance"], atP["uRadiance"], kPreExposure, 3, "sssr_intersect radiance");

    // The comparison covers the knee only if some ray hit and read a texel past it.
    int kneeHits = 0;
    for (size_t i = 0; i < at1["uHitData"].size(); ++i)
    {
        const Pixel& c = at1["uRadiance"][i];
        if (at1["uHitData"][i][2] > 0.0f && 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2] > 4.0f)
            ++kneeHits;
    }
    EXPECT_GT(kneeHits, 0) << "no ray hit a texel past the firefly knee: the threshold went untested";
}

// Test 1 of the pre-exposure design: a radiance of 1e-6 scene-linear, below fp16's normal floor
// (6.1e-5), survives scene colour. A shipped writer (the lens flare, one full-screen quad) draws it
// into an RGBA16F target at the P a settled night exposure gives, 2^16 (pre_exposure_snapshot.comp
// rounds the adapted scale to a power of two; PreExposureDeclare pins that from the shipped graph),
// and the stored value divided by P is within 1 % of 1e-6. At P = 1 the same write lands in
// fp16's subnormals, which cannot hold it to 1 %: that arm is the check that the test can fail.
TEST_F(PreExposureReader, ARadianceBelowTheHalfFloatFloorSurvivesSceneColour)
{
    constexpr float kRadiance = 1e-6f;
    constexpr float kNightPreExposure = 65536.0f; // 2^16
    const float instance[16] = {0.0f, 0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.5f, 0.5f,
                                1.0f, 1.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f};
    RigRun run;
    run.StorageBuffers["Instances"] = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(instance),
                                                            reinterpret_cast<const uint8_t*>(instance) + sizeof(instance));
    run.Textures["uAtlas"] = Filled({kRadiance, kRadiance, kRadiance, 1.0f});
    run.Push = {{"atlasHasAuthoredAlpha", int32_t(0)}};
    run.VertexCount = 6;
    run.ColorFormat = TextureFormat::R16G16B16A16_FLOAT;

    auto storedOverP = [&](float p)
    {
        const Image stored = Run("lens_flare", AtPreExposure(run, p, p))["oColor"];
        double worst = 0.0;
        for (const Pixel& texel : stored)
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(double(texel[c]) / p - kRadiance) / kRadiance);
        return worst;
    };
    const double atNight = storedOverP(kNightPreExposure);
    const double atOne = storedOverP(1.0f);
    RecordProperty("relativeErrorAtP65536", std::to_string(atNight));
    RecordProperty("relativeErrorAtP1", std::to_string(atOne));
    EXPECT_LE(atNight, 0.01) << "1e-6 stored at P = 2^16 and divided by P";
    EXPECT_GT(atOne, 0.01) << "at P = 1 fp16 holds 1e-6 to better than 1 %: the test cannot fail";
}
