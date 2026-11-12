#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Types/Types.h"

#include "MetalArgumentBufferLayout.h"
#include "MetalHdrOutput.h"
#include "MetalShaderTranslator.h"
#include "MetalShippedLibraries.h"
#include "MetalSubmitQueue.h"
#include "MetalTransientDescriptorArena.h"
#include "Types/FlatMap.h"
#include "Types/InlineVector.h"

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <dispatch/dispatch.h>

#include <array>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class MetalSwapchain;
class MetalCommandList;
class MetalQueryPool;
class MetalAccelerationStructures;
class MetalIndirectCountResources;

struct MetalBuffer
{
    MTL::Buffer* buffer = nullptr; // owned (+1)
    size_t size = 0;
    BufferUsage usage = BufferUsage::None;
    std::string debugName;
};

struct MetalTexture
{
    MTL::Texture* texture = nullptr; // owned (+1) unless swapchain slot
    TextureFormat format = TextureFormat::Unknown;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    uint32_t sampleCount = 1;
    TextureUsage usage = TextureUsage::None;
    // Swapchain slots alias the current drawable's texture; the drawable owns
    // it and the slot must never release it.
    bool isSwapchainSlot = false;
    std::string debugName;
};

struct MetalSampler
{
    MTL::SamplerState* sampler = nullptr; // owned (+1)
    std::string debugName;
};

struct MetalTextureView
{
    MTL::Texture* view = nullptr; // owned (+1)
    TextureFormat format = TextureFormat::Unknown;
    std::string debugName;
};

struct MetalSemaphore
{
    MTL::SharedEvent* event = nullptr; // owned (+1)
};

// One resident of a descriptor set as the command list declares it to the
// encoder: the resource and whether a shader may write it.
struct MetalCompactResident
{
    MTL::Resource* Resource = nullptr;
    bool Writable = false;
};

// One descriptor set = one Metal 3 argument table (Tier2, raw 8-byte slots)
// plus the residency list the command list passes to useResource.
//
// The table holds one entry per resource slot and, at
// layout.SizeConstantsSlot, the GPU address of the per-slot byte sizes that
// shaders calling .length() on runtime arrays read (SPIRV-Cross buffer-size
// constants). Those sizes follow the entries in the same memory, so binding
// the table makes them resident too. A persistent set owns a buffer of its
// own; a transient set's table is a range of a page in its frame slot's
// MetalTransientDescriptorArena, valid until that slot is recycled.
struct MetalDescriptorSet
{
    MTL::Buffer* argumentBuffer = nullptr; // owned (+1) only when ownsArgumentBuffer
    size_t argumentOffset = 0;             // byte offset of the table in argumentBuffer
    bool ownsArgumentBuffer = false;
    uint64_t* entries = nullptr;           // CPU view of the table (shared storage)
    uint32_t* sizeConstants = nullptr;     // CPU view of the sizes, one per resource slot
    // The device keeps one layout for each set layout desc. Every set made from that desc points at it.
    const MetalArgumentBufferLayout* layout = nullptr;
    // Indexed by slot id; nullptr where nothing is bound (or for samplers,
    // which need no residency).
    InlineVector<MTL::Resource*, 8> residentResources;
    InlineVector<uint8_t, 8> residentWritable; // parallel: needs Write usage
    // Parallel: a storage binding of a buffer created with BufferUsage::Indirect
    // (draw records or counts), read-only or not.
    InlineVector<uint8_t, 8> residentIndirectArguments;
    // Compacted residents rebuilt lazily after updates so binding a sparse
    // bindless array doesn't walk thousands of empty slots.
    InlineVector<MetalCompactResident, 8> compactResidents;
    // Rebuilt with compactResidents: a storage binding holds an indirect-argument buffer, so a render
    // pass that binds the set may rewrite draw records or counts. A read-only binding counts too, so a
    // layout that marks a written binding read-only does not make a later pass draw from stale counts.
    bool writesIndirectArguments = false;
    // Read-only residents that occupy no argument slot of their own: the BLAS
    // objects a bound TLAS instances. Metal makes neither the TLAS's children
    // nor anything else reachable-but-unbound resident implicitly.
    std::vector<MTL::Resource*> extraResidentReads;
    bool residentsDirty = true;
    bool transient = false;
    std::string debugName;

    void RebuildCompactResidents()
    {
        compactResidents.Clear();
        writesIndirectArguments = false;
        for (size_t slot = 0; slot < residentResources.Size(); ++slot)
        {
            if (residentResources[slot] != nullptr)
            {
                compactResidents.PushBack({residentResources[slot], residentWritable[slot] != 0});
                if (residentIndirectArguments[slot] != 0)
                {
                    writesIndirectArguments = true;
                }
            }
        }
        for (MTL::Resource* resource : extraResidentReads)
        {
            compactResidents.PushBack({resource, false});
        }
        residentsDirty = false;
    }

    // Moves the storage of the residents and the name into an empty set, so that a later set
    // with the same slot count can use it without an allocation.
    MetalDescriptorSet ReleaseStorage()
    {
        MetalDescriptorSet released;
        released.residentResources = std::move(residentResources);
        released.residentWritable = std::move(residentWritable);
        released.residentIndirectArguments = std::move(residentIndirectArguments);
        released.compactResidents = std::move(compactResidents);
        released.extraResidentReads = std::move(extraResidentReads);
        released.debugName = std::move(debugName);
        released.residentResources.Clear();
        released.residentWritable.Clear();
        released.residentIndirectArguments.Clear();
        released.compactResidents.Clear();
        released.extraResidentReads.clear();
        released.debugName.clear();
        return released;
    }
};

struct MetalPipeline
{
    MTL::RenderPipelineState* renderPipeline = nullptr;   // owned (+1)
    MTL::ComputePipelineState* computePipeline = nullptr; // owned (+1)
    MTL::DepthStencilState* depthStencilState = nullptr;  // owned (+1)
    PipelineType type = PipelineType::Graphics;
    // MeshFragment pipeline: DrawMeshTasks dispatches mesh threadgroups using
    // localSize{X,Y,Z} (the mesh stage's workgroup size) as threads-per-mesh.
    bool isMeshPipeline = false;

    // Attachment formats the PSO was specialized for. Vulkan tolerates a
    // stale pipeline bind carried across render passes (only draws validate);
    // Metal validates at setRenderPipelineState, so the command list checks
    // this against the current pass before reapplying a sticky pipeline.
    PipelineFormatKey formatKey{};

    // Encoder-time state Metal keeps outside the PSO.
    MTL::PrimitiveType primitiveType = MTL::PrimitiveTypeTriangle;
    MTL::CullMode cullMode = MTL::CullModeNone;
    MTL::Winding winding = MTL::WindingCounterClockwise;
    MTL::TriangleFillMode fillMode = MTL::TriangleFillModeFill;
    bool depthBiasEnable = false;
    float depthBiasConstant = 0.0f;
    float depthBiasSlope = 0.0f;
    float depthBiasClamp = 0.0f;
    // Vulkan's depthClampEnable. Directional shadows rely on it: casters in
    // front of the shadow camera's near plane are pancaked onto that plane
    // rather than clipped away, which is what keeps them casting at all.
    MTL::DepthClipMode depthClipMode = MTL::DepthClipModeClip;
    MTL::CompareFunction depthCompare = MTL::CompareFunctionAlways;

    // Compute: SPIR-V workgroup size captured at translation time (zero for
    // hand-written MSL, where Dispatch falls back to the PSO's execution width).
    uint32_t localSizeX = 0;
    uint32_t localSizeY = 0;
    uint32_t localSizeZ = 0;

    // The argument-buffer layouts this pipeline was translated against, by set index: the device's
    // shared layouts, which sets made from the same desc point at too. Read by the
    // GE_METAL_VALIDATE_BINDINGS bind-time check.
    std::vector<const MetalArgumentBufferLayout*> setLayouts;
    // Debug builds only: the storage bindings that a stage can write. A set bound with this pipeline must
    // declare each one writable, because Metal does not track hazards for a write through a Read declaration.
    std::vector<MetalWrittenBinding> writtenStorage;

    uint32_t pushConstantSize = 0;
    uint32_t pushConstantStagesMask = 0;
    struct PushRange
    {
        std::string name;
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t stagesMask = 0;
    };
    std::vector<PushRange> pushRanges;
    std::string debugName;
};

class MetalDevice : public IDevice
{
    friend class MetalSwapchain;
    friend class MetalCommandList;

  public:
    MetalDevice();
    ~MetalDevice() override;

    // Device management
    bool Initialize(const DeviceDesc& desc) override;
    void Shutdown() override;
    const RenderingDeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }
    GraphicsAPI GetAPI() const override { return GraphicsAPI::Metal; }
    std::string GetHardwareDescription() const override;

    // Shader stages taken from the shipped libraries, and stages compiled from MSL here, since start.
    struct StageLibraryCounts
    {
        uint32_t Shipped = 0;
        uint32_t Compiled = 0;
    };
    StageLibraryCounts GetStageLibraryCounts() const;

    // Push constant introspection
    uint32_t GetPipelinePushConstantRangeCount(PipelineHandle pipeline) const override;
    bool GetPipelinePushConstantRangeInfo(PipelineHandle pipeline, uint32_t id, PushConstantRangeInfo& outInfo) const override;
    bool FindPipelinePushConstantRangeId(PipelineHandle pipeline, const char* name, uint32_t& outId) const override;
    bool GetPipelinePushConstantInfo(PipelineHandle pipeline, PipelinePushConstantInfo& outInfo) const override;

    // Queues. Metal queues have no family semantics; all report family 0 so
    // barrier queue-ownership transfers degrade to no-ops.
    uint32_t GetGraphicsQueueFamilyIndex() const override { return 0; }
    uint32_t GetComputeQueueFamilyIndex() const override { return 0; }
    uint32_t GetTransferQueueFamilyIndex() const override { return 0; }

    // Resources
    BufferHandle CreateBuffer(const BufferDesc& desc) override;
    TextureHandle CreateTexture(const TextureDesc& desc) override;
    SamplerHandle CreateSampler(const SamplerDesc& desc) override;
    TextureViewHandle CreateTextureView(TextureHandle texture, const TextureViewDesc& desc) override;
    void DestroyTextureView(TextureViewHandle handle) override;
    PipelineHandle CreateConcreteGraphicsPipeline(const GraphicsPipelineDesc& gd, const PipelineFormatKey& fk) override;
    PipelineHandle CreateConcreteComputePipeline(const ComputePipelineDesc& cd) override;

    void DestroyBuffer(BufferHandle handle) override;
    void DestroyTexture(TextureHandle handle) override;
    void DestroySampler(SamplerHandle handle) override;
    void DestroyPipeline(PipelineHandle handle) override;

    // Debug/VRAM-panel allocation stats. Counts are live registry entries;
    // bytes sum owned buffer sizes + estimated texture footprints (swapchain
    // slots alias the drawable and own no memory, so they are excluded).
    size_t DebugGetAllocationCount() const override;
    size_t DebugGetBufferRegistryCount() const override;
    size_t DebugGetImageRegistryCount() const override;
    size_t DebugGetAllocatedBytes() const override;
    size_t DebugGetTextureBytes() const override;
    size_t DebugGetBufferBytes() const override;
    void DebugEnumerateResources(const std::function<void(const DebugResourceInfo&)>& fn) const override;
    bool GetLastFrameSyncTimings(FrameSyncTimings& out) const override;

    // Registry-backed liveness (the base defaults only check handle.IsValid(),
    // which is wrong under double-linked module copies — see Device.h).
    bool IsTextureAlive(TextureHandle texture) const override;
    bool IsTextureHandleLive(TextureHandle texture) const override;
    bool IsPipelineAlive(PipelineHandle pipeline) const override;
    // Real format check: a format the engine can't map to an MTLPixelFormat is
    // unsupported (base default unconditionally returns true).
    bool IsTextureFormatSupported(TextureFormat format, uint32_t usageFlags) const override;
    void PrintCapabilityReport() const override;

    void* MapBuffer(BufferHandle handle) override;
    void UnmapBuffer(BufferHandle handle) override;
    void UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) override;
    void UpdateBufferRanges(BufferHandle handle, std::span<const BufferUpdateRange> ranges) override;
    uint64_t GetBufferDeviceAddress(BufferHandle handle) override;
    BufferMemoryResidency GetBufferMemoryResidency(BufferHandle handle) const override;

    // Synchronization (timeline semaphores backed by MTLSharedEvent)
    SemaphoreHandle CreateTimelineSemaphore(uint64_t initialValue = 0) override;
    void DestroySemaphore(SemaphoreHandle handle) override;
    bool GetTimelineSemaphoreValue(SemaphoreHandle handle, uint64_t& outValue) const override;
    bool WaitTimelineSemaphoreValue(SemaphoreHandle handle, uint64_t value, uint64_t timeoutNs = ~0ull) override;
    bool QueueSubmit(QueueType queue,
                     const std::vector<CommandList*>& cmdLists,
                     const std::vector<std::pair<SemaphoreHandle, uint64_t>>& waitSemaphores,
                     const std::vector<std::pair<SemaphoreHandle, uint64_t>>& signalSemaphores) override;
    GpuSyncToken SubmitTextureUploads(const TextureUploadRequest* requests, uint32_t count) override;
    bool IsPreviousFrameGraphicsComplete() const override;

    // Command lists & submission
    std::unique_ptr<CommandList> CreateCommandList(QueueType queue) override;
    void ExecuteCommandLists(const std::vector<CommandList*>& commandLists) override;
    void WaitForIdle() override;

    // Frame loop
    bool BeginFrame() override;
    void Present() override;
    void FinalizeFrame() override;
    uint32_t GetFramesInFlight() const override { return kFramesInFlight; }
    void SetVsync(bool vsync) override;
    bool IsVsyncEnabled() const override { return m_Desc.vsync; }
    HdrOutputState GetHdrOutputState() const override { return m_HdrState; }
    bool SetHdrOutputMode(HdrOutputMode mode, const HdrStaticMetadata* metadata = nullptr,
                          HdrSwapchainBitDepth bitDepth = HdrSwapchainBitDepth::Bit10) override;
    uint32_t GetFrameIndex() const override { return m_FrameIndex.load(std::memory_order_relaxed); }

    // Window targets / swapchain
    WindowTargetHandle CreateWindowTarget(void* windowHandle, uint32_t width, uint32_t height) override;
    bool DestroyWindowTarget(WindowTargetHandle target) override;
    bool SetActiveWindowTarget(WindowTargetHandle target) override;
    WindowTargetHandle GetActiveWindowTarget() const override { return m_ActiveWindowTarget; }
    bool RecreateWindowTargetSwapchain(WindowTargetHandle target, uint32_t width, uint32_t height) override;
    bool GetWindowTargetSize(WindowTargetHandle target, uint32_t& outWidth, uint32_t& outHeight) const override;
    bool GetSwapchainSize(uint32_t& outWidth, uint32_t& outHeight) const override;
    bool AcquireNextImage(uint32_t& imageIndex) override;
    bool PresentImage(uint32_t imageIndex) override;
    uint32_t GetSwapchainImageCount() const override;
    TextureHandle GetSwapchainImage(uint32_t index) const override;
    TextureHandle GetCurrentSwapchainImageHandle() override;
    TextureFormat GetSwapchainTextureFormat() const override;
    bool SwapchainSupportsReadback() const override { return true; }

    // Managers (none yet for Metal)
    ResourceManager* GetResourceManager() override { return nullptr; }
    IQueryPool* GetQueryPool() override;

    // Descriptor sets (milestone 3)
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& desc) override;
    void UpdateDescriptorSet(DescriptorSetHandle descriptorSet, const DescriptorSetUpdate& update) override;
    void DestroyDescriptorSet(DescriptorSetHandle descriptorSet) override;

    // Texture introspection
    TextureFormat GetTextureFormat(TextureHandle texture) const override;
    uint32_t GetTextureSampleCount(TextureHandle texture) const override;
    uint32_t GetTextureArrayLayers(TextureHandle texture) const override;
    void GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) const override;

    IAccelerationStructureBackend* GetAccelerationStructureBackend() override;

    // Internal accessors for the command list / swapchain
    MTL::Device* GetMTLDevice() const { return m_Device; }
    MTL::CommandQueue* GetQueue(QueueType queue) const;
    MetalBuffer* GetMetalBuffer(BufferHandle handle);
    MetalTexture* GetMetalTexture(TextureHandle handle);
    MetalTextureView* GetMetalTextureView(TextureViewHandle handle);
    MetalPipeline* GetMetalPipeline(PipelineHandle handle);
    const MetalPipeline* GetMetalPipeline(PipelineHandle handle) const;
    MetalDescriptorSet* GetMetalDescriptorSet(DescriptorSetHandle handle);

    // Lazily-built internal compute pipeline implementing the u32 FillBuffer
    // contract (Metal blit fills are byte-granular).
    MTL::ComputePipelineState* GetFillBufferPipeline();
    // Render-path mip blit (sample src mip view, draw into dst mip view) for
    // the scaled copies MTLBlitCommandEncoder cannot do. One pipeline per
    // color pixel format; nullptr for depth/integer formats.
    MTL::RenderPipelineState* GetMipBlitPipeline(MTL::PixelFormat format);
    MTL::SamplerState* GetMipBlitSampler();

    // Always-pass/write depth-stencil state for the GE_METAL_FORCE_DEPTH_ALWAYS
    // bisection switch.
    MTL::DepthStencilState* GetDebugAlwaysDepthState();

    // Write-disabled variant for read-only depth passes (Vulkan parity: the
    // Vulkan backend forces depth writes off dynamically in such passes).
    MTL::DepthStencilState* GetReadOnlyDepthState(MTL::CompareFunction compare);

    // Probe helper: resolve a GPU virtual address back to the owning shared
    // buffer (linear walk; debug paths only). Returns nullptr if unknown.
    MTL::Buffer* FindBufferByGpuAddress(uint64_t address, uint64_t& outOffset);

    // GE_METAL_GPU_CAPTURE=1: arm a one-frame programmatic GPU capture on the
    // next BeginFrame; the .gputrace is written to /tmp for Xcode analysis.
    void ArmGpuCaptureIfRequested();
    void EndGpuCaptureIfActive();

    // Declares on the encoder each buffer that shaders read or write through a device address. Metal sees
    // no binding for such a buffer. The function records each buffer and its usage in `residentCache`.
    // With a residency set, the function does nothing.
    void DeclareDeviceAddressResidency(MTL::RenderCommandEncoder* encoder,
                                       FastHashMap<MTL::Resource*, MTL::ResourceUsage>& residentCache);
    void DeclareDeviceAddressResidency(MTL::ComputeCommandEncoder* encoder,
                                       FastHashMap<MTL::Resource*, MTL::ResourceUsage>& residentCache);

    // Residency-set / fence path (GE_METAL_RESIDENCY_SET). When enabled, command
    // lists drive the compute->draw hazard through this fence instead of the
    // blanket useResources tracking. Returns nullptr when the path is off.
    bool UsesResidencySet() const { return m_UseResidencySet; }
    MTL::Fence* GpuDrivenFence() const { return m_GpuDrivenFence; }

    // Every live buffer created with BufferUsage::Indirect: the draw records
    // and counts a DrawIndexedIndirectCount may read. The indirect-count
    // translation reads them through GPU addresses, so its encoder declares
    // them all read-only, which both keeps them resident and orders the
    // translation after their producers.
    void DeclareIndirectArgumentReads(MTL::ComputeCommandEncoder* encoder);
    // Shared with every command list and in-flight command buffer that holds
    // its arenas; nullptr before Initialize and after Shutdown.
    std::shared_ptr<MetalIndirectCountResources> GetIndirectCountResources() const { return m_IndirectCountResources; }

    // Queue an owned Metal object for release once the current frame slot's
    // GPU work has provably completed (kFramesInFlight frames later).
    void DeferRelease(NS::Object* object);

  private:
    static constexpr uint32_t kFramesInFlight = 3;
    // Frames the GPU may still run when the main thread starts the next frame. One keeps a
    // GPU-bound loop busy; each more adds one frame of input lag. Slots stay kFramesInFlight deep.
    static constexpr uint32_t kFramesQueuedOnGpu = 1;

    // Null `resource` out of every live descriptor set's residency before its
    // deferred release. Descriptor sets hold residency as raw MTL::Resource*
    // (not retained); a long-lived set (the persistent bindless texture set, a
    // cached material set) would otherwise keep a dangling pointer that
    // BindDescriptorSet's useResources call objc_retains after the resource is
    // freed kFramesInFlight frames later. Walks all sets: destroys are rare
    // (never per-draw) and the set count is small, so this is cheaper than the
    // per-update bookkeeping a reverse resource->sets index would cost.
    void ScrubResidency(MTL::Resource* resource);

    // The argument-buffer layout of `desc`. It is computed on the first call for a desc and kept
    // for the life of the device, so a set made from the same desc again allocates nothing for it.
    // The caller holds m_ResourceMutex.
    const MetalArgumentBufferLayout& ArgumentBufferLayoutForLocked(const DescriptorSetLayoutDesc& desc);

    // Ends the frame: recycles the next slot, then moves the frame index to it.
    void SignalFrameCompletion();
    // Waits for the frame pacing and for every command buffer stamped with `slot`, then releases
    // the slot's deferred objects and transient sets. The main thread calls it at a frame's end.
    void RecycleFrameSlot(uint32_t slot);
    void DrainDeferredReleases(uint32_t slot);
    MetalSwapchain* GetSwapchain(WindowTargetHandle target);
    const MetalSwapchain* GetSwapchain(WindowTargetHandle target) const;
    MetalSubmitQueue* GetSubmitQueue(QueueType queue);
    // Stamps `buffer` with the current frame slot before it is enqueued. RecycleFrameSlot
    // waits for all buffers stamped with a slot; the pacing semaphore sees only frame ends.
    // A buffer on m_GraphicsQueue is also counted in m_GraphicsCbsInFlight.
    void TrackFrameCommandBuffer(MTL::CommandBuffer* buffer);
    // Completion handler of a tracked buffer (a Metal-owned thread).
    void RetireTrackedCommandBuffer(uint32_t slot, bool onGraphicsQueue);
    void WaitForSlotCommandBuffers(uint32_t slot);
    // Capture the frame-terminal command buffer's GPU end time (called from its
    // completion handler on a background queue) to derive frameGpuPeriodMs.
    void RecordFrameGpuTiming(MTL::CommandBuffer* buffer);
    // Commits the BDA residency set when its membership changed since the last
    // commit, so newly-created device-address buffers are resident before the
    // next command buffer executes. No-op unless the residency-set path is on.
    void CommitResidencyIfDirty();

    // Stage costs of one pipeline build, for GE_METAL_DUMP_PIPELINE_TIMING.
    struct StageBuildTiming
    {
        double TranslateMs = 0.0;
        double LibraryMs = 0.0;
        uint32_t ShippedStages = 0;
    };
    // The function of one pipeline stage: from the shipped library for its translation inputs, or
    // translated and compiled here.
    MTL::Function* NewStageFunction(const std::vector<uint8_t>& source, MetalShaderStage stage, const char* rawEntry,
                                    const std::vector<const DescriptorSetLayoutDesc*>& setLayouts,
                                    const SpecializationConstants* specialization, const std::string& debugName,
                                    StageBuildTiming& timing, MetalShaderTranslation& outTranslation);

    MetalShippedLibraries m_ShippedLibraries;
    std::atomic<uint32_t> m_ShippedStageCount{0};
    std::atomic<uint32_t> m_CompiledStageCount{0};

    MTL::Device* m_Device = nullptr;        // owned (+1)
    MTL::CommandQueue* m_GraphicsQueue = nullptr; // owned (+1)
    MTL::CommandQueue* m_ComputeQueue = nullptr;  // owned (+1)
    MTL::CommandQueue* m_TransferQueue = nullptr; // owned (+1)
    MetalSubmitQueue m_GraphicsSubmitQueue;
    MetalSubmitQueue m_ComputeSubmitQueue;
    MetalSubmitQueue m_TransferSubmitQueue;
    std::unique_ptr<MetalQueryPool> m_QueryPool;
    HdrOutputState m_HdrState{};
    // The DeviceDesc HDR request, pending until the first window target is
    // activated (empty for an SDR launch and once applied).
    std::optional<MetalHdrOutputRequest> m_StartupHdrRequest;
    std::array<std::atomic<uint32_t>, kFramesInFlight> m_SlotInFlightCbs{};
    // Tracked command buffers on m_GraphicsQueue that are enqueued and not yet
    // completed, whatever their frame slot. Counted before a buffer can reach
    // the GPU and uncounted by its completion handler, so zero means every
    // tracked graphics buffer has completed. The untracked ones (the submit
    // worker's signal-only buffer, WaitForIdle's marker) write no resource.
    std::atomic<uint32_t> m_GraphicsCbsInFlight{0};
    std::mutex m_SlotCbMutex;
    std::condition_variable m_SlotCbCv;

    // Whole-frame GPU timing, published from command-buffer completion handlers
    // (background queue) and read on the main thread via GetLastFrameSyncTimings.
    mutable std::mutex m_FrameTimingMutex;
    double m_LastFrameGpuEndTime = 0.0; // seconds (MTL::CommandBuffer::GPUEndTime)
    FrameSyncTimings m_LastFrameSync{};

    DeviceDesc m_Desc{};
    RenderingDeviceCapabilities m_Capabilities{};
    bool m_Initialized = false;

    // Frame pacing: a frame's end waits until at most kFramesQueuedOnGpu frames are on the GPU.
    // Each frame's closing command buffer (Present / FinalizeFrame) signals on completion.
    dispatch_semaphore_t m_FrameSemaphore = nullptr;
    // The last slot wait at a frame's end, and the slot's command buffers when it began.
    // Main thread only. The frame end publishes the wait; BeginFrame traces both for the slot's frame.
    double m_SlotWaitMs = 0.0;
    uint32_t m_SlotCbsAtWait = 0;
    // Read from worker threads (SubmitTextureUploads / DeferRelease) while the
    // main thread advances it in SignalFrameCompletion, so it must be atomic —
    // a torn/UB read could index the kFramesInFlight-sized slot arrays out of
    // bounds and corrupt the recycle accounting (manifests only under release
    // optimization, as a GPU page fault from recycling in-flight resources).
    std::atomic<uint32_t> m_FrameIndex{0};
    bool m_FrameSyncIssued = false;
    // Read by TrackFrameCommandBuffer, which worker threads also call.
    std::atomic<bool> m_FrameActive{false};
    NS::AutoreleasePool* m_FramePool = nullptr;

    std::array<std::vector<NS::Object*>, kFramesInFlight> m_DeferredReleases;

    mutable std::recursive_mutex m_ResourceMutex;
    GenerationalVector<MetalBuffer> m_Buffers;
    GenerationalVector<MetalTexture> m_Textures;
    GenerationalVector<MetalTextureView> m_TextureViews;
    GenerationalVector<MetalSampler> m_Samplers;
    GenerationalVector<MetalPipeline> m_Pipelines;
    GenerationalVector<MetalSemaphore> m_Semaphores;
    GenerationalVector<MetalDescriptorSet> m_DescriptorSets;
    // Keyed by HashDescriptorSetLayoutDesc and never erased. Sets point at the layouts, so each
    // one has its own allocation and stays where it is when the map grows.
    FlatMap<uint64_t, std::unique_ptr<const MetalArgumentBufferLayout>> m_ArgumentBufferLayouts;
    // The storage of destroyed sets, keyed by slot count. CreateDescriptorSet takes it back, so the
    // transient sets of a warm frame allocate no residents and no name.
    FlatMap<uint32_t, std::vector<MetalDescriptorSet>> m_ReleasedDescriptorSets;

    // Transient descriptor sets are recycled by the backend when their frame
    // slot's GPU work completes (the engine never destroys them explicitly),
    // together with the arena their argument tables were carved from.
    std::array<std::vector<DescriptorSetHandle>, kFramesInFlight> m_TransientDescriptorSets;
    std::array<MetalTransientDescriptorArena, kFramesInFlight> m_TransientDescriptorArenas;

    // Texture upload tracking: one internal timeline, monotonically bumped
    // per SubmitTextureUploads batch.
    SemaphoreHandle m_UploadSemaphore{};
    uint64_t m_UploadCounter = 0;

    MTL::ComputePipelineState* m_FillBufferPipeline = nullptr; // owned (+1)
    MTL::Library* m_MipBlitLibrary = nullptr;                  // owned (+1)
    MTL::SamplerState* m_MipBlitSampler = nullptr;             // owned (+1)
    std::unordered_map<uint32_t, MTL::RenderPipelineState*> m_MipBlitPipelines; // keyed by MTL::PixelFormat, owned (+1)
    MTL::DepthStencilState* m_DebugAlwaysDepthState = nullptr; // owned (+1)
    // Indexed by MTL::CompareFunction (0..7); owned (+1) entries.
    std::array<MTL::DepthStencilState*, 8> m_ReadOnlyDepthStates{};

    // Borrowed pointers into m_Buffers entries created with
    // BufferUsage::ShaderDeviceAddress; maintained by Create/DestroyBuffer.
    std::vector<MTL::Buffer*> m_DeviceAddressBuffers;
    // Borrowed pointers into m_Buffers entries created with
    // BufferUsage::Indirect; maintained by Create/DestroyBuffer.
    std::vector<MTL::Buffer*> m_IndirectArgumentBuffers;
    std::shared_ptr<MetalIndirectCountResources> m_IndirectCountResources;

    // GE_METAL_RESIDENCY_SET (macOS 15+): replaces the per-encoder blanket
    // useResources of the whole BDA registry with one queue-attached
    // MTLResidencySet (residency declared once per frame) plus an MTLFence for
    // the compute->draw hazard the residency set, being residency-only, no
    // longer carries. Stays off by default until verified.
    bool m_UseResidencySet = false;
    bool m_ResidencyDirty = false;
    MTL::ResidencySet* m_BdaResidencySet = nullptr; // owned (+1)
    MTL::Fence* m_GpuDrivenFence = nullptr;         // owned (+1)

    // Created on first request when supportsRayQuery holds; destroyed in
    // Shutdown ahead of the resource pools it allocates scratch from.
    std::unique_ptr<MetalAccelerationStructures> m_AccelerationStructures;

    bool m_GpuCaptureActive = false;
    int m_GpuCaptureFramesLeft = 0;

    GenerationalVector<std::unique_ptr<MetalSwapchain>> m_WindowTargets;
    WindowTargetHandle m_ActiveWindowTarget{};
};

} // namespace Rendering
} // namespace GameEngine
