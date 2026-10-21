#include "UI/BackgroundImageCache.h"

#include "BackgroundImageGeometry.h"
#include "BackgroundImageUpload.h"
#include "SvgBackgroundRaster.h"

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/SvgRasterizer.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Engine/Rendering/TextureMipUpload.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "UI/StyleUtil.h"
#include "UI/UITextureRegistry.h"
#include "UI/UIStyle.h"
#include "UI/UiDispatcher.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace GameEngine
{
namespace UI
{

namespace
{
// What an image is, learned from its registry path on first paint. An SVG is
// drawn from its baked sizes, a raster image from the asset's own raster.
enum class AssetKind : uint8_t
{
    Unknown,
    Raster,
    Svg
};

// One texture of the cache: an asset and the pixel size it was baked at. Size
// 0 x 0 is the asset's own raster; an SVG's sizes carry their drawn size. Keys
// the in-flight uploads, the ready callbacks and the settle waits.
struct Key
{
    GUID Guid;
    SvgRasterSize Size;
    bool operator==(const Key& other) const = default;
};

struct KeyHash
{
    size_t operator()(const Key& key) const
    {
        const uint64_t size = (static_cast<uint64_t>(key.Size.Width) << 32) | key.Size.Height;
        return std::hash<GUID>{}(key.Guid) ^ (size * 0x9E3779B97F4A7C15ull);
    }
};

// GPU bytes of an RGBA8 texture with `mipLevels` successive-halving levels.
uint64_t Rgba8MipChainBytes(uint32_t width, uint32_t height, uint32_t mipLevels)
{
    uint64_t bytes = 0;
    for (uint32_t level = 0; level < mipLevels; ++level)
        bytes += static_cast<uint64_t>(std::max(1u, width >> level)) * std::max(1u, height >> level) * 4u;
    return bytes;
}

// Path -> GUID cache key. The source alias is part of it, so editor:Icons/foo.png
// cannot collide with project Icons/foo.png.
std::string MakePathKey(std::string_view path, std::string_view sourceAlias)
{
    if (sourceAlias.empty())
        return std::string(path);
    std::string key;
    key.reserve(sourceAlias.size() + 1u + path.size());
    key.append(sourceAlias);
    key.push_back('|');
    key.append(path);
    return key;
}

// The in-flight key size of an SVG source load: not a texture, so no raster
// size and not the asset raster's 0 x 0.
constexpr SvgRasterSize kSvgSourceLoadSize{~0u, ~0u};

// How long a new drawn size of an SVG that already has a baked size must be
// asked for before it bakes. A resize drag or a slider changes the size every
// frame; this bakes where the drag rests instead of every size it passes.
constexpr auto kSvgSizeSettleTime = std::chrono::milliseconds(150);

// The cache's GPU byte budget before idle textures are evicted.
constexpr uint64_t kDefaultBudgetBytes = 256ull * 1024 * 1024;

// SvgVariants order: by width, then height.
bool VariantSizeLess(const SvgBackgroundVariant& variant, SvgRasterSize size)
{
    return variant.Size.Width != size.Width ? variant.Size.Width < size.Width : variant.Size.Height < size.Height;
}

TextureAsset* AsTexture(const SharedPtr<Asset>& asset)
{
    return asset ? dynamic_cast<TextureAsset*>(asset.get()) : nullptr;
}

// SVG or raster, from the asset's registry path; Raster when unknown.
AssetKind ResolveAssetKind(AssetManager* assets, const GUID& guid)
{
    AssetMetadata metadata{};
    if (!assets || !assets->GetRegistry().TryGetAssetMetadata(guid, metadata))
        return AssetKind::Raster;
    std::string extension = metadata.Path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".svg")
        return AssetKind::Raster;
    // Nine-slice cuts are authored in the asset's own raster pixels, so a
    // nine-sliced SVG draws from that raster rather than a drawn-size bake.
    std::string sliceValue;
    NineSlice slice;
    if (assets->GetRegistry().TryGetMetaValue(metadata.Path, kTextureNineSliceMetaKey, sliceValue) &&
        ParseTextureNineSliceMeta(sliceValue, slice) && slice.IsSliced())
        return AssetKind::Raster;
    return AssetKind::Svg;
}
} // namespace

// A finished bake on its way to the UI thread.
struct BackgroundImageCache::SvgBake
{
    GUID Guid;
    SvgRasterSize Size;
    // The source the raster was made from: the completion drops the raster
    // when a reload replaced it in the meantime.
    std::shared_ptr<const TextureAsset::SvgSource> Source;
    SvgRasterizedImage Image;
    bool Srgb = false;
    std::chrono::microseconds RasterTime{};
};

struct BackgroundImageCache::CachedTexture
{
    // What the asset is, learned from its registry path on first paint. An SVG
    // is drawn from SvgVariants, a raster image from Handle.
    AssetKind Kind = AssetKind::Unknown;
    // The asset's own raster. Every raster image has one; an SVG has one only
    // when a caller asked for it by GUID without a size, or draws it nine-sliced.
    Rendering::TextureHandle Handle = Rendering::INVALID_TEXTURE_HANDLE;
    uint32_t Width = 1;
    uint32_t Height = 1;
    // The SVG's intrinsic size in CSS px, 0 until its asset has loaded.
    float SvgWidth = 0.0f;
    float SvgHeight = 0.0f;
    // Rasters of the SVG at the sizes its draws asked for, ordered by width
    // then height. Each is its own GPU texture, aged and evicted on its own.
    std::vector<SvgBackgroundVariant> SvgVariants;
    // Levels the raster carries; the byte budget counts every one.
    uint32_t MipLevels = 1;
    // A borrowed handle (a test seed) is never destroyed here.
    bool Borrowed = false;
    // 9-slice layout authored on the texture asset, read at upload.
    NineSlice Slice{};
    // State::Frame when the raster was last resolved for paint.
    uint64_t LastUsedFrame = 0;
};

struct BackgroundImageCache::State
{
    // Path -> GUID resolutions that succeeded.
    std::unordered_map<std::string, GUID> PathToGuid;
    std::unordered_map<GUID, CachedTexture> Entries;
    // Monotonic paint-frame counter the cache ages on, advanced by EndFrame.
    uint64_t Frame = 0;
    uint64_t BudgetBytes = kDefaultBudgetBytes;

    // In-flight markers, stamped. The dedupe check reads this before issuing a
    // request, so a completion that never arrives would otherwise block every
    // later attempt for the session. A stale marker is retried instead of
    // believed, a bounded number of times so a genuinely absent image cannot spin.
    struct Upload
    {
        std::chrono::steady_clock::time_point Issued;
        uint32_t Attempts = 0;
    };
    std::unordered_map<Key, Upload, KeyHash> InFlight;
    std::unordered_map<Key, std::vector<ReadyCallback>, KeyHash> ReadyCallbacks;

    // A new drawn size of an SVG waiting to settle before it bakes, one per
    // (SVG, size): two draws of one SVG at two new sizes wait side by side.
    struct Settling
    {
        std::chrono::steady_clock::time_point Since;
        // Set when the repaint that asks for the settled size has been issued.
        bool Repainted = false;
    };
    std::unordered_map<Key, Settling, KeyHash> Settle;

    bool AppliesAsyncResults = true;
};

BackgroundImageCache::BackgroundImageCache(Rendering::IDevice* device, AssetManager* assets,
                                           std::shared_ptr<UiDispatcher> dispatcher, Hooks hooks)
    : m_Device(device),
      m_Assets(assets),
      m_Dispatcher(std::move(dispatcher)),
      m_Hooks(std::move(hooks)),
      m_State(std::make_unique<State>())
{
}

BackgroundImageCache::~BackgroundImageCache()
{
    if (!m_Device)
        return;
    for (auto& [guid, entry] : m_State->Entries)
    {
        if (entry.Handle.IsValid() && !entry.Borrowed)
            m_Device->DestroyTexture(entry.Handle);
        for (const SvgBackgroundVariant& variant : entry.SvgVariants)
            if (variant.Handle.IsValid())
                m_Device->DestroyTexture(variant.Handle);
    }
}

GUID BackgroundImageCache::ResolvePath(const std::string& path, const std::string& sourceAlias)
{
    if (path.empty() || !m_Assets)
        return GUID::Null();

    const std::string norm = UIUtil::NormalizeCssUrlPath(path);
    if (norm.empty())
        return GUID::Null();

    // Source-aware resolution: when CSS specified `url("editor:path")` or
    // `url("@editor/path")`, look up under exactly that source (no implicit
    // priority fallback). Let AssetManager register the file as part of
    // resolving the GUID; this covers first-run editor assets whose registry
    // entries do not exist yet immediately after a fresh build.
    const GUID guid = sourceAlias.empty() ? m_Assets->ResolveAssetGuid(norm)
                                          : m_Assets->ResolveAssetGuid(norm, sourceAlias);
    if (!guid.IsNull())
    {
        m_State->PathToGuid[MakePathKey(path, sourceAlias)] = guid;
        if (norm != path)
            m_State->PathToGuid[MakePathKey(norm, sourceAlias)] = guid;
    }
    return guid;
}

void BackgroundImageCache::Evict(const std::string& path)
{
    if (path.empty())
        return;

    const std::string norm = UIUtil::NormalizeCssUrlPath(path);
    GUID guid = GUID::Null();
    auto& pathToGuid = m_State->PathToGuid;
    if (auto it = pathToGuid.find(path); it != pathToGuid.end())
        guid = it->second;
    else if (norm != path)
    {
        if (auto it2 = pathToGuid.find(norm); it2 != pathToGuid.end())
            guid = it2->second;
    }

    // Remove cached entries so the next load starts fresh.
    pathToGuid.erase(path);
    if (norm != path)
        pathToGuid.erase(norm);

    if (!guid.IsNull())
        Evict(guid);
}

void BackgroundImageCache::Evict(const GUID& guid)
{
    if (guid.IsNull())
        return;
    auto itEntry = m_State->Entries.find(guid);
    if (itEntry == m_State->Entries.end())
        return;

    CachedTexture& entry = itEntry->second;
    // Drop the texture-registry slot before the GPU texture: once the texture
    // is destroyed a slot would reference a freed handle and bind it next frame.
    if (entry.Handle.IsValid())
    {
        if (m_Hooks.ReleaseSlot)
            m_Hooks.ReleaseSlot(entry.Handle);
        if (m_Device && !entry.Borrowed)
            m_Device->DestroyTexture(entry.Handle);
    }
    // An SVG's drawn sizes go with it: a reload must re-bake every size.
    for (const SvgBackgroundVariant& variant : entry.SvgVariants)
        DestroySvgTexture(variant);
    m_State->Entries.erase(itEntry);

    // Drop in-flight state at every size, so the next request or paint can
    // start a fresh load.
    std::erase_if(m_State->InFlight, [&guid](const auto& kv) { return kv.first.Guid == guid; });
}

void BackgroundImageCache::DestroySvgTexture(const SvgBackgroundVariant& variant)
{
    if (!variant.Handle.IsValid())
        return;
    if (m_Hooks.ReleaseSlot)
        m_Hooks.ReleaseSlot(variant.Handle);
    if (m_Device)
        m_Device->DestroyTexture(variant.Handle);
}

void BackgroundImageCache::EvictTexture(const GUID& guid, SvgRasterSize size)
{
    auto itEntry = m_State->Entries.find(guid);
    if (itEntry == m_State->Entries.end())
        return;
    CachedTexture& entry = itEntry->second;
    if (size == SvgRasterSize{})
    {
        if (entry.Handle.IsValid())
        {
            if (m_Hooks.ReleaseSlot)
                m_Hooks.ReleaseSlot(entry.Handle);
            if (m_Device && !entry.Borrowed)
                m_Device->DestroyTexture(entry.Handle);
        }
        entry.Handle = Rendering::INVALID_TEXTURE_HANDLE;
    }
    else
    {
        auto itVariant = std::find_if(entry.SvgVariants.begin(), entry.SvgVariants.end(),
                                      [size](const SvgBackgroundVariant& v) { return v.Size == size; });
        if (itVariant != entry.SvgVariants.end())
        {
            DestroySvgTexture(*itVariant);
            entry.SvgVariants.erase(itVariant);
        }
    }
    m_State->InFlight.erase(Key{guid, size});
    if (!entry.Handle.IsValid() && entry.SvgVariants.empty())
        m_State->Entries.erase(itEntry);
}

uint64_t BackgroundImageCache::EntryBytes(const CachedTexture& entry)
{
    uint64_t bytes = 0;
    if (entry.Handle.IsValid() && !entry.Borrowed)
        bytes += Rgba8MipChainBytes(entry.Width, entry.Height, entry.MipLevels);
    for (const SvgBackgroundVariant& variant : entry.SvgVariants)
        if (variant.Handle.IsValid())
            bytes += Rgba8MipChainBytes(variant.Size.Width, variant.Size.Height, variant.MipLevels);
    return bytes;
}

void BackgroundImageCache::EndFrame(const std::function<std::vector<Rendering::TextureHandle>()>& drawnTextures)
{
    constexpr uint64_t kSweepIntervalFrames = 120;
    ++m_State->Frame;
    if ((m_State->Frame % kSweepIntervalFrames) == 0)
        SweepIdle(drawnTextures);
    RepaintSettledSvgSizes();
}

void BackgroundImageCache::SweepIdle(const std::function<std::vector<Rendering::TextureHandle>()>& drawnTextures)
{
    // The cache retains one GPU texture per distinct image and size ever
    // painted, so a long asset-browsing session grows it without bound. Once it
    // exceeds the byte budget, textures idle past the grace window drop
    // oldest-first. Textures the current draw binds are protected even when
    // their primitives were retained without regenerating. The budget is soft
    // when the visible working set alone exceeds it.
    //
    // Nothing painted this recently is evictable, whatever the budget says: it
    // is likely still on screen (retained primitives don't re-touch).
    constexpr uint64_t kMinIdleFrames = 600;

    State& state = *m_State;
    uint64_t totalBytes = 0;
    for (const auto& kv : state.Entries)
        totalBytes += EntryBytes(kv.second);
    if (totalBytes <= state.BudgetBytes)
        return;

    std::unordered_set<uint64_t> visibleHandles;
    if (drawnTextures)
        for (const Rendering::TextureHandle handle : drawnTextures())
            visibleHandles.insert(static_cast<uint64_t>(handle.id));

    // One candidate per texture: the asset raster (size 0 x 0) and each SVG
    // size age and evict on their own, so a resize leaves only the sizes still
    // drawn.
    struct Candidate
    {
        GUID Guid;
        SvgRasterSize Size;
        uint64_t LastUsed = 0;
        uint64_t Bytes = 0;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(state.Entries.size());
    for (auto& [guid, entry] : state.Entries)
    {
        if (entry.Handle.IsValid() && !entry.Borrowed)
        {
            if (visibleHandles.contains(static_cast<uint64_t>(entry.Handle.id)))
                entry.LastUsedFrame = state.Frame;
            else if (entry.LastUsedFrame + kMinIdleFrames <= state.Frame)
                candidates.push_back({guid, SvgRasterSize{}, entry.LastUsedFrame,
                                      Rgba8MipChainBytes(entry.Width, entry.Height, entry.MipLevels)});
        }
        for (SvgBackgroundVariant& variant : entry.SvgVariants)
        {
            if (!variant.Handle.IsValid())
                continue;
            if (visibleHandles.contains(static_cast<uint64_t>(variant.Handle.id)))
                variant.LastUsedFrame = state.Frame;
            else if (variant.LastUsedFrame + kMinIdleFrames <= state.Frame)
                candidates.push_back({guid, variant.Size, variant.LastUsedFrame,
                                      Rgba8MipChainBytes(variant.Size.Width, variant.Size.Height, variant.MipLevels)});
        }
    }
    if (candidates.empty())
        return;

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.LastUsed < b.LastUsed; });

    size_t evictedCount = 0;
    uint64_t evictedBytes = 0;
    for (const Candidate& c : candidates)
    {
        if (totalBytes <= state.BudgetBytes)
            break;
        EvictTexture(c.Guid, c.Size);
        totalBytes -= std::min(totalBytes, c.Bytes);
        evictedBytes += c.Bytes;
        ++evictedCount;
    }

    if (evictedCount > 0)
    {
        // A style-dirty pass alone can skip regen when CSS is unchanged,
        // leaving retained primitives bound to the dummy slot; repaint so
        // still-visible images re-resolve at once.
        if (m_Hooks.Repaint)
            m_Hooks.Repaint();
        Logger::Log::Debug("[UI] Background texture cache over budget: evicted {} entries ({} KB)", evictedCount,
                           evictedBytes / 1024);
    }
}

void BackgroundImageCache::EnsureUploaded(const GUID& guid)
{
    if (guid.IsNull() || !m_Assets || !m_Device)
        return;

    if (auto it = m_State->Entries.find(guid); it != m_State->Entries.end() && it->second.Handle.IsValid())
    {
        it->second.LastUsedFrame = m_State->Frame;
        return;
    }

    // Asset already loaded: upload now.
    if (TextureAsset* texture = AsTexture(m_Assets->GetAsset(guid)))
    {
        if (texture->GetWidth() > 0 && texture->GetHeight() > 0)
        {
            uint32_t mipLevels = 1;
            const Rendering::TextureHandle handle = UploadBackgroundImageFromAsset(m_Device, texture, mipLevels);
            if (handle.IsValid())
            {
                CachedTexture& entry = m_State->Entries[guid];
                entry.Handle = handle;
                entry.Width = texture->GetWidth();
                entry.Height = texture->GetHeight();
                entry.MipLevels = mipLevels;
                entry.Borrowed = false;
                entry.Slice = ResolveNineSlice(guid);
                entry.LastUsedFrame = m_State->Frame;
                return;
            }
        }
    }

    // Not loaded yet: start the load. Request dedupes, so repeated calls are cheap.
    Request(guid, nullptr);
}

NineSlice BackgroundImageCache::ResolveNineSlice(const GUID& guid) const
{
    NineSlice slice;
    if (guid.IsNull() || !m_Assets)
        return slice;
    const SharedPtr<Asset> asset = m_Assets->GetAsset(guid);
    if (!asset)
        return slice;
    std::string value;
    if (m_Assets->GetRegistry().TryGetMetaValue(asset->GetPath(), kTextureNineSliceMetaKey, value))
        ParseTextureNineSliceMeta(value, slice);
    return slice;
}

uint32_t BackgroundImageCache::TryRegister(const std::string& path, const std::string& sourceAlias,
                                           UITextureRegistry& registry, uint32_t* outWidth, uint32_t* outHeight,
                                           Rendering::SamplerHandle sampler)
{
    if (outWidth)
        *outWidth = 0;
    if (outHeight)
        *outHeight = 0;

    const GUID guid = ResolvePath(path, sourceAlias);
    if (guid.IsNull())
        return 0;

    EnsureUploaded(guid);
    auto it = m_State->Entries.find(guid);
    if (it == m_State->Entries.end() || !it->second.Handle.IsValid())
        return 0;

    if (outWidth)
        *outWidth = it->second.Width;
    if (outHeight)
        *outHeight = it->second.Height;
    return registry.Register(it->second.Handle, sampler);
}

Rendering::TextureHandle BackgroundImageCache::TryGetHandle(const GUID& guid, uint32_t* outWidth, uint32_t* outHeight)
{
    if (outWidth)
        *outWidth = 0;
    if (outHeight)
        *outHeight = 0;
    if (guid.IsNull())
        return Rendering::INVALID_TEXTURE_HANDLE;

    EnsureUploaded(guid);
    auto it = m_State->Entries.find(guid);
    if (it == m_State->Entries.end() || !it->second.Handle.IsValid())
        return Rendering::INVALID_TEXTURE_HANDLE;

    if (outWidth)
        *outWidth = it->second.Width;
    if (outHeight)
        *outHeight = it->second.Height;
    return it->second.Handle;
}

void BackgroundImageCache::Request(const GUID& guid, ReadyCallback onReady)
{
    if (guid.IsNull() || !m_Assets)
    {
        if (onReady)
            onReady(Rendering::TextureHandle{}, 0, 0);
        return;
    }

    // Already uploaded: answer at once.
    if (auto it = m_State->Entries.find(guid); it != m_State->Entries.end() && it->second.Handle)
    {
        it->second.LastUsedFrame = m_State->Frame;
        if (onReady)
            onReady(it->second.Handle, it->second.Width, it->second.Height);
        return;
    }

    const Key key{guid, SvgRasterSize{}};
    if (onReady)
        m_State->ReadyCallbacks[key].push_back(std::move(onReady));
    if (!BeginUpload(guid, SvgRasterSize{}))
        return;

    // Async loads can outlive a tool window. The load completes on a worker,
    // which posts through the shared dispatcher: once the owner is gone the
    // closed dispatcher refuses the post, and posted work runs only from the
    // owner's own drains.
    std::shared_ptr<UiDispatcher> dispatcher = m_Dispatcher;
    if (const SharedPtr<Asset> asset = m_Assets->GetAsset(guid))
    {
        if (AsTexture(asset))
            dispatcher->Post([this, guid]() { UploadRasterFromLoadedAsset(guid); });
        else
            dispatcher->Post(
                [this, guid]()
                {
                    // Not a texture asset: complete the callbacks with failure.
                    if (m_State->AppliesAsyncResults)
                        CompleteRaster(guid, Rendering::TextureHandle{}, 0, 0, 1);
                });
        return;
    }
    m_Assets->LoadAsset(guid, [dispatcher, this, guid](Result<SharedPtr<Asset>, AssetError>)
                        { dispatcher->Post([this, guid]() { UploadRasterFromLoadedAsset(guid); }); });
}

void BackgroundImageCache::UploadRasterFromLoadedAsset(const GUID& guid)
{
    if (!m_State->AppliesAsyncResults)
        return;
    Rendering::TextureHandle handle{};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    if (TextureAsset* texture = AsTexture(m_Assets ? m_Assets->GetAsset(guid) : SharedPtr<Asset>{}))
    {
        width = texture->GetWidth();
        height = texture->GetHeight();
        if (width && height)
            handle = UploadBackgroundImageFromAsset(m_Device, texture, mipLevels);
    }
    CompleteRaster(guid, handle, width, height, mipLevels);
}

void BackgroundImageCache::CompleteRaster(const GUID& guid, Rendering::TextureHandle handle, uint32_t width,
                                          uint32_t height, uint32_t mipLevels)
{
    const Key key{guid, SvgRasterSize{}};
    CachedTexture& entry = m_State->Entries[guid];
    if (handle && entry.Handle.IsValid() && entry.Handle != handle)
    {
        // A retried request can land after the one it replaced finally arrives.
        // Keep the texture already cached (primitives may reference its slot)
        // and free the late duplicate rather than overwriting and leaking it.
        if (m_Device && !entry.Borrowed)
            m_Device->DestroyTexture(handle);
        handle = entry.Handle;
    }
    if (handle)
    {
        entry.Handle = handle;
        entry.Width = width;
        entry.Height = height;
        entry.MipLevels = mipLevels;
        entry.Borrowed = false;
        entry.Slice = ResolveNineSlice(guid);
        entry.LastUsedFrame = m_State->Frame;
    }
    static const bool s_Log = []() -> bool
    {
        const char* e = std::getenv("GE_UI_GEOM_INVALIDATE_LOG");
        return (e && e[0] == '1');
    }();
    static int s_Budget = 24;
    if (s_Log && s_Budget-- > 0)
        Logger::Log::Warning("[UI GeoInvalidate] BackgroundTextureReady(guid={})", guid.ToString());

    m_State->InFlight.erase(key);
    if (!handle)
    {
        // The request completed with nothing to draw: the asset is not a
        // texture, or it decoded to zero. The element stays blank and nothing
        // further is coming, so say so once.
        Logger::Log::Warning("UI: background image {} completed with no texture; it stays blank", guid.ToString());
    }
    if (auto itCallbacks = m_State->ReadyCallbacks.find(key); itCallbacks != m_State->ReadyCallbacks.end())
    {
        std::vector<ReadyCallback> callbacks = std::move(itCallbacks->second);
        m_State->ReadyCallbacks.erase(itCallbacks);
        for (auto& callback : callbacks)
            callback(entry.Handle, entry.Width, entry.Height);
    }

    // Paints that asked with no callback draw nothing until a regen; a
    // style-dirty refresh alone can be ignored by elements with no layout yet,
    // so repaint as soon as the texture is available.
    if (handle && m_Hooks.Repaint)
        m_Hooks.Repaint();
}

bool BackgroundImageCache::BeginUpload(const GUID& guid, SvgRasterSize size)
{
    const Key key{guid, size};
    // Already in flight, unless the request that set the marker never came
    // back. A completion can be lost (a dispatcher that stops draining, a load
    // that never resolves), and the marker outlives it. Past the stale window
    // the request is re-issued rather than assumed to still be coming.
    static constexpr auto kUploadStale = std::chrono::seconds(5);
    static constexpr uint32_t kMaxUploadAttempts = 3;
    const auto now = std::chrono::steady_clock::now();
    auto itFlight = m_State->InFlight.find(key);
    if (itFlight == m_State->InFlight.end())
    {
        m_State->InFlight.emplace(key, State::Upload{now, 1});
        return true;
    }
    if (now - itFlight->second.Issued < kUploadStale)
        return false;
    if (itFlight->second.Attempts >= kMaxUploadAttempts)
    {
        if (itFlight->second.Attempts == kMaxUploadAttempts)
        {
            ++itFlight->second.Attempts; // log once, not every paint
            Logger::Log::Warning("UI: background texture {} ({}x{}) never completed in {} attempts; it stays blank",
                                 key.Guid.ToString(), key.Size.Width, key.Size.Height, kMaxUploadAttempts);
        }
        return false;
    }
    itFlight->second.Issued = now;
    ++itFlight->second.Attempts;
    return true;
}

BackgroundImageCache::PaintBinding BackgroundImageCache::ResolveForPaint(const BackgroundImageStyle& background,
                                                                         float paddingWidth, float paddingHeight,
                                                                         float contentScale)
{
    const BackgroundImageSource& source = background.Source;
    GUID guid = source.Guid;
    if (source.Kind == BackgroundImageSource::SourceKind::Path && guid.IsNull())
    {
        auto itGuid = m_State->PathToGuid.find(MakePathKey(source.Value, source.SourceAlias));
        guid = itGuid != m_State->PathToGuid.end() ? itGuid->second : ResolvePath(source.Value, source.SourceAlias);
    }
    if (guid.IsNull())
        return {};

    auto itEntry = m_State->Entries.try_emplace(guid).first;
    if (itEntry->second.Kind == AssetKind::Unknown)
        itEntry->second.Kind = ResolveAssetKind(m_Assets, guid);
    // A border-image measures its slices in the image's own pixels, so it draws
    // from the asset raster, as a nine-sliced SVG asset does (ResolveAssetKind);
    // every other SVG draw bakes its size.
    if (itEntry->second.Kind == AssetKind::Svg && !background.HasBorderImage)
        return ResolveSvgForPaint(guid, itEntry->second, background, paddingWidth, paddingHeight, contentScale);

    if (!itEntry->second.Handle.IsValid())
    {
        // EnsureUploaded completes at once when the asset is already loaded (the
        // startup preload), so the first paint binds without a later refresh.
        EnsureUploaded(guid);
        itEntry = m_State->Entries.find(guid);
        if (itEntry == m_State->Entries.end() || !itEntry->second.Handle.IsValid())
            return {};
    }
    CachedTexture& entry = itEntry->second;
    entry.LastUsedFrame = m_State->Frame;
    PaintBinding binding;
    binding.Handle = entry.Handle;
    binding.Width = entry.Width;
    binding.Height = entry.Height;
    binding.Slice = entry.Slice;
    return binding;
}

BackgroundImageCache::PaintBinding BackgroundImageCache::ResolveSvgForPaint(const GUID& guid, CachedTexture& entry,
                                                                            const BackgroundImageStyle& background,
                                                                            float paddingWidth, float paddingHeight,
                                                                            float contentScale)
{
    PaintBinding binding;
    // The raster size depends on the SVG's intrinsic size: Cover and Contain
    // need its aspect, Auto its size. Until the asset has loaded nothing can be
    // chosen.
    if (entry.SvgWidth <= 0.0f)
    {
        TextureAsset* texture = AsTexture(m_Assets ? m_Assets->GetAsset(guid) : SharedPtr<Asset>{});
        if (!texture)
        {
            RequestSvgSource(guid);
            return binding;
        }
        const std::shared_ptr<const TextureAsset::SvgSource> source = texture->SnapshotSvgSource();
        if (!source || source->Width <= 0.0f || source->Height <= 0.0f)
        {
            // Loaded without a source: not drawable as an SVG. The raster path
            // takes it with its bounded retries.
            entry.Kind = AssetKind::Raster;
            return binding;
        }
        entry.SvgWidth = source->Width;
        entry.SvgHeight = source->Height;
    }

    binding.NaturalWidth = entry.SvgWidth * contentScale;
    binding.NaturalHeight = entry.SvgHeight * contentScale;
    const BackgroundImageSize drawn = ComputeBackgroundImageSize(background, paddingWidth, paddingHeight, contentScale,
                                                                 binding.NaturalWidth, binding.NaturalHeight);
    const SvgRasterSize size = SvgBackgroundRasterSize(drawn);

    // Bind the raster baked at the drawn size; while it bakes, the nearest size
    // already baked, so a resize never blanks the icon.
    const size_t bound = NearestSvgBackgroundVariant(entry.SvgVariants, size);
    const bool haveBound = bound != entry.SvgVariants.size();
    if (!haveBound || entry.SvgVariants[bound].Size != size)
        RequestSvgSize(guid, size, haveBound && IsSvgRasterSizeCloseEnough(entry.SvgVariants[bound].Size, size));
    if (!haveBound)
        return binding;

    SvgBackgroundVariant& variant = entry.SvgVariants[bound];
    variant.LastUsedFrame = m_State->Frame;
    binding.Handle = variant.Handle;
    binding.Width = variant.Size.Width;
    binding.Height = variant.Size.Height;
    binding.TexelExact = variant.Size == size;
    return binding;
}

void BackgroundImageCache::RequestSvgSource(const GUID& guid)
{
    if (!m_Assets || !BeginUpload(guid, kSvgSourceLoadSize))
        return;
    std::shared_ptr<UiDispatcher> dispatcher = m_Dispatcher;
    m_Assets->LoadAsset(
        guid,
        [dispatcher, this, guid](Result<SharedPtr<Asset>, AssetError>)
        {
            dispatcher->Post(
                [this, guid]()
                {
                    if (!m_State->AppliesAsyncResults)
                        return;
                    m_State->InFlight.erase(Key{guid, kSvgSourceLoadSize});
                    TextureAsset* texture = AsTexture(m_Assets ? m_Assets->GetAsset(guid) : SharedPtr<Asset>{});
                    if (!texture || !texture->SnapshotSvgSource())
                    {
                        // No source will come (a load that failed, or a build
                        // without ThorVG). Hand the asset to the raster path,
                        // whose retries are bounded, instead of asking and
                        // repainting every paint.
                        if (auto it = m_State->Entries.find(guid); it != m_State->Entries.end())
                            it->second.Kind = AssetKind::Raster;
                        return;
                    }
                    // The next paint reads the size and asks for its raster.
                    if (m_Hooks.Repaint)
                        m_Hooks.Repaint();
                });
        });
}

bool BackgroundImageCache::SvgSizeHasSettled(const GUID& guid, SvgRasterSize size)
{
    // One wait per (SVG, size): two elements that draw one SVG at two new sizes
    // settle side by side instead of restarting each other's wait.
    const auto now = std::chrono::steady_clock::now();
    auto [it, inserted] = m_State->Settle.try_emplace(Key{guid, size}, State::Settling{now});
    if (now - it->second.Since < kSvgSizeSettleTime)
        return false;
    m_State->Settle.erase(it);
    return true;
}

void BackgroundImageCache::RepaintSettledSvgSizes()
{
    // A wait that had its repaint and was not asked for again is a size a
    // resize passed through: the paint after the repaint would have settled it.
    std::erase_if(m_State->Settle, [](const auto& kv) { return kv.second.Repainted; });

    const auto now = std::chrono::steady_clock::now();
    bool repaint = false;
    for (auto& [key, settling] : m_State->Settle)
    {
        if (now - settling.Since < kSvgSizeSettleTime)
            continue;
        settling.Repainted = true;
        repaint = true;
    }
    // Retained primitives do not ask again on their own: repaint once so the
    // paint that drew the nearest size asks for its own size, now settled.
    if (repaint && m_Hooks.Repaint)
        m_Hooks.Repaint();
}

void BackgroundImageCache::RequestSvgSize(const GUID& guid, SvgRasterSize size, bool drawsFromCloseSize)
{
    if (guid.IsNull() || size.Width == 0 || size.Height == 0 || !m_Assets || !m_Device)
        return;
    // A size with no close baked size to draw from bakes at once: the first
    // raster of an SVG, or a resize past twice or half the size baked. A size
    // close to one already drawn bakes once it has settled, and the nearest
    // baked size draws meanwhile.
    if (drawsFromCloseSize && !SvgSizeHasSettled(guid, size))
        return;
    const Key key{guid, size};
    if (!BeginUpload(guid, size))
        return;

    TextureAsset* texture = AsTexture(m_Assets->GetAsset(guid));
    std::shared_ptr<const TextureAsset::SvgSource> source = texture ? texture->SnapshotSvgSource() : nullptr;
    if (!source)
    {
        m_State->InFlight.erase(key);
        return;
    }
    const bool srgb = texture->GetColorSpace() == TextureColorSpace::SRGB;

    // The worker touches only what it captured: the source snapshot (immutable,
    // and the token the completion checks against a reload) and the
    // dispatcher. The cache is reached only from the posted completion, which
    // runs from the owner's drains and not at all once the dispatcher is
    // closed. Bakes run side by side, as asset loads rasterize.
    std::shared_ptr<UiDispatcher> dispatcher = m_Dispatcher;
    BackgroundImageCache* cache = this;
    auto bake = [dispatcher, cache, guid, size, source, srgb]()
    {
        auto result = std::make_shared<SvgBake>();
        result->Guid = guid;
        result->Size = size;
        result->Source = source;
        result->Srgb = srgb;
        const auto begin = std::chrono::steady_clock::now();
        if (!RasterizeSvgToRgbaAtDimensions(source->Text, size.Width, size.Height, result->Image))
            result->Image.Width = 0;
        result->RasterTime =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin);
        dispatcher->Post([cache, result]() { cache->CompleteSvgSize(*result); });
    };
    if (m_Assets->HasJobSystem())
        m_Assets->GetJobSystem().Submit(std::move(bake));
    else
        bake();
}

void BackgroundImageCache::CompleteSvgSize(const SvgBake& bake)
{
    const GUID& guid = bake.Guid;
    const SvgRasterSize size = bake.Size;
    const SvgRasterizedImage& image = bake.Image;
    const bool srgb = bake.Srgb;
    const std::chrono::microseconds rasterTime = bake.RasterTime;
    if (!m_State->AppliesAsyncResults)
        return;
    m_State->InFlight.erase(Key{guid, size});

    // A bake that started before a reload carries the old picture. The reload
    // evicted the entry and repainted, so the next paint asks again.
    TextureAsset* texture = AsTexture(m_Assets ? m_Assets->GetAsset(guid) : SharedPtr<Asset>{});
    const std::shared_ptr<const TextureAsset::SvgSource> current = texture ? texture->SnapshotSvgSource() : nullptr;
    if (!current || current != bake.Source)
        return;
    if (image.Width == 0 || image.Height == 0 || !image.Data)
    {
        Logger::Log::Warning("UI: SVG background {} failed to rasterize at {}x{} px; it stays blank", guid.ToString(),
                             size.Width, size.Height);
        return;
    }

    // Level 0 only: a size is drawn at the size it was baked at, so a chain
    // would cost the UI thread a CPU resample per bake for levels never read.
    // A size standing in for one still settling is at most 2x off
    // (IsSvgRasterSizeCloseEnough) and only until that size lands.
    const auto uploadBegin = std::chrono::steady_clock::now();
    TextureCookSettings levelZeroOnly;
    levelZeroOnly.MipsEnabled = false;
    uint32_t mipLevels = 1;
    const Rendering::TextureHandle handle = UploadRgba8WithMips(
        m_Device, image.Data.get(), image.Width, image.Height,
        srgb ? Rendering::TextureFormat::RGBA8_SRGB : Rendering::TextureFormat::RGBA8_UNORM, levelZeroOnly, srgb,
        "UI_BG_Svg", "UI_BG_Svg_Staging", mipLevels);
    if (!handle.IsValid())
        return;
    const auto uploadTime =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - uploadBegin);

    CachedTexture& entry = m_State->Entries[guid];
    entry.Kind = AssetKind::Svg;
    if (entry.SvgWidth <= 0.0f)
    {
        entry.SvgWidth = current->Width;
        entry.SvgHeight = current->Height;
    }
    SvgBackgroundVariant variant;
    variant.Size = size;
    variant.Handle = handle;
    variant.MipLevels = mipLevels;
    variant.LastUsedFrame = m_State->Frame;
    auto itVariant = std::lower_bound(entry.SvgVariants.begin(), entry.SvgVariants.end(), size, VariantSizeLess);
    if (itVariant != entry.SvgVariants.end() && itVariant->Size == size)
    {
        // A retried request landed after the one it replaced: keep one.
        DestroySvgTexture(*itVariant);
        *itVariant = variant;
    }
    else
    {
        entry.SvgVariants.insert(itVariant, variant);
    }

    // GE_UI_SVG_BAKE_LOG=1: one line per bake, the instrument for the cost of
    // opening an icon-heavy panel (bake count, raster and upload time).
    static const bool s_Log = []() -> bool
    {
        const char* e = std::getenv("GE_UI_SVG_BAKE_LOG");
        return (e && e[0] == '1');
    }();
    if (s_Log)
    {
        Logger::Log::Info("[UI SvgBake] {} {}x{} raster {:.3f} ms upload {:.3f} ms", guid.ToString(), image.Width,
                          image.Height, static_cast<double>(rasterTime.count()) / 1000.0,
                          static_cast<double>(uploadTime.count()) / 1000.0);
    }

    // Paints bound the nearest baked size or nothing while this baked; repaint
    // so they bind the exact one now.
    if (m_Hooks.Repaint)
        m_Hooks.Repaint();
}

bool BackgroundImageCache::OnTextureReloaded(const GUID& guid)
{
    if (guid.IsNull() || !m_State->Entries.contains(guid))
        return false;
    Evict(guid);
    return true;
}

std::pair<size_t, size_t> BackgroundImageCache::DropAfterDeviceRebuild()
{
    const std::pair<size_t, size_t> dropped{m_State->Entries.size(), m_State->InFlight.size()};
    m_State->Entries.clear();
    m_State->InFlight.clear();
    return dropped;
}

void BackgroundImageCache::StopApplyingAsyncResults()
{
    m_State->AppliesAsyncResults = false;
}

size_t BackgroundImageCache::PendingCount() const
{
    return m_State->InFlight.size();
}

size_t BackgroundImageCache::EntryCount() const
{
    return m_State->Entries.size();
}

bool BackgroundImageCache::HasRasterForTesting(const GUID& guid) const
{
    auto it = m_State->Entries.find(guid);
    return it != m_State->Entries.end() && it->second.Handle.IsValid();
}

uint32_t BackgroundImageCache::RasterMipLevelsForTesting(const GUID& guid) const
{
    auto it = m_State->Entries.find(guid);
    return it != m_State->Entries.end() && it->second.Handle.IsValid() ? it->second.MipLevels : 0u;
}

std::vector<SvgRasterSize> BackgroundImageCache::SvgSizesForTesting(const GUID& guid) const
{
    std::vector<SvgRasterSize> sizes;
    auto it = m_State->Entries.find(guid);
    if (it == m_State->Entries.end())
        return sizes;
    for (const SvgBackgroundVariant& variant : it->second.SvgVariants)
        if (variant.Handle.IsValid())
            sizes.push_back(variant.Size);
    return sizes;
}

Rendering::TextureHandle BackgroundImageCache::SvgTextureForTesting(const GUID& guid, SvgRasterSize size) const
{
    auto it = m_State->Entries.find(guid);
    if (it == m_State->Entries.end())
        return Rendering::INVALID_TEXTURE_HANDLE;
    for (const SvgBackgroundVariant& variant : it->second.SvgVariants)
        if (variant.Size == size && variant.Handle.IsValid())
            return variant.Handle;
    return Rendering::INVALID_TEXTURE_HANDLE;
}

uint64_t BackgroundImageCache::BytesForTesting() const
{
    uint64_t bytes = 0;
    for (const auto& kv : m_State->Entries)
        bytes += EntryBytes(kv.second);
    return bytes;
}

void BackgroundImageCache::SetBudgetForTesting(uint64_t bytes)
{
    m_State->BudgetBytes = bytes;
}

void BackgroundImageCache::SeedRasterForTesting(const GUID& guid, Rendering::TextureHandle handle, uint32_t width,
                                                uint32_t height)
{
    if (guid.IsNull())
        return;
    CachedTexture& entry = m_State->Entries[guid];
    entry.Handle = handle;
    entry.Width = width;
    entry.Height = height;
    entry.Borrowed = true;
}

} // namespace UI
} // namespace GameEngine
