#pragma once

// CBTResources — the persistent GPU buffer container for one CBT terrain
// instance. Owns the 14 SoA storage buffers, the descriptor set that binds them,
// and a host-visible readback buffer for debug counters. The device-local
// footprint scales with kDefaultBisectorPoolSize (SpecFor in CBTResources.cpp
// sizes each buffer); GetPersistentByteSize reports the actual total, which
// EnsureInitialized logs at bring-up.

#include <array>
#include <cstdint>
#include <span>

#include "CBTTerrain/CBTLayout.h"
#include "Rendering/Core/DeviceFrameCounter.h"
#include "Rendering/Core/Handle.h"

namespace GameEngine::Rendering
{
class IDevice;
}

namespace GameEngine::CBTTerrain
{

class CBTKernelSet;
struct SphereEditRegions;

class CBTResources
{
  public:
    CBTResources() = default;
    ~CBTResources();

    CBTResources(const CBTResources&) = delete;
    CBTResources& operator=(const CBTResources&) = delete;

    // Allocates every persistent buffer for `poolSize` bisectors and writes the
    // descriptor set from the kernel-set layout. `poolSize` must equal
    // kDefaultBisectorPoolSize for C1 (the sum-tree kernels bake its geometry).
    bool Initialize(Rendering::IDevice& device, const CBTKernelSet& kernelSet,
                    uint32_t poolSize = kDefaultBisectorPoolSize);
    void Shutdown();

    // Device-rebuild recovery. An in-place device rebuild already destroyed every
    // VkBuffer / VkImage / VkSampler behind these handles (VulkanDevice::
    // TeardownDeviceScopedForRebuild walks m_LiveBuffers / m_LiveTextures /
    // m_LiveSamplers), yet the handle members still read IsValid(). Forget them
    // WITHOUT calling Destroy* — Shutdown() would re-enter destruction on
    // already-freed handles — so the next Initialize allocates cleanly against the
    // rebuilt device. Mirrors the "drop the handles (no DestroyBuffer)" contract the
    // other re-provision consumers follow in RenderServices::OnDeviceRebuilt.
    void ReprovisionAfterDeviceRebuild();

    bool IsReady() const { return m_Ready; }
    uint32_t GetPoolSize() const { return m_PoolSize; }

    // The set a given kernel binds. Where every kernel shares one layout this is that one set
    // whatever the index; where each kernel carries only its own bindings (see
    // CBTKernelSet::KernelLayout) it is that kernel's matching set — WebGPU requires the bind
    // group and the pipeline's layout to agree exactly.
    Rendering::DescriptorSetHandle GetDescriptorSet(uint32_t kernelIndex = 0) const;
    Rendering::BufferHandle GetBuffer(CBTBinding binding) const;

    // Byte size of a storage buffer (binding). 0 for an out-of-range binding. Used to
    // bound debug readbacks against the source.
    uint64_t GetBufferByteSize(CBTBinding binding) const;

    // C3 indexed-indirect draw resources (not part of the kernel descriptor set —
    // consumed only by the graphics DrawIndexedIndirectCount). The identity index
    // buffer (index[i] = i, 3*P u32) makes gl_VertexIndex walk 0..indexCount-1; the
    // draw-count buffer holds the single-command count (1). Both are filled once by
    // CBTInstance::InitializeRoots and never rewritten. See CBTLayout.h §draw.
    Rendering::BufferHandle GetIdentityIndexBuffer() const { return m_IdentityIndexBuffer; }
    Rendering::BufferHandle GetDrawCountBuffer() const { return m_DrawCountBuffer; }

    // Host-visible readback buffer (large enough for the indirect-draw records
    // and validation counters copied for debug asserts).
    Rendering::BufferHandle GetReadbackBuffer() const { return m_Readback; }
    uint64_t GetReadbackBufferSize() const { return m_ReadbackSize; }

    // This frame's ring frame counter — unwrapped and monotonically increasing, the value
    // every per-frame entry point below reduces with CBTFrameRingSlot and the value
    // CBTInstance::RecordUpdate pushes for the GPU to reduce the same way. Call once per
    // device frame with IDevice::GetFrameIndex(), which is a CHANGE TOKEN here and never a
    // slot: its domain is [0, framesInFlight), too narrow to name this ring's last element,
    // so reducing its value directly would cap the rotation at the device's pacing and
    // strand that element on its Initialize-time default (see CBTFrameRingSlot for the
    // write-after-fence margin that costs). Idempotent within a device frame, so every view
    // declaring against one frame selects the same element and agrees with the push constant.
    uint32_t AdvanceFrameCounter(uint32_t deviceFrameIndex)
    {
        // The push constant is 32-bit; the CPU and the GPU reduce the same low 32 bits.
        return static_cast<uint32_t>(m_FrameCounter.Tick(deviceFrameIndex));
    }

    // C4 per-frame params UBO (binding 14) — camera + terrain params consumed by
    // Classify (screen-space error) and VertexEval (displacement). Host-visible,
    // ring-buffered over kCBTFrameParamsRing frames. Write one slot per frame.
    Rendering::BufferHandle GetFrameParamsBuffer() const { return m_FrameParams; }
    void UploadFrameParams(uint32_t frameCounter, const CBTFrameParams& params, const SphereEditRegions& sphereDirty);

    // C4 terrain height source (binding 15, a CBTTextureRingFor ring). Rebinds ONE
    // ring element — this frame's, per CBTFrameRingSlot — to the terrain's heightmap (or
    // the built-in flat default when invalid). Only that element is touched, and the
    // frame fence retires the frame that last read it before frame N+ring reuses it, so a
    // mid-session heightmap handle change (streaming/hot-reload/identity) never rewrites a
    // descriptor element an in-flight frame is still reading. No-op when that element's
    // handle is unchanged. The default is a 1x1 zero R32_FLOAT so every element is
    // always valid before any terrain binds (heightScale = 0 makes its content moot).
    void SetHeightSource(uint32_t frameCounter, Rendering::TextureHandle texture);
    Rendering::TextureHandle GetDefaultHeightTexture() const { return m_DefaultHeight; }
    // The height texture bound in `frameCounter`'s ring element (the flat default when none).
    Rendering::TextureHandle GetBoundHeightSource(uint32_t frameCounter) const;
    // The height-range pyramid (binding 21); invalid on the narrow-heap arm.
    Rendering::BufferHandle GetHeightRangeBuffer() const { return m_HeightRange; }
    // The draw's prefiltered height chain (binding 24 on the compute set, gCbtChain on the graphics
    // set 2). Valid on both arms so the draw always binds a buffer; it holds only a zeroed header
    // until ProvisionHeightChainWords grows it, and the narrow arm never does, so a header that names
    // no lattice makes the draw keep the cached corners.
    Rendering::BufferHandle GetHeightChainBuffer() const { return m_HeightChain; }
    uint32_t GetHeightChainWords() const { return m_HeightChainWords; }
    // Grows the chain to hold `words` (CBTHeightChainWords of the bound lattice; the wide arm only),
    // never shrinking. The new buffer starts with a zeroed header, so whatever the old one held is
    // rebuilt before the draw reads it. False when the allocation fails or the arm builds no chain:
    // the previous buffer stays bound and the terrain draws its cached corners.
    bool ProvisionHeightChainWords(uint32_t words);
    // The far-field normal chain (binding 25 on the compute set as one storage view per mip,
    // cbt_NormalChain on the graphics set 2). A 1x1 placeholder until ProvisionNormalChain creates it for
    // a lattice; the texture rests in ShaderResource between frames. Wide arm only, like the height chain.
    Rendering::TextureHandle GetNormalChainTexture() const { return m_NormalChain; }
    uint32_t GetNormalChainMips() const { return m_NormalChainMips; }
    // Creates the chain for a lattice of `samplesX` x `samplesZ` (CBTNormalChainTexels / Mips), replacing
    // one made for another lattice; true when it matches. False when the arm builds no chain or the
    // allocation fails: the previous texture stays bound and the surface keeps the level-0 normal map.
    bool ProvisionNormalChain(uint32_t samplesX, uint32_t samplesZ);
    Rendering::SamplerHandle GetHeightSampler() const { return m_HeightSampler; }

    // In-place re-provision safety. The per-slot SetHeightSource/SetAtlasSource/SetCoarseSource
    // refresh above only rewrites the current frame's ring element, so evicting a retired terrain
    // texture from EVERY element relies on the ring rotating through all slots before that image
    // clears its owner's frames-in-flight quarantine and is freed — a self-heal with only a
    // one-frame margin that a SamplesPerMeter/size edit (destroy + recreate of the unified
    // textures) can lose under load, leaving a ring element sampling a freed VkImageView from CBT
    // VertexEval a few frames later (VK_ERROR_DEVICE_LOST). RebindTerrainSourcesToDefault rewrites
    // EVERY height (15) / atlas (18) / coarse (19) ring element back to its built-in default in one
    // shot and bumps GetTerrainSourceGeneration, so a retired texture is dropped from the whole ring
    // deterministically — no dependence on ring rotation. The caller MUST first retire the graphics
    // work already submitted (CBTInstance::RefreshTerrainSources waits that timeline value): this
    // rewrites descriptor elements in-flight frames may still be sampling. Returns true when any
    // element actually changed.
    bool RebindTerrainSourcesToDefault();

    // Monotonic version of the terrain-texture ring bindings (height/atlas/coarse). Bumped whenever
    // SetHeightSource/SetAtlasSource/SetCoarseSource actually change a slot, and by
    // RebindTerrainSourcesToDefault. The re-provision oracle asserts this advances in the same update
    // a retired heightmap is dropped, proving no ring element outlives the retired texture.
    uint64_t GetTerrainSourceGeneration() const { return m_TerrainSourceGeneration; }

    // Test/diagnostic: how many ring elements (across the height/atlas/coarse rings) currently bind
    // `texture`. The oracle asserts this is 0 for a retired texture after the rebind.
    uint32_t CountTerrainSourceSlots(Rendering::TextureHandle texture) const;

    // ---- Phase E resident-window atlas (bindings 17/18) ----
    //
    // Binding 18: rebind ONE ring element (this frame's, per CBTFrameRingSlot) to the
    // terrain's atlas height texture (or the flat default when invalid). Same in-flight
    // safety as SetHeightSource — only this frame's element is touched. No-op when that
    // element's handle is unchanged.
    void SetAtlasSource(uint32_t frameCounter, Rendering::TextureHandle atlasTexture);
    // Binding 19: rebind the frame's coarse height field texture (out-of-window fallback), or
    // the flat default when invalid. Same per-slot in-flight safety as SetAtlasSource.
    void SetCoarseSource(uint32_t frameCounter, Rendering::TextureHandle coarseTexture);
    // Binding 17: write `rowCount` 16-B indirection rows into the frame's ring slot from
    // the residency controller's table (host-visible; completes before submit, like the
    // params UBO). `rows` points at kAtlasRowBytes-sized TileAtlasSlot entries; the copy is
    // clamped to kAtlasWindowRows. Rows past `rowCount` in the slot are zeroed so a shrunk
    // table never resolves a stale tile as resident.
    void UploadAtlasRows(uint32_t frameCounter, const void* rows, uint32_t rowCount);
    // ---- The paged height resolve (bindings 22/23, wide arm only; no-ops on the narrow arm) ----
    //
    // Binding 23: rebind this frame's ring element to the height page cache texture (or the flat
    // default when invalid). Same per-slot in-flight safety as SetAtlasSource.
    void SetPageCacheSource(uint32_t frameCounter, Rendering::TextureHandle cacheTexture);
    // Binding 22: binds TerrainPageFeature's page-table ring, whose slot for this frame starts at word
    // `firstWord`; an invalid ring binds the zeroed default. A new ring handle rewrites the sets.
    void SetPageTable(Rendering::BufferHandle ring, uint32_t firstWord);
    Rendering::BufferHandle GetPageTableBuffer() const { return m_PageTable; }
    Rendering::TextureHandle GetDefaultAtlasHeightTexture() const { return m_DefaultAtlasHeight; }
    Rendering::TextureHandle GetDefaultAtlasCoarseTexture() const { return m_DefaultAtlasCoarse; }
    Rendering::BufferHandle GetAtlasRowsBuffer() const { return m_AtlasRows; }

    // Planet-shading surface params (plan §planet-shading) — a small host-visible ring the
    // fragment reads (radius, relief amplitude/frequency/octaves, the planet pages). Written
    // once per frame like the params UBO; offset-bound per slot on the graphics side.
    Rendering::BufferHandle GetSurfaceParamsBuffer() const { return m_SurfaceParams; }
    void UploadSurfaceParams(uint32_t frameCounter, const CBTSurfaceParams& params);

    // The draw's level rule (CBTDrawLevelParams), a host-visible uniform ring the vertex modifier reads
    // as gCbtDrawLevel; written once per frame, offset-bound per slot (kCBTDrawLevelParamsSlotStride).
    Rendering::BufferHandle GetDrawLevelParamsBuffer() const { return m_DrawLevelParams; }
    void UploadDrawLevelParams(uint32_t frameCounter, const CBTDrawLevelParams& params);

    // Total device-local footprint of the persistent buffers, in bytes.
    uint64_t GetPersistentByteSize() const { return m_PersistentBytes; }

  private:
    Rendering::BufferHandle CreateStorageBuffer(uint64_t size, uint32_t extraUsage, const char* name);
    Rendering::BufferHandle CreateHeightChain(uint32_t words);
    bool CreateNormalChain(uint32_t width, uint32_t height, uint32_t mips);
    void DestroyNormalChain();
    void WriteDescriptorSet();

    Rendering::IDevice* m_Device = nullptr;
    // Unwraps the device's wrapped frame slot so all kCBTFrameParamsRing elements rotate
    // (see AdvanceFrameCounter). Lives with the rings it indexes.
    Rendering::DeviceFrameCounter m_FrameCounter;
    uint32_t m_PoolSize = 0;
    std::array<Rendering::BufferHandle, kCBTBindingCount> m_Buffers{};
    Rendering::BufferHandle m_IdentityIndexBuffer{};
    Rendering::BufferHandle m_DrawCountBuffer{};
    Rendering::BufferHandle m_FrameParams{};       // UBO ring (binding 14)
    Rendering::BufferHandle m_SurfaceParams{};     // host-visible ring, graphics set 2 (plan §planet-shading)
    Rendering::TextureHandle m_DefaultHeight{};    // 1x1 flat fallback heightmap
    std::array<Rendering::TextureHandle, kCBTTextureRingMax> m_BoundHeight{}; // per-ring-slot bound texture (binding 15)
    Rendering::SamplerHandle m_HeightSampler{};    // linear-clamp, matches CDLOD's GE_TS_CLAMP
    Rendering::BufferHandle m_AtlasRows{};         // host-visible SSBO ring (binding 17)
    Rendering::BufferHandle m_HeightRange{};       // device-local height-range pyramid (binding 21)
    Rendering::BufferHandle m_HeightChain{};       // device-local prefiltered height chain (binding 24)
    uint32_t m_HeightChainWords = 0u;              // words m_HeightChain holds
    Rendering::BufferHandle m_DrawLevelParams{};   // host-visible ring, graphics set 2 (gCbtDrawLevel)
    Rendering::TextureHandle m_NormalChain{};      // far-field normal chain, rg16f with mips (binding 25)
    std::array<Rendering::TextureViewHandle, kCBTNormalChainMaxMips> m_NormalChainViews{}; // one per mip
    uint32_t m_NormalChainMips = 0u;
    uint32_t m_NormalChainLattice = 0u;            // samples x | z << 16 the chain was made for
    Rendering::TextureHandle m_DefaultAtlasHeight{}; // 1x1 flat fallback atlas height
    std::array<Rendering::TextureHandle, kCBTTextureRingMax> m_BoundAtlasHeight{}; // per-ring-slot atlas texture (binding 18)
    Rendering::TextureHandle m_DefaultAtlasCoarse{}; // 1x1 flat fallback coarse field
    std::array<Rendering::TextureHandle, kCBTTextureRingMax> m_BoundAtlasCoarse{}; // per-ring-slot coarse field (binding 19)
    Rendering::BufferHandle m_DefaultPageTable{};    // a zeroed table header (binding 22), wide arm only
    Rendering::BufferHandle m_PageTable{};           // what binding 22 holds: the page ring or the default
    uint32_t m_PageTableBaseWord = 0;                // this frame's first word in m_PageTable
    Rendering::TextureHandle m_DefaultPageCache{};   // 1x1 flat page cache (binding 23), wide arm only
    std::array<Rendering::TextureHandle, kCBTTextureRingMax> m_BoundPageCache{}; // per-ring-slot page cache (binding 23)
    uint32_t m_TextureRing = kCBTTextureRingMax; // elements the loaded kernels declare per texture
    // One shared set where the device binds the whole family's storage buffers in one
    // stage; one set per kernel where it cannot (CBTKernelSet::UsesPerKernelLayouts).
    Rendering::DescriptorSetHandle m_DescriptorSet{};
    std::vector<Rendering::DescriptorSetHandle> m_KernelSets;
    std::vector<std::vector<uint32_t>> m_KernelSetBindings;
    Rendering::BufferHandle CreateDefaultPageTable();
    void WriteOneSet(Rendering::DescriptorSetHandle set, const std::vector<uint32_t>& only);
    // Rewrites one ring element of a terrain-source binding in every set that
    // binds it (the per-kernel sets, or the single shared set).
    void UpdateTerrainSourceBinding(uint32_t binding, Rendering::TextureHandle texture,
                                    uint32_t slot);
    Rendering::BufferHandle m_Readback{};
    uint64_t m_ReadbackSize = 0;
    uint64_t m_PersistentBytes = 0;
    // Binding version of the terrain-texture rings (height 15 / atlas 18 / coarse 19). See
    // GetTerrainSourceGeneration — the re-provision drop advances it, and the oracle keys on it.
    uint64_t m_TerrainSourceGeneration = 0;
    bool m_Ready = false;
};

} // namespace GameEngine::CBTTerrain
