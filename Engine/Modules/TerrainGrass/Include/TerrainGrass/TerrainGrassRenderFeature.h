#pragma once

#include "Types/ScopedSubscription.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/Pipeline/PipelineIdentifiers.h"
#include "Rendering/Core/RecomputeElision.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TerrainGrass/GrassPlacementModel.h"
#include "TerrainGrass/TerrainGrassPlacementStats.h"
#include "Types/Types.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace GameEngine::Engine::Renderer
{
class Material;
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::TerrainECS
{
class TerrainRenderFeature;
class TerrainPageFeature;
} // namespace GameEngine::TerrainECS

namespace GameEngine::TerrainGrass
{

class TerrainGrassForwardContributor;
struct SurfacePageSource;
struct TerrainGrassForwardContributorDeleter
{
    void operator()(TerrainGrassForwardContributor* ptr) const;
};

class TerrainGrassRenderFeature final : public Engine::Renderer::IRenderFeature
{
public:
    ~TerrainGrassRenderFeature() override;

    bool Initialize(Rendering::IDevice* device);
    bool IsInitialized() const { return m_Initialized; }

    void EnsureForwardContributor(Engine::Renderer::RenderServices& rs,
                                  TerrainECS::TerrainRenderFeature& terrainFeature);
    // Whether this frame's grass draws for `viewId` carry their camera-prepass heads
    // (GrassPrepassHead), so the node declares the prepass's reads of the placement buffers.
    bool DrawsPrepassHeads(Rendering::ViewId viewId) const;
    void DispatchPlacementForView(Rendering::RenderGraph::RGContext& ctx,
                                  Engine::Renderer::RenderServices& rs,
                                  TerrainECS::TerrainRenderFeature& terrainFeature,
                                  Rendering::ViewId viewId,
                                  uint32 frameIndex);
    // Asks the device (IDevice::RequestComputePipeline) for the classify, plan and place pipelines that
    // each placement dispatch uses. The backend builds run off the calling thread. Main thread.
    void RequestPlacementPipelines(Engine::Renderer::RenderServices& rs);

    // One sub-mesh of the shared blade VB/IB, one per LOD. The placement compute picks a LOD per
    // cell; each LOD is drawn by its own indirect record over the range that record selects in the
    // shared instance pool.
    struct BladeLodMesh
    {
        uint32 IndexCount = 0;
        uint32 FirstIndex = 0;
        int32 VertexOffset = 0;

        bool operator==(const BladeLodMesh&) const = default;
    };

    Rendering::BufferHandle GetBladeVB() const { return m_BladeVB; }
    Rendering::BufferHandle GetBladeIB() const { return m_BladeIB; }
    const BladeLodMesh& GetBladeLod(uint32 lod) const { return m_BladeLods[lod % kGrassLodCount]; }
    Rendering::BufferHandle GetInstanceBuffer(Rendering::ViewId viewId, uint32 frameIndex) const;
    Rendering::BufferHandle GetIndirectArgsBuffer(Rendering::ViewId viewId, uint32 frameIndex) const;
    Rendering::BufferHandle GetIndirectCountBuffer(Rendering::ViewId viewId, uint32 frameIndex) const;
    uint32 GetInstanceCapacity(Rendering::ViewId viewId, uint32 frameIndex) const;
    // The draw-side twin of the placement dispatch's fit scale. The vertex stage derives the far
    // LOD band from it, so the band ends where the field ends rather than where it was authored.
    Rendering::BufferHandle GetDrawParamsUBO(Rendering::ViewId viewId, uint32 frameIndex) const;

    // The plan the placement dispatch was fitted to for this view/frame: the pool capacity the draws
    // bind and the range the budget fit settled on. Resolved once at graph DECLARATION so the draw
    // and the dispatch cannot disagree about the pool.
    const GrassPlacementPlan& GetPlan(Rendering::ViewId viewId, uint32 frameIndex) const;

    // RenderGraph: ensure the per-view placement buffers exist and resolve this view's plan at
    // DECLARATION, so the node can ImportExternalBuffer them before the placement pass writes them.
    // Returns true when all three handles are valid. Also flushes the deferred-destroy ring (the
    // once-per-frame retirement).
    bool EnsurePlacementBuffersForViewRG(Engine::Renderer::RenderServices& rs,
                                         TerrainECS::TerrainRenderFeature& terrainFeature,
                                         Rendering::ViewId viewId, uint32 frameIndex,
                                         uint32 activeGrassCount);

    // True when this (view, frame slot) may keep the placement it already holds: every input the
    // two kernels read is byte-identical to the last time this slot was visited, so re-running
    // would reproduce the retained buffers exactly. Call ONCE per view per frame, from the
    // declaration — it advances the gate's own frame stamp.
    //
    // Must run after EnsurePlacementBuffersForViewRG, which is what resolves the plan and the GPU
    // params this compares.
    // `pages` may be null (no page feature yet).
    bool ShouldSkipPlacement(TerrainECS::TerrainRenderFeature& terrainFeature,
                             const TerrainECS::TerrainPageFeature* pages, Rendering::ViewId viewId,
                             uint32 frameIndex);

    // Per-view instance ceiling. Placement is camera-relative, so this is a property of the VIEW,
    // not of how many terrains exist or how large they are: one budget buys one view's grass at
    // whatever density the author asked for, and over-subscription shortens range rather than
    // thinning the near field (GrassFitPlacementToBudget).
    static constexpr uint32 kMaxInstancesPerView = 524288u;

    // std140 mirror of GrassPlaceParamsBuffer in terrain_grass_place.comp.
    struct alignas(16) GrassPlaceParamsGPU
    {
        float FrustumPlanes[6][4] = {};
        float CameraPos[4] = {};   // xyz world camera, w = range scale from the budget fit
        int32 CellWindow[4] = {};  // xy camera cell, z cells per side, w terrain params count
        float Bounds[4] = {};      // x world min Y, y world max Y, z LOD1 start, w max range
        // The three COLUMNS of the upper-left 3x3 of this view's view matrix (CameraData::view),
        // the projection's Y scale in ViewRotCol2[3], and this view's viewport height in pixels in
        // ViewRotCol0[3]. The placement compute turns a blade's world width into a screen width
        // with those three numbers, so it can size the minimum projected width once per blade
        // instead of once per vertex. Only the rotation: positions are taken relative to the
        // camera, so the translation never enters.
        //
        // The viewport height belongs in this block because it is per-VIEW data like the rest of
        // it: two views drawn in one frame have two viewports, and a width sized for the wrong one
        // is wrong on screen.
        float ViewRotCol0[4] = {}; // [3] = viewport height in pixels, 0 = no world pass yet
        float ViewRotCol1[4] = {}; // [3] = viewport height for the ground footprint, negated when orthographic
        float ViewRotCol2[4] = {}; // [3] = projection Y scale (CameraData::proj[5], = uP[1][1])
        // Worst case across every active grass terrain: [0] = densest blades/m² at the camera, [1] = flattest
        // falloff, [2] = thinnest blade (0 when a terrain draws cards), [3] = shortest blade. Terms that
        // dominate every terrain make the budget fit's per-ring total a bound on the per-cell scan.
        float Summary[4] = {};
        // This view's render origin: xyz = sector, [3] = sector size in metres. The placement
        // compute anchors the root colour's projection UVs to it, as the draw's stages do.
        int32 RenderOriginSector[4] = {};
        // x = widest blade over every active grass terrain, for the same bound; y = the lowest far-thinning dial.
        Mathematics::Vector2 FloorThinning{};
        // Meters: the farthest any blade vertex lies from its root over every active grass terrain. The
        // classify kernel grows each cell's own box by it on every side.
        float BladeReach = 0.0f;
        // Meters: the tallest authored blade over every active grass terrain. The range fit's band box
        // takes it above the highest ground.
        float MaxBladeHeight = 0.0f;
    };
    static_assert(sizeof(GrassPlaceParamsGPU) == 240);

    // std140 mirror of GrassDrawParamsBuffer in terrain_grass_vertex_modifier.glsl.
    //
    // The budget fit shortens the authored range, and BOTH stages have to agree on the result or
    // they describe different fields: placement spawns out to the fitted radius while the vertex
    // stage's distance band still ramps toward the authored one, so the outermost blades never
    // finish blending into the ground and the field ends on a visible step. The compute reads the
    // same scale out of GrassPlaceParamsGPU::CameraPos[3]; this is the draw's copy of it.
    struct alignas(16) GrassDrawParamsGPU
    {
        float RangeFitScale = 1.0f; // plan.Params.FarRadius / summary.MaxRange; 1 = fit did nothing
        float _Pad[3] = {};
    };
    static_assert(sizeof(GrassDrawParamsGPU) == 16);

    // Placed-blade instrumentation. The placement compute writes its atomicAdd totals into
    // the indirect args' InstanceCount; the node copies that word into a per-view
    // readback slot each frame so the count is answerable off the GPU.
    TerrainGrassPlacementStats& PlacementStats() { return m_PlacementStats; }
    const TerrainGrassPlacementStats& PlacementStats() const { return m_PlacementStats; }

    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override;
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override;
    void OnDeviceRebuilt(Rendering::IDevice* device) override;

private:
    struct PlacementBuffers;

    void EnsureBladeSegments(uint32 bladeSegments);
    void CreateBladeMesh(uint32 bladeSegments);
    bool EnsureComputePipeline(Engine::Renderer::RenderServices& rs);
    // Compiles the three placement programs and interns their pipelines, once.
    bool InternComputePipelines(Engine::Renderer::RenderServices& rs);
    PlacementBuffers* EnsurePlacementBuffers(Rendering::ViewId viewId, uint32 frameIndex);
    // Bring a slot's CPU-owned indirect fields up to date: each LOD's sub-mesh record, the shared
    // pool size and the draw count. They describe the blade mesh and the budget rather than the
    // frame, so this records fills only when the slot has never held them or one of them has moved.
    // Every range it writes is disjoint from the per-frame clear, so the two groups need no
    // ordering against each other.
    void EnsureIndirectConstants(Rendering::CommandList& cmd, PlacementBuffers& buffers) const;
    // Fit this view's authored placement to the per-view budget. Pure in (camera, terrain summary),
    // so calling it from declaration and from exec yields the same plan.
    GrassPlacementPlan ResolvePlan(Engine::Renderer::RenderServices& rs,
                                   TerrainECS::TerrainRenderFeature& terrainFeature,
                                   Rendering::ViewId viewId, uint32 paramsSlot) const;
    // This view's GPU placement inputs. Resolved at DECLARATION so the elision gate can compare
    // them before deciding whether to add the pass at all; the exec only uploads the result.
    // Returns false when the view has no camera, which is the one input that cannot be defaulted —
    // placement is camera-relative.
    bool ResolvePlaceParams(Engine::Renderer::RenderServices& rs,
                            TerrainECS::TerrainRenderFeature& terrainFeature,
                            Rendering::ViewId viewId, uint32 paramsSlot, uint32 terrainParamsCount,
                            const GrassPlacementPlan& plan, GrassPlaceParamsGPU& out) const;
    // Placement-dispatch body over a raw command list + device; DispatchPlacementForView
    // is its only caller.
    void DispatchPlacementBody(Rendering::CommandList* cmd, Rendering::IDevice* device,
                               Engine::Renderer::RenderServices& rs,
                               TerrainECS::TerrainRenderFeature& terrainFeature,
                               Rendering::ViewId viewId, uint32 frameIndex);
    void DestroyGpuResources();

    // GPU-safe deferred buffer destruction: a placement buffer that grew may
    // still be in flight on the GPU, so it is retired for a few frames before
    // the actual DestroyBuffer (the immediate destroy on growth was unsafe).
    // Flushed from BOTH arms' per-view entry points; the retirement clock
    // advances only when the DEVICE frame index changes, so per-view and
    // per-arm calls within one frame cannot shrink the in-flight window.
    void DeferBufferDestroy(Rendering::BufferHandle buf);
    void FlushDeferredDestroys();

    Rendering::IDevice* m_Device = nullptr;
    bool m_Initialized = false;

    Rendering::BufferHandle m_BladeVB;
    Rendering::BufferHandle m_BladeIB;
    std::array<BladeLodMesh, kGrassLodCount> m_BladeLods{};
    uint32 m_BladeSegments = 0;

    // Frame-slot rings below are indexed directly by IDevice::GetFrameIndex(), which every
    // backend keeps below this bound.
    static constexpr uint32 kMaxFrames = Rendering::IDevice::kMaxSupportedFramesInFlight;

    // The paged terrain's surface params, one per frame slot (placement binding 4). Binding 5 takes the
    // page feature's table ring slot, or m_DefaultPageTable (one word) while no terrain is paged.
    Rendering::BufferHandle m_SurfaceParamsUBO[kMaxFrames] = {};
    Rendering::BufferHandle m_DefaultPageTable;

    // What the placement binds for the paged terrain this frame.
    struct SurfaceBinding
    {
        Rendering::BufferHandle ParamsUBO;
        Rendering::BufferHandle Table;
        uint64 TableOffset = 0;
        uint64 TableBytes = 0;
        Rendering::TextureHandle HeightCache;
        std::array<Rendering::TextureHandle, 4> Planes{}; // splat, normal, grass control, color
    };
    // The drawn terrain's page tables and caches from the page feature; false when its height is not
    // paged (the wide arm only: the narrow arm publishes no table).
    static bool ReadSurfacePageSource(const TerrainECS::TerrainPageFeature& pages,
                                      const TerrainECS::TerrainRenderFeature& terrainFeature,
                                      SurfacePageSource& source, SurfaceBinding* textures);
    // Writes this frame slot's params from `source`, binds the page feature's table ring slot, and
    // returns what to bind (the one-word default table and invalid textures when no terrain is paged).
    SurfaceBinding EnsureSurfaceBinding(TerrainECS::TerrainPageFeature* pages, const SurfacePageSource& source,
                                        const SurfaceBinding& textures, uint32 frameIndex);

    Rendering::ComputePipelineId m_ComputePipelineId{};
    Rendering::PipelineHandle m_ComputePipeline{};
    // The parallel half of the plan: one invocation per cell decides visibility, terrain and ring.
    Rendering::ComputePipelineId m_ClassifyPipelineId{};
    Rendering::PipelineHandle m_ClassifyPipeline{};
    // The serial half: fits the density radius to the pool against the classified cells and hands
    // each one a contiguous slot range. Both draw records are known when it finishes, so nothing
    // has to close the pool afterwards.
    Rendering::ComputePipelineId m_PlanPipelineId{};
    Rendering::PipelineHandle m_PlanPipeline{};
    bool m_ComputePipelinesInternAttempted = false;
    bool m_ComputePipelineAttempted = false;

    // What EnsureIndirectConstants writes into the args block: index count, first index and vertex
    // offset for each LOD, then the shared pool size.
    static constexpr uint32 kSeededIndirectWordsPerLod = 3u;
    static constexpr uint32 kGrassSeededIndirectWordCount =
        kGrassLodCount * kSeededIndirectWordsPerLod + 1u;

    // Holds GrassPlaceParamsGPU by value, whose std140 alignment pads this struct. That padding
    // is the point of the member, not an accident, so the warning is suppressed the way the
    // engine's other alignment-bearing aggregates suppress it.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct PlacementBuffers
    {
        Rendering::BufferHandle Instances;
        Rendering::BufferHandle IndirectArgs;
        Rendering::BufferHandle IndirectCount;
        // Host-written only while the render node declares or executes, behind
        // IDevice::BeginFrame, so a ring of the device's pacing suffices; FrameSlotted.
        Rendering::BufferHandle PlaceParamsUBO;
        Rendering::BufferHandle DrawParamsUBO;
        // Per-cell slot ranges, written by the plan dispatch and read by the emit dispatch in the
        // same pass. Per frame slot like the rest, so a frame in flight never reads the plan the
        // next one is writing.
        Rendering::BufferHandle CellPlan;
        uint32 Capacity = 0;
        GrassPlacementPlan Plan{};
        // The terrain params ring slot Plan and Place were resolved against. The exec binds this
        // slot rather than re-reading the ring, so the array the kernels index is the array the
        // budget bound was computed over.
        uint32 ParamsSlot = 0;
        // This view's GPU inputs, resolved at DECLARATION for the same reason Plan is: the elision
        // gate below compares them before the pass is added, and the exec only uploads them.
        GrassPlaceParamsGPU Place{};

        // Idle elision, per (view, FRAME SLOT) rather than per view. What a skip retains is THIS
        // slot's buffers, last written the last time this slot was visited, so comparing this
        // slot's inputs against its own previous ones is a local, provable invariant; a per-view
        // gate would additionally have to prove every slot in the ring had been written under the
        // settled inputs. Stamped with the slot's own visit counter for the same reason.
        Rendering::RecomputeElisionGate Gate;
        uint64 VisitStamp = 0;
        bool GateLogState = false;
        // Hoisted so the per-frame blob build does not allocate.
        std::vector<uint8> TerrainParamsScratch;
        std::vector<uint8> TerrainMaterialScratch;
        // The words EnsureIndirectConstants seeds into the args block, as a span source for the
        // same reason. Gathered as words rather than as the struct below, because the gate compares
        // raw bytes and a struct's padding bytes are indeterminate.
        std::array<uint32, kGrassSeededIndirectWordCount> SeededIndirectScratch{};

        // The indirect fields the CPU owns — each LOD's sub-mesh record and the shared pool size —
        // as last written into this slot's buffer. They describe the blade mesh and the budget, not
        // the frame, so the dispatch compares them by value and re-seeds only when the blade mesh is
        // rebuilt or the fit lands on a different capacity. `Written` distinguishes a freshly
        // created slot, whose device-local buffer holds nothing, from one seeded with these values;
        // it is cleared with the rest of the slot when the buffers are recreated.
        struct IndirectConstants
        {
            bool Written = false;
            std::array<BladeLodMesh, kGrassLodCount> Lods{};
            uint32 Capacity = 0;

            bool operator==(const IndirectConstants&) const = default;
        };
        IndirectConstants SeededConstants{};
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    struct ViewPlacementScratch
    {
        std::array<PlacementBuffers, kMaxFrames> Frames;
    };
    std::unordered_map<Rendering::ViewId, ViewPlacementScratch> m_PlacementScratch;

    // The last budget-fit result announced at INFO. The fit runs every frame for every view, so the
    // log needs a memory or it would repeat once a frame; keying on inputs AND output rather than a
    // once-per-process latch means an author who edits the density or the range is told the new
    // answer instead of only the first one.
    struct RangeFitLogKey
    {
        float32 NearDensity = -1.0f;
        float32 AuthoredRange = -1.0f;
        float32 Falloff = -1.0f;
        float32 FittedRange = -1.0f;

        bool operator==(const RangeFitLogKey& other) const
        {
            return NearDensity == other.NearDensity && AuthoredRange == other.AuthoredRange
                && Falloff == other.Falloff && FittedRange == other.FittedRange;
        }
    };
    // Mutable: ResolvePlan is const because it derives a plan rather than mutating state, and this
    // is log bookkeeping, not part of that derivation.
    mutable RangeFitLogKey m_LastRangeFitLogged{};

    std::unique_ptr<TerrainGrassForwardContributor, TerrainGrassForwardContributorDeleter> m_ForwardContributor;
    Engine::Renderer::RenderServices* m_RegisteredOn = nullptr;
    ScopedSubscription m_ForwardProducer;

    // Deferred buffer destruction (DeferBufferDestroy/FlushDeferredDestroys).
    // m_MonotonicFrame is the feature's own retirement clock (the device
    // frame index cycles and would wrap the subtraction); it advances only
    // when the device frame index CHANGES across flush calls.
    struct DeferredBufferDestroy
    {
        Rendering::BufferHandle Buffer;
        uint32 FrameRetired = 0;
    };
    std::vector<DeferredBufferDestroy> m_DeferredDestroys;
    uint32 m_MonotonicFrame = 0;
    uint32 m_LastFlushDeviceFrame = 0xFFFFFFFFu;

    TerrainGrassPlacementStats m_PlacementStats;
};

} // namespace GameEngine::TerrainGrass
