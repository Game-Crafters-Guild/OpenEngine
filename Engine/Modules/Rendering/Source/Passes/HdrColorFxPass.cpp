#include "Rendering/Passes/HdrColorFxPass.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Rendering/Utils/CubeLutGpuUpload.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine {
namespace Rendering {
namespace Passes {

namespace
{
// hdr_color_fx.frag's lutStage for the Grade stage (Includes/cube_lut.glsl, kCubeLutStageGrade).
constexpr int32_t kLutStageGrade = 1;

// The exposure-history state the shader reads ({float scale; uint valid; float preExposure; float
// previousPreExposure}): zero-filled, so the static exposure applies and pre-exposure reads as 1.
constexpr uint32_t kExposureStateBytes = 16u;

struct HdrColorFxShader
{
	std::vector<uint8_t> vs;
	std::vector<uint8_t> fs;
	ShaderMeta meta;
	uint32_t gradeParamsBytes = 0; // the reflected ColorGradeParams block
	bool loaded = false;
	bool warned = false;
};

struct HdrColorFxDeviceResources
{
	SamplerHandle sampler{};
	BufferHandle exposureBuffer{};
	BufferHandle gradeParamsBuffer{}; // zero-filled: the identity grade
	CubeLutGpuTextures identityLut{};
	bool created = false;
};

std::unordered_map<IDevice*, HdrColorFxDeviceResources> g_HdrColorFxPerDevice;

uint32_t ReflectedBlockBytes(const ShaderMeta& meta, const char* name)
{
	for (const DescriptorSetMeta& set : meta.Sets)
		for (const DescriptorBindingMeta& binding : set.Bindings)
			if (binding.Name == name && binding.Block)
				return binding.Block->Size;
	return 0;
}

HdrColorFxShader& GetShader(const IDevice& device)
{
	static HdrColorFxShader s;
	if (!s.loaded)
	{
		ShaderPackage pkg{};
		std::string err;
		if (LoadShaderPkg("Shaders/hdr_color_fx.shaderpkg", device.PreferredShaderSource(), pkg, &err))
		{
			if (auto it = pkg.stageBytes.find("vs"); it != pkg.stageBytes.end())
				s.vs = std::move(it->second);
			if (auto it = pkg.stageBytes.find("fs"); it != pkg.stageBytes.end())
				s.fs = std::move(it->second);
			s.meta = std::move(pkg.meta);
			s.gradeParamsBytes = ReflectedBlockBytes(s.meta, "ColorGradeParams");
		}
		// Latch only on success: a loader that is not ready yet retries; the warning fires once.
		s.loaded = !s.vs.empty() && !s.fs.empty() && s.gradeParamsBytes > 0;
		if (!s.loaded && !s.warned)
		{
			s.warned = true;
			Logger::Log::Warning("Passes::HdrColorFx: Shaders/hdr_color_fx.shaderpkg unavailable ({}); retrying", err);
		}
	}
	return s;
}

bool ResourcesValid(const HdrColorFxDeviceResources& res)
{
	return res.sampler.IsValid() && res.exposureBuffer.IsValid() && res.gradeParamsBuffer.IsValid() &&
		   res.identityLut.Lut3D.IsValid() && res.identityLut.Lut1DStrip.IsValid();
}

HdrColorFxDeviceResources* GetOrCreateResources(IDevice* device, uint32_t gradeParamsBytes)
{
	if (!device)
		return nullptr;
	auto [it, inserted] = g_HdrColorFxPerDevice.try_emplace(device);
	if (inserted)
	{
		// The cached objects belong to one device: drop them when its GPU objects die (shutdown or
		// an in-place rebuild after device loss).
		device->RegisterPerDeviceCacheCleanup("Passes.HdrColorFx",
											  [](IDevice* d) { CleanupHdrColorFxPassForDevice(d); });
	}
	HdrColorFxDeviceResources& res = it->second;
	if (res.created)
		return &res;
	res.created = true; // a failed creation is logged once, not retried every frame

	SamplerDesc samplerDesc{};
	samplerDesc.minFilter = 1;
	samplerDesc.magFilter = 1;
	samplerDesc.mipFilter = 1;
	samplerDesc.addressModeU = 2; // clamp
	samplerDesc.addressModeV = 2;
	samplerDesc.addressModeW = 2;
	samplerDesc.debugName = "HdrColorFx.LinearClamp";
	res.sampler = device->CreateSampler(samplerDesc);

	// Host-visible, so the zeros are in place before any submit that reads them. A device-local
	// fill goes through a staging copy that nothing orders before the next graphics submit, and
	// the first pass on a device could read the buffer unfilled.
	const std::vector<uint8_t> zeros(std::max(gradeParamsBytes, kExposureStateBytes), 0);
	BufferDesc exposureDesc{};
	exposureDesc.size = kExposureStateBytes;
	exposureDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
	exposureDesc.memoryUsage = BufferMemoryUsage::Upload;
	exposureDesc.debugName = "HdrColorFx.Exposure";
	res.exposureBuffer = CreateBufferInitialized(device, exposureDesc, zeros.data(), kExposureStateBytes);

	BufferDesc gradeDesc{};
	gradeDesc.size = gradeParamsBytes;
	gradeDesc.usage = static_cast<uint32_t>(BufferUsage::Uniform);
	gradeDesc.memoryUsage = BufferMemoryUsage::Upload;
	gradeDesc.debugName = "HdrColorFx.IdentityGrade";
	res.gradeParamsBuffer = CreateBufferInitialized(device, gradeDesc, zeros.data(), gradeParamsBytes);

	res.identityLut = CreateIdentityCubeLutTextures(device);
	if (!ResourcesValid(res))
		Logger::Log::Error("Passes::HdrColorFx: the pass's sampler, buffers or identity Cube LUT tables could not be "
						   "created; the pass declares nothing on this device");
	return &res;
}
} // namespace

void CleanupHdrColorFxPassForDevice(IDevice* device)
{
	if (!device)
		return;
	auto it = g_HdrColorFxPerDevice.find(device);
	if (it == g_HdrColorFxPerDevice.end())
		return;
	HdrColorFxDeviceResources& res = it->second;
	if (res.sampler.IsValid())
		device->DestroySampler(res.sampler);
	if (res.exposureBuffer.IsValid())
		device->DestroyBuffer(res.exposureBuffer);
	if (res.gradeParamsBuffer.IsValid())
		device->DestroyBuffer(res.gradeParamsBuffer);
	if (res.identityLut.Lut3D.IsValid())
		device->DestroyTexture(res.identityLut.Lut3D);
	if (res.identityLut.Lut1DStrip.IsValid())
		device->DestroyTexture(res.identityLut.Lut1DStrip);
	g_HdrColorFxPerDevice.erase(it);
}

RenderGraph::RGPass AddHdrColorFxPassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture src, RenderGraph::RGTexture dst,
                                      const HdrColorFxParams& params, const char* passName)
{
	IDevice* device = frame.Device();
	if (!device || !src.IsValid() || !dst.IsValid() || src.Id == dst.Id)
		return {};
	const HdrColorFxShader& shader = GetShader(*device);
	if (!shader.loaded)
		return {};
	const HdrColorFxDeviceResources* res = GetOrCreateResources(device, shader.gradeParamsBytes);
	if (!res || !ResourcesValid(*res))
		return {};

	// Layout and push block from the package's reflection, never hand-mirrored.
	GraphicsPipelineDesc gd{};
	gd.Kind = GraphicsPipelineKind::VertexFragment;
	gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(shader.vs);
	gd.PixelShader = std::make_shared<const std::vector<uint8_t>>(shader.fs);
	gd.Topology = PrimitiveTopology::TriangleList;
	gd.Rasterization.cullMode = CullModeFlagBits::None;
	gd.Rasterization.frontFace = FrontFace::CounterClockwise;
	gd.DepthStencil.depthTestEnable = false;
	gd.DepthStencil.depthWriteEnable = false;
	DynamicStateInfo dyn{};
	dyn.states.push_back(DynamicState::Viewport);
	dyn.states.push_back(DynamicState::Scissor);
	gd.DynamicState = std::move(dyn);
	gd.DebugName = "HdrColorFx.Pipeline";
	DescriptorSetLayoutDesc set0{};
	std::string metaErr;
	if (!MaterialHelper::ApplyShaderMetaToGraphicsDesc(
			*device, shader.meta, gd, MaterialBuilder::MergeMode::Auto, {true, 128},
			[&](uint32_t setIndex, DescriptorSetLayoutDesc& dsl)
			{
				if (setIndex == 0)
					set0 = dsl;
			},
			&metaErr))
	{
		static bool s_WarnedMeta = false;
		if (!s_WarnedMeta)
		{
			s_WarnedMeta = true;
			Logger::Log::Warning("Passes::HdrColorFx: shader meta rejected ({})", metaErr);
		}
		return {};
	}
	const GraphicsPipelineId pipelineId = device->InternGraphicsPipeline(std::move(gd));
	if (!pipelineId.IsValid())
		return {};

	// By member name against the reflected block. Unnamed members stay zero: no color filter
	// (intensity 0), no metered exposure.
	NamedPushConstantWriter pcw = shader.meta.PushConstants.empty()
									  ? NamedPushConstantWriter(shader.meta, std::string{})
									  : NamedPushConstantWriter(shader.meta, shader.meta.PushConstants[0].Name);
	if (pcw.IsValid())
	{
		pcw.Add("exposure", params.exposure);
		pcw.Add("lutIntensity", std::clamp(params.lutIntensity, 0.0f, 1.0f));
		pcw.Add("lutStage", kLutStageGrade);
		pcw.Add("lutInputEncoding", params.lut.InputEncoding);
		pcw.Add("lutSize3d", params.lut.Size3D);
		pcw.Add("lutSize1d", params.lut.Size1D);
		pcw.Add("lutFlags", params.lut.Flags);
		pcw.Add("lut1dInMin", params.lut.In1DMin);
		pcw.Add("lut1dInMax", params.lut.In1DMax);
		pcw.Add("lut3dInMin", params.lut.In3DMin);
		pcw.Add("lut3dInMax", params.lut.In3DMax);
	}

	const RenderGraph::RGResourceDesc dd = frame.Graph().ResourceDesc(dst.Id);
	const uint32_t w = dd.Width > 0 ? dd.Width : 1u;
	const uint32_t h = dd.Height > 0 ? dd.Height : 1u;
	const uint32_t gradeParamsBytes = shader.gradeParamsBytes;

	return frame.AddPass(
		passName ? passName : "HdrColorFx", PassPhase::kFinalize,
		[&](RenderGraph::RGPassBuilder& p)
		{
			p.Read(src, RenderGraph::RGTextureRead::Sampled);
			RenderGraph::RGAttachmentOps ops{};
			ops.Load = RenderGraph::RGLoadOp::DontCare; // the full-screen triangle writes every pixel
			ops.Store = RenderGraph::RGStoreOp::Store;
			p.AttachColor(0, dst, ops);
		},
		[pipelineId, src, set0, pcw, meta = &shader.meta, w, h, gradeParamsBytes, lut = params.lut](RenderGraph::RGContext& ctx)
		{
			auto* cl = ctx.Cmd;
			auto* dev = ctx.GetDevice();
			if (!cl || !dev)
				return;
			const auto srcTex = ctx.GetTexture(src);
			const auto resIt = g_HdrColorFxPerDevice.find(dev);
			if (!srcTex.IsValid() || resIt == g_HdrColorFxPerDevice.end() || !ResourcesValid(resIt->second))
				return;
			const HdrColorFxDeviceResources& res = resIt->second;
			const PipelineHandle pipeline = ctx.GetOrCreatePipelineVariant(pipelineId);
			if (!pipeline.IsValid())
				return;

			cl->SetPipeline(pipeline);
			if (pcw.IsValid())
				pcw.Flush(cl);
			cl->SetViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
			cl->SetScissor(0, 0, static_cast<int>(w), static_cast<int>(h));

			DescriptorSetDesc dsDesc{};
			dsDesc.layout = set0;
			dsDesc.transient = true;
			dsDesc.debugName = "HdrColorFx.DS0";
			const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
			if (!ds.IsValid())
				return;
			NamedDescriptorWriter wd(dev, ds, *meta, 0);
			wd.AddCombinedImageSampler("uSceneColor", srcTex, res.sampler);
			wd.AddStorageBuffer("uExposure", res.exposureBuffer, 0, kExposureStateBytes);
			wd.AddUniformBuffer("ColorGradeParams", res.gradeParamsBuffer, 0, gradeParamsBytes);
			wd.AddCombinedImageSampler("uLut3d", lut.Lut3D.IsValid() ? lut.Lut3D : res.identityLut.Lut3D, res.sampler);
			wd.AddCombinedImageSampler("uLut1d", lut.Lut1DStrip.IsValid() ? lut.Lut1DStrip : res.identityLut.Lut1DStrip,
									   res.sampler);
			wd.Flush();
			cl->BindDescriptorSet(0, ds, pipeline);
			cl->Draw(3, 1);
		});
}

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
