// The Cube LUT's two stages and its preview, measured on the real passes: the HDRColorFx pass
// (hdr_color_fx.frag) samples a Grade LUT before the look, and the preview's probe row reads, at
// each patch's center, exactly the probe's own value through the same passes.
//
// The instrument is the shipped shaders through AddHdrColorFxPassRG and AddTonemapPassRG into
// R32G32B32A32_FLOAT targets read back as floats, so the comparison is the shaders' arithmetic and
// nothing quantizes it.

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Passes/CubeLutPreviewSources.h"
#include "Rendering/Passes/HdrColorFxPass.h"
#include "Rendering/Passes/TonemapPass.h"
#include "Rendering/Utils/CubeLutFileParser.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Rendering/Utils/CubeLutGpuUpload.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;
namespace RG = GameEngine::Rendering::RenderGraph;

namespace
{

void PinEnvironment()
{
#if defined(_WIN32)
	_putenv_s("GE_HEADLESS_TEST", "1");
#else
	setenv("GE_HEADLESS_TEST", "1", 1);
#endif
#ifdef RENDERING_SHADER_OUTPUT_DIR
	Utils::SetShaderPathResolver(
		+[](const std::filesystem::path& rel) -> std::filesystem::path
		{ return std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR).parent_path() / rel; });
#endif
}

std::unique_ptr<IDevice> CreateDeviceOrNull()
{
	DeviceDesc dd{};
	dd.preferredAPI = GraphicsAPI::Vulkan;
	dd.enableDynamicRendering = true;
	auto device = DeviceFactory::CreateDevice(dd);
	if (!device || !device->Initialize(dd))
		return nullptr;
	// The Tonemap pass fills its ACES 2 tables through a device-local staging copy on first use,
	// which nothing orders before that pass's own submit (issue #4198): fill them and wait here,
	// so the first chain on this device does not read unfilled tables.
	if (!Passes::GetAces2TablesBuffer(device.get()).IsValid())
		return nullptr;
	device->WaitForIdle();
	return device;
}

struct FramePools
{
	RG::RGResourcePool Persistent;
	RG::RGTransientPool Transient;
	RG::RGUploadRing Ring;
	explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 64 * 1024) {}
};

using Texels = std::vector<float>; // RGBA, one row

// A one-row RGBA32F image, uploaded and rested at ShaderResource.
TextureHandle CreateRow(IDevice& device, const Texels& texels)
{
	const uint32_t width = static_cast<uint32_t>(texels.size() / 4);
	TextureDesc desc{};
	desc.width = width;
	desc.height = 1;
	desc.depth = 1;
	desc.mipLevels = 1;
	desc.arrayLayers = 1;
	desc.sampleCount = 1;
	desc.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
	desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
	desc.debugName = "CubeLutStage.Source";
	const TextureHandle tex = device.CreateTexture(desc);
	const size_t bytes = texels.size() * sizeof(float);
	const BufferHandle upload = device.CreateUploadBuffer(bytes, "CubeLutStage.Upload");
	if (!tex.IsValid() || !upload.IsValid())
		return {};
	device.UpdateBuffer(upload, 0, bytes, texels.data());
	auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
	cl->Begin();
	cl->CopyBufferToTextureSubresource(upload, tex, 0, 0, width, 1, 0, static_cast<size_t>(width) * 4 * sizeof(float));
	cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest, ResourceState::ShaderResource));
	cl->End();
	std::vector<CommandList*> lists{cl.get()};
	device.ExecuteCommandLists(lists);
	device.WaitForIdle();
	return tex;
}

TextureDesc FloatTarget(uint32_t width, const char* name, bool copySource)
{
	TextureDesc d{};
	d.width = width;
	d.height = 1;
	d.depth = 1;
	d.mipLevels = 1;
	d.arrayLayers = 1;
	d.sampleCount = 1;
	d.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
	d.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource) |
			  (copySource ? static_cast<uint32_t>(TextureUsage::TransferSrc) : 0u);
	d.debugName = name;
	return d;
}

// What a chain declares from the source into its float target; false = it declared nothing.
using Chain = std::function<bool(RG::RGFrame&, RG::RGTexture src, RG::RGTexture dst, const CubeLutGpuBindingState& lut,
								   int look)>;

// Runs one chain over a source row and returns the target's texels; empty when it declared nothing.
Texels RunChain(IDevice& device, TextureHandle source, uint32_t width, const CubeLutGpuBindingState& lut, int look,
				const Chain& chain)
{
	const size_t bytes = static_cast<size_t>(width) * 4 * sizeof(float);
	const BufferHandle readback = device.CreateReadbackBuffer(bytes, "CubeLutStage.Readback");
	if (!readback.IsValid())
		return {};
	struct DestroyOnExit
	{
		IDevice& Device;
		BufferHandle Buffer;
		~DestroyOnExit() { Device.DestroyBuffer(Buffer); }
	} destroyReadback{device, readback};
	FramePools pools(&device);
	RG::RGFrame frame(&device, &pools.Persistent, &pools.Transient, &pools.Ring);
	frame.BeginFrame(0);
	const RG::RGTexture src = frame.ImportExternalTexture("CubeLutStage.Source", source, ResourceState::ShaderResource,
														   TextureFormat::R32G32B32A32_FLOAT);
	const RG::RGTexture dst = frame.CreateTexture("CubeLutStage.Out", FloatTarget(width, "CubeLutStage.Out", true));
	if (!src.IsValid() || !dst.IsValid() || !chain(frame, src, dst, lut, look))
		return {};
	frame.AddPass(
		"CubeLutStage.Readback", PassPhase::kFinalize,
		[&](RG::RGPassBuilder& p)
		{
			p.Read(dst, RG::RGTextureRead::CopySrc);
			p.PreventCulling();
		},
		[dst, readback, width](RG::RGContext& ctx)
		{ ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(dst), 0, 0, readback, width, 1); });
	frame.Execute();
	device.WaitForIdle();
	const void* mapped = device.MapBuffer(readback);
	if (!mapped)
		return {};
	Texels out(static_cast<const float*>(mapped), static_cast<const float*>(mapped) + width * 4);
	device.UnmapBuffer(readback);
	return out;
}

// The look alone.
bool LookChain(RG::RGFrame& frame, RG::RGTexture src, RG::RGTexture dst, const CubeLutGpuBindingState&, int look)
{
	Passes::TonemapParams tonemap{};
	tonemap.look = look;
	return Passes::AddTonemapPassRG(frame, src, dst, tonemap, "CubeLutStage.Look").IsValid();
}

// The LUT at the Finish stage: inside the Tonemap pass, after the look.
bool FinishChain(RG::RGFrame& frame, RG::RGTexture src, RG::RGTexture dst, const CubeLutGpuBindingState& lut, int look)
{
	Passes::TonemapParams tonemap{};
	tonemap.look = look;
	tonemap.lut = lut;
	tonemap.lutIntensity = 1.0f;
	return Passes::AddTonemapPassRG(frame, src, dst, tonemap, "CubeLutStage.Finish").IsValid();
}

// The LUT at the Grade stage: the HDRColorFx pass, then the look.
bool GradeChain(RG::RGFrame& frame, RG::RGTexture src, RG::RGTexture dst, const CubeLutGpuBindingState& lut, int look)
{
	const uint32_t width = frame.Graph().ResourceDesc(dst.Id).Width;
	const RG::RGTexture graded = frame.CreateTexture("CubeLutStage.Graded", FloatTarget(width, "CubeLutStage.Graded", false));
	Passes::HdrColorFxParams grade{};
	grade.lut = lut;
	grade.lutIntensity = 1.0f;
	return Passes::AddHdrColorFxPassRG(frame, src, graded, grade, "CubeLutStage.Grade").IsValid() &&
		   LookChain(frame, graded, dst, lut, look);
}

// A 33^3 table mapping each channel x to map(x) (red fastest, as the format orders it). The knots
// are 1/32 apart, so the filter's fixed-point weights (8 fractional bits on common hardware) err by
// at most about 1/32/256 in a linear map's output; a 2-knot table would err by 1/256.
std::string ChannelMapLutText(float (*map)(float))
{
	constexpr uint32_t kSize = 33;
	std::string text = "LUT_3D_SIZE " + std::to_string(kSize) + "\n";
	char line[96];
	for (uint32_t b = 0; b < kSize; ++b)
		for (uint32_t g = 0; g < kSize; ++g)
			for (uint32_t r = 0; r < kSize; ++r)
			{
				const float step = 1.0f / static_cast<float>(kSize - 1);
				std::snprintf(line, sizeof(line), "%.7f %.7f %.7f\n", map(r * step), map(g * step), map(b * step));
				text += line;
			}
	return text;
}

std::string InvertingLutText()
{
	return ChannelMapLutText(+[](float x) { return 1.0f - x; });
}

// A 33^3 table inverting red and keeping green and blue: unlike a table that treats every channel
// alike, it does not commute with a hue rotation, which permutes the channels.
std::string RedInvertingLutText()
{
	constexpr uint32_t kSize = 33;
	std::string text = "LUT_3D_SIZE " + std::to_string(kSize) + "\n";
	char line[96];
	const float step = 1.0f / static_cast<float>(kSize - 1);
	for (uint32_t b = 0; b < kSize; ++b)
		for (uint32_t g = 0; g < kSize; ++g)
			for (uint32_t r = 0; r < kSize; ++r)
			{
				std::snprintf(line, sizeof(line), "%.7f %.7f %.7f\n", 1.0f - r * step, g * step, b * step);
				text += line;
			}
	return text;
}

struct LutFixture
{
	CubeLutGpuTextures Tables{};
	CubeLutGpuBindingState Binding{};
};

LutFixture UploadLut(IDevice& device, const std::string& text, int32_t inputEncoding = 0)
{
	LutFixture f{};
	CubeLutParseResult parsed{};
	if (!ParseCubeLutFromText(text, parsed) || !parsed.Ok ||
		!CreateCubeLutGpuTextures(&device, parsed, TextureFormat::R32G32B32A32_FLOAT, f.Tables))
		return f;
	const CubeLutGpuTextures identity = CreateIdentityCubeLutTextures(&device);
	f.Binding.Lut3D = f.Tables.Lut3D;
	f.Binding.Lut1DStrip = identity.Lut1DStrip;
	f.Binding.Size3D = static_cast<int32_t>(parsed.Size3D);
	f.Binding.Flags = 1; // a 3D table, no shaper
	f.Binding.In3DMin = parsed.In3DMin;
	f.Binding.In3DMax = parsed.In3DMax;
	f.Binding.InputEncoding = inputEncoding; // CubeLutInputEncoding; 0 = linear
	return f;
}

// The Grade pass alone: its output is scene-linear, with no look.
bool GradeOnlyChain(RG::RGFrame& frame, RG::RGTexture src, RG::RGTexture dst, const CubeLutGpuBindingState& lut, int)
{
	Passes::HdrColorFxParams grade{};
	grade.lut = lut;
	grade.lutIntensity = 1.0f;
	return Passes::AddHdrColorFxPassRG(frame, src, dst, grade, "CubeLutStage.GradeOnly").IsValid();
}

// One gray texel through the Grade pass with a LUT; the red channel of the result (gray in, gray out).
float GradeGray(IDevice& device, float gray, const LutFixture& lut)
{
	const TextureHandle single = CreateRow(device, Texels{gray, gray, gray, 1.0f});
	const Texels out = RunChain(device, single, 1, lut.Binding, 0, &GradeOnlyChain);
	device.DestroyTexture(single);
	return out.empty() ? -1.0f : out[0];
}

// hdr_color_fx.frag with a color filter and a hue shift as well as a Grade LUT: the frame's
// HDRColorFx node in small. AddHdrColorFxPassRG runs the grade's other stages at identity, where the
// LUT's place among them cannot show, so this declares the shader itself with the filter's push
// values and a third of a turn of hue rotation in the grade block.
struct FilteredGradeRun
{
	ShaderPackage Package{};
	SamplerHandle Sampler{};
	BufferHandle Exposure{};
	BufferHandle GradeParams{};
	uint32_t GradeParamsBytes = 0;
	uint32_t HueTurnsOffset = 0;
	GraphicsPipelineId Pipeline{};
	DescriptorSetLayoutDesc Set0{};
};

bool PrepareFilteredGrade(IDevice& device, FilteredGradeRun& run)
{
	std::string err;
	if (!LoadShaderPkg("Shaders/hdr_color_fx.shaderpkg", device.PreferredShaderSource(), run.Package, &err))
		return false;
	for (const DescriptorSetMeta& set : run.Package.meta.Sets)
		for (const DescriptorBindingMeta& binding : set.Bindings)
			if (binding.Name == "ColorGradeParams" && binding.Block)
			{
				run.GradeParamsBytes = binding.Block->Size;
				for (const auto& member : binding.Block->Members)
					if (member.Name == "uWhiteBalance")
						run.HueTurnsOffset = member.Offset + 3u * sizeof(float); // .w: hue rotation in turns
			}
	GraphicsPipelineDesc gd{};
	gd.Kind = GraphicsPipelineKind::VertexFragment;
	gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(run.Package.stageBytes.at("vs"));
	gd.PixelShader = std::make_shared<const std::vector<uint8_t>>(run.Package.stageBytes.at("fs"));
	gd.Topology = PrimitiveTopology::TriangleList;
	gd.Rasterization.cullMode = CullModeFlagBits::None;
	gd.DepthStencil.depthTestEnable = false;
	gd.DepthStencil.depthWriteEnable = false;
	DynamicStateInfo dyn{};
	dyn.states = {DynamicState::Viewport, DynamicState::Scissor};
	gd.DynamicState = std::move(dyn);
	if (!MaterialHelper::ApplyShaderMetaToGraphicsDesc(
			device, run.Package.meta, gd, MaterialBuilder::MergeMode::Auto, {true, 128},
			[&](uint32_t setIndex, DescriptorSetLayoutDesc& dsl)
			{
				if (setIndex == 0)
					run.Set0 = dsl;
			},
			&err))
		return false;
	run.Pipeline = device.InternGraphicsPipeline(std::move(gd));
	SamplerDesc linearClamp{};
	linearClamp.minFilter = linearClamp.magFilter = linearClamp.mipFilter = 1;
	linearClamp.addressModeU = linearClamp.addressModeV = linearClamp.addressModeW = 2; // clamp
	linearClamp.debugName = "CubeLutStage.FilteredGrade";
	run.Sampler = device.CreateSampler(linearClamp);
	const std::vector<uint8_t> zeros(std::max<uint32_t>(run.GradeParamsBytes, 16u), 0);
	BufferDesc bd{};
	bd.memoryUsage = BufferMemoryUsage::Upload; // host-visible like the pass's own: filled before any submit reads it
	bd.size = 16;
	bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
	run.Exposure = CreateBufferInitialized(&device, bd, zeros.data(), 16);
	bd.size = run.GradeParamsBytes;
	bd.usage = static_cast<uint32_t>(BufferUsage::Uniform);
	std::vector<uint8_t> grade(run.GradeParamsBytes, 0);
	constexpr float kHueTurns = 1.0f / 3.0f;
	if (run.HueTurnsOffset == 0 || run.HueTurnsOffset + sizeof(float) > grade.size())
		return false;
	std::memcpy(grade.data() + run.HueTurnsOffset, &kHueTurns, sizeof(float));
	run.GradeParams = CreateBufferInitialized(&device, bd, grade.data(), run.GradeParamsBytes);
	return run.Pipeline.IsValid() && run.Sampler.IsValid() && run.Exposure.IsValid() && run.GradeParams.IsValid();
}

// A multiply color filter of tint (0.5, 1, 1) and a Grade LUT, on one source row.
bool DeclareFilteredGrade(FilteredGradeRun& run, RG::RGFrame& frame, RG::RGTexture src, RG::RGTexture dst,
						  const CubeLutGpuBindingState& lut)
{
	NamedPushConstantWriter pcw(run.Package.meta, run.Package.meta.PushConstants[0].Name);
	pcw.Add("colorFilterR", 0.5f);
	pcw.Add("colorFilterG", 1.0f);
	pcw.Add("colorFilterB", 1.0f);
	pcw.Add("colorFilterIntensity", 1.0f);
	pcw.Add("colorFilterBlendMode", int32_t{0});
	pcw.Add("exposure", 1.0f);
	pcw.Add("lutIntensity", 1.0f);
	pcw.Add("lutStage", int32_t{1});
	pcw.Add("lutInputEncoding", lut.InputEncoding);
	pcw.Add("lutSize3d", lut.Size3D);
	pcw.Add("lutSize1d", lut.Size1D);
	pcw.Add("lutFlags", lut.Flags);
	pcw.Add("lut1dInMin", lut.In1DMin);
	pcw.Add("lut1dInMax", lut.In1DMax);
	pcw.Add("lut3dInMin", lut.In3DMin);
	pcw.Add("lut3dInMax", lut.In3DMax);
	const uint32_t width = frame.Graph().ResourceDesc(dst.Id).Width;
	frame.AddPass(
		"CubeLutStage.FilteredGrade", PassPhase::kFinalize,
		[&](RG::RGPassBuilder& p)
		{
			p.Read(src, RG::RGTextureRead::Sampled);
			RG::RGAttachmentOps ops{};
			ops.Load = RG::RGLoadOp::DontCare;
			ops.Store = RG::RGStoreOp::Store;
			p.AttachColor(0, dst, ops);
		},
		[&run, src, pcw, width, lut](RG::RGContext& ctx)
		{
			const PipelineHandle pipeline = ctx.GetOrCreatePipelineVariant(run.Pipeline);
			ctx.Cmd->SetPipeline(pipeline);
			pcw.Flush(ctx.Cmd);
			ctx.Cmd->SetViewport(0.0f, 0.0f, static_cast<float>(width), 1.0f);
			ctx.Cmd->SetScissor(0, 0, static_cast<int>(width), 1);
			DescriptorSetDesc dsDesc{};
			dsDesc.layout = run.Set0;
			dsDesc.transient = true;
			const DescriptorSetHandle ds = ctx.GetDevice()->CreateDescriptorSet(dsDesc);
			NamedDescriptorWriter wd(ctx.GetDevice(), ds, run.Package.meta, 0);
			wd.AddCombinedImageSampler("uSceneColor", ctx.GetTexture(src), run.Sampler);
			wd.AddStorageBuffer("uExposure", run.Exposure, 0, 16);
			wd.AddUniformBuffer("ColorGradeParams", run.GradeParams, 0, run.GradeParamsBytes);
			wd.AddCombinedImageSampler("uLut3d", lut.Lut3D, run.Sampler);
			wd.AddCombinedImageSampler("uLut1d", lut.Lut1DStrip, run.Sampler);
			wd.Flush();
			ctx.Cmd->BindDescriptorSet(0, ds, pipeline);
			ctx.Cmd->Draw(3, 1);
		});
	return true;
}

constexpr std::array<int, 3> kLooks{0, 1, 2}; // Neutral, Filmic, Photographic
constexpr const char* kLookNames[] = {"Neutral", "Filmic", "Photographic"};

float MaxAbsDifference(const Texels& a, const Texels& b)
{
	float worst = 0.0f;
	for (size_t i = 0; i < a.size(); ++i)
		if (i % 4 != 3)
			worst = std::max(worst, std::abs(a[i] - b[i]));
	return worst;
}

} // namespace

// A Grade LUT runs before the look: a gray ramp through an inverting LUT at the Grade stage equals
// the look of the inverted ramp. The same LUT at Finish inverts the look's output instead, which
// differs, so the comparison can tell the two stages apart.
//
// REDDENS IF: hdr_color_fx.frag does not sample a Grade LUT (the output is the look of the ramp
// itself), or samples it after un-anchoring the exposure, or the Tonemap pass samples it too.
TEST(CubeLutStage, GradeSamplesTheLutBeforeTheLook)
{
	PinEnvironment();
	auto device = CreateDeviceOrNull();
	if (!device)
		GTEST_SKIP() << "Vulkan device unavailable on this machine";

	constexpr uint32_t kWidth = 64;
	Texels ramp(kWidth * 4), inverted(kWidth * 4);
	for (uint32_t x = 0; x < kWidth; ++x)
	{
		const float v = 0.02f + 0.96f * static_cast<float>(x) / static_cast<float>(kWidth - 1);
		for (int c = 0; c < 3; ++c)
		{
			ramp[x * 4 + c] = v;
			inverted[x * 4 + c] = 1.0f - v;
		}
		ramp[x * 4 + 3] = inverted[x * 4 + 3] = 1.0f;
	}
	const TextureHandle rampTex = CreateRow(*device, ramp);
	const TextureHandle invertedTex = CreateRow(*device, inverted);
	const LutFixture lut = UploadLut(*device, InvertingLutText());
	ASSERT_TRUE(rampTex.IsValid() && invertedTex.IsValid() && lut.Binding.Lut3D.IsValid());

	for (const int look : kLooks)
	{
		SCOPED_TRACE(kLookNames[look]);
		const Texels grade = RunChain(*device, rampTex, kWidth, lut.Binding, look, &GradeChain);
		const Texels reference = RunChain(*device, invertedTex, kWidth, lut.Binding, look, &LookChain);
		const Texels finish = RunChain(*device, rampTex, kWidth, lut.Binding, look, &FinishChain);
		ASSERT_FALSE(grade.empty()) << "the Grade chain declared nothing: hdr_color_fx.shaderpkg or "
									   "tonemap.shaderpkg not staged under the build root's Shaders/";
		ASSERT_FALSE(reference.empty());
		ASSERT_FALSE(finish.empty());
		EXPECT_LT(MaxAbsDifference(grade, reference), 2e-3f) << "Grade must equal the look of the inverted ramp";
		EXPECT_GT(MaxAbsDifference(grade, finish), 0.05f)
			<< "the same LUT at Finish must differ, or the test cannot tell the stages apart";
	}
}

// A Grade LUT is the grade's last stage: after the color filter, the corrector and the hue shift.
// Gray 0.4 through a multiply filter of (0.5, 1, 1) is (0.2, 0.4, 0.4); a third of a turn of hue
// rotation moves each channel to the next, (0.4, 0.2, 0.4); a LUT inverting red then gives
// (0.6, 0.2, 0.4). The LUT before the filter gives (0.4, 0.3, 0.4), between the filter and the hue
// shift (0.4, 0.8, 0.4).
//
// REDDENS IF: hdr_color_fx.frag samples the Grade LUT before any of the grade's other stages.
TEST(CubeLutStage, GradeLutRunsAfterTheGradesOtherStages)
{
	PinEnvironment();
	auto device = CreateDeviceOrNull();
	if (!device)
		GTEST_SKIP() << "Vulkan device unavailable on this machine";

	FilteredGradeRun run{};
	ASSERT_TRUE(PrepareFilteredGrade(*device, run)) << "hdr_color_fx.shaderpkg not staged under the build root's Shaders/";
	const LutFixture lut = UploadLut(*device, RedInvertingLutText());
	const TextureHandle source = CreateRow(*device, Texels{0.4f, 0.4f, 0.4f, 1.0f});
	ASSERT_TRUE(lut.Binding.Lut3D.IsValid() && source.IsValid());
	const Texels out = RunChain(*device, source, 1, lut.Binding, 0,
								[&run](RG::RGFrame& frame, RG::RGTexture src, RG::RGTexture dst,
									   const CubeLutGpuBindingState& binding, int)
								{ return DeclareFilteredGrade(run, frame, src, dst, binding); });
	ASSERT_EQ(out.size(), 4u);
	EXPECT_NEAR(out[0], 0.6f, 2e-3f) << "red: the LUT inverts the filtered, rotated value";
	EXPECT_NEAR(out[1], 0.2f, 2e-3f) << "green: the filtered red, rotated";
	EXPECT_NEAR(out[2], 0.4f, 2e-3f) << "blue: the rotated green";
	device->DestroyTexture(source);
	device->DestroyBuffer(run.Exposure);
	device->DestroyBuffer(run.GradeParams);
	device->DestroySampler(run.Sampler);
}

// The DaVinci Intermediate encoding against Blackmagic's published curve: a log2 body
// (log2(x + A) + B) * C above the linear cut and x * M below it, A = 0.0075, B = 7, C = 0.07329248,
// M = 10.44426855, the cut at 0.00262409 linear and 0.02740668 encoded. A table of one constant
// value reads the decode; a table that adds C to its input doubles x + A through the encode and the
// decode, which only the right slope does (issue #4182: the slope was 0.1).
//
// REDDENS IF: either side of the DaVinci Intermediate curve departs from the published constants.
TEST(CubeLutStage, DaVinciIntermediateFollowsThePublishedCurve)
{
	PinEnvironment();
	auto device = CreateDeviceOrNull();
	if (!device)
		GTEST_SKIP() << "Vulkan device unavailable on this machine";

	constexpr int32_t kDaVinciIntermediate = 3;
	constexpr float kA = 0.0075f, kB = 7.0f, kC = 0.07329248f;
	const float midGrayEncoded = (std::log2(0.18f + kA) + kB) * kC; // 0.336
	const LutFixture midGray = UploadLut(
		*device, ChannelMapLutText(+[](float) { return (std::log2(0.18f + 0.0075f) + 7.0f) * 0.07329248f; }),
		kDaVinciIntermediate);
	const LutFixture logCut = UploadLut(*device, ChannelMapLutText(+[](float) { return 0.02740668f; }), kDaVinciIntermediate);
	const LutFixture addC = UploadLut(*device, ChannelMapLutText(+[](float u) { return u + 0.07329248f; }), kDaVinciIntermediate);
	ASSERT_TRUE(midGray.Binding.Lut3D.IsValid() && logCut.Binding.Lut3D.IsValid() && addC.Binding.Lut3D.IsValid());
	ASSERT_NEAR(midGrayEncoded, 0.3360f, 5e-4f);

	EXPECT_NEAR(GradeGray(*device, 0.5f, midGray), 0.18f, 1e-4f) << "mid-gray decodes from its published code";
	EXPECT_NEAR(GradeGray(*device, 0.5f, logCut), 0.00262409f, 1e-5f) << "the encoded cut decodes to the linear cut";
	EXPECT_NEAR(GradeGray(*device, 0.18f, addC), 2.0f * (0.18f + kA) - kA, 2e-3f)
		<< "adding C in the encoded domain doubles x + A only on the published slope";
}

// The probe row's patches read their probes: at each patch's center, the probe source through the
// look and a LUT at Finish equals that probe's value alone through the same pass.
//
// REDDENS IF: a probe's value, its patch's place in the source, or the source's layout changes
// without the other.
TEST(CubeLutStage, ProbePatchCentersEqualTheirProbeThroughTheSamePass)
{
	PinEnvironment();
	auto device = CreateDeviceOrNull();
	if (!device)
		GTEST_SKIP() << "Vulkan device unavailable on this machine";

	using Passes::CubeLutPreviewSource;
	const Texels probes = Passes::MakeCubeLutPreviewSourceTexels(CubeLutPreviewSource::Probes);
	const uint32_t width = Passes::CubeLutPreviewSourceWidth(CubeLutPreviewSource::Probes);
	ASSERT_EQ(probes.size(), static_cast<size_t>(width) * 4u);
	const TextureHandle probeTex = CreateRow(*device, probes);
	const LutFixture lut = UploadLut(*device, InvertingLutText());
	ASSERT_TRUE(probeTex.IsValid() && lut.Binding.Lut3D.IsValid());

	constexpr int kFilmic = 1;
	const Texels row = RunChain(*device, probeTex, width, lut.Binding, kFilmic, &FinishChain);
	ASSERT_FALSE(row.empty()) << "tonemap.shaderpkg not staged under the build root's Shaders/";
	for (size_t probe = 0; probe < Passes::kCubeLutPreviewProbes.size(); ++probe)
	{
		for (size_t stop = 0; stop < Passes::kCubeLutPreviewProbeStops.size(); ++stop)
		{
			SCOPED_TRACE(std::string(Passes::kCubeLutPreviewProbes[probe].Name) + " at " +
						 std::to_string(Passes::kCubeLutPreviewProbeStops[stop]) + " stops");
			const std::array<float, 3> value = Passes::CubeLutPreviewProbeValue(probe, stop);
			const TextureHandle single = CreateRow(*device, Texels{value[0], value[1], value[2], 1.0f});
			ASSERT_TRUE(single.IsValid());
			const Texels direct = RunChain(*device, single, 1, lut.Binding, kFilmic, &FinishChain);
			ASSERT_EQ(direct.size(), 4u);
			const size_t x = Passes::CubeLutPreviewProbePatchCenter(probe, stop);
			for (int c = 0; c < 3; ++c)
				EXPECT_NEAR(row[x * 4 + c], direct[c], 1e-5f) << "channel " << c;
			device->DestroyTexture(single);
		}
	}
}

// The probe row: three saturated hues and a skin tone, each at -2, 0 and +1 stops. A colorist checks a
// look LUT on skin first, and +1 stop keeps the brightest patch below the looks' clip, so its column
// shows what the LUT does to a highlight.
TEST(CubeLutStage, TheProbesAreThreeHuesAndASkinToneAtMinusTwoZeroAndPlusOne)
{
	std::vector<std::string> names;
	for (const Passes::CubeLutPreviewProbe& probe : Passes::kCubeLutPreviewProbes)
		names.emplace_back(probe.Name);
	EXPECT_EQ(names, (std::vector<std::string>{"ember", "sky", "skin", "foliage"}));
	EXPECT_EQ(Passes::kCubeLutPreviewProbeStops, (std::array<float, 3>{-2.0f, 0.0f, 1.0f}));

	// Each patch sits at its stop's luminance: 0.18 times 2^stop.
	for (size_t probe = 0; probe < Passes::kCubeLutPreviewProbes.size(); ++probe)
		for (size_t stop = 0; stop < Passes::kCubeLutPreviewProbeStops.size(); ++stop)
		{
			const std::array<float, 3> v = Passes::CubeLutPreviewProbeValue(probe, stop);
			const float luminance = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
			EXPECT_NEAR(luminance, 0.18f * std::exp2(Passes::kCubeLutPreviewProbeStops[stop]), 1e-5f)
				<< Passes::kCubeLutPreviewProbes[probe].Name << " at stop " << stop;
		}
}
