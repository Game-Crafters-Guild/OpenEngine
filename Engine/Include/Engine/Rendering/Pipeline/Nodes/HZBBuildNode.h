#pragma once

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"

#include <cstdint>
#include <string>

namespace GameEngine::Rendering { struct ShaderPackage; }

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::ComputePipelineId;
using ::GameEngine::Rendering::DescriptorSetLayoutDesc;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Hierarchical-Z pyramid build. One instance builds one pyramid per view, a
// per-view pooled R32F texture at the EXACT depth extent (not next-pow2-down —
// that would weaken culling ~2x) with a full mip chain: mip 0 copies the source
// depth, each further mip is a 2x2 reduce of the mip below. Reverse-Z: near = 1,
// far = 0 (Includes/hzb_reduce.glsl).
//   * Default, the occlusion pyramid "View.HZB" (R2.1 two-phase HZB occlusion,
//     design §5-A5): MIN keeps the FARTHEST depth, so culling stays
//     conservative. Built from View.DepthResolved for a view with a phase-B
//     slice.
//   * "reduce": "nearest", the reflection traversal pyramid "View.SSRHiZ": MAX
//     keeps the NEAREST depth, so a ray march registers a crossing as soon as
//     it could hit a surface. ScreenSpaceReflectionsNode owns that instance and
//     builds it from the depth the reflections trace, for a view with SSSR
//     active.
//
// ONE node object serves EVERY view in the frame (perView declares per view, it
// does not instantiate per view), so this node owns no per-texture device state:
// the single-mip views it binds come from the persistent pool via
// RGContext::GetOrCreatePooledMipView, which ties their lifetime to the image.
//
// Two build paths producing BIT-IDENTICAL pyramids (HzbSpdParityTests, for both
// reductions):
//   * SPD two-dispatch (default): the tile pass builds mips 0..6 per 64x64
//     tile (LDS ping-pong, folds skipped for mips 2..6 — the miss is confined
//     to each level's global last row/column), then the finalize pass (ONE
//     workgroup) repairs those edges with the exact fold rule and builds
//     mips 7..12 whole-level from LDS (hzb_spd[_nearest].comp,
//     hzb_spd[_nearest]_finalize.comp). No storage image is re-read within a
//     dispatch, so nothing is `coherent` (a measured ~2x dispatch cost) and
//     no atomic election exists; the render-graph barrier between the two
//     passes is the only synchronization. Capped at mip-7 extents <= 32
//     (depth extents <= 4223 per axis); larger falls back to the chain.
//   * Per-mip chain (fallback: GE_HZB_SPD=0, over-large extents, or the SPD
//     shaderpkgs missing): one dispatch per mip via hzb_build.comp. Per-mip
//     subresource edges — each reduce pass Reads mip m-1 and Writes mip m of
//     the SAME texture (RGRange BaseMip) — give the graph the RAW barrier
//     between dispatches without self-hazarding (RGTypes' Hi-Z case).
//
// JSON shape:
// {
//   "id": "HZBBuild",
//   "type": "HZBBuild",
//   "enabled": true,
//   "reduce": "farthest",           // optional: "farthest" (default) or "nearest"
//   "input": "View.DepthResolved",  // optional depth source key
//   "output": "View.HZB",           // optional blackboard key to publish
//   "poolName": "Pipeline.HZB.View" // optional persistent-pool prefix
// }
class HZBBuildNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "HZBBuild"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    void LoadShaders(IDevice* device);
    void LoadSpdShader(IDevice* device);
    // Slang-lane spike (GE_SHADER_SLANG_LANE=1): compile the staged
    // Shaders/Slang/hzb_build.slang through ShaderCompileService instead of
    // loading the prebuilt GLSL package — the Gate-2 A/B wiring. Fail-visible:
    // any miss warns and the caller falls back to the .shaderpkg.
    bool TryCompileSlangChainShader(::GameEngine::Rendering::ShaderSourceKind kind,
                                    ::GameEngine::Rendering::ShaderPackage& outPkg);
    // Two-dispatch SPD build (tile pass + finalize pass) under this instance's reduction.
    void DeclareSpdBuild(ViewDeclare& d, ::GameEngine::Rendering::RenderGraph::RGTexture depth,
                         ::GameEngine::Rendering::RenderGraph::RGTexture hzb, uint32_t w,
                         uint32_t h, uint32_t levels);
    // Per-mip chain build (fallback path; content-identical to SPD for both reductions).
    // reduceMode selects the mip>=1 op: kReduceMin = farthest/occlusion HZB,
    // kReduceMax = nearest-surface SSR Hi-Z.
    void DeclareChainBuild(ViewDeclare& d, ::GameEngine::Rendering::RenderGraph::RGTexture depth,
                           ::GameEngine::Rendering::RenderGraph::RGTexture hzb, uint32_t w,
                           uint32_t h, uint32_t levels, uint32_t reduceMode);

    // hzb_build.comp push-constant `mode` values for mips >= 1.
    static constexpr uint32_t kReduceMin = 1u;
    static constexpr uint32_t kReduceMax = 2u;

    std::string m_Id;
    std::string m_InputKey = Names::View::DepthResolved;
    std::string m_OutputKey = Names::View::HZB;
    std::string m_PoolPrefix = "Pipeline.HZB.View";
    // The pass-name stem: "HZBBuild" for the occlusion pyramid, "SSRHiZ" for the nearest one.
    std::string m_PassStem = "HZBBuild";
    // "reduce": "nearest": MAX keeps the nearest depth (the reflection traversal pyramid).
    bool m_Nearest = false;

    bool m_LoadAttempted = false;
    ComputePipelineId m_PipelineId{};
    DescriptorSetLayoutDesc m_Set0Layout{};
    ComputePipelineId m_SpdPipelineId{};
    DescriptorSetLayoutDesc m_SpdSet0Layout{};
    ComputePipelineId m_SpdFinalizePipelineId{};
    DescriptorSetLayoutDesc m_SpdFinalizeSet0Layout{};
    // Set0 binding indices resolved from the shaders' reflected meta by name
    // at load time (fallback to the GLSL literals on a reflection miss).
    // Cached so the execute paths add zero lookup.
    uint32_t m_SrcBinding = 0u;      // hzb_build.comp uSrc
    uint32_t m_DstBinding = 1u;      // hzb_build.comp uDst
    uint32_t m_SpdDepthBinding = 0u; // hzb_spd.comp uDepth
    uint32_t m_SpdMip0Binding = 1u;  // hzb_spd.comp uMip0
    uint32_t m_SpdMipsBinding = 2u;  // hzb_spd.comp uMips[6]
    uint32_t m_SpdFinalizeMipsBinding = 0u; // hzb_spd_finalize.comp uMips[12]

    // Once-per-episode fail-visible warnings for SPD pipeline-creation
    // failures at exec time (a silently skipped finalize would over-cull).
    bool m_WarnedSpdTilePipeline = false;
    bool m_WarnedSpdFinalizePipeline = false;
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
