#pragma once

#include "Rendering/Core/Device.h"
#include "UI/UICompatDrawRuns.h"
#include "GenerationalVector/GenerationalHandle.h"
#include "Types/FlatMap.h"
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace GameEngine { namespace Rendering { namespace Text { class FontAtlas; } } }

namespace GameEngine
{
namespace UI
{

// Manages texture descriptor indices for the unified SDF UI renderer.
// Maps GPU TextureHandles to array indices that UIPrimitive.textureIndex
// references in the fragment shader's texture array. The state tables
// (m_Binding0Entries / m_Binding1Entries) are the source of truth; each
// UI draw calls GetFrameSet for the descriptor set that holds them
// in that frame.
class UITextureRegistry
{
public:
    explicit UITextureRegistry(Rendering::IDevice* device);
    ~UITextureRegistry();

    // Register a GPU texture and get its array index.
    // Returns 0 on failure (index 0 is reserved as "no texture").
    // If the texture is already registered, returns the existing index.
    uint32_t Register(Rendering::TextureHandle tex,
                      Rendering::SamplerHandle sampler = {});

    // Register a texture by handle value + sampler. If already registered, returns existing index.
    // This variant takes explicit texture and sampler handles for pre-uploaded textures
    // (e.g., font atlas pages).
    uint32_t RegisterPreUploaded(Rendering::TextureHandle tex,
                                 Rendering::SamplerHandle sampler);

    // Unregister a texture, freeing its array index for reuse.
    void Unregister(uint32_t index);

    // Unregister a texture by its handle id. No-op if not found.
    // Used by the UI texture-cache hot-reload path: when the underlying GPU
    // TextureHandle is destroyed and recreated (e.g. after AssetReloaded for
    // a panel-background .png), the old slot still references the destroyed
    // handle. This drops that slot back to the dummy and frees the index.
    void UnregisterByHandle(Rendering::TextureHandle tex);

    // Snapshot binding-0 handles referenced by the current draw's slots.
    // Retained primitives need not re-register their textures each frame.
    std::vector<Rendering::TextureHandle> GetTextureHandlesForSlots(
        const std::vector<uint32_t>& slots) const;

    // Register Slug curve+band textures for all of a font's pages.
    // Each SlugPage has its own fixed-size GPU curve/band texture pair; within a
    // device generation pages and their descriptor slots are append-only and never
    // recreated, so primitives emitted earlier in the frame stay valid across
    // new-glyph growth events (no flicker). A device rebuild is the one event that
    // drops them: the whole cache is discarded and the next call re-uploads.
    // Returns a reference to the cached indices array, one entry per page.
    // The reference is stable until the next call for any atlas (don't hold
    // across calls). Index by glyph.pageIndex.
    struct SlugTextureIndices { uint32_t CurveTexIdx = 0; uint32_t BandTexIdx = 0; };
    const std::vector<SlugTextureIndices>& RegisterSlugTextures(const Rendering::Text::FontAtlas& atlas);

    // Register a color emoji atlas page. Returns descriptor array index (binding 0).
    uint32_t RegisterColorAtlasPage(const Rendering::Text::FontAtlas& atlas, int pageIndex);

    /// Reserve a descriptor slot for a texture that is bound later, during render
    /// pass execute. The slot reads the white dummy until UpdateSlot binds it.
    /// The handle's Index() is the descriptor slot. An invalid handle means no slot is free.
    GenerationalVector::Handle ReserveSlot();

    /// End a reservation and return its slot to the free pool. A reservation that
    /// does not hold its slot (its slot was freed, or a device rebuild dropped it) does nothing.
    void ReleaseSlot(GenerationalVector::Handle reservation);

    /// Bind a texture to a reserved slot. A reservation that does not hold its slot
    /// writes nothing. GetFrameSet writes the descriptor.
    void UpdateSlot(GenerationalVector::Handle reservation, Rendering::TextureHandle tex,
                    Rendering::SamplerHandle sampler = {});

    // Return the device's descriptor set that holds every occupied slot of the state
    // tables. The device builds it on the first call for that content and keeps it
    // while the content repeats. Call once per UI draw, before BindDescriptorSet(1, ...).
    Rendering::DescriptorSetHandle GetFrameSet();

    // Compat-profile twin of GetFrameSet: one descriptor set per
    // UICompatDrawRun, holding only the textures that run's primitives name.
    // Slots the run left unclaimed take the dummy white so no binding is left
    // unwritten.
    Rendering::DescriptorSetHandle GetDrawRunSet(const UICompatDrawRun& run);

    // True when the registry built the fixed-slot layout instead of the
    // bindless arrays. Resolved once at construction from the renderer's shader
    // profile, so the layout can never disagree with the compiled shader.
    bool IsCompat() const { return m_Compat; }

    // Descriptor set layout description for pipeline creation.
    const Rendering::DescriptorSetLayoutDesc& GetLayoutDesc() const { return m_LayoutDesc; }

    // Policy ceiling for UI texture slots. The constructor clamps this to the
    // backend's per-stage sampled-image/resource limits before creating the
    // descriptor layout.
    static constexpr uint32_t kDefaultMaxTextures = 512;
    static constexpr uint32_t kDefaultMaxBandTextures = 16;
    static constexpr uint32_t kMaxFramesInFlight = Rendering::IDevice::kMaxSupportedFramesInFlight;

    void Shutdown();

private:
    // Protects all mutable slot state + GPU writes from concurrent access.
    // UpdateSlot runs from the render-graph execute callback which may land on
    // a JobSystem worker under parallel dispatch, while Register / Unregister /
    // RegisterSlugTextures / RegisterColorAtlasPage run from the main thread's
    // primitive-generation pass. GetFrameSet iterates the slot
    // maps and must not race with writers.
    mutable std::mutex m_Mutex;

    // Construction (UI) thread, captured for Debug asserts: the parallel
    // drain guarantees JobSystem workers only ever hit the read-only fast
    // paths, so any write path taken off this thread is a broken invariant
    // (would also invalidate references returned to other workers).
    std::thread::id m_OwnerThreadId = std::this_thread::get_id();

    Rendering::IDevice* m_Device = nullptr;
    uint32_t m_FramesInFlight = 1;
    Rendering::DescriptorSetLayoutDesc m_LayoutDesc{};
    Rendering::SamplerHandle m_DefaultSampler{};
    Rendering::TextureHandle m_DummyTexture{};  // 1x1 white at binding 0 slot 0
    // 1x1 zero-filled R16G16_UINT at binding 1 slot 0, and the filler every
    // unclaimed compat glyph-band binding resolves to. Integer-formatted to
    // match the utexture2D array (the white dummy is a format-class mismatch
    // there), and zeroed so a band record read out of it decodes to a curve
    // count of 0 — a blank glyph rather than a runaway shader loop.
    Rendering::TextureHandle m_BandDummyTexture{};

    // Device generation these GPU handles belong to. An in-place device rebuild
    // destroys every VkObject while keeping the IDevice* alive, and a handle
    // minted before the rebuild keeps reporting IsValid() (nothing re-stamps the
    // id bits), so generation equality — not IsValid() — is what says a handle
    // still resolves. Every entry point that reads or writes slot state compares
    // this against the device and heals first.
    uint64_t m_DeviceRebuildGeneration = 0;

    uint32_t m_MaxTextures = kDefaultMaxTextures;
    uint32_t m_MaxBandTextures = kDefaultMaxBandTextures;
    bool m_Compat = false;

    // Per-slot state maps. Source of truth for the descriptor set contents;
    // mutators update these, and GetFrameSet returns the device set for
    // their current content each draw. The registry holds no set.
    struct SlotEntry
    {
        Rendering::TextureHandle Texture{};
        Rendering::SamplerHandle Sampler{};
    };
    FlatMap<uint32_t, SlotEntry> m_Binding0Entries; // slot → texture (CIS)
    FlatMap<uint32_t, SlotEntry> m_Binding1Entries; // band slot → texture (usampler2D)
    // The writes GetFrameSet and GetDrawRunSet pass to the device, kept to reuse their capacity. Guarded by m_Mutex.
    std::vector<Rendering::DescriptorWrite> m_SetWrites;

    uint32_t m_NextSlot = 1; // 0 is reserved
    // Dedup key = {FULL 64-bit generational texture id, sampler id}. Packing
    // both into one uint64 truncated the texture handle's generation bits, so
    // a pool-recycled texture (same index, bumped generation) dedup-hit its
    // dead predecessor's slot and the UI sampled a destroyed texture (the
    // white-scene-view-after-tab-return bug); UnregisterByHandle's reverse
    // comparison silently never matched for the same reason.
    FlatMap<std::pair<uint64_t, uint64_t>, uint32_t> m_HandleToIndex;
    std::vector<uint32_t> m_FreeSlots;
    // Generation per binding-0 slot, indexed by slot. Every free of a slot (release,
    // unregister, heal, shutdown) bumps it, so a reservation handle matches only while
    // its slot is still reserved for it. Never reset, also across a heal.
    std::vector<GenerationalVector::Handle::GenerationType> m_SlotGenerations;

    // Binding ordinals of the compat layout. Must match the layout(binding=)
    // qualifiers in Shaders/UI/ui_sdf_textures.glsl.
    static constexpr uint32_t kCompatTextureBinding0 = 0;
    static constexpr uint32_t kCompatGlyphCurveBinding0 = kCompatTextureBinding0 + kUiCompatTextureSlots;
    static constexpr uint32_t kCompatGlyphBandBinding0 = kCompatGlyphCurveBinding0 + kUiCompatGlyphSlots;
    static constexpr uint32_t kCompatLinearSamplerBinding = kCompatGlyphBandBinding0 + kUiCompatGlyphSlots;
    static constexpr uint32_t kCompatNearestSamplerBinding = kCompatLinearSamplerBinding + 1;

    void BuildBindlessLayoutDesc(const Rendering::RenderingDeviceCapabilities& caps);
    void BuildCompatLayoutDesc();
    // Resolves a registry index to the texture to bind, repairing dead handles
    // the way GetFrameSet does. Caller holds m_Mutex.
    Rendering::TextureHandle ResolveSlotTextureLocked(
        FlatMap<uint32_t, SlotEntry>& entries, uint32_t index);

    uint32_t AllocateSlot();
    uint32_t AllocateBandSlot();
    void FreeSlot(uint32_t index);
    void LogTextureLimitExceededLocked(const char* poolName);

    // Create the registry-owned GPU objects (1x1 white dummy, linear and nearest
    // samplers) on the current device generation and seed slot 0 with the dummy.
    // Shared by the constructor and the post-rebuild heal.
    void CreateDeviceResourcesLocked();

    // Drop every handle minted on a dead device generation and re-provision, once
    // per rebuild. Called under m_Mutex from each entry point rather than from a
    // device callback: RegisterDeviceRebuiltCallback / RegisterPerDeviceCacheCleanup
    // registrations are permanent and idempotent per id, so a per-instance
    // registration would both dangle once this registry died and be silently
    // dropped for the second registry on the same device (one per window, and one
    // per test in the UI fixtures). GetDeviceRebuildGeneration is the seam the
    // engine provides for exactly that case.
    //
    // Dead handles are dropped, never destroyed: the rebuild already destroyed the
    // VkObjects, so a Destroy* here would resolve to nothing anyway.
    void HealIfDeviceRebuiltLocked();

    // Internal variants of RegisterPreUploaded / Unregister that do NOT take
    // m_Mutex; used by RegisterSlugTextures / RegisterColorAtlasPage which
    // already hold it.
    uint32_t RegisterPreUploadedLocked(Rendering::TextureHandle tex,
                                       Rendering::SamplerHandle sampler);
    void UnregisterLocked(uint32_t index);
    // True when the reservation still holds its slot. Caller holds m_Mutex.
    bool HoldsSlotLocked(GenerationalVector::Handle reservation) const;
    // End every outstanding reservation: bump the generation of every slot.
    void EndAllReservationsLocked();

    // Cached font atlas page GPU textures.
    struct AtlasPageEntry
    {
        Rendering::TextureHandle Texture{};
        uint32_t RegistryIndex = 0;
        uint32_t ContentGeneration = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
    };
    // Key: (atlasId << 32) | (pageIndex << 1) | isColor
    FlatMap<uint64_t, AtlasPageEntry> m_AtlasPageCache;

    static uint64_t MakeAtlasPageKey(uint32_t atlasId, int pageIndex, bool isColor)
    {
        return (static_cast<uint64_t>(atlasId) << 32)
             | (static_cast<uint64_t>(pageIndex) << 1)
             | (isColor ? 1u : 0u);
    }

    // Band texture slot allocator (binding 1 — few slots, no free list needed).
    // Starts at 1: slot 0 is reserved for the band dummy, mirroring binding 0.
    uint32_t m_NextBandSlot = 1;
    int m_TextureLimitWarningBudget = 8;
    Rendering::SamplerHandle m_NearestSampler{};
    std::vector<uint16_t> m_HalfConversionBuffer; // reused across RegisterSlugTextures calls

    // Cached GPU state for a single Slug page. Each page has a fixed-size
    // curve and band texture pair plus stable descriptor slot indices that
    // never change over the page's lifetime.
    struct SlugPageEntry
    {
        Rendering::TextureHandle CurveTexture{};
        Rendering::TextureHandle BandTexture{};
        uint32_t CurveSlot = 0;   // binding 0 index (stable)
        uint32_t BandSlot  = 0;   // binding 1 index (stable)
        uint32_t UploadedGen = 0; // last page generation uploaded to GPU
    };
    struct SlugFontCache
    {
        std::vector<SlugPageEntry> Pages;           // one per FontAtlas::SlugPage
        std::vector<SlugTextureIndices> Indices;    // mirror of pages for fast access
    };
    FlatMap<uint32_t, SlugFontCache> m_SlugCache; // keyed by atlasId
};

} // namespace UI
} // namespace GameEngine
