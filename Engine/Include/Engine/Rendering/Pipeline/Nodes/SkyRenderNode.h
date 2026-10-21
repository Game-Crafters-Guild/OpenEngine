#pragma once

#include "Engine/Rendering/Pipeline/Nodes/ClearColorBackdrop.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Sky/SkyRenderer.h"

#include <string>
#include <unordered_map>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::BufferHandle;
using ::GameEngine::Rendering::IDevice;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
class SkyRenderNode final : public IRenderPipelineNode
{
  public:
    ~SkyRenderNode() override;
    const char* GetTypeName() const override { return "SkyRender"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    void RebuildPassNames(const std::string& pipelineName);

    // Build the five transient descriptor set descs once from the renderer's
    // reflected binding slots. Cached as members so the per-frame execute
    // callbacks never heap-allocate a bindings vector (the property the old
    // static Get*DSD() helpers preserved).
    void EnsureDescriptorSetDescs(const Rendering::SkyRenderer& renderer);

    // The height fog's sky fill for one view: whether the sky and the stars draw their height fog
    // variants, and the buffers and the sampler those bind.
    struct SkyHeightFog
    {
        bool Active = false;
        PipelineBufferBindingRG ViewParams{};
        PipelineBufferBindingRG Params{};
        Rendering::SamplerHandle BackdropSampler{};
    };
    // Active when the view's height fog is active (HeightFogPassKeyword), the variants were built and
    // the view's graph published the ViewParams and HeightFogParams buffers.
    static SkyHeightFog ResolveSkyHeightFog(ViewDeclare& d, const Rendering::SkyRenderer& renderer);

    Rendering::BufferHandle GetOrCreatePerViewSkyUBO(Rendering::IDevice* dev, uint32_t viewKey, uint32_t slot);
    void RecordStars(Rendering::RenderGraph::RGContext& passCtx, Rendering::SkyRenderer& renderer, uint32_t viewKey,
                     uint32_t vpX, uint32_t vpY, uint32_t vpW, uint32_t vpH, const PipelineBufferBindingRG& exposure,
                     const PipelineBufferBindingRG& compensation, uint32_t starCount, const SkyHeightFog& fog);

    std::string m_Id;
    std::string m_Json;

    std::string m_OutputRef = Names::View::Color;

    // Cached pass names for frame-scope passes
    std::string m_CachedPipelineName;
    std::string m_TransmittancePassName;
    std::string m_MultiscatterPassName;
    std::string m_SkyViewLutPassName;

    // Per-view sky UBOs to avoid multi-view overwrites on shared buffers.
    static constexpr uint32_t kMaxFrameSlots = Rendering::IDevice::kMaxSupportedFramesInFlight;
    struct PerViewUBOs
    {
        Rendering::BufferHandle skyUBO[kMaxFrameSlots] = {};
    };
    std::unordered_map<uint32_t, PerViewUBOs> m_PerViewUBOs;

    Rendering::IDevice* m_Device = nullptr;
    bool m_WarnedNoComponent = false;

    // The background of a view whose sky draws nothing: its clear colour at the frame's pre-exposure.
    ClearColorBackdrop m_ClearColorBackdrop;

    // The LUT passes' transient descriptor set descs, built once from reflected binding slots. The
    // sky and star passes bind the renderer's (SkyRenderer::GetSkyRenderSetDesc).
    bool m_DescriptorSetDescsBuilt = false;
    Rendering::DescriptorSetDesc m_TransmittanceDSD;
    Rendering::DescriptorSetDesc m_MultiscatterDSD;
    Rendering::DescriptorSetDesc m_SkyViewLutDSD;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
