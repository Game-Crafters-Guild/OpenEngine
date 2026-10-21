#pragma once

#include "AssetCore/NineSlice.h"
#include "Rendering/Core/Device.h"
#include "AssetCore/GUID.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
class AssetManager;
class UIManager;
struct BackgroundImageStyle;

namespace UI
{
class UiDispatcher;
class UITextureRegistry;
struct SvgRasterSize;
struct SvgBackgroundVariant;

/**
 * @brief The GPU textures a UIManager draws CSS background images from.
 *
 * A raster image (PNG, KTX2, ...) is uploaded once from its asset, with the mip
 * chain the UI sampler reads. An SVG is baked at the size each draw shows it,
 * in device pixels, and drawn texel for texel; each size is its own texture,
 * aged and evicted on its own. Loads, bakes and uploads are asynchronous: a
 * request returns at once, and the owner is asked to repaint when a texture
 * lands. Every call is made on the UI thread.
 */
class BackgroundImageCache
{
  public:
    /// What the cache asks of its owner.
    struct Hooks
    {
        /// Repaint the whole tree: a texture landed, or a size is ready to bake.
        std::function<void()> Repaint;
        /// Drop any texture-registry slot that names `handle` before it is destroyed.
        std::function<void(Rendering::TextureHandle)> ReleaseSlot;
    };

    using ReadyCallback = std::function<void(Rendering::TextureHandle, uint32_t, uint32_t)>;

    BackgroundImageCache(Rendering::IDevice* device, AssetManager* assets, std::shared_ptr<UiDispatcher> dispatcher,
                         Hooks hooks);
    ~BackgroundImageCache();
    BackgroundImageCache(const BackgroundImageCache&) = delete;
    BackgroundImageCache& operator=(const BackgroundImageCache&) = delete;

    /// Resolve a CSS background-image path to an asset GUID. A non-empty
    /// `sourceAlias` ("editor", "project") looks up under exactly that source,
    /// as `url("editor:path")` does; empty falls back to source priority.
    GUID ResolvePath(const std::string& path, const std::string& sourceAlias = {});
    /// Upload the asset's own raster now when it is loaded, else start its load.
    void EnsureUploaded(const GUID& guid);
    /// Upload the asset's own raster and call `onReady` on the UI thread with
    /// (handle, width, height); the handle is invalid on failure.
    void Request(const GUID& guid, ReadyCallback onReady);
    /// Resolve `path` like CSS, ensure the upload, and register the cached
    /// texture into `registry` with `sampler`. Returns 0 until it is cached.
    uint32_t TryRegister(const std::string& path, const std::string& sourceAlias, UITextureRegistry& registry,
                         uint32_t* outWidth = nullptr, uint32_t* outHeight = nullptr,
                         Rendering::SamplerHandle sampler = {});
    /// The asset's own raster when cached; starts the upload otherwise and
    /// returns an invalid handle until it lands.
    Rendering::TextureHandle TryGetHandle(const GUID& guid, uint32_t* outWidth = nullptr,
                                          uint32_t* outHeight = nullptr);
    /// Drop every texture of the image so the next draw reloads it.
    void Evict(const std::string& path);
    /// Loads, bakes and uploads in flight. Non-zero means some element draws
    /// nothing where it will draw once its texture lands.
    size_t PendingCount() const;
    /// Images with at least one cached texture or a load under way.
    size_t EntryCount() const;

    /// Test-only: a valid raster is cached for `guid`.
    bool HasRasterForTesting(const GUID& guid) const;
    /// Test-only: mip levels of the cached raster, 0 when none.
    uint32_t RasterMipLevelsForTesting(const GUID& guid) const;
    /// Test-only: every SVG size baked for `guid`, ordered by width then height.
    std::vector<SvgRasterSize> SvgSizesForTesting(const GUID& guid) const;
    /// Test-only: the SVG texture baked at `size`, or an invalid handle.
    Rendering::TextureHandle SvgTextureForTesting(const GUID& guid, SvgRasterSize size) const;
    /// Test-only: the GPU bytes the byte budget counts.
    uint64_t BytesForTesting() const;
    /// Test-only: replace the 256 MB byte budget so eviction runs in a test.
    void SetBudgetForTesting(uint64_t bytes);
    /// Test-only: cache `handle` as the raster of `guid` without a load. The
    /// handle is treated as borrowed and never destroyed.
    void SeedRasterForTesting(const GUID& guid, Rendering::TextureHandle handle, uint32_t width = 1,
                              uint32_t height = 1);

  private:
    // The owner feeds the cache from paint, frame end, reloads, a device
    // rebuild and shutdown; nothing else may.
    friend class GameEngine::UIManager;

    /// The texture a draw binds and how it maps onto the drawn box.
    struct PaintBinding
    {
        Rendering::TextureHandle Handle = Rendering::INVALID_TEXTURE_HANDLE;
        /// Texels of the bound texture: the UV space nine-slice cuts measure in.
        uint32_t Width = 0;
        uint32_t Height = 0;
        /// The image's natural size in physical px when it is not the texel
        /// size (an SVG is its intrinsic CSS size x the content scale); 0 means
        /// "the texel size".
        float NaturalWidth = 0.0f;
        float NaturalHeight = 0.0f;
        NineSlice Slice{};
        /// The texture was baked for this draw's drawn size: the quad is drawn
        /// texel for texel on the device grid.
        bool TexelExact = false;
    };

    /// The texture a draw of the GUID or path in `background` binds, in a
    /// positioning area of paddingWidth x paddingHeight physical px. Requests
    /// whatever is missing; an invalid handle draws nothing this frame.
    PaintBinding ResolveForPaint(const BackgroundImageStyle& background, float paddingWidth, float paddingHeight,
                                 float contentScale);
    /// Once per rendered frame: age the cache, evict idle textures over the
    /// byte budget (never one in `drawnTextures`, called only when a sweep
    /// runs), and repaint for sizes that finished settling.
    void EndFrame(const std::function<std::vector<Rendering::TextureHandle>()>& drawnTextures);
    /// The asset changed on disk: drop its textures. True when any were cached.
    bool OnTextureReloaded(const GUID& guid);
    /// After a device rebuild: drop, never destroy, every cached texture (the
    /// rebuild destroyed them) and every in-flight marker; the next paint
    /// re-uploads from disk. Returns (textures dropped, uploads in flight).
    std::pair<size_t, size_t> DropAfterDeviceRebuild();
    /// The owner is shutting down: completions still queued are dropped.
    void StopApplyingAsyncResults();
    void Evict(const GUID& guid);

    struct CachedTexture;
    struct State;
    struct SvgBake;

    PaintBinding ResolveSvgForPaint(const GUID& guid, CachedTexture& entry, const BackgroundImageStyle& background,
                                    float paddingWidth, float paddingHeight, float contentScale);
    void RequestSvgSource(const GUID& guid);
    void RequestSvgSize(const GUID& guid, SvgRasterSize size, bool drawsFromCloseSize);
    bool SvgSizeHasSettled(const GUID& guid, SvgRasterSize size);
    void RepaintSettledSvgSizes();
    void CompleteSvgSize(const SvgBake& bake);
    void CompleteRaster(const GUID& guid, Rendering::TextureHandle handle, uint32_t width, uint32_t height,
                        uint32_t mipLevels);
    void UploadRasterFromLoadedAsset(const GUID& guid);
    void SweepIdle(const std::function<std::vector<Rendering::TextureHandle>()>& drawnTextures);
    void EvictTexture(const GUID& guid, SvgRasterSize size);
    void DestroySvgTexture(const SvgBackgroundVariant& variant);
    bool BeginUpload(const GUID& guid, SvgRasterSize size);
    NineSlice ResolveNineSlice(const GUID& guid) const;
    static uint64_t EntryBytes(const CachedTexture& entry);

    Rendering::IDevice* m_Device = nullptr;
    AssetManager* m_Assets = nullptr;
    std::shared_ptr<UiDispatcher> m_Dispatcher;
    Hooks m_Hooks;
    std::unique_ptr<State> m_State;
};

} // namespace UI
} // namespace GameEngine
