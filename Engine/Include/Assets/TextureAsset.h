#pragma once

#include "Assets/RuntimeAssetMetadata.h"
#include "AssetCore/Asset.h"
#include "AssetCore/NineSlice.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine {

class AssetRegistry;
class ByteBuffer;
struct AssetDecodeInputs;

/**
 * @brief Texture format types
 */
enum class TextureFormat {
    Unknown,
    R8,
    RG8,
    RGB8,
    RGBA8,
    R16F,
    RG16F,
    RGB16F,
    RGBA16F,
    R32F,
    RG32F,
    RGB32F,
    RGBA32F,
    // GPU block-compressed payloads (4x4 blocks). sRGB-ness is carried by
    // TextureColorSpace, not the format: the GPU view format (e.g. BC7_SRGB
    // vs BC7_UNORM) is chosen at upload time. BC1/BC4 use 8 bytes/block, the
    // rest 16. Produced by the import cook (TextureCook) and by authored
    // KTX2/BasisU containers (BC7 transcode target).
    BC1,
    BC4,
    BC5,
    BC6H,
    BC7
};

// True when the format holds GPU block-compressed blocks (not addressable as
// per-pixel channels).
inline bool IsBlockCompressedTextureFormat(TextureFormat fmt)
{
    switch (fmt)
    {
    case TextureFormat::BC1:
    case TextureFormat::BC4:
    case TextureFormat::BC5:
    case TextureFormat::BC6H:
    case TextureFormat::BC7:
        return true;
    default:
        return false;
    }
}

// GPU transcode target for universal-container textures (KTX2 BasisU/UASTC).
// The renderer sets this once at device init from the device's block-compression
// caps; asset decode (worker threads) reads it when picking the libktx transcode
// format. Default RGBA32 preserves the uncompressed pipeline on devices without
// BC support and before renderer init.
enum class TextureGpuTranscodeTarget : uint8 {
    RGBA32 = 0,
    BC7
};

/**
 * @brief Texture color space for sRGB/Linear handling.
 *
 * This is an asset-level classification that is used when choosing
 * GPU formats (e.g. RGBA8_SRGB vs RGBA8_UNORM) and for tooling.
 *
 * - Unknown: import/editor has not classified the texture yet.
 * - SRGB:    authored in sRGB space; use *_SRGB GPU formats.
 * - Linear:  non-sRGB data; use linear/UNORM or float formats.
 */
enum class TextureColorSpace : uint8 {
    Unknown = 0,
    SRGB,
    Linear
};

// kTextureColorSpaceMetaKey (AssetDatabase KV, keyed by GUID) overrides the
// extension-based color-space guess. Read/written via
// AssetRegistry::TryGetMetaValue / SetMetaValue. Values: "auto" (use the guess),
// "srgb", "linear". Set automatically on import for data textures (normal /
// metallic-roughness / AO / ...) and editable in the texture inspector. The
// engine reads it at upload time to pick *_SRGB vs *_UNORM.

// Shared extension default used before per-asset metadata overrides it.
TextureColorSpace GuessTextureColorSpace(const std::string& extensionLower);

// Parse a color-space meta string. Returns true and sets `out` for an explicit
// "srgb"/"linear"; returns false for "auto"/empty/unrecognized (caller keeps the guess).
bool ParseTextureColorSpaceMeta(const std::string& value, TextureColorSpace& out);

// Canonical meta string for an explicit color space ("srgb"/"linear"); "auto" for Unknown.
const char* TextureColorSpaceMetaValue(TextureColorSpace cs);

// kTextureSwizzleMetaKey stores a channel swizzle applied at upload. The value
// is a 4-character string, one source per output channel (R,G,B,A), each of:
// 'r' 'g' 'b' 'a' (source channel), '0' (zero) or '1' (one). "rgba" = identity.
// Editable in the texture inspector; useful for single-channel / repacked maps.

// Parse a swizzle string into 4 source chars (out[0..3] for R,G,B,A). Returns true
// only for a VALID, NON-identity swizzle (caller applies it); false for empty /
// "rgba" / malformed (caller leaves the texture untouched).
bool ParseTextureSwizzleMeta(const std::string& value, char out[4]);

// The NineSlice POD + NineSliceFill enum live in Assets/NineSlice.h (a small
// dependency-free header) so the UI texture cache and primitive path can use them
// without pulling in texture-format / GPU headers. The metadata key + parse/format
// helpers below bind that layout to a texture asset's AssetDatabase KV entry.

// kTextureNineSliceMetaKey stores the 9-slice layout above. Value is a compact
// versioned CSV, parsed/formatted by the
// functions below. Editable in the texture inspector; read by the UI renderer when
// the texture is used as a background image.

// Parse a 9-slice meta string into `out`. Returns true for a valid, enabled slice
// (caller applies it); false for empty / disabled / malformed (caller treats the
// texture as un-sliced). Cut values are clamped to non-decreasing order on parse.
bool ParseTextureNineSliceMeta(const std::string& value, NineSlice& out);

// Format a NineSlice into its canonical meta string (round-trips through Parse).
std::string TextureNineSliceMetaValue(const NineSlice& slice);

/**
 * @brief Texture filtering modes
 */
enum class TextureFilter {
    Nearest,
    Linear,
    NearestMipmapNearest,
    LinearMipmapNearest,
    NearestMipmapLinear,
    LinearMipmapLinear
};

/**
 * @brief Texture wrap modes
 */
enum class TextureWrap {
    Repeat,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder
};

/**
 * @brief One mip level within TextureAsset pixel data (byte range into
 * GetPixelData() plus the level's texel dimensions).
 */
struct TextureMipDesc {
    uint64 Offset = 0;
    uint64 Size = 0;
    uint32 Width = 0;
    uint32 Height = 0;
};

/**
 * @brief Texture asset implementation
 */
class TextureAsset : public Asset {
public:
    TextureAsset(const GUID& guid, const std::filesystem::path& path);
    virtual ~TextureAsset();

    /**
     * @brief Set/get the process-wide GPU transcode target for container
     * formats (KTX2). Set once by the renderer after device caps are known.
     */
    static void SetGpuTranscodeTarget(TextureGpuTranscodeTarget target);
    static TextureGpuTranscodeTarget GetGpuTranscodeTarget();

    /** The cooked artifacts a load of `sourceBytes` adopts, in order, with their normal-variance companions, and
     * the cook plan that names them, which its decode takes as it is (TryLoadViaCook); empty without the import cook.
     * The AssetIOService reads them next to the source, so the decode adopts an artifact without reading the disk. */
    static AssetDecodeInputs ResolveCookDecodeInputs(const AssetRegistry& registry, const GUID& guid,
                                                     const String& extensionLower,
                                                     std::span<const uint8> sourceBytes);

    /** The decode inputs of a cook hit, resolved before `source` is read: the plan keys the artifacts by the
     * source's content hash, read through a fixed buffer. When the AssetIOService reads every file named, it
     * does not read the source, and the decode adopts the artifact. Empty unless the import cook applies. */
    static AssetDecodeInputs ResolveCookHitDecodeInputs(const AssetRegistry& registry, const GUID& guid,
                                                        const std::filesystem::path& source);

    /** The normal-variance companion beside the packaged `payload` when the texture's settings call for one
     * (LoadPackagedTexture), so the AssetIOService reads it with the payload; empty otherwise. */
    static AssetDecodeInputs ResolvePackagedDecodeInputs(const AssetRegistry& registry, const GUID& guid,
                                                         const std::filesystem::path& payload);

    /**
     * @brief Load texture from file
     */
    bool Load() override;

    /**
     * @brief Load texture from memory data (for async loading)
     */
    bool LoadFromData(std::span<const uint8> data) override;

    /**
     * @brief Unload texture from memory
     */
    void Unload() override;

    /**
     * @brief Textures hot-reload through the async pipeline: decode on a
     * worker, adopt the decoded payload in place on the main thread.
     */
    bool SupportsAsyncReload() const override { return true; }

    /**
     * @brief Get texture width
     */
    uint32 GetWidth() const { return m_Width; }

    /**
     * @brief Get texture height
     */
    uint32 GetHeight() const { return m_Height; }

    /**
     * @brief Get number of channels
     */
    uint32 GetChannels() const { return m_Channels; }

    /**
     * @brief Get texture format
     */
    TextureFormat GetFormat() const { return m_Format; }

	    /**
	     * @brief Get texture color space (sRGB vs Linear).
	     */
	    TextureColorSpace GetColorSpace() const { return m_ColorSpace; }

	    /**
	     * @brief Override texture color space (for tools / user workflows).
	     *
	     * This only affects how the texture will be treated when uploaded
	     * to the GPU (e.g. choice of *_SRGB vs *_UNORM formats). It does
	     * not modify the stored pixel data.
	     */
	    void SetColorSpace(TextureColorSpace colorSpace) { m_ColorSpace = colorSpace; }

    /**
     * @brief Get raw pixel data
     */
    const uint8* GetPixelData() const { return m_PixelData; }

    /**
     * @brief Get data size in bytes
     */
    uint64 GetDataSize() const { return m_DataSize; }

    /**
     * @brief Check if texture has mipmaps
     */
    bool HasMipmaps() const { return m_HasMipmaps; }

    /**
     * @brief Get number of mipmap levels
     */
    uint32 GetMipmapLevels() const { return m_MipmapLevels; }

    /**
     * @brief Per-mip byte ranges into GetPixelData(). Non-empty only for
     * container formats that author mip chains (KTX2); level 0 first.
     */
    const std::vector<TextureMipDesc>& GetMipChain() const { return m_MipChain; }

    /**
     * @brief True when GetPixelData() holds GPU block-compressed blocks
     * (not addressable as per-pixel channels).
     */
    bool IsBlockCompressed() const { return IsBlockCompressedTextureFormat(m_Format); }

    /**
     * @brief True when the payload came from a cook artifact that already baked
     * the per-asset swizzle meta into the pixels; upload must not re-apply it.
     */
    bool IsSwizzleBaked() const { return m_SwizzleBaked; }

    /**
     * @brief The BC4 normal-variance companion the cook wrote beside this normal map
     * (HasNormalVarianceCompanion), or null. Its level j holds sigma of mip j + 1 in R.
     */
    const TextureAsset* GetNormalVarianceCompanion() const { return m_NormalVarianceCompanion.get(); }

    /// The SVG text an asset loaded from, with its intrinsic size in CSS px
    /// (its width/height or viewBox). Immutable once built; a reload swaps the
    /// asset's pointer to a new one. Rasterize it at any size with
    /// RasterizeSvgToRgbaAtDimensions (Assets/SvgRasterizer.h).
    struct SvgSource
    {
        std::string Text;
        float32 Width = 0.0f;
        float32 Height = 0.0f;
    };

    /**
     * @brief The SVG source the asset holds right now, null for a non-SVG
     * asset. Safe from any thread; two snapshots compare equal by pointer
     * exactly when no reload happened between them, which is how a consumer
     * that baked from one tells a stale result from a current one.
     */
    std::shared_ptr<const SvgSource> SnapshotSvgSource() const;

    /**
     * @brief Set texture filtering
     */
    void SetFilter(TextureFilter minFilter, TextureFilter magFilter);

    /**
     * @brief Set texture wrapping
     */
    void SetWrap(TextureWrap wrapS, TextureWrap wrapT);

protected:
    bool AdoptReloadedPayload(Asset& staged) override;

private:
    enum class AlphaCoveragePolicy { Disabled, Enabled, Invalid };

    /**
     * @brief Derived-cache cook path for raster sources (PNG/JPG/TGA/BMP/HDR):
     * resolve per-asset cook settings from AssetDatabase meta, then load the
     * cached KTX2 artifact (or, inside an AssetManager load context, cook +
     * cache it) instead of raw-decoding the source. Returns false (caller falls
     * back to the raw decode) outside an AssetManager context, for authored
     * containers, or when the cook would be byte-identical to the raw path. Invalid policy forbids fallback; enabled policy requires actual
     * decoded pixels and forbids placeholder success. See Assets/TextureCook.h.
     * Empty `sourceBytes` only adopts: the plan comes from the file (or the reader's plan), and a miss returns false.
     */
    bool TryLoadViaCook(std::span<const uint8> sourceBytes, const String& extensionLower,
                        AlphaCoveragePolicy& coveragePolicy);

    /**
     * @brief Reads the source image file at GetPath() and loads it through the cook, or
     * decodes it with the Image module (the file's own channels; float for .hdr).
     */
    bool DecodeSourceFile();

    // Nullopt for authored input; packaged input succeeds or fails without raw fallback.
    std::optional<bool> LoadPackagedTexture(const std::span<const uint8>* data);

    // Replace a loaded block-compressed payload with its decode, every level, for a CPU reader
    // of a package (whose source the export removed): RGBA32F for BC6H, RGBA8 otherwise.
    bool DecodeBlockPayloadForCpuReaders();

    /**
     * @brief Loads the source image file's bytes through the cook, or decodes them with
     * the Image module (the file's own channels; float for .hdr).
     */
    bool DecodeSourceBytes(std::span<const uint8> data);

    /**
     * @brief Load DDS texture
     */
    bool LoadDDS();

    /**
     * @brief Load DDS texture from memory data
     */
    bool LoadDDSFromData(std::span<const uint8> data);

#if defined(GE_HAVE_KTX)
    /**
     * @brief Load KTX/KTX2 texture from file
     */
    bool LoadWithKTX();

    /**
     * @brief Load KTX/KTX2 texture from memory data
     */
    bool LoadWithKTXFromData(std::span<const uint8> data);

    /// Loads the KTX2 file in `data`. A container with no Basis payload, no supercompression and no
    /// gaps between its levels puts them in place inside `data`, which becomes the payload with no
    /// copy. Any other container copies its levels into a new payload.
    bool LoadWithKTXFromOwnedData(ByteBuffer data);

    /// Read (ReadAssetDecodeInputFile) and load a cooked normal-variance companion; null when it
    /// is absent or not BC4.
    std::unique_ptr<TextureAsset> LoadVarianceCompanion(const std::filesystem::path& file) const;

    /// Load the companion an export wrote beside a packaged normal map's payload. False (and
    /// an error) when the texture's settings call for one and it is missing or corrupt.
    bool LoadPackagedVarianceCompanion(const AssetRegistry& registry, const std::filesystem::path& payload);

    /**
     * @brief Adopts the cooked container at `artifact` (TryLoadViaCook's hit path), together with
     * `varianceCompanion` when that path is not empty. A missing or empty file is a miss; a corrupt
     * one is logged and dropped, so the caller recooks.
     */
    bool AdoptCookArtifact(const std::filesystem::path& artifact, const std::filesystem::path& varianceCompanion,
                           bool swizzleBaked);

    /**
     * @brief Shared tail of the KTX loaders: transcode, adopt the mip chain and DFD color space.
     * The caller destroys the ktx texture. A non-null `file` holds the image data
     * (LoadWithKTXFromOwnedData) and becomes the payload in place when its levels have no gaps.
     */
    bool PopulateFromKtxTexture(void* ktxTexture2Ptr, const char* sourceName, ByteBuffer* file);
#endif


    /// Rasterize an SVG string via ThorVG at the asset's own raster size,
    /// populate the pixel data members and keep the source for SnapshotSvgSource.
    bool LoadFromSvgData(const std::string& svgText, const String& extensionLower);

    /**
     * @brief Determine format from file data
     */
    TextureFormat DetermineFormat(int channels, bool isHDR) const;

private:
	    uint32 m_Width;
	    uint32 m_Height;
	    uint32 m_Channels;
	    TextureFormat m_Format;
	    TextureColorSpace m_ColorSpace;
	    uint8* m_PixelData;
	    uint64 m_DataSize;
	    bool m_HasMipmaps;
	    bool m_SwizzleBaked = false;
	    uint32 m_MipmapLevels;
	    std::vector<TextureMipDesc> m_MipChain;
	    std::unique_ptr<TextureAsset> m_NormalVarianceCompanion;
	    TextureFilter m_MinFilter;
	    TextureFilter m_MagFilter;
	    TextureWrap m_WrapS;
	    TextureWrap m_WrapT;
	    std::shared_ptr<const SvgSource> m_SvgSource;
	    mutable std::mutex m_SvgSourceMutex;

    DISALLOW_COPY_AND_ASSIGN(TextureAsset);
};

} // namespace GameEngine
