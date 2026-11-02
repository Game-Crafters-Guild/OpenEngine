#include "UI/UITextureRegistry.h"

#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Materials/Shader/ShaderProfileDefines.h"
#include "Rendering/Text/FontAtlas.h"

#include <algorithm>
#include <cassert>
#include <unordered_set>

using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::Text;

namespace GameEngine
{
namespace UI
{
namespace
{
using SlotGeneration = ::GenerationalVector::Handle::GenerationType;

// The next generation of a slot. Skips kInvalidGeneration on wrap, so a live
// reservation handle is never invalid.
SlotGeneration NextSlotGeneration(SlotGeneration generation)
{
    ++generation;
    return generation == ::GenerationalVector::Handle::kInvalidGeneration ? generation + 1 : generation;
}

struct UiTextureLimits
{
    uint32_t colorTextures = UITextureRegistry::kDefaultMaxTextures;
    uint32_t bandTextures = UITextureRegistry::kDefaultMaxBandTextures;
};

UiTextureLimits ResolveUiTextureLimits(const RenderingDeviceCapabilities& caps)
{
    UiTextureLimits limits{};

    // The SDF pipeline has two sampled-image arrays in the fragment stage:
    // binding 0 for general UI/color/curve textures and binding 1 for Slug
    // band textures. Band pages are few, so reserve a small fixed slice instead
    // of halving MoltenVK's tight sampled-image budget.
    uint32_t imageBudget = UITextureRegistry::kDefaultMaxTextures
                         + UITextureRegistry::kDefaultMaxBandTextures;
    if (caps.maxPerStageSampledImages > 0)
        imageBudget = std::min(imageBudget, caps.maxPerStageSampledImages);

    static constexpr uint32_t kUiSdfNonImageFragmentBindings = 4;
    // Set 0 exposes two fragment SSBOs, while set 1 exposes two sampler objects.
    if (caps.maxPerStageResources > kUiSdfNonImageFragmentBindings)
        imageBudget = std::min(imageBudget, caps.maxPerStageResources - kUiSdfNonImageFragmentBindings);

    imageBudget = std::max(2u, imageBudget);

    const uint32_t bandReservation = std::min(
        UITextureRegistry::kDefaultMaxBandTextures,
        std::max(1u, imageBudget / 8u));
    limits.bandTextures = std::min(bandReservation, imageBudget - 1u);
    limits.colorTextures = std::min(
        UITextureRegistry::kDefaultMaxTextures,
        std::max(1u, imageBudget - limits.bandTextures));

    return limits;
}

// Create a 1x1 shader-resource texture seeded with a single texel.
// Backs the reserved slot-0 fallback of each bindless array: a real, live,
// correctly-typed image for slots that are unpopulated or whose texture has
// been retired.
TextureHandle CreateDummyTexture1x1(IDevice* device, TextureFormat format,
                                    const void* texel, uint32_t texelBytes,
                                    const char* debugName)
{
    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(format);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
             | static_cast<uint32_t>(TextureUsage::TransferDst);
    td.debugName = debugName;
    TextureHandle tex = device->CreateTexture(td);
    if (!tex.IsValid())
        return {};

    BufferDesc st{};
    st.size = texelBytes;
    st.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    st.memoryUsage = BufferMemoryUsage::Upload;
    st.debugName = "UISdf_DummyStaging";
    BufferHandle staging = device->CreateBuffer(st);
    device->UpdateBuffer(staging, 0, texelBytes, texel);

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::Undefined, ResourceState::CopyDest));
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, 1, 1, 0, texelBytes);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::CopyDest, ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device->ExecuteCommandLists(lists);
    device->DestroyBuffer(staging);
    return tex;
}
}

// Bindless layout (set 1): two runtime-sized sampled-image arrays plus two
// fixed samplers, so the layout is not constrained by
// maxPerStageDescriptorSamplers. PARTIALLY_BOUND lets unused image slots stay
// untouched; GetFrameSet never writes a set it has returned, so
// cross-frame writes never happen.
void UITextureRegistry::BuildBindlessLayoutDesc(const RenderingDeviceCapabilities& caps)
{
    const UiTextureLimits limits = ResolveUiTextureLimits(caps);
    m_MaxTextures = limits.colorTextures;
    m_MaxBandTextures = limits.bandTextures;

    // PARTIALLY_BOUND does not zero descriptor memory: an unwritten slot holds a stale descriptor.
    // Binding 1 drives a shader loop bound, so GetFrameSet writes every slot.

    m_LayoutDesc.debugName = "UISdfTextureSet";
    m_LayoutDesc.bindings.clear();

    DescriptorBinding binding{};
    binding.binding = 0;
    binding.type = DescriptorType::Texture;
    binding.count = m_MaxTextures;
    binding.shaderStages = kShaderStageFragment;
    binding.debugName = "textures";
    binding.flags = kDescriptorBindingPartiallyBound;
    m_LayoutDesc.bindings.push_back(binding);

    // Binding 1: integer texture array for Slug band data (utexture2D)
    DescriptorBinding bandBinding{};
    bandBinding.binding = 1;
    bandBinding.type = DescriptorType::Texture;
    bandBinding.count = m_MaxBandTextures;
    bandBinding.shaderStages = kShaderStageFragment;
    bandBinding.debugName = "bandTextures";
    bandBinding.flags = kDescriptorBindingPartiallyBound;
    m_LayoutDesc.bindings.push_back(bandBinding);

    DescriptorBinding linearSamplerBinding{};
    linearSamplerBinding.binding = 2;
    linearSamplerBinding.type = DescriptorType::Sampler;
    linearSamplerBinding.count = 1;
    linearSamplerBinding.shaderStages = kShaderStageFragment;
    linearSamplerBinding.debugName = "uiLinearSampler";
    m_LayoutDesc.bindings.push_back(linearSamplerBinding);

    DescriptorBinding nearestSamplerBinding{};
    nearestSamplerBinding.binding = 3;
    nearestSamplerBinding.type = DescriptorType::Sampler;
    nearestSamplerBinding.count = 1;
    nearestSamplerBinding.shaderStages = kShaderStageFragment;
    nearestSamplerBinding.debugName = "uiNearestSampler";
    m_LayoutDesc.bindings.push_back(nearestSamplerBinding);
}

// Compat layout (set 1): one binding per slot, in the order
// Shaders/UI/ui_sdf_textures.glsl declares them. Registration stays
// registry-wide — only the per-run bind group narrows indices to slots — so the
// slot ceilings above are unrelated to this layout's width.
void UITextureRegistry::BuildCompatLayoutDesc()
{
    m_MaxTextures = kDefaultMaxTextures;
    m_MaxBandTextures = kDefaultMaxBandTextures;

    m_LayoutDesc.debugName = "UISdfTextureSetCompat";
    m_LayoutDesc.bindings.clear();

    auto AddTexture = [this](uint32_t slot, const char* name, bool unsignedInteger)
    {
        DescriptorBinding b{};
        b.binding = slot;
        b.type = DescriptorType::Texture;
        b.count = 1;
        b.shaderStages = kShaderStageFragment;
        b.debugName = name;
        b.imageIsUnsignedInteger = unsignedInteger;
        m_LayoutDesc.bindings.push_back(b);
    };

    for (uint32_t i = 0; i < kUiCompatTextureSlots; ++i)
        AddTexture(kCompatTextureBinding0 + i, "uiTexture", false);
    for (uint32_t i = 0; i < kUiCompatGlyphSlots; ++i)
        AddTexture(kCompatGlyphCurveBinding0 + i, "uiGlyphCurve", false);
    // Slug band pages are R16G16_UINT, read only with texelFetch.
    for (uint32_t i = 0; i < kUiCompatGlyphSlots; ++i)
        AddTexture(kCompatGlyphBandBinding0 + i, "uiGlyphBand", true);

    DescriptorBinding linearSampler{};
    linearSampler.binding = kCompatLinearSamplerBinding;
    linearSampler.type = DescriptorType::Sampler;
    linearSampler.count = 1;
    linearSampler.shaderStages = kShaderStageFragment;
    linearSampler.debugName = "uiLinearSampler";
    m_LayoutDesc.bindings.push_back(linearSampler);

    DescriptorBinding nearestSampler = linearSampler;
    nearestSampler.binding = kCompatNearestSamplerBinding;
    nearestSampler.debugName = "uiNearestSampler";
    m_LayoutDesc.bindings.push_back(nearestSampler);
}

UITextureRegistry::UITextureRegistry(IDevice* device)
    : m_Device(device)
    , m_FramesInFlight(device ? device->GetFramesInFlight() : 1u)
{
    if (!device)
        return;

    // Create a default linear-clamp sampler (1 = linear, 2 = clamp-to-edge).
    SamplerDesc sampDesc{};
    sampDesc.minFilter = 1;
    sampDesc.magFilter = 1;
    sampDesc.mipFilter = 0;
    sampDesc.addressModeU = 2;
    sampDesc.addressModeV = 2;
    sampDesc.addressModeW = 2;
    sampDesc.maxAnisotropy = 1.0f;
    sampDesc.debugName = "UISdfDefaultSampler";
    m_DefaultSampler = device->CreateSampler(sampDesc);

    m_Compat = Rendering::IsCompatShaderProfile();
    if (m_Compat)
        BuildCompatLayoutDesc();
    else
        BuildBindlessLayoutDesc(device->GetCapabilities());
    m_SlotGenerations.assign(m_MaxTextures, NextSlotGeneration(::GenerationalVector::Handle::kInvalidGeneration));

    // Nothing else can observe the registry during construction, so the *Locked
    // helper runs here without taking m_Mutex.
    m_DeviceRebuildGeneration = device->GetDeviceRebuildGeneration();
    CreateDeviceResourcesLocked();
}

void UITextureRegistry::CreateDeviceResourcesLocked()
{
    if (!m_Device)
        return;

    // Create a default linear-clamp sampler (1 = linear, 2 = clamp-to-edge).
    SamplerDesc sampDesc{};
    sampDesc.minFilter = 1;
    sampDesc.magFilter = 1;
    sampDesc.mipFilter = 0;
    sampDesc.addressModeU = 2;
    sampDesc.addressModeV = 2;
    sampDesc.addressModeW = 2;
    sampDesc.maxAnisotropy = 1.0f;
    sampDesc.debugName = "UISdfDefaultSampler";
    m_DefaultSampler = m_Device->CreateSampler(sampDesc);

    // Create nearest-neighbor sampler for integer textures (band data).
    // texelFetch ignores the sampler, but Vulkan requires a valid one.
    {
        SamplerDesc nearestDesc{};
        nearestDesc.minFilter = 0; // nearest
        nearestDesc.magFilter = 0; // nearest
        nearestDesc.mipFilter = 0;
        nearestDesc.addressModeU = 2; // clamp-to-edge
        nearestDesc.addressModeV = 2;
        nearestDesc.addressModeW = 2;
        nearestDesc.maxAnisotropy = 1.0f;
        nearestDesc.debugName = "UISdf_NearestSampler";
        m_NearestSampler = m_Device->CreateSampler(nearestDesc);
    }

    // Slot 0 of each array is its "no texture" fallback. An unwritten slot holds a stale descriptor,
    // so every slot that the shader can index must resolve to a live image of the correct type.
    {
        const uint32_t white = 0xFFFFFFFFu;
        m_DummyTexture = CreateDummyTexture1x1(m_Device, TextureFormat::RGBA8_UNORM,
                                               &white, sizeof(white), "UISdf_DummyWhite");
        if (m_DummyTexture.IsValid())
            m_Binding0Entries.InsertOrAssign(0, { m_DummyTexture, m_DefaultSampler });
    }

    // Binding 1 is an integer array (utexture2D), so its fallback must be an
    // integer-format image — substituting the RGBA8_UNORM white dummy is a
    // format-class mismatch with undefined results. The format matches the real
    // band textures exactly (R16G16_UINT, see RegisterSlugTextures).
    //
    // Zero-filled is what makes it safe rather than merely well-typed: the
    // shader reads a band record's .x as a curve count and runs it as a loop
    // trip count, so a zeroed record means zero iterations and a blank glyph.
    {
        const uint32_t zeroBandRecord = 0u;
        m_BandDummyTexture = CreateDummyTexture1x1(m_Device, TextureFormat::R16G16_UINT,
                                                   &zeroBandRecord, sizeof(zeroBandRecord),
                                                   "UISdf_DummyBandZero");
        if (m_BandDummyTexture.IsValid())
            m_Binding1Entries.InsertOrAssign(0, { m_BandDummyTexture, m_NearestSampler });
    }
}

void UITextureRegistry::HealIfDeviceRebuiltLocked()
{
    if (!m_Device)
        return;

    const uint64_t generation = m_Device->GetDeviceRebuildGeneration();
    const uint64_t previousGeneration = m_DeviceRebuildGeneration;
    if (generation == previousGeneration)
        return;

    // Counts are logged with the populations they came from: a registry that was
    // empty at rebuild time drops zero of zero, which must stay distinguishable
    // from a registry that dropped nothing because the heal never ran.
    const size_t colorSlots = m_Binding0Entries.Size();
    const size_t bandSlots = m_Binding1Entries.Size();
    const size_t atlasPages = m_AtlasPageCache.Size();
    size_t slugPages = 0;
    for (const auto& [atlasId, fontCache] : m_SlugCache)
        slugPages += fontCache.Pages.size();

    // Drop, never destroy: the rebuild already destroyed every VkObject these
    // handles named, so a Destroy* here resolves to nothing. Borrowed slot
    // entries go with the owned ones — their owners (UI texture cache, font
    // atlases) re-register during the next paint, and every call site takes its
    // slot index fresh from primitive generation, so no caller is holding an
    // index across this reset.
    m_Binding0Entries.Clear();
    m_Binding1Entries.Clear();
    m_HandleToIndex.Clear();
    m_FreeSlots.clear();
    EndAllReservationsLocked();
    m_SlugCache.Clear();
    m_AtlasPageCache.Clear();
    m_NextSlot = 1;
    // Mirrors the member's initial value, so it must be changed with it: band
    // slot 0 is reserved for the band dummy, exactly as color slot 0 is for the
    // white one. A reset to 0 would hand the dummy's reserved slot to the first
    // Slug page, which the fallback path would then silently overwrite.
    m_NextBandSlot = 1;
    m_DummyTexture = {};
    m_BandDummyTexture = {};
    m_DefaultSampler = {};
    m_NearestSampler = {};

    m_DeviceRebuildGeneration = generation;
    CreateDeviceResourcesLocked();

    // Deliberately not re-derived: m_MaxTextures, m_MaxBandTextures and
    // m_LayoutDesc still hold the limits resolved from the ORIGINAL device's
    // capabilities. A rebuild that lands on a different adapter (a fallback
    // after the primary was lost) could report smaller per-stage limits, and
    // this registry would keep handing out slots above them. Re-deriving is not
    // local to this function: the layout desc is baked into the descriptor set
    // layout the SDF pipelines were created against, so changing the binding
    // counts mid-session invalidates those pipeline layouts too.

    Logger::Log::Info(
        "UITextureRegistry: device rebuild {} -> {}; dropped {} color slot(s), {} band slot(s), "
        "{} Slug page(s), {} atlas page(s); re-provisioned dummy={} bandDummy={} linearSampler={} "
        "nearestSampler={}",
        previousGeneration,
        generation,
        colorSlots,
        bandSlots,
        slugPages,
        atlasPages,
        m_DummyTexture.IsValid(),
        m_BandDummyTexture.IsValid(),
        m_DefaultSampler.IsValid(),
        m_NearestSampler.IsValid());
}

UITextureRegistry::~UITextureRegistry()
{
    Shutdown();
}

void UITextureRegistry::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device)
        return;

    // Destroy cached atlas page GPU textures.
    for (auto& [key, entry] : m_AtlasPageCache)
    {
        if (entry.Texture.IsValid())
            m_Device->DestroyTexture(entry.Texture);
    }
    m_AtlasPageCache.Clear();

    m_HandleToIndex.Clear();
    m_FreeSlots.clear();
    EndAllReservationsLocked();
    m_Binding0Entries.Clear();
    m_Binding1Entries.Clear();

    if (m_DummyTexture.IsValid())
    {
        m_Device->DestroyTexture(m_DummyTexture);
        m_DummyTexture = {};
    }

    if (m_BandDummyTexture.IsValid())
    {
        m_Device->DestroyTexture(m_BandDummyTexture);
        m_BandDummyTexture = {};
    }

    // Destroy Slug cached textures (all pages per font).
    for (auto& [id, fontCache] : m_SlugCache)
    {
        for (auto& page : fontCache.Pages)
        {
            if (page.CurveTexture.IsValid())
                m_Device->DestroyTexture(page.CurveTexture);
            if (page.BandTexture.IsValid())
                m_Device->DestroyTexture(page.BandTexture);
        }
    }
    m_SlugCache.Clear();

    if (m_NearestSampler.IsValid())
    {
        m_Device->DestroySampler(m_NearestSampler);
        m_NearestSampler = {};
    }

    if (m_DefaultSampler.IsValid())
    {
        m_Device->DestroySampler(m_DefaultSampler);
        m_DefaultSampler = {};
    }

    m_Device = nullptr;
}

uint32_t UITextureRegistry::AllocateSlot()
{
    if (!m_FreeSlots.empty())
    {
        uint32_t slot = m_FreeSlots.back();
        m_FreeSlots.pop_back();
        return slot;
    }

    if (m_NextSlot >= m_MaxTextures)
    {
        LogTextureLimitExceededLocked("binding0");
        return 0;
    }

    return m_NextSlot++;
}

uint32_t UITextureRegistry::AllocateBandSlot()
{
    if (m_NextBandSlot >= m_MaxBandTextures)
    {
        LogTextureLimitExceededLocked("band");
        // Slot 0 (the zero-filled band dummy) is the exhaustion fallback, as it
        // is for binding 0. Returning m_MaxBandTextures would hand the shader an
        // index one past the end of the descriptor array.
        return 0;
    }

    return m_NextBandSlot++;
}

void UITextureRegistry::LogTextureLimitExceededLocked(const char* poolName)
{
    if (m_TextureLimitWarningBudget <= 0)
        return;
    --m_TextureLimitWarningBudget;

    // distinctTex far below active binding0 means the pool is full of duplicate
    // or reserved-but-not-released slots (a leak) rather than genuinely-distinct
    // images; handleToIndex is the deduped (registered) slot count, so the gap
    // to active is the per-frame RG ReserveSlot bindings. This breakdown is what
    // distinguishes a real fill from a slot leak. Only runs when already over
    // budget (rare) and is capped by m_TextureLimitWarningBudget.
    std::unordered_set<uint64_t> distinctTex;
    for (const auto& [slot, e] : m_Binding0Entries)
        distinctTex.insert(static_cast<uint64_t>(e.Texture.id));

    Logger::Log::Warning(
        "[UITextureRegistry] Exceeded UI texture limit in {}; max binding0={}, max band={}, active binding0={}, band={}, free={} | distinctTex={} handleToIndex={}",
        poolName ? poolName : "unknown",
        m_MaxTextures,
        m_MaxBandTextures,
        m_Binding0Entries.Size(),
        m_Binding1Entries.Size(),
        m_FreeSlots.size(),
        distinctTex.size(),
        m_HandleToIndex.Size());
}

void UITextureRegistry::FreeSlot(uint32_t index)
{
    if (index == 0)
        return;
    // The slot returns to the pool at once: each in-flight frame's set
    // holds its own copy of the slot's descriptor. The generation bump ends the
    // reservation that held the slot, if any.
    m_SlotGenerations[index] = NextSlotGeneration(m_SlotGenerations[index]);
    m_FreeSlots.push_back(index);
}

uint32_t UITextureRegistry::Register(TextureHandle tex, SamplerHandle sampler)
{
    return RegisterPreUploaded(tex, sampler);
}

std::vector<TextureHandle> UITextureRegistry::GetTextureHandlesForSlots(
    const std::vector<uint32_t>& slots) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<TextureHandle> result;
    result.reserve(slots.size());
    for (const auto slot : slots)
        if (const SlotEntry* entry = m_Binding0Entries.Find(slot))
            result.push_back(entry->Texture);
    return result;
}

uint32_t UITextureRegistry::RegisterPreUploadedLocked(TextureHandle tex, SamplerHandle sampler)
{
    if (!m_Device || !tex.IsValid())
        return 0;

    // Use texture+sampler as the map key so the same image can be displayed
    // with either linear or point filtering in different UI elements.
    const uint64_t samplerId = sampler.IsValid() ? static_cast<uint64_t>(sampler.id) : 0ull;
    const auto key = std::make_pair(static_cast<uint64_t>(tex.id), samplerId);
    if (const uint32_t* existing = m_HandleToIndex.Find(key))
        return *existing;

    uint32_t slot = AllocateSlot();
    if (slot == 0)
        return 0;

    SamplerHandle samp = sampler.IsValid() ? sampler : m_DefaultSampler;
    m_Binding0Entries.InsertOrAssign(slot, { tex, samp });
    m_HandleToIndex.InsertOrAssign(key, slot);
    return slot;
}

uint32_t UITextureRegistry::RegisterPreUploaded(TextureHandle tex, SamplerHandle sampler)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    return RegisterPreUploadedLocked(tex, sampler);
}

void UITextureRegistry::Unregister(uint32_t index)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    UnregisterLocked(index);
}

void UITextureRegistry::UnregisterByHandle(TextureHandle tex)
{
    if (!tex.IsValid())
        return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    const uint64_t textureId = static_cast<uint64_t>(tex.id);
    std::vector<uint32_t> slots;
    for (const auto& [key, slot] : m_HandleToIndex)
    {
        if (key.first == textureId)
            slots.push_back(slot);
    }
    for (uint32_t slot : slots)
        UnregisterLocked(slot);
}

void UITextureRegistry::UnregisterLocked(uint32_t index)
{
    if (index == 0)
        return;

    // A slot is the value of at most one dedup key.
    m_HandleToIndex.EraseIf([index](const auto&, uint32_t slot) { return slot == index; });

    // Write the dummy to the slot before the slot goes back to the pool. Frames in flight read
    // the set that they bound. That set holds the old texture, and the device keeps the set until
    // those frames finish.
    if (m_DummyTexture.IsValid())
        m_Binding0Entries.InsertOrAssign(index, { m_DummyTexture, m_DefaultSampler });
    else
        m_Binding0Entries.Erase(index);

    FreeSlot(index);
}

::GenerationalVector::Handle UITextureRegistry::ReserveSlot()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    const uint32_t slot = AllocateSlot();
    if (slot == 0)
        return {};
    // Seed the slot with the dummy so a primitive referencing it before
    // UpdateSlot lands reads white rather than a stale binding.
    if (m_DummyTexture.IsValid())
        m_Binding0Entries.InsertOrAssign(slot, { m_DummyTexture, m_DefaultSampler });
    return ::GenerationalVector::Handle(slot, m_SlotGenerations[slot]);
}

bool UITextureRegistry::HoldsSlotLocked(::GenerationalVector::Handle reservation) const
{
    return reservation.IsValid() && reservation.Index() < m_SlotGenerations.size()
        && m_SlotGenerations[reservation.Index()] == reservation.Generation();
}

void UITextureRegistry::EndAllReservationsLocked()
{
    for (SlotGeneration& generation : m_SlotGenerations)
        generation = NextSlotGeneration(generation);
}

void UITextureRegistry::ReleaseSlot(::GenerationalVector::Handle reservation)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    if (!HoldsSlotLocked(reservation))
        return;
    const uint32_t slot = reservation.Index();
    // Reserved slots are not part of m_HandleToIndex dedup. Clear their binding
    // state before recycling so GetFrameSet never re-emits a stale
    // texture handle after the source RG texture is retired.
    if (m_DummyTexture.IsValid())
        m_Binding0Entries.InsertOrAssign(slot, { m_DummyTexture, m_DefaultSampler });
    else
        m_Binding0Entries.Erase(slot);
    FreeSlot(slot);
}

void UITextureRegistry::UpdateSlot(::GenerationalVector::Handle reservation, TextureHandle tex, SamplerHandle sampler)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    if (!m_Device || !tex.IsValid() || !HoldsSlotLocked(reservation))
        return;
    SamplerHandle samp = sampler.IsValid() ? sampler : m_DefaultSampler;
    m_Binding0Entries.InsertOrAssign(reservation.Index(), { tex, samp });
}

DescriptorSetHandle UITextureRegistry::GetFrameSet()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device)
        return {};

    // Heal before the first descriptor write of the frame. Every handle written
    // below is then one the live device can resolve, so the descriptor-buffer
    // path has no dropped write to report for this set.
    HealIfDeviceRebuiltLocked();

    m_SetWrites.clear();
    if (m_DefaultSampler.IsValid())
        m_SetWrites.push_back({.Binding = 2, .Type = DescriptorType::Sampler, .Sampler = m_DefaultSampler});
    if (m_NearestSampler.IsValid())
        m_SetWrites.push_back({.Binding = 3, .Type = DescriptorType::Sampler, .Sampler = m_NearestSampler});

    // Binding 0: sampled-image array of UI textures (icons, atlas pages,
    // Slug curve textures, RG-resolved scene views). Typically <30 active slots.
    for (auto& [slot, e] : m_Binding0Entries)
    {
        TextureHandle texture = e.Texture;
        if (texture.IsValid() && !m_Device->IsTextureHandleLive(texture))
        {
            texture = m_DummyTexture;
            e.Texture = texture;
            e.Sampler = m_DefaultSampler;
        }
        if (texture.IsValid())
            m_SetWrites.push_back(
                {.Binding = 0, .ArrayElement = slot, .Type = DescriptorType::Texture, .Texture = texture});
    }

    // Binding 1: one Slug band texture per font page. Every slot gets a write: the shader uses this
    // data as a loop count, so a stale descriptor in an unwritten slot can hang the GPU. An empty
    // slot gets the integer dummy, because the RGBA8_UNORM dummy is not an integer format.
    for (uint32_t slot = 0; slot < m_MaxBandTextures; ++slot)
    {
        TextureHandle texture = m_BandDummyTexture;
        if (SlotEntry* entry = m_Binding1Entries.Find(slot))
        {
            SlotEntry& e = *entry;
            if (e.Texture.IsValid() && !m_Device->IsTextureHandleLive(e.Texture))
            {
                e.Texture = m_BandDummyTexture;
                e.Sampler = m_NearestSampler;
            }
            if (e.Texture.IsValid())
                texture = e.Texture;
        }
        if (texture.IsValid())
            m_SetWrites.push_back(
                {.Binding = 1, .ArrayElement = slot, .Type = DescriptorType::Texture, .Texture = texture});
    }

    return m_Device->GetOrCreateDescriptorSet(m_LayoutDesc, m_SetWrites, "UISdfTextureDescSet");
}

TextureHandle UITextureRegistry::ResolveSlotTextureLocked(
    FlatMap<uint32_t, SlotEntry>& entries, uint32_t index)
{
    SlotEntry* entry = entries.Find(index);
    if (!entry)
        return m_DummyTexture;

    TextureHandle texture = entry->Texture;
    if (texture.IsValid() && !m_Device->IsTextureHandleLive(texture))
    {
        texture = m_DummyTexture;
        entry->Texture = texture;
        entry->Sampler = m_DefaultSampler;
    }
    return texture.IsValid() ? texture : m_DummyTexture;
}

DescriptorSetHandle UITextureRegistry::GetDrawRunSet(const UICompatDrawRun& run)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device)
        return {};

    m_SetWrites.clear();
    if (m_DefaultSampler.IsValid())
        m_SetWrites.push_back(
            {.Binding = kCompatLinearSamplerBinding, .Type = DescriptorType::Sampler, .Sampler = m_DefaultSampler});
    if (m_NearestSampler.IsValid())
        m_SetWrites.push_back(
            {.Binding = kCompatNearestSamplerBinding, .Type = DescriptorType::Sampler, .Sampler = m_NearestSampler});

    // Every slot is written, claimed or not: the compat layout has no
    // partially-bound flag to lean on, and an unwritten binding is a validation
    // error on the strict backends this profile exists for.
    for (uint32_t i = 0; i < kUiCompatTextureSlots; ++i)
    {
        const TextureHandle tex = i < run.TextureCount
                                      ? ResolveSlotTextureLocked(m_Binding0Entries, run.Textures[i])
                                      : m_DummyTexture;
        m_SetWrites.push_back({.Binding = kCompatTextureBinding0 + i, .Type = DescriptorType::Texture, .Texture = tex});
    }
    for (uint32_t i = 0; i < kUiCompatGlyphSlots; ++i)
    {
        const bool claimed = i < run.GlyphCount;
        const TextureHandle curve =
            claimed ? ResolveSlotTextureLocked(m_Binding0Entries, run.GlyphCurves[i]) : m_DummyTexture;
        const TextureHandle band =
            claimed ? ResolveSlotTextureLocked(m_Binding1Entries, run.GlyphBands[i]) : m_BandDummyTexture;
        m_SetWrites.push_back(
            {.Binding = kCompatGlyphCurveBinding0 + i, .Type = DescriptorType::Texture, .Texture = curve});
        m_SetWrites.push_back({.Binding = kCompatGlyphBandBinding0 + i, .Type = DescriptorType::Texture, .Texture = band});
    }

    return m_Device->GetOrCreateDescriptorSet(m_LayoutDesc, m_SetWrites, "UISdfTextureDescSetCompat");
}

// Helper: upload raw data to a GPU texture via staging buffer.
static void UploadTextureData(IDevice* device, TextureHandle tex,
                              uint32_t w, uint32_t h,
                              const void* data, size_t dataSize, uint32_t rowPitch,
                              bool wasShaderResource)
{
    BufferDesc st{};
    st.size = dataSize;
    st.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    st.memoryUsage = BufferMemoryUsage::Upload;
    st.debugName = "UISdf_TexStaging";
    BufferHandle staging = device->CreateBuffer(st);
    device->UpdateBuffer(staging, 0, dataSize, data);

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex,
        wasShaderResource ? ResourceState::ShaderResource : ResourceState::Undefined,
        ResourceState::CopyDest));
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, w, h, 0, rowPitch);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::CopyDest, ResourceState::ShaderResource));
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->DestroyBuffer(staging);
}

// Convert a block of float values to half-float (IEEE 754 binary16).
// Used to upload Slug curve data into an RGBA16F texture.
static void ConvertFloatsToHalf(const float* src, uint16_t* dst, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        float f = src[i];
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        uint32_t sign = (bits >> 31) & 1;
        int32_t exp = ((bits >> 23) & 0xFF) - 127;
        uint32_t mantissa = bits & 0x7FFFFF;
        uint16_t h;
        if (exp > 15) h = static_cast<uint16_t>((sign << 15) | 0x7C00);
        else if (exp < -14) h = static_cast<uint16_t>(sign << 15);
        else h = static_cast<uint16_t>((sign << 15) | ((exp + 15) << 10) | (mantissa >> 13));
        dst[i] = h;
    }
}

const std::vector<UITextureRegistry::SlugTextureIndices>&
UITextureRegistry::RegisterSlugTextures(const FontAtlas& atlas)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    static const std::vector<SlugTextureIndices> kEmpty;
    if (!m_Device)
        return kEmpty;

    // Runs ahead of the read-only fast path below: after a rebuild the cached
    // pages name destroyed textures, so returning them would hand a worker
    // indices into dead slots. In practice the collect phase pre-registers every
    // font from the owner thread, so the heal lands there and workers only ever
    // see the already-healed cache.
    HealIfDeviceRebuiltLocked();

    const uint32_t atlasId = atlas.GetAtlasId();
    const int pageCount = atlas.GetSlugPageCount();
    if (pageCount <= 0)
        return kEmpty;

    // Read-only fast path: everything registered and up to date. This keeps
    // steady-state calls free of writes, which matters for the parallel
    // drain — workers re-request the slug pages per text emission while the
    // UI thread's collect phase has already registered/uploaded them.
    if (const SlugFontCache* cached = m_SlugCache.Find(atlasId);
        cached && cached->Pages.size() == static_cast<size_t>(pageCount))
    {
        bool current = true;
        for (int pi = 0; pi < pageCount; ++pi)
        {
            const SlugPageEntry& entry = cached->Pages[pi];
            if (!entry.CurveTexture.IsValid() ||
                entry.UploadedGen != atlas.GetSlugPageGeneration(pi))
            {
                current = false;
                break;
            }
        }
        if (current)
            return cached->Indices;
    }

    // Write path: it creates GPU textures, can reallocate Indices, and for a new atlas
    // moves the cache of every font. This invalidates the references that workers hold,
    // so the parallel drain registers every font during collect.
    assert(std::this_thread::get_id() == m_OwnerThreadId &&
           "RegisterSlugTextures write path reached from a non-owner thread");

    SlugFontCache& cache = m_SlugCache.GetOrInsert(atlasId);
    cache.Pages.resize(static_cast<size_t>(pageCount));
    cache.Indices.resize(static_cast<size_t>(pageCount));

    static constexpr int kTexWidth = FontAtlas::kSlugTextureWidth;
    static constexpr int kPageRows = FontAtlas::kSlugPageRows;

    for (int pi = 0; pi < pageCount; ++pi)
    {
        SlugPageEntry& entry = cache.Pages[pi];
        const uint32_t pageGen = atlas.GetSlugPageGeneration(pi);

        // First time we see this page: allocate the GPU textures and
        // claim stable bindless slots. Within a device generation neither the
        // textures nor the slots change after this — only the texture CONTENTS
        // are re-uploaded when pageGen advances. A device rebuild discards the
        // cache entirely, so the next call lands here again on fresh handles.
        if (!entry.CurveTexture.IsValid())
        {
            TextureDesc ct{};
            ct.width = kTexWidth;
            ct.height = kPageRows;
            ct.mipLevels = 1;
            ct.arrayLayers = 1;
            ct.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
            ct.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
                     | static_cast<uint32_t>(TextureUsage::TransferDst);
            ct.debugName = "UISlug_CurveTex";
            entry.CurveTexture = m_Device->CreateTexture(ct);
            entry.CurveSlot = RegisterPreUploadedLocked(entry.CurveTexture, m_DefaultSampler);

            TextureDesc bt{};
            bt.width = kTexWidth;
            bt.height = kPageRows;
            bt.mipLevels = 1;
            bt.arrayLayers = 1;
            bt.format = static_cast<uint32_t>(TextureFormat::R16G16_UINT);
            bt.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
                     | static_cast<uint32_t>(TextureUsage::TransferDst);
            bt.debugName = "UISlug_BandTex";
            entry.BandTexture = m_Device->CreateTexture(bt);
            entry.BandSlot = AllocateBandSlot();
            // Slot 0 means the band pool is exhausted; it holds the zero-filled
            // dummy, so this page's glyphs render blank instead of sampling a
            // slot that was never assigned to them.
            if (entry.BandSlot != 0)
            {
                SamplerHandle samp = m_NearestSampler.IsValid() ? m_NearestSampler : m_DefaultSampler;
                m_Binding1Entries.InsertOrAssign(entry.BandSlot, { entry.BandTexture, samp });
            }

            // uploadedGen is still 0 — the initial upload below will populate the texture.
        }

        // Re-upload only when the page's generation changes (new glyphs packed in).
        // Uploads the entire page; the in-use rows are contiguous at the top so
        // this is a single staged copy. Existing primitives keep pointing at the
        // same stable slot; only the texel data changes.
        if (entry.UploadedGen != pageGen)
        {
            int curveRows = atlas.GetSlugPageCurveRowsUsed(pi);
            int bandRows  = atlas.GetSlugPageBandRowsUsed(pi);
            if (curveRows < 1) curveRows = 1;
            if (bandRows  < 1) bandRows  = 1;

            // Curve data: convert float → half and upload.
            {
                const float* data = atlas.GetSlugPageCurveData(pi);
                size_t floatCount = static_cast<size_t>(kTexWidth) * curveRows * 4;
                m_HalfConversionBuffer.resize(floatCount);
                ConvertFloatsToHalf(data, m_HalfConversionBuffer.data(), floatCount);
                uint32_t rowPitch = static_cast<uint32_t>(kTexWidth * 4 * sizeof(uint16_t));
                UploadTextureData(m_Device, entry.CurveTexture,
                                  kTexWidth, static_cast<uint32_t>(curveRows),
                                  m_HalfConversionBuffer.data(),
                                  floatCount * sizeof(uint16_t), rowPitch,
                                  /*wasShaderResource=*/entry.UploadedGen != 0);
            }

            // Band data: direct upload.
            {
                const uint16_t* data = atlas.GetSlugPageBandData(pi);
                size_t dataSize = static_cast<size_t>(kTexWidth) * bandRows * 2 * sizeof(uint16_t);
                uint32_t rowPitch = static_cast<uint32_t>(kTexWidth * 2 * sizeof(uint16_t));
                UploadTextureData(m_Device, entry.BandTexture,
                                  kTexWidth, static_cast<uint32_t>(bandRows),
                                  data, dataSize, rowPitch,
                                  /*wasShaderResource=*/entry.UploadedGen != 0);
            }

            entry.UploadedGen = pageGen;
        }

        cache.Indices[pi] = {entry.CurveSlot, entry.BandSlot};
    }

    return cache.Indices;
}

uint32_t UITextureRegistry::RegisterColorAtlasPage(const FontAtlas& atlas, int pageIndex)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device)
        return 0;

    HealIfDeviceRebuiltLocked();

    const uint32_t atlasId = atlas.GetAtlasId();
    const uint64_t key = MakeAtlasPageKey(atlasId, pageIndex, /*isColor=*/true);
    const uint32_t contentGen = atlas.GetColorContentGeneration();

    if (const AtlasPageEntry* cached = m_AtlasPageCache.Find(key))
    {
        if (cached->Texture.IsValid() && cached->RegistryIndex != 0
            && cached->ContentGeneration == contentGen)
            return cached->RegistryIndex;
    }

    if (pageIndex >= atlas.GetColorPageCount())
        return 0;
    auto ref = atlas.GetColorAtlasRef(pageIndex);
    auto pixOpt = atlas.GetColorAtlasPixels(pageIndex);
    if (!pixOpt || ref.width <= 0 || ref.height <= 0)
        return 0;

    const auto& pix = *pixOpt;
    const uint32_t w = static_cast<uint32_t>(ref.width);
    const uint32_t h = static_cast<uint32_t>(ref.height);

    AtlasPageEntry& entry = m_AtlasPageCache.GetOrInsert(key);
    bool reuseTexture = entry.Texture.IsValid() && entry.Width == w && entry.Height == h;

    if (!reuseTexture)
    {
        if (entry.Texture.IsValid())
        {
            if (entry.RegistryIndex != 0) { UnregisterLocked(entry.RegistryIndex); entry.RegistryIndex = 0; }
            m_Device->DestroyTexture(entry.Texture);
            entry.Texture = {};
        }
        TextureDesc td{};
        td.width = w; td.height = h; td.mipLevels = 1; td.arrayLayers = 1;
        td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
                 | static_cast<uint32_t>(TextureUsage::TransferDst);
        td.debugName = "UISdf_ColorAtlas";
        entry.Texture = m_Device->CreateTexture(td);
        entry.Width = w; entry.Height = h;
    }
    if (!entry.Texture.IsValid())
        return 0;

    uint32_t rowPitch = static_cast<uint32_t>(pix.rowPitch);
    size_t uploadSize = pix.rowPitch * static_cast<size_t>(h);
    UploadTextureData(m_Device, entry.Texture, w, h, pix.data, uploadSize, rowPitch, reuseTexture);

    if (entry.RegistryIndex == 0)
        entry.RegistryIndex = RegisterPreUploadedLocked(entry.Texture, m_DefaultSampler);

    entry.ContentGeneration = contentGen;
    return entry.RegistryIndex;
}

} // namespace UI
} // namespace GameEngine
