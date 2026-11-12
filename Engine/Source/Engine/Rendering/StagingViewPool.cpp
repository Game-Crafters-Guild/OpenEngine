#include "Engine/Rendering/StagingViewPool.h"

#include "ECS/Entity.h"
#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "Engine/Rendering/WorldDrawBuilder.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NoneCullingStrategy.h"
#include "Rendering/Core/ReverseZ.h"
#include "Rendering/Passes/TonemapPass.h"

#include <cassert>

namespace GameEngine::Engine::Renderer
{
namespace
{
constexpr Rendering::TextureFormat kColorFormat = Rendering::TextureFormat::R16G16B16A16_FLOAT;

// Pool targets are keyed by size, so views of one size share one set within a frame.
std::string SizedTargetName(const char* base, uint32 width, uint32 height)
{
    return std::string(base) + "." + std::to_string(width) + "x" + std::to_string(height);
}

Rendering::TextureDesc MakeTargetDesc(uint32 width, uint32 height, Rendering::TextureFormat format, uint32 usage,
                                      uint32 samples, const char* debugName)
{
    Rendering::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = samples;
    desc.format = static_cast<uint32>(format);
    desc.usage = usage;
    desc.debugName = debugName;
    return desc;
}
} // namespace

struct StagingViewPool::Entry
{
    // The view that holds the entry; null while the entry is free.
    StagingView* Owner = nullptr;
    std::unique_ptr<ECS::World> World;
    Rendering::CameraId Camera = 0;
    Rendering::ViewId View = 0;
    Renderer::Camera CameraPose{};
    uint32 Width = 0;
    uint32 Height = 0;
    std::string DebugName;
    std::string UiTextureName;
    // Built when the name or the size changes, so a declared frame allocates no string.
    std::string ColorMsaaTargetName;
    std::string DepthTargetName;
    std::string HdrTargetName;
    std::string OutputName;
    Rendering::TextureHandle Output{};
    uint64 OutputDeviceGeneration = 0;
    uint32 OutputWidth = 0;
    uint32 OutputHeight = 0;
    bool OutputInitialized = false;
    uint64 OutputRevision = 0;
};

StagingViewPool::StagingViewPool(RenderServices& renderServices)
    : m_RenderServices(renderServices)
{
}

StagingViewPool::~StagingViewPool()
{
    ViewRegistry& views = m_RenderServices.Views();
    for (const std::unique_ptr<Entry>& entry : m_Entries)
    {
        if (!entry->World)
            continue;
        if (entry->Owner)
            entry->Owner->Detach();
        DestroyOutputTexture(*entry);
        views.ReleaseView(entry->View);
        views.ReleaseCamera(entry->Camera);
    }
}

StagingView StagingViewPool::Acquire(const StagingViewDesc& desc)
{
    assert(desc.Width > 0 && desc.Height > 0 && "StagingViewPool::Acquire: the size must be non-zero");
    const StagingViewHandle handle = m_Handles.Create();
    const uint64 index = handle.Index();
    if (index == m_Entries.size())
    {
        auto entry = std::make_unique<Entry>();
        entry->World = std::make_unique<ECS::World>();
        RegisterRenderWorldHooks(*entry->World);
        entry->UiTextureName = "staging_view_" + std::to_string(index);
        m_Entries.push_back(std::move(entry));
    }

    Entry& entry = *m_Entries[index];
    entry.Width = desc.Width;
    entry.Height = desc.Height;
    entry.DebugName = desc.DebugName ? desc.DebugName : "Staging View";
    entry.OutputName = entry.DebugName + ".Output";
    entry.CameraPose = Camera{};
    UpdateTargetNames(entry);

    ViewRegistry& views = m_RenderServices.Views();
    if (entry.Camera == 0)
        entry.Camera = views.AllocateCamera(entry.DebugName.c_str());
    if (entry.View == 0)
    {
        // Every entity of a staging world is meant to render: no culling dispatch.
        entry.View = views.AllocateView(entry.DebugName.c_str(), entry.Camera, Rendering::ViewPurpose::EditorPreview,
                                        Rendering::ViewParticipation::OnDemand);
        views.SetViewCullingStrategy(entry.View, std::make_shared<Rendering::NoneCullingStrategy>());
    }
    views.SetViewWorldId(entry.View, entry.World->GetWorldId());
    WriteCameraData(entry);
    return StagingView(*this, handle);
}

void StagingViewPool::RetireEntry(Entry& entry)
{
    ViewRegistry& views = m_RenderServices.Views();
    views.ReleaseView(entry.View);
    views.ReleaseCamera(entry.Camera);
    entry.View = 0;
    entry.Camera = 0;
    entry.World.reset();
}

void StagingViewPool::Release(StagingViewHandle handle)
{
    Entry& entry = Resolve(handle);
    entry.Owner = nullptr;
    entry.World->Clear();
    // The view's draw lists outlive the world's entities until the next extraction: drop them here.
    m_RenderServices.GetWorldDrawBuilder().ClearView(entry.View);
    DestroyOutputTexture(entry);
    m_RenderServices.Views().ClearViewPostProcessOverride(entry.View);
    m_RenderServices.Views().SetViewForcedLOD(entry.View, ViewRegistry::kAutomaticLod);
    // The manager never hands out an index again after its last generation.
    const bool retires = handle.Generation() == Rendering::Detail::kGenerationMask;
    m_Handles.Destroy(handle);
    if (retires)
        RetireEntry(entry);
}

bool StagingViewPool::IsAlive(StagingViewHandle handle) const
{
    return m_Handles.IsAlive(handle);
}

void StagingViewPool::SetOwner(StagingViewHandle handle, StagingView* owner)
{
    Resolve(handle).Owner = owner;
}

ECS::World& StagingViewPool::World(StagingViewHandle handle)
{
    return *Resolve(handle).World;
}

Rendering::ViewId StagingViewPool::View(StagingViewHandle handle) const
{
    return Resolve(handle).View;
}

void StagingViewPool::SetCamera(StagingViewHandle handle, const Camera& camera)
{
    Entry& entry = Resolve(handle);
    entry.CameraPose = camera;
    WriteCameraData(entry);
}

void StagingViewPool::Resize(StagingView& view, uint32 width, uint32 height)
{
    assert(width > 0 && height > 0 && "StagingViewPool::Resize: the size must be non-zero");
    Entry& entry = Resolve(view.m_Handle);
    if (entry.Width == width && entry.Height == height)
        return;
    entry.Width = width;
    entry.Height = height;
    UpdateTargetNames(entry);
    WriteCameraData(entry);
}

uint32 StagingViewPool::RenderWidth(StagingViewHandle handle) const
{
    return Resolve(handle).Width;
}

uint32 StagingViewPool::RenderHeight(StagingViewHandle handle) const
{
    return Resolve(handle).Height;
}

StagingWorldPassRG StagingViewPool::DeclareWorldPass(StagingViewHandle handle, Rendering::RenderGraph::RGFrame& frame,
                                                     Rendering::MaterialKeyword keywords)
{
    namespace RenderGraph = Rendering::RenderGraph;
    Entry& entry = Resolve(handle);
    StagingWorldPassRG result{};
    if (!m_RenderServices.GetDevice())
        return result;

    m_RenderServices.WriteViewLightBuffer(entry.View);
    m_RenderServices.BuildWorldBatchKeysForView(entry.View);
    // The bucketer twin records before the world pass: the record order gives the draw-stream
    // edge its direction, so the draws read this frame's buffers.
    if (!m_RenderServices.ScheduleBucketerDispatchesForView(frame, entry.View))
        return result;
    // Alpha 0, so the subject composites over the panel; reverse-Z far depth.
    m_RenderServices.Views().SetViewClearConfig(
        entry.View, Rendering::ViewClearConfig(/*clearColorIn=*/true, ColorLinear(0.0f, 0.0f, 0.0f, 0.0f),
                                               /*clearDepthIn=*/true, Rendering::ReverseZ::kClearFar));

    // World targets are pool imports, not transients: the world pass resolves the physical
    // depth at declaration for ge_sceneDepth.
    const uint32 width = entry.Width;
    const uint32 height = entry.Height;
    const uint32 samples = m_RenderServices.GetDefaultMSAASampleCount();
    const bool msaa = samples > 1u;
    const uint32 colorUsage = static_cast<uint32>(Rendering::TextureUsage::RenderTarget) |
                              static_cast<uint32>(Rendering::TextureUsage::ShaderResource);
    const uint32 depthUsage = static_cast<uint32>(Rendering::TextureUsage::DepthStencil) |
                              static_cast<uint32>(Rendering::TextureUsage::ShaderResource);

    RenderGraph::RGTexture msaaColor{};
    if (msaa)
    {
        const char* name = entry.ColorMsaaTargetName.c_str();
        msaaColor =
            frame.ImportPersistentTexture(name, MakeTargetDesc(width, height, kColorFormat, colorUsage, samples, name));
    }
    const char* depthName = entry.DepthTargetName.c_str();
    const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(
        depthName, MakeTargetDesc(width, height, m_RenderServices.GetDepthFormat(), depthUsage, msaa ? samples : 1u,
                                  depthName));
    const char* hdrName = entry.HdrTargetName.c_str();
    const RenderGraph::RGTexture hdr =
        frame.ImportPersistentTexture(hdrName, MakeTargetDesc(width, height, kColorFormat, colorUsage, 1u, hdrName));
    if (!depth.IsValid() || !hdr.IsValid() || (msaa && !msaaColor.IsValid()))
        return result;

    // The world pass reads the camera upload at declaration.
    WriteCameraData(entry);
    RenderServices::WorldPassTargetsRG targets{};
    targets.Color = msaa ? msaaColor : hdr;
    targets.Depth = depth;
    if (msaa)
        targets.Resolve = hdr;
    const RenderServices::WorldPassRG world = m_RenderServices.AddWorldPassForView(frame, entry.View, targets, keywords);
    if (!world.Pass.IsValid())
        return result;

    result.Color = world.EffectiveColor.IsValid() ? world.EffectiveColor : hdr;
    result.Depth = depth;
    result.Width = width;
    result.Height = height;
    return result;
}

bool StagingViewPool::DeclareOutput(StagingViewHandle handle, Rendering::RenderGraph::RGFrame& frame,
                                    Rendering::RenderGraph::RGTexture source,
                                    const Rendering::Passes::TonemapParams& params)
{
    namespace RenderGraph = Rendering::RenderGraph;
    Entry& entry = Resolve(handle);
    if (!source.IsValid() || !EnsureOutputTexture(entry))
        return false;
    const RenderGraph::RGTexture output = frame.ImportExternalTexture(
        entry.UiTextureName.c_str(), entry.Output,
        entry.OutputInitialized ? Rendering::ResourceState::ShaderResource : Rendering::ResourceState::Undefined,
        kColorFormat);
    if (!output.IsValid())
        return false;
    if (!Rendering::Passes::AddTonemapPassRG(frame, source, output, params, entry.OutputName.c_str()).IsValid())
        return false;
    frame.MarkOutput(output, RenderGraph::RGImageLayout::ShaderReadOnly);
    entry.OutputInitialized = true;
    ++entry.OutputRevision;
    return true;
}

const std::string& StagingViewPool::UiTextureName(StagingViewHandle handle) const
{
    return Resolve(handle).UiTextureName;
}

Rendering::TextureHandle StagingViewPool::OutputTexture(StagingViewHandle handle) const
{
    const Entry& entry = Resolve(handle);
    return IsOutputCurrent(entry) ? entry.Output : Rendering::TextureHandle{};
}

uint32 StagingViewPool::OutputWidth(StagingViewHandle handle) const
{
    const Entry& entry = Resolve(handle);
    return IsOutputCurrent(entry) ? entry.OutputWidth : 0u;
}

uint32 StagingViewPool::OutputHeight(StagingViewHandle handle) const
{
    const Entry& entry = Resolve(handle);
    return IsOutputCurrent(entry) ? entry.OutputHeight : 0u;
}

uint64 StagingViewPool::OutputRevision(StagingViewHandle handle) const
{
    return Resolve(handle).OutputRevision;
}

StagingViewPool::Entry& StagingViewPool::Resolve(StagingViewHandle handle)
{
    assert(IsAlive(handle) && "StagingViewPool: the handle was released or never acquired");
    return *m_Entries[handle.Index()];
}

const StagingViewPool::Entry& StagingViewPool::Resolve(StagingViewHandle handle) const
{
    assert(IsAlive(handle) && "StagingViewPool: the handle was released or never acquired");
    return *m_Entries[handle.Index()];
}

void StagingViewPool::WriteCameraData(Entry& entry)
{
    const float aspect = static_cast<float>(entry.Width) / static_cast<float>(entry.Height);
    m_RenderServices.Views().SetCameraData(entry.Camera, entry.CameraPose.ToCameraData(aspect),
                                           entry.CameraPose.ToCameraRanges());
}

void StagingViewPool::UpdateTargetNames(Entry& entry)
{
    entry.ColorMsaaTargetName = SizedTargetName("StagingView.ColorMSAA", entry.Width, entry.Height);
    entry.DepthTargetName = SizedTargetName("StagingView.Depth", entry.Width, entry.Height);
    entry.HdrTargetName = SizedTargetName("StagingView.HDRColor", entry.Width, entry.Height);
}

bool StagingViewPool::IsOutputCurrent(const Entry& entry) const
{
    const Rendering::IDevice* device = m_RenderServices.GetDevice();
    return entry.Output.IsValid() && device && device->GetDeviceRebuildGeneration() == entry.OutputDeviceGeneration;
}

void StagingViewPool::ForgetOutputIfDeviceRebuilt(Entry& entry)
{
    // An in-place device rebuild destroys every live texture: the old handle names a freed slot.
    if (!entry.Output.IsValid() || IsOutputCurrent(entry))
        return;
    entry.Output = {};
    entry.OutputWidth = 0;
    entry.OutputHeight = 0;
    entry.OutputInitialized = false;
}

bool StagingViewPool::EnsureOutputTexture(Entry& entry)
{
    ForgetOutputIfDeviceRebuilt(entry);
    if (entry.Output.IsValid() && entry.OutputWidth == entry.Width && entry.OutputHeight == entry.Height)
        return true;
    Rendering::IDevice* device = m_RenderServices.GetDevice();
    if (!device)
        return false;
    DestroyOutputTexture(entry);
    // TransferSrc: a capture reads the output back to a host buffer.
    const uint32 usage = static_cast<uint32>(Rendering::TextureUsage::RenderTarget) |
                         static_cast<uint32>(Rendering::TextureUsage::ShaderResource) |
                         static_cast<uint32>(Rendering::TextureUsage::TransferSrc);
    entry.Output = device->CreateTexture(
        MakeTargetDesc(entry.Width, entry.Height, kColorFormat, usage, 1u, entry.OutputName.c_str()));
    if (!entry.Output.IsValid())
    {
        Logger::Log::Error("StagingViewPool: the {}x{} output texture of '{}' could not be created", entry.Width,
                           entry.Height, entry.DebugName);
        return false;
    }
    entry.OutputDeviceGeneration = device->GetDeviceRebuildGeneration();
    entry.OutputWidth = entry.Width;
    entry.OutputHeight = entry.Height;
    entry.OutputInitialized = false;
    return true;
}

void StagingViewPool::DestroyOutputTexture(Entry& entry)
{
    // The device defers the destroy on its timeline: frames in flight may still sample the output.
    ForgetOutputIfDeviceRebuilt(entry);
    if (entry.Output.IsValid())
        if (Rendering::IDevice* device = m_RenderServices.GetDevice())
            device->DestroyTexture(entry.Output);
    entry.Output = {};
    entry.OutputWidth = 0;
    entry.OutputHeight = 0;
    entry.OutputInitialized = false;
}
} // namespace GameEngine::Engine::Renderer
