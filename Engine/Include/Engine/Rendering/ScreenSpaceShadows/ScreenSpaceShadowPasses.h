#pragma once

#include <memory>
#include <span>
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/Pipeline/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::Rendering
{
class IDevice;
struct CameraData;
struct ShaderMeta;
}
namespace GameEngine::Engine::Renderer
{
// The same per-view cascade constants consumed by directional lighting.
// No shadow-map texture is needed to project its fixed PCF footprint.
struct ScreenSpaceShadowFilter
{
    Rendering::BufferHandle Constants{};
    uint64_t Offset = 0;
    uint64_t Bytes = 0;
    uint32_t Resolution = 0;
    uint32_t Quality = 0;
};
// Device-local compute pass bundle, not a registered/global service. Owns the
// cached pipelines and sampler; RGFrame owns per-view textures and scratch.
// The world-pass contributor decides when these depth-only passes are needed.
class ScreenSpaceShadowPasses
{
  public:
    explicit ScreenSpaceShadowPasses(Rendering::IDevice* device);
    ~ScreenSpaceShadowPasses();
    // After an in-place device rebuild: the sampler died with the old device.
    // Forgets it without destroying it; the next pass recreates it.
    void OnDeviceRebuilt() { m_Sampler = {}; }
    // Asks the device (IDevice::RequestComputePipeline) for the four pipelines that DeclareMaskPass
    // uses. A paused view calls it, so the mask of its first live frame builds no pipeline.
    void RequestPipelines();
    Rendering::RenderGraph::RGTexture DeclareMaskPass(
        Rendering::RenderGraph::RGFrame& frame, uint32_t viewId,
        Rendering::RenderGraph::RGTexture depth, const Rendering::CameraData& camera,
        const float towardLight[3], float thickness,
        const ScreenSpaceShadowFilter* filter = nullptr);

  private:
    // A pass that writes its set by binding index names each binding here. The load
    // resolves the names before it makes the pipeline, so a renamed binding fails the load.
    struct NamedBinding
    {
        const char* Name;
        uint32_t& Binding;
    };
    Rendering::ComputePipelineId m_MatchPipeline{};
    std::unique_ptr<Rendering::ShaderMeta> m_MatchMeta;
    Rendering::DescriptorSetLayoutDesc m_MatchLayout{};
    bool LoadShader();
    // Interns the four pass pipelines once. Builds no backend pipeline.
    bool InternPipelines();
    bool InternPassPipeline(const char* path, Rendering::ComputePipelineId& pipeline,
                            std::unique_ptr<Rendering::ShaderMeta>& meta,
                            Rendering::DescriptorSetLayoutDesc& layout,
                            std::span<const NamedBinding> bindings);
    Rendering::IDevice* m_Device = nullptr;
    Rendering::ComputePipelineId m_Pipeline{};
    Rendering::ComputePipelineId m_PreparePipeline{};
    Rendering::ComputePipelineId m_ResolvePipeline{};
    std::unique_ptr<Rendering::ShaderMeta> m_ResolveMeta;
    Rendering::DescriptorSetLayoutDesc m_ResolveLayout{};
    std::unique_ptr<Rendering::ShaderMeta> m_PrepareMeta;
    Rendering::DescriptorSetLayoutDesc m_PrepareLayout{};
    // Set-0 bindings of the prepare and resolve shaders. LoadShader resolves them by name.
    uint32_t m_PrepareDepthBinding = 0;
    uint32_t m_PrepareVisibilityBinding = 0;
    uint32_t m_ResolveDepthBinding = 0;
    uint32_t m_ResolveVisibilityBinding = 0;
    uint32_t m_ResolveMaskBinding = 0;
    std::unique_ptr<Rendering::ShaderMeta> m_Meta;
    Rendering::DescriptorSetLayoutDesc m_Layout{};
    Rendering::SamplerHandle m_Sampler{};
    // Each failure kind logs once, so one kind does not hide the other.
    bool m_WarnedPackageLoadFailure = false;
    bool m_LoggedMissingBinding = false;
};
} // namespace GameEngine::Engine::Renderer
