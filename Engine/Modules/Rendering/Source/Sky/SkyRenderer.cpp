#include "Rendering/Sky/SkyRenderer.h"

#include "StarCatalog.h"

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderLayoutShape.h"

#include "Image/Decode.h"
#include "Logger/Logger.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

namespace
{
// Must match SkyRenderNode / sky_render.frag: skyboxParams.y < 0 selects procedural sky LUT sampling.
constexpr float kSkyHdriProceduralSentinel = -1.0f;

// Reads a whole file in one sized read. False when it cannot be opened or read.
bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& outBytes)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const std::streamoff size = file.tellg();
    if (size < 0)
        return false;
    outBytes.resize(static_cast<size_t>(size));
    file.seekg(0);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(outBytes.data()), static_cast<std::streamsize>(size)));
}

TextureHandle LoadSkyTexture2D(IDevice* device, const std::filesystem::path& path, TextureFormat format, const char* debugName)
{
    if (!device)
        return {};

    std::vector<uint8_t> fileBytes;
    if (!ReadWholeFile(path, fileBytes))
        return {};
    const Image::Image8 decoded = Image::Decode8(fileBytes, path.string(), Image::DecodeChannels::Rgba);
    if (!decoded.Valid)
        return {};

    TextureDesc td{};
    td.width = decoded.Width;
    td.height = decoded.Height;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(format);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
             | static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = debugName;
    TextureHandle texture = device->CreateTexture(td);
    if (texture.IsValid())
        UploadTexture2D(device, texture, decoded.Pixels.data(), td.width, td.height, td.width * 4u, debugName);
    return texture;
}

// Resolve a set0 binding index from reflected shader meta by name. Only the
// index is trusted from reflection (the reflected type enum is unreliable for
// images); a miss falls back to the GLSL literal with a warning so the C++
// binds keep working while the divergence stays diagnosable.
uint32_t ResolveSet0Binding(const ShaderMeta& meta, const char* name, uint32_t fallback,
                            const char* pkgName)
{
    for (const auto& s : meta.Sets)
    {
        if (s.Set != 0)
            continue;
        for (const auto& b : s.Bindings)
            if (b.Name == name)
                return b.Binding;
        break;
    }
    Logger::Log::Warning("SkyRenderer: failed to resolve set0 binding '{}' in {} from reflection; "
                         "falling back to literal {}",
                         name, pkgName, fallback);
    return fallback;
}

// The texel format a kernel declares for a set 0 storage image (its GLSL layout qualifier, from the
// package's reflection), or TextureFormat{} when the image is absent or declares none.
TextureFormat DeclaredStorageFormat(const ShaderMeta& meta, const char* name)
{
    for (const auto& s : meta.Sets)
    {
        if (s.Set != 0)
            continue;
        for (const auto& b : s.Bindings)
            if (b.Name == name && b.Image)
                return static_cast<TextureFormat>(b.Image->Format);
    }
    return TextureFormat{};
}

TextureHandle CreateSolidSkyTexture2D(IDevice* device, uint32_t rgba, TextureFormat format, const char* debugName)
{
    if (!device)
        return {};

    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(format);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
             | static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = debugName;

    TextureHandle texture = device->CreateTexture(td);
    if (texture.IsValid())
        UploadTexture2D(device, texture, &rgba, td.width, td.height, sizeof(rgba), debugName);
    return texture;
}
} // namespace

SkyRenderer::SkyRenderer() = default;

SkyRenderer::~SkyRenderer()
{
    DestroyResources();
}

void SkyRenderer::DestroyResources()
{
    if (!m_Device)
        return;

    if (m_TransmittanceLutTex.IsValid())
        m_Device->DestroyTexture(m_TransmittanceLutTex);
    if (m_SkyViewLutTex.IsValid())
        m_Device->DestroyTexture(m_SkyViewLutTex);
    if (m_MultiscatterLutTex.IsValid())
        m_Device->DestroyTexture(m_MultiscatterLutTex);
    if (m_LinearSampler.IsValid())
        m_Device->DestroySampler(m_LinearSampler);
    if (m_SkyViewSampler.IsValid())
        m_Device->DestroySampler(m_SkyViewSampler);
    if (m_StarSSBO.IsValid())
        m_Device->DestroyBuffer(m_StarSSBO);
    if (m_MoonFullTexture.IsValid())
        m_Device->DestroyTexture(m_MoonFullTexture);

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
    {
        if (m_SkyViewLutSkyUBO[i].IsValid())   m_Device->DestroyBuffer(m_SkyViewLutSkyUBO[i]);
        if (m_SkyViewLutAtmoUBO[i].IsValid())   m_Device->DestroyBuffer(m_SkyViewLutAtmoUBO[i]);
        if (m_MultiscatterAtmoUBO[i].IsValid()) m_Device->DestroyBuffer(m_MultiscatterAtmoUBO[i]);
        if (m_SkyRenderUBO[i].IsValid())         m_Device->DestroyBuffer(m_SkyRenderUBO[i]);
        if (m_TransmittanceAtmoUBO[i].IsValid()) m_Device->DestroyBuffer(m_TransmittanceAtmoUBO[i]);
        if (m_RenderAtmoUBO[i].IsValid())        m_Device->DestroyBuffer(m_RenderAtmoUBO[i]);
    }

    m_TransmittanceLutTex = {};
    m_SkyViewLutTex = {};
    m_MultiscatterLutTex = {};
    m_StarSSBO = {};
    m_MoonFullTexture = {};
    m_StarCount = 0;
    m_LinearSampler = {};
    m_SkyViewSampler = {};
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
    {
        m_SkyViewLutSkyUBO[i] = {};
        m_SkyViewLutAtmoUBO[i] = {};
        m_MultiscatterAtmoUBO[i] = {};
        m_SkyRenderUBO[i] = {};
        m_TransmittanceAtmoUBO[i] = {};
        m_RenderAtmoUBO[i] = {};
    }
}

void SkyRenderer::ReprovisionAfterDeviceRebuild()
{
    if (!m_Device)
        return;
    IDevice* device = m_Device;
    // Moon art is lazily loaded (EnsureMoonFullTexture guards on IsValid); zero the
    // dead handle so it reloads.
    m_MoonFullTexture = {};
    // Initialize resets m_TransmittanceComputed but not the other two one-shot LUT
    // flags — reset them so all three LUTs recompute into the fresh textures.
    m_SkyViewLutComputedOnce   = false;
    m_MultiscatterComputedOnce = false;
    // Recreates every LUT/sampler/UBO/star handle over the dead ones (unconditional
    // overwrite, no destroy -> no double-free) and re-interns the pipelines.
    Initialize(device, m_Config);
}

TextureHandle SkyRenderer::EnsureMoonFullTexture()
{
    if (m_MoonFullTexture.IsValid() || !m_Device)
        return m_MoonFullTexture;

    // With no root there is nothing to resolve against. Deliberately NOT falling back to
    // the bare relative path: that resolves against the process working directory, which is
    // the defect this replaced — it finds the repo on a developer's machine and the wrong
    // file (or nothing) anywhere else.
    // make_preferred so the path a human reads in the warning below is spelled in one
    // separator, not the '\' of the root joined to the '/' of the relative constant.
    std::filesystem::path path;
    if (!m_Config.installAssetsRoot.empty())
        path = (m_Config.installAssetsRoot / kSkyMoonFullTextureRelativePath).make_preferred();

    if (!path.empty())
        m_MoonFullTexture = LoadSkyTexture2D(m_Device, path, TextureFormat::RGBA8_SRGB, "SkyMoonFull");

    if (!m_MoonFullTexture.IsValid())
    {
        // The substitute keeps the sky render descriptor complete, and a transparent moon is
        // indistinguishable from a working night sky — so say what is missing, and name the
        // root too, so an unset root reads differently from a missing file. Runs once per
        // device: the fallback handle is itself valid, so the guard above stops the repeat.
        Logger::Log::Warning("SkyRenderer: moon texture not loaded (resolved '{}', install "
                             "assets root '{}') — the moon will not render. The build stages "
                             "it next to the executable; check the staged Assets tree.",
                             path.string(), m_Config.installAssetsRoot.string());
        m_MoonFullTexture = CreateSolidSkyTexture2D(m_Device, 0x00000000u, TextureFormat::RGBA8_SRGB, "SkyMoonFallbackTransparent");
    }
    return m_MoonFullTexture;
}

TextureFormat SkyRenderer::SkyViewLutFormat(ShaderSourceKind kernels)
{
    return kernels == ShaderSourceKind::Wgsl ? TextureFormat::R16G16B16A16_FLOAT
                                             : TextureFormat::R32G32B32A32_FLOAT;
}

bool SkyRenderer::Initialize(IDevice* device, const Config& config)
{
    m_Device = device;
    m_Config = config;
    m_TransmittanceComputed = false;

    // Every reader filters the sky-view LUT linearly, so an RGBA32F LUT needs a device that filters
    // 32-bit floats. Vulkan and Metal always report that they do (the capability defaults to true);
    // only WebGPU reports the optional float32-filterable feature, and a WebGPU device that loads
    // SPIR-V kernels without it would sample the LUT as black. Refuse that device here, by name.
    const TextureFormat skyViewFormat = SkyViewLutFormat(m_Device->PreferredShaderSource());
    if (skyViewFormat == TextureFormat::R32G32B32A32_FLOAT && !m_Device->GetCapabilities().supportsFilterableFloat32)
    {
        Logger::Log::Error("SkyRenderer: this device loads SPIR-V kernels but cannot filter 32-bit float textures "
                           "(WebGPU feature 'float32-filterable' is missing), and the sky-view LUT is RGBA32F. The "
                           "physical sky is not supported on this device: use an adapter that offers "
                           "float32-filterable, or a backend that loads WGSL kernels.");
        return false;
    }

    {
        TextureDesc d{};
        d.width = m_Config.transmittanceLutWidth;
        d.height = m_Config.transmittanceLutHeight;
        d.format = (uint32_t)TextureFormat::R16G16B16A16_FLOAT;
        d.usage = (uint32_t)(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
        d.debugName = "Sky_Transmittance_LUT";
        m_TransmittanceLutTex = m_Device->CreateTexture(d);
    }
    {
        TextureDesc d{};
        d.width = m_Config.skyViewLutWidth;
        d.height = m_Config.skyViewLutHeight;
        d.format = static_cast<uint32_t>(skyViewFormat);
        d.usage = (uint32_t)(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
        d.debugName = "Sky_View_LUT";
        m_SkyViewLutTex = m_Device->CreateTexture(d);
    }
    {
        TextureDesc d{};
        d.width = m_Config.multiscatterLutSize;
        d.height = m_Config.multiscatterLutSize;
        d.format = (uint32_t)TextureFormat::R16G16B16A16_FLOAT;
        d.usage = (uint32_t)(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
        d.debugName = "Sky_Multiscatter_LUT";
        m_MultiscatterLutTex = m_Device->CreateTexture(d);
    }

    {
        // Fully-clamped sampler for the transmittance / multiscatter / moon reads,
        // whose UVs are parametric (no wrap at the edges).
        SamplerDesc sd{};
        sd.minFilter = 1;    // Linear
        sd.magFilter = 1;    // Linear
        sd.addressModeU = 2; // ClampToEdge
        sd.addressModeV = 2; // ClampToEdge
        m_LinearSampler = m_Device->CreateSampler(sd);
    }
    {
        // Sky-view LUT reads only: u is the full-circle azimuth, so it must Repeat
        // to wrap seamlessly across u=0/1; v is the elevation, clamped at the poles.
        SamplerDesc sd{};
        sd.minFilter = 1;    // Linear
        sd.magFilter = 1;    // Linear
        sd.addressModeU = 0; // Repeat (lat-long azimuth wrap)
        sd.addressModeV = 2; // ClampToEdge (elevation poles)
        m_SkyViewSampler = m_Device->CreateSampler(sd);
    }

    m_FramesInFlight = m_Device->GetFramesInFlight();
    if (m_FramesInFlight > kMaxFramesInFlight)
        m_FramesInFlight = kMaxFramesInFlight;

    for (uint32_t i = 0; i < m_FramesInFlight; ++i)
    {
        auto makeUBO = [&](size_t size, const char* name) -> BufferHandle {
            BufferDesc bd{};
            bd.size = size;
            bd.usage = static_cast<uint32_t>(BufferUsage::Uniform);
            bd.memoryUsage = BufferMemoryUsage::Upload;
            // FrameSlotted: one element of a ring exactly as deep as the device paces.
            // SkyRenderNode fills it from its pass execute lambdas, which run behind
            // BeginFrame; a fill from anywhere earlier would race a frame in flight.
            bd.flags = BufferCreateFlags::PersistentlyMapped | BufferCreateFlags::FrameSlotted;
            bd.debugName = name;
            return m_Device->CreateBuffer(bd);
        };

        std::string suffix = ".Frame" + std::to_string(i);
        m_SkyViewLutSkyUBO[i]   = makeUBO(sizeof(SkyUBOGPU), ("Sky_LUT_SkyUBO" + suffix).c_str());
        m_SkyViewLutAtmoUBO[i]  = makeUBO(sizeof(AtmosphereParametersGPU), ("Sky_LUT_AtmoUBO" + suffix).c_str());
        m_MultiscatterAtmoUBO[i] = makeUBO(sizeof(AtmosphereParametersGPU), ("Sky_MS_AtmoUBO" + suffix).c_str());
        m_SkyRenderUBO[i]       = makeUBO(sizeof(SkyUBOGPU), ("Sky_Render_SkyUBO" + suffix).c_str());
        m_TransmittanceAtmoUBO[i] = makeUBO(sizeof(AtmosphereParametersGPU), ("Sky_Trans_AtmoUBO" + suffix).c_str());
        m_RenderAtmoUBO[i]      = makeUBO(sizeof(AtmosphereParametersGPU), ("Sky_Render_AtmoUBO" + suffix).c_str());
    }

    GenerateStarCatalog();

    if (!CreatePipelines())
    {
        DestroyResources();
        return false;
    }

    return true;
}

bool SkyRenderer::CreatePipelines()
{
    if (!m_Device)
        return false;
    const ShaderSourceKind sourceKind = m_Device->PreferredShaderSource();

    // Transmittance Compute
    {
        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/sky_transmittance_lut.shaderpkg", sourceKind, pkg, &err))
            return false;
        auto itCs = pkg.stageBytes.find("cs");
        if (itCs == pkg.stageBytes.end() || itCs->second.empty())
            return false;

        m_Bindings.Transmittance.TransLUT =
            ResolveSet0Binding(pkg.meta, "uTransLUT", 0, "sky_transmittance_lut");
        m_Bindings.Transmittance.Atmos =
            ResolveSet0Binding(pkg.meta, "uAtmos", 1, "sky_transmittance_lut");

        DescriptorSetLayoutDesc sl{};
        sl.bindings = {
            {m_Bindings.Transmittance.TransLUT, DescriptorType::StorageImage, 1, 0x20},
            {m_Bindings.Transmittance.Atmos, DescriptorType::UniformBuffer, 1, 0x20}
        // The LUT is rgba16f and the two LUT samplers filter rgba16f from
        // compute; WebGPU bakes both facts into the bind group layout and
        // rejects the pipeline if the defaults (RGBA8, unfilterable) stand.
        };
        for (auto& b : sl.bindings)
        {
            if (b.type == DescriptorType::StorageImage)
                b.storageTexelFormat = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
            if (b.type == DescriptorType::CombinedImageSampler)
                b.imageFilterableFloat = true;
        }
        m_TransmittancePipe.type = PipelineType::Compute;
        m_TransmittancePipe.computeShader = itCs->second;
        m_TransmittancePipe.descriptorSetLayouts.push_back(sl);
        m_TransmittancePipe.debugName = "Sky_Transmittance_Pipe";
    }

    // Multiscatter LUT Compute
    {
        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/sky_multiscatter_lut.shaderpkg", sourceKind, pkg, &err))
            return false;
        auto itCs = pkg.stageBytes.find("cs");
        if (itCs == pkg.stageBytes.end() || itCs->second.empty())
            return false;

        m_Bindings.Multiscatter.MultiscatterLUT =
            ResolveSet0Binding(pkg.meta, "oMultiscatterLUT", 0, "sky_multiscatter_lut");
        m_Bindings.Multiscatter.TransmittanceLUT =
            ResolveSet0Binding(pkg.meta, "uTransmittanceLUT", 1, "sky_multiscatter_lut");
        m_Bindings.Multiscatter.Atmos =
            ResolveSet0Binding(pkg.meta, "uAtmos", 2, "sky_multiscatter_lut");

        DescriptorSetLayoutDesc sl{};
        sl.bindings = {
            {m_Bindings.Multiscatter.MultiscatterLUT, DescriptorType::StorageImage, 1, 0x20},
            {m_Bindings.Multiscatter.TransmittanceLUT, DescriptorType::CombinedImageSampler, 1, 0x20},
            {m_Bindings.Multiscatter.Atmos, DescriptorType::UniformBuffer, 1, 0x20}
        };
        for (auto& b : sl.bindings)
        {
            if (b.type == DescriptorType::StorageImage)
                b.storageTexelFormat = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
            if (b.type == DescriptorType::CombinedImageSampler)
                b.imageFilterableFloat = true;
        }
        m_MultiscatterPipe.type = PipelineType::Compute;
        m_MultiscatterPipe.computeShader = itCs->second;
        m_MultiscatterPipe.descriptorSetLayouts.push_back(sl);
        m_MultiscatterPipe.debugName = "Sky_Multiscatter_Pipe";
    }

    // Sky View LUT Compute
    {
        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/sky_view_lut.shaderpkg", sourceKind, pkg, &err))
            return false;
        auto itCs = pkg.stageBytes.find("cs");
        if (itCs == pkg.stageBytes.end() || itCs->second.empty())
            return false;

        m_Bindings.SkyViewLut.SkyViewLUT =
            ResolveSet0Binding(pkg.meta, "oSkyViewLUT", 0, "sky_view_lut");
        m_Bindings.SkyViewLut.TransmittanceLUT =
            ResolveSet0Binding(pkg.meta, "uTransmittanceLUT", 1, "sky_view_lut");
        m_Bindings.SkyViewLut.Atmos = ResolveSet0Binding(pkg.meta, "uAtmos", 2, "sky_view_lut");
        m_Bindings.SkyViewLut.Sky = ResolveSet0Binding(pkg.meta, "uSky", 3, "sky_view_lut");
        m_Bindings.SkyViewLut.MultiscatterLUT =
            ResolveSet0Binding(pkg.meta, "uMultiscatterLUT", 4, "sky_view_lut");

        // The kernel's storage qualifier and the LUT's texture format change together: a kernel that
        // writes rgba16f into an RGBA32F image (or the reverse) is refused here, before any frame.
        const TextureFormat declared = DeclaredStorageFormat(pkg.meta, "oSkyViewLUT");
        if (declared != SkyViewLutFormat(sourceKind))
        {
            Logger::Log::Error("SkyRenderer: sky_view_lut declares its storage image as format {}, but "
                               "SkyRenderer::SkyViewLutFormat creates the LUT as {}. Change the kernel's layout "
                               "qualifier and the accessor together.",
                               ToString(declared), ToString(SkyViewLutFormat(sourceKind)));
            return false;
        }

        DescriptorSetLayoutDesc sl{};
        sl.bindings = {
            {m_Bindings.SkyViewLut.SkyViewLUT, DescriptorType::StorageImage, 1, 0x20},
            {m_Bindings.SkyViewLut.TransmittanceLUT, DescriptorType::CombinedImageSampler, 1, 0x20},
            {m_Bindings.SkyViewLut.Atmos, DescriptorType::UniformBuffer, 1, 0x20},
            {m_Bindings.SkyViewLut.Sky, DescriptorType::UniformBuffer, 1, 0x20},
            {m_Bindings.SkyViewLut.MultiscatterLUT, DescriptorType::CombinedImageSampler, 1, 0x20}
        };
        for (auto& b : sl.bindings)
        {
            if (b.type == DescriptorType::StorageImage)
                b.storageTexelFormat = static_cast<uint32_t>(SkyViewLutFormat(sourceKind));
            if (b.type == DescriptorType::CombinedImageSampler)
                b.imageFilterableFloat = true;
        }
        m_SkyViewLutPipe.type = PipelineType::Compute;
        m_SkyViewLutPipe.computeShader = itCs->second;
        m_SkyViewLutPipe.descriptorSetLayouts.push_back(sl);
        m_SkyViewLutPipe.debugName = "Sky_View_LUT_Pipe";
    }

    if (!CreateSkyRenderPipeline("Shaders/sky_render.shaderpkg", false, m_SkyRender) ||
        !CreateStarBillboardPipeline("Shaders/sky_stars_billboard.shaderpkg", false, m_StarBillboard))
        return false;
    m_HasHeightFogPipelines =
        CreateSkyRenderPipeline("Shaders/sky_render_fog.shaderpkg", true, m_SkyRenderFog) &&
        CreateStarBillboardPipeline("Shaders/sky_stars_billboard_fog.shaderpkg", true, m_StarBillboardFog);
    if (!m_HasHeightFogPipelines)
        Logger::Log::Info("SkyRenderer: the height fog variants of the sky are not built here; the sky "
                          "draws without the height fog's sky fill");

    return true;
}

bool SkyRenderer::CreateSkyRenderPipeline(const char* packagePath, bool heightFog, SkyDrawPipeline& out)
{
    const ShaderSourceKind sourceKind = m_Device->PreferredShaderSource();
    ShaderPackage pkg{};
    std::string err;
    if (!LoadShaderPkg(packagePath, sourceKind, pkg, &err))
        return false;
    auto itVs = pkg.stageBytes.find("vs");
    auto itFs = pkg.stageBytes.find("fs");
    if (itVs == pkg.stageBytes.end() || itVs->second.empty() || itFs == pkg.stageBytes.end() || itFs->second.empty())
        return false;

    const char* name = heightFog ? "sky_render_fog" : "sky_render";
    auto& slots = m_Bindings.SkyRender;
    slots.Sky = ResolveSet0Binding(pkg.meta, "uSky", 0, name);
    slots.SkyViewLUT = ResolveSet0Binding(pkg.meta, "uSkyViewLUT", 1, name);
    slots.Atmos = ResolveSet0Binding(pkg.meta, "uAtmos", 2, name);
    slots.TransLUT = ResolveSet0Binding(pkg.meta, "uTransLUT", 3, name);
    slots.MoonFull = ResolveSet0Binding(pkg.meta, "uMoonFullTexture", 4, name);
    slots.Exposure = ResolveSet0Binding(pkg.meta, "uExposure", 5, name);

    DescriptorSetLayoutDesc sl{};
    sl.bindings = {
        {slots.Sky, DescriptorType::UniformBuffer, 1, 0x10},
        {slots.SkyViewLUT, DescriptorType::CombinedImageSampler, 1, 0x10},
        {slots.Atmos, DescriptorType::UniformBuffer, 1, 0x10},
        {slots.TransLUT, DescriptorType::CombinedImageSampler, 1, 0x10},
        {slots.MoonFull, DescriptorType::CombinedImageSampler, 1, 0x10},
        {slots.Exposure, DescriptorType::StorageBuffer, 1, 0x10},
    };
    if (heightFog)
    {
        slots.ViewParams = ResolveSet0Binding(pkg.meta, "ViewParams", 6, name);
        slots.HeightFog = ResolveSet0Binding(pkg.meta, "HeightFogParams", 7, name);
        slots.HeightFogBackdrop = ResolveSet0Binding(pkg.meta, "uHeightFogBackdrop", 8, name);
        sl.bindings.push_back({slots.ViewParams, DescriptorType::UniformBuffer, 1, 0x10});
        sl.bindings.push_back({slots.HeightFog, DescriptorType::UniformBuffer, 1, 0x10});
        sl.bindings.push_back({slots.HeightFogBackdrop, DescriptorType::CombinedImageSampler, 1, 0x10});
    }
    // Each storage buffer's access class and each image's shape come from the shader's
    // reflection: WebGPU bakes both into the layout and refuses a pipeline that disagrees.
    ApplyMetaImageShapeToLayout(pkg.meta, sl);
    out.Pipe = {};
    out.Pipe.type = PipelineType::Graphics;
    out.Pipe.vertexShader = itVs->second;
    out.Pipe.pixelShader = itFs->second;
    out.Pipe.descriptorSetLayouts.push_back(sl);
    out.SetDesc = {};
    out.SetDesc.layout = sl;
    out.SetDesc.layout.debugName = heightFog ? "Sky.SkyRenderFog" : "Sky.SkyRender";
    out.SetDesc.transient = true;
    out.SetDesc.debugName = heightFog ? "Sky.SkyRenderFog.Set0" : "Sky.SkyRender.Set0";
    out.Pipe.debugName = heightFog ? "Sky_RenderFog_Pipe" : "Sky_Render_Pipe";

    out.Pipe.rasterizationSamples = 1;
    out.Pipe.EnableDepthTest(false);
    out.Pipe.SetCullingMode(CullModeFlagBits::None);
    out.Pipe.AddDynamicState(DynamicState::Viewport);
    out.Pipe.AddDynamicState(DynamicState::Scissor);
    out.Id = {};
    return true;
}

bool SkyRenderer::CreateStarBillboardPipeline(const char* packagePath, bool heightFog, SkyDrawPipeline& out)
{
    const ShaderSourceKind sourceKind = m_Device->PreferredShaderSource();
    ShaderPackage pkg{};
    std::string err;
    if (!LoadShaderPkg(packagePath, sourceKind, pkg, &err))
        return false;
    auto itVs = pkg.stageBytes.find("vs");
    auto itFs = pkg.stageBytes.find("fs");
    if (itVs == pkg.stageBytes.end() || itVs->second.empty() ||
        itFs == pkg.stageBytes.end() || itFs->second.empty())
        return false;

    const char* name = heightFog ? "sky_stars_billboard_fog" : "sky_stars_billboard";
    auto& slots = m_Bindings.StarBillboard;
    slots.Stars = ResolveSet0Binding(pkg.meta, "sb", 0, name);
    slots.Sky = ResolveSet0Binding(pkg.meta, "uSky", 1, name);
    slots.Exposure = ResolveSet0Binding(pkg.meta, "uExposure", 2, name);
    slots.SkyViewLUT = ResolveSet0Binding(pkg.meta, "uSkyViewLUT", 3, name);
    slots.Atmos = ResolveSet0Binding(pkg.meta, "uAtmos", 4, name);
    slots.TransLUT = ResolveSet0Binding(pkg.meta, "uTransLUT", 5, name);
    slots.Compensation = ResolveSet0Binding(pkg.meta, "uCompensation", 6, name);

    constexpr uint32_t kVertFrag = kShaderStageVertex | kShaderStageFragment;
    DescriptorSetLayoutDesc sl{};
    sl.bindings = {
        {slots.Stars, DescriptorType::StorageBuffer, 1, kVertFrag},
        {slots.Sky, DescriptorType::UniformBuffer, 1, kVertFrag},
        {slots.Exposure, DescriptorType::StorageBuffer, 1, kShaderStageFragment},
        {slots.SkyViewLUT, DescriptorType::CombinedImageSampler, 1, kShaderStageVertex},
        {slots.Atmos, DescriptorType::UniformBuffer, 1, kShaderStageVertex},
        {slots.TransLUT, DescriptorType::CombinedImageSampler, 1, kShaderStageVertex},
        {slots.Compensation, DescriptorType::StorageBuffer, 1, kShaderStageFragment}
    };
    if (heightFog)
    {
        slots.HeightFog = ResolveSet0Binding(pkg.meta, "HeightFogParams", 7, name);
        sl.bindings.push_back({slots.HeightFog, DescriptorType::UniformBuffer, 1, kShaderStageVertex});
    }
    ApplyMetaImageShapeToLayout(pkg.meta, sl);
    out.Pipe = {};
    out.Pipe.type = PipelineType::Graphics;
    out.Pipe.vertexShader = itVs->second;
    out.Pipe.pixelShader = itFs->second;
    out.Pipe.descriptorSetLayouts.push_back(sl);
    out.SetDesc = {};
    out.SetDesc.layout = sl;
    out.SetDesc.layout.debugName = heightFog ? "Sky.StarBillboardFog" : "Sky.StarBillboard";
    out.SetDesc.transient = true;
    out.SetDesc.debugName = heightFog ? "Sky.StarBillboardFog.Set0" : "Sky.StarBillboard.Set0";
    out.Pipe.debugName = heightFog ? "Sky_StarBillboardFog_Pipe" : "Sky_StarBillboard_Pipe";

    out.Pipe.rasterizationSamples = 1;
    out.Pipe.EnableDepthTest(false);
    out.Pipe.SetCullingMode(CullModeFlagBits::None);
    // Additive into scene colour (colour 0) and into the star light (colour 1): both attachments
    // add, so they share one blend state.
    out.Pipe.EnableBlending(true, BlendFactor::One, BlendFactor::One);
    out.Pipe.colorBlendState.attachments.push_back(out.Pipe.colorBlendState.attachments[0]);
    out.Pipe.AddDynamicState(DynamicState::Viewport);
    out.Pipe.AddDynamicState(DynamicState::Scissor);
    out.Id = {};
    return true;
}

void SkyRenderer::GenerateStarCatalog()
{
    // Any star field the renderer draws holds its stars brightest first (BuildStarField's contract;
    // a catalog loaded from data sorts to it), so a limiting magnitude draws a prefix.
    const std::vector<StarInstanceGPU> stars = BuildStarField();
    m_StarCount = static_cast<uint32_t>(stars.size());
    m_StarMagnitudes.resize(stars.size());
    for (size_t i = 0; i < stars.size(); ++i)
        m_StarMagnitudes[i] = stars[i].dirAndMagnitude[3];
    assert(std::is_sorted(m_StarMagnitudes.begin(), m_StarMagnitudes.end()));

    BufferDesc bd{};
    bd.size = m_StarCount * sizeof(StarInstanceGPU);
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    bd.memoryUsage = BufferMemoryUsage::Upload;
    bd.flags = BufferCreateFlags::PersistentlyMapped;
    bd.debugName = "Sky_StarCatalog_SSBO";
    m_StarSSBO = m_Device->CreateBuffer(bd);

    void* mapped = m_Device->MapBuffer(m_StarSSBO);
    if (mapped)
    {
        std::memcpy(mapped, stars.data(), m_StarCount * sizeof(StarInstanceGPU));
        m_Device->UnmapBuffer(m_StarSSBO);
    }
}

uint32_t SkyRenderer::GetStarInstanceCount(float limitingMagnitude) const
{
    constexpr float kLimitFade = 0.25f;
    const auto end = std::upper_bound(m_StarMagnitudes.begin(), m_StarMagnitudes.end(), limitingMagnitude + kLimitFade);
    return static_cast<uint32_t>(end - m_StarMagnitudes.begin());
}

void SkyRenderer::Update(const SkySettings& settings, const SkySystemState& state)
{
    m_Settings = settings;
    m_State = state;
}

void SkyRenderer::SetCamera(const CameraState& camera)
{
    m_Camera = camera;
}

// Earth's atmosphere on a crisp clear day: Hillaire 2020's Earth ("A Scalable and Production Ready
// Sky and Atmosphere Rendering Technique", table 1, after Bruneton's reference) with the aerosol of
// a crisp clear day, optical depth 0.06 at 550 nm, in place of the paper's pristine 0.0053: a deep
// blue zenith over a light haze at the horizon. The hazier days are the presets an aerosol setting
// will carry: a generic clear day at 0.10 and the satellite record's typical day at 0.15 (its
// multiannual global mean is 0.13 to 0.14 over the oceans and 0.19 over land; MODIS, Remer et al.
// 2008, JGR 113, D14S07). Per-metre coefficients at sea level, scale heights in metres, ozone as a
// tent between 10 and 40 km.
void SkyRenderer::FillDefaultAtmosphere(AtmosphereParametersGPU& atmo)
{
    atmo.planetRadius = 6360.0e3f;
    atmo.atmosphereRadius = 6460.0e3f;
    atmo.rayleighScaleHeight = 8000.0f;
    atmo.mieScaleHeight = 1200.0f;
    atmo.betaRayleigh[0] = 5.802e-6f;
    atmo.betaRayleigh[1] = 13.558e-6f;
    atmo.betaRayleigh[2] = 33.100e-6f;
    // Mie extinction 0.06 / 1200 m = 5e-5 per metre at 550 nm, at the paper's single-scattering
    // albedo, 0.9 (absorption is the extinction less the scattering), and falling with wavelength as
    // (lambda / 550 nm)^-1.6, the Angstrom exponent of an aerosol of mostly fine particles, sampled at
    // the 680, 550 and 440 nm the Rayleigh coefficients are given at. A grey aerosol greys the sky:
    // this one scatters blue 43 % more than green, red 29 % less.
    atmo.betaMie[0] = 3.20463e-5f;
    atmo.betaMie[1] = 4.5e-5f;
    atmo.betaMie[2] = 6.43085e-5f;
    atmo.mieAbsorption[0] = 3.56071e-6f;
    atmo.mieAbsorption[1] = 5.0e-6f;
    atmo.mieAbsorption[2] = 7.14539e-6f;
    // Ozone per linear-sRGB channel. The paper samples the cross section at 680, 550 and 440 nm
    // (0.650, 1.881, 0.085)e-6, but the Chappuis band peaks at 600 nm, between the red and green
    // samples, so that under-weights red and turns the twilight zenith violet. These are the same
    // data (300 Dobson units, Serdyuchenko 233 K cross sections, the solar spectrum) integrated
    // against the CIE 1931 functions and the sRGB primaries: what each channel of the sun loses
    // through the zenith column. From the zenith column to the horizon path red moves 0.9 % and
    // green 2.6 %; blue stays within -0.03..0.05e-6, so it is 0.
    atmo.ozoneAbsorption[0] = 2.480e-6f;
    atmo.ozoneAbsorption[1] = 1.795e-6f;
    atmo.ozoneAbsorption[2] = 0.0f;
    atmo.ozoneCenterHeight = 25.0e3f;
    atmo.ozoneHalfWidth = 15.0e3f;
    atmo.groundAlbedo[0] = kDefaultGroundAlbedo;
    atmo.groundAlbedo[1] = kDefaultGroundAlbedo;
    atmo.groundAlbedo[2] = kDefaultGroundAlbedo;
    atmo.groundBrightness = 0.8f;
    atmo.groundNightColor[0] = 0.025f;
    atmo.groundNightColor[1] = 0.025f;
    atmo.groundNightColor[2] = 0.035f;
    atmo.groundNightColorStd140Pad = 0.0f;
    atmo.groundHorizonColor[0] = 0.04f;
    atmo.groundHorizonColor[1] = 0.05f;
    atmo.groundHorizonColor[2] = 0.07f;
    atmo.groundHorizonColorStd140Pad = 0.0f;
    // sRGB #020305 (scene-linear).
    atmo.groundHorizonNightColor[0] = 0.000607054f;
    atmo.groundHorizonNightColor[1] = 0.000910581f;
    atmo.groundHorizonNightColor[2] = 0.001517635f;
    atmo.nightSkyHorizonColor[0] = 0.12f;
    atmo.nightSkyHorizonColor[1] = 0.035f;
    atmo.nightSkyHorizonColor[2] = 0.19f;
    atmo.nightSkyHorizonColorStd140Pad = 0.0f;
    atmo.groundHorizonDayCosWidth = 0.035f;
    atmo.groundHorizonNightCosWidth = 0.035f;
    atmo.belowHorizonBlendSharpness = 2.0f;
    atmo.belowHorizonDarkness = 1.0f;
    atmo.belowHorizonDarkPadding[0] = 0.0f;
    atmo.belowHorizonDarkPadding[1] = 0.0f;
    atmo.belowHorizonDarkColorStd140PrePad[0] = 0.0f;
    atmo.belowHorizonDarkColorStd140PrePad[1] = 0.0f;
    atmo.belowHorizonDarkColorStd140PrePad[2] = 0.0f;
    atmo.belowHorizonDarkColor[0] = 0.02f;
    atmo.belowHorizonDarkColor[1] = 0.02f;
    atmo.belowHorizonDarkColor[2] = 0.025f;
    atmo.belowHorizonMode = static_cast<uint32_t>(SkyBelowHorizonMode::ContinueHorizon);
    atmo.groundHazeStrength = 1.0f;
}

uint32_t SkyRenderer::GetFrameSlot(IDevice* device) const
{
    return device->GetFrameIndex() % m_FramesInFlight;
}

ComputePipelineId SkyRenderer::EnsureTransmittanceId(IDevice& device)
{
    if (!m_TransmittanceId.IsValid())
    {
        m_TransmittancePipe.debugName = "Sky_Transmittance_Pipe";
        m_TransmittanceId = PipelineDescTranslator::InternCompute(device, m_TransmittancePipe);
    }
    return m_TransmittanceId;
}

ComputePipelineId SkyRenderer::EnsureMultiscatterId(IDevice& device)
{
    if (!m_MultiscatterId.IsValid())
    {
        m_MultiscatterPipe.debugName = "Sky_Multiscatter_Pipe";
        m_MultiscatterId = PipelineDescTranslator::InternCompute(device, m_MultiscatterPipe);
    }
    return m_MultiscatterId;
}

ComputePipelineId SkyRenderer::EnsureSkyViewLutId(IDevice& device)
{
    if (!m_SkyViewLutId.IsValid())
    {
        m_SkyViewLutPipe.debugName = "Sky_View_LUT_Pipe";
        m_SkyViewLutId = PipelineDescTranslator::InternCompute(device, m_SkyViewLutPipe);
    }
    return m_SkyViewLutId;
}

GraphicsPipelineId SkyRenderer::EnsureDrawPipelineId(IDevice& device, SkyDrawPipeline& pipeline)
{
    if (!pipeline.Id.IsValid())
        pipeline.Id = PipelineDescTranslator::InternGraphics(device, pipeline.Pipe);
    return pipeline.Id;
}

GraphicsPipelineId SkyRenderer::EnsureStarBillboardId(IDevice& device, bool heightFog)
{
    assert((!heightFog || m_HasHeightFogPipelines) && "the stars' height fog variant was not built");
    return EnsureDrawPipelineId(device, heightFog ? m_StarBillboardFog : m_StarBillboard);
}

GraphicsPipelineId SkyRenderer::EnsureSkyRenderId(IDevice& device, bool heightFog)
{
    assert((!heightFog || m_HasHeightFogPipelines) && "the sky's height fog variant was not built");
    return EnsureDrawPipelineId(device, heightFog ? m_SkyRenderFog : m_SkyRender);
}

} // namespace Rendering
} // namespace GameEngine
