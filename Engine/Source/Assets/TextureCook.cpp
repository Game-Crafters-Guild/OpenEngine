#include "Assets/TextureCook.h"

#include "AssetCore/SharedFileRead.h"

#include "Assets/TextureCookWorkers.h"
#include "Assets/Textures/BC7TransparentEndpointRepair.h"
#include "Image/Decode.h"
#include "Image/Resample.h"
#include "Logger/Logger.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"
#include "Memory/Arena.h"
#include "Memory/ScratchScope.h"
#include "Types/Fnv1a.h"
#include "Types/FunctionRef.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

#if defined(__APPLE__)
#include <xlocale.h>
#endif

#if defined(GE_HAVE_KTX)
#include <ktx.h>
#include <vulkan/vulkan_core.h>
#endif
#if defined(GE_HAVE_DIRECTXTEX)
#include <DirectXTex.h>
#endif
#if defined(GE_HAVE_BC7E)
#include <bc7e_isa_ispc.h>
#include <bc7e_ispc.h>
#endif
// BC7 encodes through bc7e and every other BC format through DirectXTex: the
// cook encodes only when both are compiled in.
#if defined(GE_HAVE_DIRECTXTEX) && defined(GE_HAVE_BC7E)
#define GE_TEXTURE_COOK_ENCODER 1
#endif

namespace GameEngine {

namespace
{
struct SlotUsage
{
    std::string_view Name;
    StringId Slot;
    TextureCookUsage Usage;
};

constexpr SlotUsage MakeSlotUsage(std::string_view name, TextureCookUsage usage)
{
    return {name, HashStringId(name), usage};
}

constexpr SlotUsage kSlotUsages[] = {
    MakeSlotUsage("albedoMap", TextureCookUsage::Color),
    MakeSlotUsage("emissiveMap", TextureCookUsage::Color),
    MakeSlotUsage("triplanarAlbedoTop", TextureCookUsage::Color),
    MakeSlotUsage("triplanarAlbedoSide", TextureCookUsage::Color),
    MakeSlotUsage("triplanarAlbedoBottom", TextureCookUsage::Color),
    MakeSlotUsage("normalMap", TextureCookUsage::Normal),
    MakeSlotUsage("coatNormalMap", TextureCookUsage::Normal),
    MakeSlotUsage("triplanarNormalTop", TextureCookUsage::Normal),
    MakeSlotUsage("triplanarNormalSide", TextureCookUsage::Normal),
    MakeSlotUsage("triplanarNormalBottom", TextureCookUsage::Normal),
    MakeSlotUsage("metallicRoughnessMap", TextureCookUsage::Packed),
    MakeSlotUsage("aoMap", TextureCookUsage::Mask),
    MakeSlotUsage("roughnessMap", TextureCookUsage::Mask),
    MakeSlotUsage("metallicMap", TextureCookUsage::Mask),
    // The relief march reads R of a linear height map (1 = the polygon surface).
    MakeSlotUsage("heightMap", TextureCookUsage::Mask),
};

const SlotUsage* FindSlotUsage(StringId slotName)
{
    for (const auto& entry : kSlotUsages)
    {
        if (entry.Slot == slotName)
            return &entry;
    }
    return nullptr;
}
} // namespace

TextureCookUsage TextureCookUsageForMaterialSlot(StringId slotName)
{
    const SlotUsage* entry = FindSlotUsage(slotName);
    return entry ? entry->Usage : TextureCookUsage::Auto;
}

std::string_view TextureCookUsageSlotName(StringId slotName)
{
    const SlotUsage* entry = FindSlotUsage(slotName);
    return entry ? entry->Name : std::string_view{};
}

std::optional<TextureCookUsage> WidenTextureCookUsage(TextureCookUsage current, TextureCookUsage required)
{
    if (required == TextureCookUsage::Auto || required == current)
        return current;
    if (current == TextureCookUsage::Auto)
        return required;
    // Two different usages, neither Auto: Mask and Packed are the only pair one cook serves.
    const bool maskAndPacked = (current == TextureCookUsage::Mask && required == TextureCookUsage::Packed) ||
                               (current == TextureCookUsage::Packed && required == TextureCookUsage::Mask);
    if (!maskAndPacked)
        return std::nullopt;
    return TextureCookUsage::Packed;
}

bool IsLinearTextureUsage(TextureCookUsage usage)
{
    return usage == TextureCookUsage::Normal || usage == TextureCookUsage::Mask ||
           usage == TextureCookUsage::Packed;
}

bool IsLinearTextureSlot(StringId slotName)
{
    return IsLinearTextureUsage(TextureCookUsageForMaterialSlot(slotName));
}

bool ResolveTextureCookInputs(
    const std::string& extensionLower,
    const std::function<bool(const char*, std::string&)>& readMetadata,
    TextureCookInputs& inputs, std::string& error)
{
    inputs = {};
    error.clear();
    std::string value;
    if (readMetadata(kTextureCompressionMetaKey, value))
        ParseTextureCookCompressionMeta(value, inputs.Settings.Compression);
    value.clear();
    if (readMetadata(kTextureUsageMetaKey, value))
        ParseTextureCookUsageMeta(value, inputs.Settings.Usage);
    value.clear();
    if (readMetadata(kTextureMipsMetaKey, value))
        inputs.Settings.MipsEnabled = value != "0";
    value.clear();
    if (readMetadata(kTextureMipLimitMetaKey, value) && !value.empty())
        inputs.Settings.MipLimit = static_cast<uint32>(std::strtoul(value.c_str(), nullptr, 10));
    inputs.ColorSpace = GuessTextureColorSpace(extensionLower);
    value.clear();
    if (readMetadata(kTextureColorSpaceMetaKey, value))
        ParseTextureColorSpaceMeta(value, inputs.ColorSpace);
    value.clear();
    if (!readMetadata(kTextureSwizzleMetaKey, value) || !ParseTextureSwizzleMeta(value, inputs.Swizzle))
        std::memset(inputs.Swizzle, 0, sizeof(inputs.Swizzle));
    std::string coverageEnabled, coverageCutoff;
    readMetadata(kTextureAlphaCoverageMetaKey, coverageEnabled);
    readMetadata(kTextureAlphaCutoffMetaKey, coverageCutoff);
    return ParseTextureAlphaCoverageMeta(coverageEnabled, coverageCutoff, inputs.Settings, error) &&
           ValidateTextureAlphaCoverageSettings(inputs.Settings, extensionLower == ".hdr", error);
}

bool IsTextureCookSourceExtension(const std::string& extensionLower)
{
    return extensionLower == ".png" || extensionLower == ".jpg" || extensionLower == ".jpeg" ||
           extensionLower == ".bmp" || extensionLower == ".tga" || extensionLower == ".hdr" ||
           extensionLower == ".psd" || extensionLower == ".gif" || extensionLower == ".pic" ||
           extensionLower == ".pnm" || extensionLower == ".ppm" || extensionLower == ".pgm";
}

bool NeedsTextureCook(const TextureCookInputs& inputs, TextureCookOutput output)
{
    return output != TextureCookOutput::Uncompressed || inputs.Settings.MipsEnabled || inputs.Swizzle[0] != 0;
}

const char* TextureCookOutputName(TextureCookOutput output)
{
    // No default: -Wswitch fails the build when an output has no name.
    switch (output)
    {
    case TextureCookOutput::Uncompressed: return "Uncompressed";
    case TextureCookOutput::BC1:          return "BC1";
    case TextureCookOutput::BC4:          return "BC4";
    case TextureCookOutput::BC5:          return "BC5";
    case TextureCookOutput::BC6H:         return "BC6H";
    case TextureCookOutput::BC7:          return "BC7";
    }
    return "Unknown";
}

namespace
{
    // The start of every artifact name of `guid` and `output` (TextureCookArtifactName).
    std::string TextureCookArtifactPrefix(const GUID& guid, TextureCookOutput output)
    {
        return guid.ToString() + "-" + TextureCookOutputName(output) + "-";
    }
}

std::string TextureCookArtifactName(const GUID& guid, uint64 sourceHash,
                                    const TextureCookInputs& inputs, TextureCookOutput output)
{
    char hashes[34];
    std::snprintf(hashes, sizeof(hashes), "%016llx-%016llx",
                  static_cast<unsigned long long>(sourceHash),
                  static_cast<unsigned long long>(ComputeTextureCookConfigHash(inputs, output)));
    return TextureCookArtifactPrefix(guid, output) + hashes + ".ktx2";
}

std::filesystem::path TextureCookVarianceCompanionPath(const std::filesystem::path& parentArtifact)
{
    std::filesystem::path companion = parentArtifact;
    companion.replace_extension(".nvar.ktx2");
    return companion;
}

void RemoveSupersededTextureCookArtifacts(const std::filesystem::path& kept, const GUID& guid,
                                          TextureCookOutput output)
{
    const std::string prefix = TextureCookArtifactPrefix(guid, output);
    const std::filesystem::path keptCompanion = TextureCookVarianceCompanionPath(kept);
    std::error_code listError;
    for (std::filesystem::directory_iterator it(kept.parent_path(), listError), end; !listError && it != end;
         it.increment(listError))
    {
        const std::filesystem::path& file = it->path();
        std::error_code removeError;
        if (file != kept && file != keptCompanion && file.filename().string().starts_with(prefix))
            std::filesystem::remove(file, removeError);
    }
}

namespace
{
    // Bump when the cook recipe changes (filtering, encoder settings, container
    // layout): every cached artifact re-keys to a clean miss.
    constexpr uint32 kTextureCookVersion = 3;
    // Bump when the normal-variance companion recipe changes: only normal maps re-key.
    constexpr uint32 kNormalVarianceCompanionVersion = 1;

    // Encoder-library versions folded into the config hash: a vcpkg bump that
    // changes artifact bytes under an unchanged recipe must re-key to a clean
    // miss, never serve a stale artifact (mirrors GE_MESHOPT_VERSION in the
    // mesh-LOD cook). The encoder versions only reach block-compressed outputs;
    // stbir shapes every mip chain.
#if defined(GE_HAVE_DIRECTXTEX)
    constexpr uint32 kDirectXTexVersion = DIRECTX_TEX_VERSION;
#else
    constexpr uint32 kDirectXTexVersion = 0;
#endif
    // The bc7e build id: the kernel commit, the ISPC version and, on arm64, the
    // pinned ISPC CPU (cmake/ports/bc7e).
#if defined(BC7E_VERSION)
    constexpr const char* kBc7eVersion = BC7E_VERSION;
#else
    constexpr const char* kBc7eVersion = "none";
#endif

    // The instruction set bc7e runs on (on x64, the set ISPC's dispatcher picks
    // for this CPU). It keys BC7 artifacts because the sets emit different
    // bytes for a small share of blocks (the search's float arithmetic rounds
    // differently on each), so a cache built on a CPU with another set
    // re-cooks rather than serving bytes this one would not produce.
    const char* Bc7eInstructionSet()
    {
#if defined(GE_HAVE_BC7E)
        switch (ispc::bc7e_isa())
        {
        case 1: return "sse4";
        case 2: return "avx2";
        case 3: return "avx512";
        case 4: return "neon";
        default: return "unknown";
        }
#else
        return "none";
#endif
    }
#if defined(GE_STBIR_VERSION)
    constexpr const char* kStbirVersion = GE_STBIR_VERSION;
#else
    constexpr const char* kStbirVersion = "stbir-none";
#endif

    constexpr uint64 kFnvOffset = 14695981039346656037ull;
    constexpr uint64 kFnvPrime = 1099511628211ull;

    uint64 FnvAppend(uint64 hash, const void* data, size_t size)
    {
        const auto* bytes = static_cast<const uint8*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= kFnvPrime;
        }
        return hash;
    }
}

bool ParseTextureCookCompressionMeta(const std::string& value, TextureCookCompression& out)
{
    if (value == "bc1") { out = TextureCookCompression::BC1; return true; }
    if (value == "bc4") { out = TextureCookCompression::BC4; return true; }
    if (value == "bc5") { out = TextureCookCompression::BC5; return true; }
    if (value == "bc6h") { out = TextureCookCompression::BC6H; return true; }
    if (value == "bc7") { out = TextureCookCompression::BC7; return true; }
    if (value == "none") { out = TextureCookCompression::None; return true; }
    return false; // "auto" / empty / unrecognized -> keep default
}

const char* TextureCookCompressionMetaValue(TextureCookCompression c)
{
    switch (c)
    {
    case TextureCookCompression::BC1:  return "bc1";
    case TextureCookCompression::BC4:  return "bc4";
    case TextureCookCompression::BC5:  return "bc5";
    case TextureCookCompression::BC6H: return "bc6h";
    case TextureCookCompression::BC7:  return "bc7";
    case TextureCookCompression::None: return "none";
    default:                           return "auto";
    }
}

bool ParseTextureCookUsageMeta(const std::string& value, TextureCookUsage& out)
{
    if (value == "color") { out = TextureCookUsage::Color; return true; }
    if (value == "normal") { out = TextureCookUsage::Normal; return true; }
    if (value == "mask") { out = TextureCookUsage::Mask; return true; }
    if (value == "packed") { out = TextureCookUsage::Packed; return true; }
    return false;
}

const char* TextureCookUsageMetaValue(TextureCookUsage u)
{
    switch (u)
    {
    case TextureCookUsage::Color:  return "color";
    case TextureCookUsage::Normal: return "normal";
    case TextureCookUsage::Mask:   return "mask";
    case TextureCookUsage::Packed: return "packed";
    default:                       return "auto";
    }
}

bool ParseTextureFilterMeta(const std::string& value, TextureFilterMode& out)
{
    if (value == "trilinear") { out = TextureFilterMode::Trilinear; return true; }
    if (value == "bilinear") { out = TextureFilterMode::Bilinear; return true; }
    if (value == "point") { out = TextureFilterMode::Point; return true; }
    return false;
}

const char* TextureFilterMetaValue(TextureFilterMode f)
{
    switch (f)
    {
    case TextureFilterMode::Trilinear: return "trilinear";
    case TextureFilterMode::Bilinear:  return "bilinear";
    case TextureFilterMode::Point:     return "point";
    default:                           return "inherit";
    }
}

bool ParseTextureAlphaCoverageMeta(const std::string& enabled, const std::string& cutoff,
                                   TextureCookSettings& settings, std::string& error)
{
    error.clear();
    if (enabled.empty() || enabled == "0")
    {
        settings.PreserveAlphaCoverage = false;
        settings.AlphaCoverageCutoff = 0.5f;
        return true;
    }
    if (enabled != "1")
    {
        error = "alpha coverage enable must be 0 or 1";
        return false;
    }
    float value = 0.5f;
    if (!cutoff.empty())
    {
#if defined(__APPLE__)
        // Floating-point from_chars requires macOS 26. Use an explicit C locale
        // on older deployment targets without changing the process locale.
        locale_t locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
        char* end = nullptr;
        const int savedErrno = errno;
        errno = 0;
        if (locale) value = strtof_l(cutoff.c_str(), &end, locale);
        const bool rangeError = errno == ERANGE && (value == 0.0f || !std::isfinite(value));
        errno = savedErrno;
        if (locale) freelocale(locale);
        // Match from_chars: no leading whitespace/plus or hexadecimal input.
        const bool decimalStart = (cutoff[0] >= '0' && cutoff[0] <= '9') ||
                                  cutoff[0] == '-' || cutoff[0] == '.';
        const bool parsedAll = locale && decimalStart && cutoff.find_first_of("xX") == std::string::npos &&
                               end == cutoff.data() + cutoff.size() && !rangeError;
#else
        const auto parsed = std::from_chars(cutoff.data(), cutoff.data() + cutoff.size(), value);
        const bool parsedAll = parsed.ec == std::errc{} && parsed.ptr == cutoff.data() + cutoff.size();
#endif
        if (!parsedAll ||
            !std::isfinite(value) || value < 0.0f || value > 1.0f)
        {
            error = "alpha coverage cutoff must be a finite number in [0,1]";
            return false;
        }
    }
    settings.PreserveAlphaCoverage = true;
    settings.AlphaCoverageCutoff = value == 0.0f ? 0.0f : value;
    return true;
}

std::string TextureAlphaCutoffMetaValue(float cutoff)
{
    if (!std::isfinite(cutoff) || cutoff < 0.0f || cutoff > 1.0f)
        return {};
    if (cutoff == 0.0f) cutoff = 0.0f; // canonical positive zero
    char text[32];
    const auto result = std::to_chars(text, text + sizeof(text), cutoff,
                                      std::chars_format::general,
                                      std::numeric_limits<float>::max_digits10);
    return result.ec == std::errc{} ? std::string(text, result.ptr) : std::string{};
}

bool ValidateTextureAlphaCoverageSettings(const TextureCookSettings& settings,
                                          bool isFloat, std::string& error)
{
    error.clear();
    if (!settings.PreserveAlphaCoverage)
        return true;
    if (!std::isfinite(settings.AlphaCoverageCutoff) || settings.AlphaCoverageCutoff < 0.0f ||
        settings.AlphaCoverageCutoff > 1.0f)
        error = "alpha coverage cutoff must be finite and in [0,1]";
    else if (isFloat || settings.Usage == TextureCookUsage::Normal)
        error = "alpha coverage requires LDR color alpha, not HDR or normal-map data";
    else if ((settings.Compression != TextureCookCompression::Auto &&
              settings.Compression != TextureCookCompression::None &&
              settings.Compression != TextureCookCompression::BC7) ||
             (settings.Compression == TextureCookCompression::Auto &&
              settings.Usage == TextureCookUsage::Mask))
        error = "alpha coverage requires requested uncompressed or BC7 output";
    return error.empty();
}

bool IsTextureCookEncoderAvailable()
{
#if defined(GE_TEXTURE_COOK_ENCODER) && defined(GE_HAVE_KTX)
    return true;
#else
    return false;
#endif
}

bool IsTextureCookDisabled()
{
    static const bool disabled = []() {
        const char* v = std::getenv("GE_TEXTURE_COOK_DISABLE");
        return v && v[0] == '1';
    }();
    return disabled;
}

bool DecodeBlockPayloadRGBA8(TextureFormat format, uint32 width, uint32 height,
                             const uint8* payload, size_t payloadSize,
                             std::vector<uint8>& outRgba8)
{
    outRgba8.clear();
#if defined(GE_HAVE_DIRECTXTEX)
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    switch (format)
    {
    case TextureFormat::BC1: dxgi = DXGI_FORMAT_BC1_UNORM; break;
    case TextureFormat::BC4: dxgi = DXGI_FORMAT_BC4_UNORM; break;
    case TextureFormat::BC5: dxgi = DXGI_FORMAT_BC5_UNORM; break;
    case TextureFormat::BC7: dxgi = DXGI_FORMAT_BC7_UNORM; break;
    default: return false; // BC6H (HDR) and non-block formats unsupported here
    }

    const uint32 blocksW = (width + 3) / 4;
    const uint32 blocksH = (height + 3) / 4;
    DirectX::Image src{};
    src.width = width;
    src.height = height;
    src.format = dxgi;
    src.rowPitch = static_cast<size_t>(blocksW) * (format == TextureFormat::BC7 ? 16 : (format == TextureFormat::BC5 ? 16 : 8));
    src.slicePitch = src.rowPitch * blocksH;
    src.pixels = const_cast<uint8*>(payload);
    if (payloadSize < src.slicePitch)
        return false;

    DirectX::ScratchImage decoded;
    if (FAILED(DirectX::Decompress(src, DXGI_FORMAT_R8G8B8A8_UNORM, decoded)) ||
        !decoded.GetImage(0, 0, 0))
        return false;
    const DirectX::Image* img = decoded.GetImage(0, 0, 0);
    outRgba8.resize(static_cast<size_t>(width) * height * 4);
    for (uint32 y = 0; y < height; ++y)
        std::memcpy(outRgba8.data() + static_cast<size_t>(y) * width * 4,
                    img->pixels + static_cast<size_t>(y) * img->rowPitch,
                    static_cast<size_t>(width) * 4);
    return true;
#else
    (void)format; (void)width; (void)height; (void)payload; (void)payloadSize;
    return false;
#endif
}

bool DecodeBlockPayloadRGBA32F(TextureFormat format, uint32 width, uint32 height,
                               const uint8* payload, size_t payloadSize,
                               std::vector<float>& outRgba32f)
{
    outRgba32f.clear();
#if defined(GE_HAVE_DIRECTXTEX)
    if (format != TextureFormat::BC6H)
        return false;
    constexpr size_t kBc6hBlockBytes = 16;
    DirectX::Image src{};
    src.width = width;
    src.height = height;
    src.format = DXGI_FORMAT_BC6H_UF16;
    src.rowPitch = static_cast<size_t>((width + 3) / 4) * kBc6hBlockBytes;
    src.slicePitch = src.rowPitch * ((height + 3) / 4);
    src.pixels = const_cast<uint8*>(payload);
    if (payloadSize < src.slicePitch)
        return false;
    DirectX::ScratchImage decoded;
    if (FAILED(DirectX::Decompress(src, DXGI_FORMAT_R32G32B32A32_FLOAT, decoded)) || !decoded.GetImage(0, 0, 0))
        return false;
    const DirectX::Image* img = decoded.GetImage(0, 0, 0);
    const size_t rowFloats = static_cast<size_t>(width) * 4;
    outRgba32f.resize(rowFloats * height);
    for (uint32 y = 0; y < height; ++y)
        std::memcpy(outRgba32f.data() + y * rowFloats, img->pixels + static_cast<size_t>(y) * img->rowPitch,
                    rowFloats * sizeof(float));
    return true;
#else
    (void)format; (void)width; (void)height; (void)payload; (void)payloadSize;
    return false;
#endif
}

namespace
{
    // One mip level in storage that its owner keeps: tightly packed RGBA8, or RGBA32F as bytes.
    struct MipLevelView
    {
        std::span<uint8> Bytes;
        uint32 Width = 0;
        uint32 Height = 0;
    };

    MipLevelView ViewOf(TextureCookMipLevel& level)
    {
        return {level.Bytes, level.Width, level.Height};
    }

    // The extent of mip `level` of a base extent, as successive halving to at least 1 gives it.
    uint32 MipExtent(uint32 baseExtent, uint32 level)
    {
        return level < 32u ? std::max(baseExtent >> level, 1u) : 1u;
    }
}

#if defined(GE_TEXTURE_COOK_ENCODER)
namespace
{
    // BCn blocks are 4x4 pixels.
    constexpr uint32 kBlockDim = 4;

    DXGI_FORMAT DxgiTargetFor(TextureCookOutput out)
    {
        switch (out)
        {
        case TextureCookOutput::BC1:  return DXGI_FORMAT_BC1_UNORM;
        case TextureCookOutput::BC4:  return DXGI_FORMAT_BC4_UNORM;
        case TextureCookOutput::BC5:  return DXGI_FORMAT_BC5_UNORM;
        case TextureCookOutput::BC6H: return DXGI_FORMAT_BC6H_UF16;
        case TextureCookOutput::BC7:  return DXGI_FORMAT_BC7_UNORM;
        default:                      return DXGI_FORMAT_UNKNOWN;
        }
    }

    // A level's BCn blocks: ceil(w/4) in each row, and ceil(h/4) rows, at least 1.
    struct LevelBlocks
    {
        uint32 PerRow = 0;
        uint32 Rows = 0;
        size_t RowBytes = 0;
        size_t Bytes() const { return RowBytes * Rows; }
    };

    LevelBlocks BlocksOf(DXGI_FORMAT format, uint32 width, uint32 height)
    {
        const uint32 perRow = (width + kBlockDim - 1) / kBlockDim;
        const size_t blockBytes = DirectX::BitsPerPixel(format) * kBlockDim * kBlockDim / 8;
        return {perRow, std::max(1u, (height + kBlockDim - 1) / kBlockDim), perRow * blockBytes};
    }
}
#endif

#if defined(GE_TEXTURE_COOK_ENCODER)
namespace
{
    // One level's encode, split into block-row bands that each land at their own
    // offset of the level's payload. BCn blocks encode independently (bc7e
    // encodes each 16-texel block alone; DirectXTex keeps no cross-block state at
    // TEX_COMPRESS_DEFAULT, and dithering, which would diffuse error across block
    // rows, is not enabled) and every band starts on a block-row boundary, so the
    // payload is byte-identical however the bands are sized and whichever thread
    // encodes each one.
    struct LevelBands
    {
        std::span<const uint8> Pixels; // the level, tightly packed
        uint32 Width = 0;
        uint32 Height = 0;
        bool IsFloat = false;
        TextureCookOutput Output = TextureCookOutput::BC7;
        DXGI_FORMAT SourceFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT TargetFormat = DXGI_FORMAT_UNKNOWN;
        size_t RowPitch = 0;      // source bytes per pixel row
        size_t BlockRowBytes = 0; // payload bytes per block row
        uint32 BandHeight = 0;    // pixel rows per band, a multiple of kBlockDim
        uint32 BandCount = 0;
        uint8* Payload = nullptr; // the level's whole payload, sized up front
        std::atomic<long> FirstFailure{0}; // HRESULT of the first band that failed
    };

    constexpr uint32 kBlockTexels = kBlockDim * kBlockDim;
    constexpr size_t kBC7BlockBytes = 16;

    // bc7e's `slow` profile with RGB error (perceptual = false). The codec's
    // tables are global and filled once; bc7e_compress_blocks writes zero blocks
    // when called before that, so every BC7 encode takes its parameters here.
    const ispc::bc7e_compress_block_params& BC7EncodeParams()
    {
        static std::once_flag initialized;
        static ispc::bc7e_compress_block_params params{};
        std::call_once(initialized, [] {
            ispc::bc7e_compress_block_init();
            ispc::bc7e_compress_block_params_init_slow(&params, /*perceptual=*/false);
        });
        return params;
    }

    uint8 SaturateToUnorm8(float value)
    {
        return static_cast<uint8>(Math::Clamp01(value) * 255.0f + 0.5f);
    }

    // One texel of the level as RGBA8 in memory order (R first), as bc7e reads it.
    uint32 LevelTexelRGBA8(const LevelBands& bands, uint32 x, uint32 y)
    {
        const size_t texel = static_cast<size_t>(y) * bands.Width + x;
        uint8 rgba[4];
        if (bands.IsFloat)
        {
            float channels[4];
            std::memcpy(channels, bands.Pixels.data() + texel * sizeof(channels), sizeof(channels));
            for (int c = 0; c < 4; ++c)
                rgba[c] = SaturateToUnorm8(channels[c]);
        }
        else
        {
            std::memcpy(rgba, bands.Pixels.data() + texel * 4, 4);
        }
        uint32 packed = 0;
        std::memcpy(&packed, rgba, sizeof(packed));
        return packed;
    }

    // Encodes the band's block rows with bc7e: each row's blocks are gathered to
    // 16 contiguous texels, edge-replicated past the level's right and bottom
    // edges (only extents that are not a multiple of 4 reach them, the 2x2 and
    // 1x1 tails among them), and compressed in one call per row. The gathered row lives in
    // the encoding thread's scratch, so a steady encode takes nothing from the heap.
    void EncodeBC7Band(const LevelBands& bands, uint32 y, uint32 height, uint8* payload)
    {
        const uint32 blocksPerRow = static_cast<uint32>(bands.BlockRowBytes / kBC7BlockBytes);
        Memory::ScratchScope scratch;
        const std::span<uint32> gathered =
            scratch.AllocateArrayForOverwrite<uint32>(static_cast<size_t>(blocksPerRow) * kBlockTexels);
        const ispc::bc7e_compress_block_params& params = BC7EncodeParams();
        for (uint32 rowY = 0; rowY < height; rowY += kBlockDim)
        {
            for (uint32 block = 0; block < blocksPerRow; ++block)
            {
                for (uint32 ty = 0; ty < kBlockDim; ++ty)
                {
                    const uint32 sy = std::min(y + rowY + ty, bands.Height - 1);
                    for (uint32 tx = 0; tx < kBlockDim; ++tx)
                    {
                        const uint32 sx = std::min(block * kBlockDim + tx, bands.Width - 1);
                        gathered[static_cast<size_t>(block) * kBlockTexels + ty * kBlockDim + tx] =
                            LevelTexelRGBA8(bands, sx, sy);
                    }
                }
            }
            uint8* rowBlocks = payload + static_cast<size_t>(rowY / kBlockDim) * bands.BlockRowBytes;
            ispc::bc7e_compress_blocks(blocksPerRow, reinterpret_cast<uint64*>(rowBlocks), gathered.data(), &params);
        }
    }

    bool EncodeDirectXTexBand(LevelBands& bands, const DirectX::Image& src, uint8* payload)
    {
        DirectX::ScratchImage compressed;
        const HRESULT hr = DirectX::Compress(src, bands.TargetFormat, DirectX::TEX_COMPRESS_DEFAULT,
                                             DirectX::TEX_THRESHOLD_DEFAULT, compressed);
        const DirectX::Image* img = SUCCEEDED(hr) ? compressed.GetImage(0, 0, 0) : nullptr;
        const size_t expectedBytes = bands.BlockRowBytes * ((src.height + kBlockDim - 1) / kBlockDim);
        if (!img || img->slicePitch != expectedBytes)
        {
            long expected = 0;
            bands.FirstFailure.compare_exchange_strong(expected, FAILED(hr) ? static_cast<long>(hr) : E_FAIL);
            return false;
        }
        std::memcpy(payload, img->pixels, img->slicePitch);
        return true;
    }

    bool EncodeLevelBand(LevelBands& bands, uint32 band)
    {
        const uint32 y = band * bands.BandHeight;

        DirectX::Image src{};
        src.width = bands.Width;
        src.height = std::min(bands.BandHeight, bands.Height - y);
        src.format = bands.SourceFormat;
        src.rowPitch = bands.RowPitch;
        src.slicePitch = bands.RowPitch * src.height;
        src.pixels = const_cast<uint8*>(bands.Pixels.data()) + static_cast<size_t>(y) * bands.RowPitch;

        uint8* payload = bands.Payload + static_cast<size_t>(y / kBlockDim) * bands.BlockRowBytes;
        if (bands.Output != TextureCookOutput::BC7)
            return EncodeDirectXTexBand(bands, src, payload);

        const uint32 height = static_cast<uint32>(src.height);
        EncodeBC7Band(bands, y, height, payload);
        // A transparent endpoint can come out with p-bit 1 and decode to alpha 1/255
        // or 4/255; see BC7TransparentEndpointRepair.h. Per block, so banding stays
        // byte-exact.
        if (!bands.IsFloat)
        {
            const size_t bandBytes = bands.BlockRowBytes * ((height + kBlockDim - 1) / kBlockDim);
            RepairBC7TransparentEndpoints(src.pixels, bands.Width, height, std::span<uint8>(payload, bandBytes));
        }
        return true;
    }
}
#endif

namespace
{
    // Block-compresses one tightly packed level into `payload`, which holds exactly its
    // ceil(w/4) x ceil(h/4) blocks: CompressTextureCookLevel's encode, into storage the caller
    // owns. A failure leaves `payload` partly written.
    bool CompressLevelInto(std::span<const uint8> pixels, uint32 width, uint32 height, bool isFloat,
                           TextureCookOutput output, uint32 bandBlocks, std::span<uint8> payload,
                           std::string& outError, const std::function<bool()>& cancelRequested,
                           TextureCookWorkers* workers)
    {
#if defined(GE_TEXTURE_COOK_ENCODER)
        const DXGI_FORMAT dstFormat = DxgiTargetFor(output);
        if (dstFormat == DXGI_FORMAT_UNKNOWN)
        {
            outError = "not a block-compressed output";
            return false;
        }

        LevelBands bands;
        bands.Pixels = pixels;
        bands.Width = width;
        bands.Height = height;
        bands.IsFloat = isFloat;
        bands.Output = output;
        bands.SourceFormat = isFloat ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        bands.TargetFormat = dstFormat;
        bands.RowPitch = static_cast<size_t>(width) * 4 * (isFloat ? sizeof(float) : 1);

        // No TEX_COMPRESS_PARALLEL: it starts an OpenMP thread team for each calling thread, so
        // concurrent cooks starve the machine. One texture spreads across the JobSystem's workers
        // through `workers` instead, in a budget that every cook shares.
        const LevelBlocks blocks = BlocksOf(dstFormat, width, height);
        const uint32 targetBlocks = bandBlocks != 0 ? bandBlocks : kTextureCookBandBlocks;
        // Clamp to the level's own block rows: a large bandBlocks over a narrow level otherwise
        // wraps bandHeight to 0, and the encoder refuses a zero-height band. A band at or above
        // the level is one whole-level call.
        const uint32 bandBlockRows = std::clamp(targetBlocks / std::max(1u, blocks.PerRow), 1u, blocks.Rows);
        bands.BandHeight = bandBlockRows * kBlockDim;
        bands.BandCount = (blocks.Rows + bandBlockRows - 1) / bandBlockRows;
        bands.BlockRowBytes = blocks.RowBytes;
        if (payload.size() != blocks.Bytes())
        {
            outError = "the payload does not hold the level's blocks";
            return false;
        }
        bands.Payload = payload.data();

        const auto encodeBand = [&bands](uint32 band) { return EncodeLevelBand(bands, band); };
        bool encoded = true;
        if (workers)
        {
            encoded = workers->RunBands(bands.BandCount, encodeBand, cancelRequested);
        }
        else
        {
            for (uint32 band = 0; band < bands.BandCount && encoded; ++band)
                encoded = !(cancelRequested && cancelRequested()) && encodeBand(band);
        }
        if (encoded)
            return true;

        const long failure = bands.FirstFailure.load();
        if (failure != 0)
            outError = "BC encode failed (hr=" + std::to_string(failure) + ")";
        else
            outError = kTextureCookCancelledError;
        return false;
#else
        (void)pixels;
        (void)width;
        (void)height;
        (void)isFloat;
        (void)output;
        (void)bandBlocks;
        (void)payload;
        (void)cancelRequested;
        (void)workers;
        outError = "BC encoder not compiled in";
        return false;
#endif
    }
}

bool CompressTextureCookLevel(const TextureCookMipLevel& level, bool isFloat,
                              TextureCookOutput output, uint32 bandBlocks,
                              std::vector<uint8>& outBytes, std::string& outError,
                              const std::function<bool()>& cancelRequested,
                              TextureCookWorkers* workers)
{
    outBytes.clear();
#if defined(GE_TEXTURE_COOK_ENCODER)
    const DXGI_FORMAT dstFormat = DxgiTargetFor(output);
    if (dstFormat == DXGI_FORMAT_UNKNOWN)
    {
        outError = "not a block-compressed output";
        return false;
    }
    outBytes.resize(BlocksOf(dstFormat, level.Width, level.Height).Bytes());
#endif
    if (CompressLevelInto(level.Bytes, level.Width, level.Height, isFloat, output, bandBlocks, outBytes, outError,
                          cancelRequested, workers))
        return true;
    // A partial payload must never reach the container: drop every band.
    outBytes.clear();
    return false;
}

TextureCookOutput ResolveTextureCookOutput(const TextureCookSettings& settings,
                                           bool sourceIsHDR,
                                           bool deviceSupportsBC,
                                           bool encoderAvailable)
{
    const bool bcViable = deviceSupportsBC && encoderAvailable;

    TextureCookOutput wanted = TextureCookOutput::Uncompressed;
    switch (settings.Compression)
    {
    case TextureCookCompression::BC1:  wanted = TextureCookOutput::BC1; break;
    case TextureCookCompression::BC4:  wanted = TextureCookOutput::BC4; break;
    case TextureCookCompression::BC5:  wanted = TextureCookOutput::BC5; break;
    case TextureCookCompression::BC6H: wanted = TextureCookOutput::BC6H; break;
    case TextureCookCompression::BC7:  wanted = TextureCookOutput::BC7; break;
    case TextureCookCompression::None: return TextureCookOutput::Uncompressed;
    case TextureCookCompression::Auto:
        if (sourceIsHDR)
        {
            // HDR content (sky / environment) compresses to BC6H regardless of
            // usage; LDR usages below never apply to float sources.
            wanted = TextureCookOutput::BC6H;
        }
        else
        {
            switch (settings.Usage)
            {
            case TextureCookUsage::Color:  wanted = TextureCookOutput::BC7; break;
            case TextureCookUsage::Normal: wanted = TextureCookOutput::BC5; break;
            case TextureCookUsage::Mask:   wanted = TextureCookOutput::BC4; break;
            case TextureCookUsage::Packed: wanted = TextureCookOutput::BC7; break;
            case TextureCookUsage::Auto:
            default:
                // Unknown usage: stay uncompressed so CPU consumers (UI
                // backgrounds, terrain masks) keep per-pixel access. In the
                // editor a material bind tags a usage and re-keys the cook
                // (TextureService's usage tag); for packaged content the export
                // resolves it once (CollectPackagedTextureUsages), because a
                // packaged mount derives no import data at runtime. A texture no
                // material document references stays here.
                return TextureCookOutput::Uncompressed;
            }
        }
        break;
    }

    return bcViable ? wanted : TextureCookOutput::Uncompressed;
}

uint64 ComputeTextureCookSourceHash(const uint8* bytes, size_t size)
{
    return Hashing::Fnv1a64(bytes, size);
}

std::optional<uint64> ComputeTextureCookSourceFileHash(const std::filesystem::path& source)
{
    constexpr size_t kChunkBytes = 64 * 1024;
    SharedFileReader reader;
    if (!reader.Open(source))
        return std::nullopt;
    std::array<uint8, kChunkBytes> chunk;
    uint64 hash = Hashing::kFnv1a64OffsetBasis;
    for (;;)
    {
        const int64 read = reader.Read(chunk.data(), chunk.size());
        if (read < 0)
            return std::nullopt;
        if (read == 0)
            return hash;
        hash = Hashing::Fnv1a64(chunk.data(), static_cast<size_t>(read), hash);
    }
}

bool HasNormalVarianceCompanion(const TextureCookInputs& inputs, TextureCookOutput output)
{
    return inputs.Settings.Usage == TextureCookUsage::Normal &&
           inputs.Settings.Compression == TextureCookCompression::Auto &&
           output == TextureCookOutput::BC5 && inputs.Settings.MipsEnabled && inputs.Settings.MipLimit != 1;
}

uint64 ComputeTextureCookConfigHash(const TextureCookInputs& inputs, TextureCookOutput output)
{
    // Canonical debuggable key string. The encoder-version folds tie the key to
    // the built libraries, so keys can differ across platforms/toolchains —
    // fine: the derived cache is machine-local by design.
    char swizzle[5] = {'-', '-', '-', '-', 0};
    if (inputs.Swizzle[0] != 0)
        for (int i = 0; i < 4; ++i)
            swizzle[i] = inputs.Swizzle[i];

    std::string key = "texcook.v" + std::to_string(kTextureCookVersion);
    key += "|cmp="; key += TextureCookCompressionMetaValue(inputs.Settings.Compression);
    key += "|use="; key += TextureCookUsageMetaValue(inputs.Settings.Usage);
    key += "|mips="; key += inputs.Settings.MipsEnabled ? '1' : '0';
    key += "|lim="; key += std::to_string(inputs.Settings.MipLimit);
    key += "|cs="; key += TextureColorSpaceMetaValue(inputs.ColorSpace);
    key += "|sw="; key += swizzle;
    key += "|out="; key += std::to_string(static_cast<uint32>(output));
    key += "|mip="; key += kStbirVersion;
    if (HasNormalVarianceCompanion(inputs, output))
    {
        key += "|nvar="; key += std::to_string(kNormalVarianceCompanionVersion);
    }
    if (inputs.Settings.PreserveAlphaCoverage)
    {
        // Preserve every legacy key when disabled. Only the opt-in recipe is
        // versioned; exact float bits distinguish cutoff-adjacent references.
        const float cutoff = inputs.Settings.AlphaCoverageCutoff == 0.0f
            ? 0.0f : inputs.Settings.AlphaCoverageCutoff;
        key += "|acov=1|cut=";
        key += std::to_string(std::bit_cast<uint32>(cutoff));
    }
    if (output == TextureCookOutput::BC7)
    {
        static const char* const isa = Bc7eInstructionSet();
        key += "|enc=bc7e-"; key += kBc7eVersion; key += '-'; key += isa;
    }
    else if (output != TextureCookOutput::Uncompressed)
    {
        key += "|enc=dxtex-"; key += std::to_string(kDirectXTexVersion);
    }
    return FnvAppend(kFnvOffset, key.data(), key.size());
}

uint32 ComputeTextureCookMipCount(uint32 width, uint32 height, const TextureCookSettings& settings)
{
    if (!settings.MipsEnabled || width == 0 || height == 0)
        return 1;
    uint32 levels = 1;
    uint32 w = width, h = height;
    while (w > 1 || h > 1)
    {
        w = std::max(w / 2u, 1u);
        h = std::max(h / 2u, 1u);
        ++levels;
    }
    if (settings.MipLimit > 0)
        levels = std::min(levels, settings.MipLimit);
    return levels;
}

namespace
{
    using AlphaHistogram = std::array<uint64, 256>;
    using Mathematics::Vector3;

    constexpr float kMinNormalLength = 1e-5f;

    // An RGBA8 texel's XYZ as a unit normal; a degenerate texel decodes as +Z.
    Vector3 DecodeUnitNormal(const uint8* px)
    {
        const Vector3 n(static_cast<float>(px[0]) / 255.0f * 2.0f - 1.0f,
                        static_cast<float>(px[1]) / 255.0f * 2.0f - 1.0f,
                        static_cast<float>(px[2]) / 255.0f * 2.0f - 1.0f);
        const float length = n.Length();
        return length > kMinNormalLength ? n / length : Vector3(0.0f, 0.0f, 1.0f);
    }

    AlphaHistogram AlphaCounts(const MipLevelView& mip)
    {
        AlphaHistogram counts{};
        for (size_t i = 3; i < mip.Bytes.size(); i += 4)
            ++counts[mip.Bytes[i]];
        return counts;
    }

    uint8 ScaleAlphaByte(uint32 alpha, double scale)
    {
        return static_cast<uint8>(std::clamp(std::floor(alpha * scale + 0.5), 0.0, 255.0));
    }

    void PreserveMipAlphaCoverage(std::span<MipLevelView> chain, float cutoff)
    {
        // The material discards alpha < cutoff: cutoff 0 already admits every
        // texel, including transparent zero. UNORM8 conversion also defines 1.
        uint32 passingByte = 0;
        while (passingByte < 255 && static_cast<float>(passingByte) / 255.0f < cutoff)
            ++passingByte;
        if (passingByte == 0)
            return;
        const auto base = AlphaCounts(chain[0]);
        const uint64 basePixels = static_cast<uint64>(chain[0].Width) * chain[0].Height;
        uint64 basePassing = 0;
        for (uint32 a = passingByte; a < 256; ++a) basePassing += base[a];

        for (size_t level = 1; level < chain.size(); ++level)
        {
            auto& mip = chain[level];
            const auto histogram = AlphaCounts(mip);
            const uint64 pixels = static_cast<uint64>(mip.Width) * mip.Height;
            const auto error = [&](uint64 count) {
                // Admission bounds each count to uint32, so products fit uint64.
                const uint64 have = count * basePixels, want = basePassing * pixels;
                return have > want ? have - want : want - have;
            };
            uint64 unscaledPassing = 0;
            for (uint32 a = passingByte; a < 256; ++a) unscaledPassing += histogram[a];
            double bestScale = 1.0;
            uint64 bestError = error(unscaledPassing);
            const auto consider = [&](uint64 count, double scale) {
                const uint64 candidateError = error(count);
                const double distortion = std::abs(scale - 1.0);
                const double bestDistortion = std::abs(bestScale - 1.0);
                if (candidateError < bestError ||
                    (candidateError == bestError && (distortion < bestDistortion ||
                     (distortion == bestDistortion && scale < bestScale))))
                {
                    bestError = candidateError;
                    bestScale = scale;
                }
            };
            // Coverage changes only when one of the 255 nonzero alpha bins
            // crosses the quantized cutoff. Test each side of those boundaries,
            // plus the already-tested identity; no repeated whole-image scans.
            uint64 above = 0;
            for (uint32 a = 255; a > 0; --a)
            {
                if (!histogram[a]) continue;
                const double boundary = (static_cast<double>(passingByte) - 0.5) / a;
                double before = boundary, after = boundary;
                while (ScaleAlphaByte(a, before) >= passingByte)
                    before = std::nextafter(before, 0.0);
                while (ScaleAlphaByte(a, after) < passingByte)
                    after = std::nextafter(after, std::numeric_limits<double>::infinity());
                consider(above, before);
                above += histogram[a];
                consider(above, after);
            }
            if (bestScale == 1.0) continue;
            std::array<uint8, 256> scaled{};
            for (uint32 a = 0; a < 256; ++a) scaled[a] = ScaleAlphaByte(a, bestScale);
            for (size_t i = 3; i < mip.Bytes.size(); i += 4) mip.Bytes[i] = scaled[mip.Bytes[i]];
        }
    }

    // Re-unit-length XYZ after filtering (same recipe as Tools/TextureCompiler):
    // filtered normals shorten, which flattens relief at distance.
    void RenormalizeNormalsRGBA8(const MipLevelView& mip)
    {
        const auto toByte = [](float v) {
            return static_cast<uint8>(std::clamp((v * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
        };
        const size_t count = static_cast<size_t>(mip.Width) * mip.Height;
        for (size_t i = 0; i < count; ++i)
        {
            uint8* px = mip.Bytes.data() + i * 4;
            const Vector3 n = DecodeUnitNormal(px);
            px[0] = toByte(n.x);
            px[1] = toByte(n.y);
            px[2] = toByte(n.z);
        }
    }
}

namespace
{
    // The mip chain's input contract: valid coverage settings and, when coverage is on, one
    // tightly packed RGBA8 base (`levelsHeld` == 1) and a natural mip count.
    bool ValidateMipChainInputs(const MipLevelView& base, size_t levelsHeld, uint32 mipCount, bool isFloat,
                                const TextureCookSettings& settings, std::string& error)
    {
        if (!ValidateTextureAlphaCoverageSettings(settings, isFloat, error))
            return false;
        if (!settings.PreserveAlphaCoverage)
            return true;
        const uint64 pixels = levelsHeld == 0 ? 0 : static_cast<uint64>(base.Width) * base.Height;
        if (levelsHeld != 1 || !pixels || pixels > std::numeric_limits<uint32>::max() || base.Width > INT_MAX ||
            base.Height > INT_MAX || pixels * 4 != base.Bytes.size() || mipCount == 0 ||
            mipCount > ComputeTextureCookMipCount(base.Width, base.Height, TextureCookSettings{}))
        {
            error = "alpha coverage requires one valid tightly packed RGBA8 base and a natural mip count";
            return false;
        }
        return true;
    }

    // One step of the mip recipe: `source` resampled into `destination`, which holds the next
    // extent, and a normal map's XYZ made unit length again. False when the resample fails.
    bool ResampleMipLevel(const MipLevelView& source, const MipLevelView& destination, bool isFloat, bool srgb,
                          TextureCookUsage usage)
    {
        if (isFloat)
        {
            return Image::ResampleRgba32F(
                std::span(reinterpret_cast<const float*>(source.Bytes.data()), source.Bytes.size() / sizeof(float)),
                source.Width, source.Height,
                std::span(reinterpret_cast<float*>(destination.Bytes.data()),
                          destination.Bytes.size() / sizeof(float)),
                destination.Width, destination.Height);
        }
        if (!Image::ResampleRgba8(source.Bytes, source.Width, source.Height, destination.Bytes, destination.Width,
                                  destination.Height,
                                  srgb ? Image::ResampleColorSpace::Srgb : Image::ResampleColorSpace::Linear))
            return false;
        if (usage == TextureCookUsage::Normal)
            RenormalizeNormalsRGBA8(destination);
        return true;
    }

    // Fills levels 1 and up of `chain` from level 0, each into the storage its view names, then
    // corrects their alpha coverage when the settings ask. False when a resample fails.
    bool BuildMipLevels(std::span<MipLevelView> chain, bool isFloat, bool srgb, const TextureCookSettings& settings)
    {
        for (size_t level = 1; level < chain.size(); ++level)
            if (!ResampleMipLevel(chain[level - 1], chain[level], isFloat, srgb, settings.Usage))
                return false;
        if (settings.PreserveAlphaCoverage)
            PreserveMipAlphaCoverage(chain, settings.AlphaCoverageCutoff);
        return true;
    }

    // The most levels a chain of 32-bit extents has.
    constexpr uint32 kMaxMipLevels = 32;

    using MipChainStorage = std::array<MipLevelView, kMaxMipLevels>;

    // Sets the first `count` views of `storage` to `viewOfLevel(level)` and returns them, so a
    // chain of views takes no allocation. `count` is at most kMaxMipLevels.
    std::span<MipLevelView> FillMipChainViews(MipChainStorage& storage, size_t count,
                                              FunctionRef<MipLevelView(uint32)> viewOfLevel)
    {
        const std::span<MipLevelView> chain = std::span(storage).first(count);
        for (uint32 level = 0; level < chain.size(); ++level)
            chain[level] = viewOfLevel(level);
        return chain;
    }
}

bool BuildTextureCookMipChain(std::vector<TextureCookMipLevel>& chain, uint32 mipCount, bool isFloat,
                              bool srgb, const TextureCookSettings& settings, std::string* error)
{
    if (error) error->clear();
    std::string validationError;
    const MipLevelView base = chain.empty() ? MipLevelView{} : ViewOf(chain[0]);
    if (!ValidateMipChainInputs(base, chain.size(), mipCount, isFloat, settings, validationError))
    {
        if (error) *error = validationError;
        return false;
    }
    while (chain.size() < mipCount)
    {
        TextureCookMipLevel next;
        next.Width = std::max(chain.back().Width / 2u, 1u);
        next.Height = std::max(chain.back().Height / 2u, 1u);
        next.Bytes.resize(static_cast<size_t>(next.Width) * next.Height * 4 * (isFloat ? sizeof(float) : 1));
        chain.push_back(std::move(next));
        if (!ResampleMipLevel(ViewOf(chain[chain.size() - 2]), ViewOf(chain.back()), isFloat, srgb, settings.Usage))
        {
            chain.pop_back();
            return false;
        }
    }
    if (settings.PreserveAlphaCoverage)
    {
        // Validation holds the chain to its natural count, at most kMaxMipLevels.
        MipChainStorage storage;
        PreserveMipAlphaCoverage(
            FillMipChainViews(storage, chain.size(), [&chain](uint32 level) { return ViewOf(chain[level]); }),
            settings.AlphaCoverageCutoff);
    }
    return true;
}

#if defined(GE_HAVE_KTX)

namespace
{
    void ApplySwizzleRGBA8(std::span<uint8> pixels, const char swizzle[4])
    {
        const size_t count = pixels.size() / 4;
        for (size_t i = 0; i < count; ++i)
        {
            uint8* px = pixels.data() + i * 4;
            const uint8 src[4] = {px[0], px[1], px[2], px[3]};
            for (int c = 0; c < 4; ++c)
            {
                switch (swizzle[c])
                {
                case 'r': px[c] = src[0]; break;
                case 'g': px[c] = src[1]; break;
                case 'b': px[c] = src[2]; break;
                case 'a': px[c] = src[3]; break;
                case '0': px[c] = 0; break;
                case '1': px[c] = 255; break;
                default: break;
                }
            }
        }
    }

    // Output rows per variance band: a band of a 512-texel-wide level is 16k means, enough work
    // to repay a helper's hand-off.
    constexpr uint32 kVarianceBandRows = 32;

    // sigma of a mean normal of length L, sigma^2 = (1 - L) / L (Toksvig), L clamped to [0.5, 1].
    uint8 EncodeNormalSigmaByte(const Vector3& mean)
    {
        const float length = std::clamp(mean.Length(), 0.5f, 1.0f);
        const float sigma = std::sqrt((1.0f - length) / length);
        return static_cast<uint8>(std::clamp(sigma * 255.0f + 0.5f, 0.0f, 255.0f));
    }

    // Box-filters rows [rowBegin, rowEnd) of a level of normals halved to max(extent / 2, 1), and
    // writes each mean and its sigma texel. An odd last row or column folds into the last texel,
    // so every source texel counts once. The means stay unnormalized.
    template <typename FetchNormal>
    void HalveNormalRows(uint32 width, uint32 height, const MipLevelView& sigmaLevel, uint32 rowBegin,
                         uint32 rowEnd, const FetchNormal& fetch, std::span<Vector3> means)
    {
        const uint32 halfWidth = sigmaLevel.Width;
        const uint32 halfHeight = sigmaLevel.Height;
        for (uint32 y = rowBegin; y < rowEnd; ++y)
        {
            const uint32 y0 = std::min(y * 2u, height - 1u);
            const uint32 y1 = y + 1u == halfHeight ? height : std::min(y * 2u + 2u, height);
            for (uint32 x = 0; x < halfWidth; ++x)
            {
                const uint32 x0 = std::min(x * 2u, width - 1u);
                const uint32 x1 = x + 1u == halfWidth ? width : std::min(x * 2u + 2u, width);
                Vector3 sum(0.0f, 0.0f, 0.0f);
                for (uint32 sy = y0; sy < y1; ++sy)
                    for (uint32 sx = x0; sx < x1; ++sx)
                        sum = sum + fetch(static_cast<size_t>(sy) * width + sx);
                const size_t index = static_cast<size_t>(y) * halfWidth + x;
                means[index] = sum / static_cast<float>((y1 - y0) * (x1 - x0));
                const uint8 sigma = EncodeNormalSigmaByte(means[index]);
                uint8* px = sigmaLevel.Bytes.data() + index * 4;
                px[0] = sigma;
                px[1] = sigma;
                px[2] = sigma;
                px[3] = 255;
            }
        }
    }

    // One companion level from a level of normals: the sigma level at half extent, and its means.
    // Bands run on the cook's workers or on this thread; each writes its own rows, so the bytes do not
    // depend on the split. False on a cancel.
    template <typename FetchNormal>
    bool BuildVarianceLevel(uint32 width, uint32 height, const FetchNormal& fetch, TextureCookWorkers* workers,
                            const std::function<bool()>& cancelRequested, const MipLevelView& outLevel,
                            std::span<Vector3> outMeans)
    {
        const uint32 bandCount = (outLevel.Height + kVarianceBandRows - 1u) / kVarianceBandRows;
        const auto runBand = [&](uint32 band)
        {
            const uint32 rowBegin = band * kVarianceBandRows;
            const uint32 rowEnd = std::min(rowBegin + kVarianceBandRows, outLevel.Height);
            HalveNormalRows(width, height, outLevel, rowBegin, rowEnd, fetch, outMeans);
            return true;
        };
        if (workers && bandCount > 1u)
            return workers->RunBands(bandCount, runBand, cancelRequested);
        for (uint32 band = 0; band < bandCount; ++band)
            runBand(band);
        return true;
    }

    // The companion's level count for a normal map of `normalMipCount` mips: level j is sigma of
    // normal mip j + 1, at that mip's extent.
    uint32 CompanionLevelCount(uint32 normalMipCount)
    {
        return std::max(normalMipCount, 2u) - 1u;
    }

    // Fills the companion's levels, each sized to its extent. The means box-filter the base's unit
    // normals, so each level sees its whole footprint; `means` and `previousMeans` each hold the
    // texels of level 0. False on a cancel.
    bool BuildNormalVarianceCompanionLevels(const MipLevelView& base, std::span<const MipLevelView> levels,
                                            std::span<Vector3> means, std::span<Vector3> previousMeans,
                                            TextureCookWorkers* workers,
                                            const std::function<bool()>& cancelRequested)
    {
        const uint8* baseBytes = base.Bytes.data();
        if (!BuildVarianceLevel(base.Width, base.Height,
                                [baseBytes](size_t index) { return DecodeUnitNormal(baseBytes + index * 4); },
                                workers, cancelRequested, levels[0], means))
            return false;
        for (size_t level = 1; level < levels.size(); ++level)
        {
            std::swap(previousMeans, means);
            const MipLevelView& previous = levels[level - 1];
            const std::span<const Vector3> previousLevelMeans = previousMeans;
            if (!BuildVarianceLevel(previous.Width, previous.Height,
                                    [previousLevelMeans](size_t index) { return previousLevelMeans[index]; },
                                    workers, cancelRequested, levels[level], means))
                return false;
        }
        return true;
    }

    uint32 VkFormatFor(TextureCookOutput out, bool srgb, bool isFloat)
    {
        switch (out)
        {
        case TextureCookOutput::BC1:  return srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case TextureCookOutput::BC4:  return VK_FORMAT_BC4_UNORM_BLOCK;
        case TextureCookOutput::BC5:  return VK_FORMAT_BC5_UNORM_BLOCK;
        case TextureCookOutput::BC6H: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case TextureCookOutput::BC7:  return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
        default:
            if (isFloat)
                return VK_FORMAT_R32G32B32A32_SFLOAT;
            return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        }
    }

    // A ktxStream over bytes written in order; with no bytes it only counts. A write whose source
    // is already at the write position is skipped: a level payload encoded in place stays there.
    struct ByteRangeStream
    {
        uint8* Data = nullptr;
        size_t Size = 0;
        size_t Position = 0;
    };

    ByteRangeStream& RangeOf(ktxStream* stream)
    {
        return *static_cast<ByteRangeStream*>(stream->data.custom_ptr.address);
    }

    KTX_error_code WriteToByteRange(ktxStream* stream, const void* source, const ktx_size_t size,
                                    const ktx_size_t count)
    {
        ByteRangeStream& range = RangeOf(stream);
        const size_t bytes = static_cast<size_t>(size * count);
        if (range.Data)
        {
            if (bytes > range.Size - range.Position)
                return KTX_FILE_OVERFLOW;
            uint8* destination = range.Data + range.Position;
            const auto sourceAddress = reinterpret_cast<std::uintptr_t>(source);
            const auto rangeAddress = reinterpret_cast<std::uintptr_t>(range.Data);
            if (source != destination)
            {
                // A source inside the range that is not in place: libktx laid the file out
                // differently from the level storage it was given.
                if (sourceAddress >= rangeAddress && sourceAddress < rangeAddress + range.Size)
                    return KTX_INVALID_OPERATION;
                std::memcpy(destination, source, bytes);
            }
        }
        range.Position += bytes;
        return KTX_SUCCESS;
    }

    // Writes `texture`'s file into `range`, or counts its bytes when the range has none.
    KTX_error_code WriteKtx2(ktxTexture2* texture, ByteRangeStream& range)
    {
        ktxStream stream{};
        stream.read = [](ktxStream*, void*, const ktx_size_t) { return KTX_INVALID_OPERATION; };
        stream.skip = [](ktxStream*, const ktx_size_t) { return KTX_INVALID_OPERATION; };
        stream.write = WriteToByteRange;
        stream.getpos = [](ktxStream* s, ktx_off_t* const offset) {
            *offset = static_cast<ktx_off_t>(RangeOf(s).Position);
            return KTX_SUCCESS;
        };
        stream.setpos = [](ktxStream*, const ktx_off_t) { return KTX_INVALID_OPERATION; };
        stream.getsize = [](ktxStream* s, ktx_size_t* const size) {
            *size = RangeOf(s).Position;
            return KTX_SUCCESS;
        };
        stream.destruct = [](ktxStream*) {};
        stream.type = eStreamTypeCustom;
        stream.data.custom_ptr.address = &range;
        return ktxTexture2_WriteToStream(texture, &stream);
    }

    // Room for the header, the level index of kMaxMipLevels levels and the metadata, so the
    // file's bytes are sized once. A larger header costs one reallocation before any payload.
    constexpr size_t kKtx2HeaderReserveBytes = 2048;

    // A KTX2 file that libktx lays out and the cook writes in place: Begin sizes the bytes once
    // for the whole file, each level payload is written once at its file offset (Level), and
    // Finish writes the header, the level index and the metadata in front of the payloads.
    class Ktx2InPlaceWriter
    {
      public:
        explicit Ktx2InPlaceWriter(std::vector<uint8>& bytes) : m_Bytes(bytes) {}
        ~Ktx2InPlaceWriter()
        {
            if (!m_Texture)
                return;
            m_Texture->pData = nullptr; // the bytes are the caller's
            ktxTexture_Destroy(ktxTexture(m_Texture));
        }
        Ktx2InPlaceWriter(const Ktx2InPlaceWriter&) = delete;
        Ktx2InPlaceWriter& operator=(const Ktx2InPlaceWriter&) = delete;

        // The vkFormat carries the block layout and the transfer function; the loader reads the
        // color space from the container's format descriptor.
        bool Begin(TextureCookOutput output, bool srgb, bool isFloat, uint32 width, uint32 height,
                   uint32 levelCount, std::string& outError)
        {
            ktxTextureCreateInfo createInfo{};
            createInfo.vkFormat = VkFormatFor(output, srgb, isFloat);
            createInfo.baseWidth = width;
            createInfo.baseHeight = height;
            createInfo.baseDepth = 1;
            createInfo.numDimensions = 2;
            createInfo.numLevels = levelCount;
            createInfo.numLayers = 1;
            createInfo.numFaces = 1;
            createInfo.isArray = KTX_FALSE;
            createInfo.generateMipmaps = KTX_FALSE;
            KTX_error_code err = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_NO_STORAGE, &m_Texture);
            if (err != KTX_SUCCESS)
            {
                m_Texture = nullptr;
                outError = std::string("ktxTexture2_Create failed: ") + ktxErrorString(err);
                return false;
            }

            // The writer key names the cook. libktx appends its own version to it, so a libktx
            // update changes the cooked bytes.
            const char* writer = "GameEngine.TextureCook v1";
            ktxHashList_AddKVPair(&m_Texture->kvDataHead, KTX_WRITER_KEY,
                                  static_cast<ktx_uint32_t>(std::strlen(writer) + 1), writer);

            // The levels sit smallest first, so the base level ends the payloads.
            ktx_size_t baseOffset = 0;
            ktxTexture_GetImageOffset(ktxTexture(m_Texture), 0, 0, 0, &baseOffset);
            const size_t payloadBytes = baseOffset + ktxTexture_GetImageSize(ktxTexture(m_Texture), 0);

            // Count the header first, with the payload range standing in for levels not written yet.
            m_Bytes.clear();
            m_Bytes.reserve(payloadBytes + kKtx2HeaderReserveBytes);
            m_Bytes.resize(payloadBytes);
            m_Texture->pData = m_Bytes.data();
            m_Texture->dataSize = payloadBytes;
            ByteRangeStream counter;
            err = WriteKtx2(m_Texture, counter);
            if (err != KTX_SUCCESS || counter.Position < payloadBytes)
            {
                outError = std::string("ktx header layout failed: ") + ktxErrorString(err);
                return false;
            }
            m_Bytes.resize(counter.Position);
            m_Texture->pData = m_Bytes.data() + (counter.Position - payloadBytes);
            return true;
        }

        // Level `level`'s payload, at its place in the file.
        std::span<uint8> Level(uint32 level) const
        {
            ktx_size_t offset = 0;
            ktxTexture_GetImageOffset(ktxTexture(m_Texture), level, 0, 0, &offset);
            return {m_Texture->pData + offset, ktxTexture_GetImageSize(ktxTexture(m_Texture), level)};
        }

        bool Finish(std::string& outError)
        {
            ByteRangeStream file{m_Bytes.data(), m_Bytes.size()};
            const KTX_error_code err = WriteKtx2(m_Texture, file);
            if (err != KTX_SUCCESS || file.Position != m_Bytes.size())
            {
                outError = std::string("ktx write failed: ") + ktxErrorString(err);
                return false;
            }
            return true;
        }

      private:
        std::vector<uint8>& m_Bytes;
        ktxTexture2* m_Texture = nullptr;
    };

    // Encodes each level of `chain` into its place in `file`; an uncompressed chain is there
    // already. This check catches a cancel between levels; the band encode checks again
    // between block-row bands within a level, which bounds the wait on a large base level.
    bool EncodeLevels(std::span<const MipLevelView> chain, bool isFloat, TextureCookOutput output,
                      const Ktx2InPlaceWriter& file, std::string& outError,
                      const std::function<bool()>& cancelRequested, TextureCookWorkers* workers)
    {
        for (uint32 level = 0; level < chain.size(); ++level)
        {
            if (cancelRequested && cancelRequested())
            {
                outError = kTextureCookCancelledError;
                return false;
            }
            if (output != TextureCookOutput::Uncompressed &&
                !CompressLevelInto(chain[level].Bytes, chain[level].Width, chain[level].Height, isFloat, output,
                                   /*bandBlocks=*/0, file.Level(level), outError, cancelRequested, workers))
                return false;
        }
        return true;
    }

    // Arena allocations start on this boundary, so a level read as floats or Vector3s is aligned.
    constexpr size_t kCookLevelAlignment = 16;

    size_t AlignedCookBytes(size_t bytes)
    {
        return (bytes + kCookLevelAlignment - 1) / kCookLevelAlignment * kCookLevelAlignment;
    }

    size_t MipLevelBytes(uint32 baseWidth, uint32 baseHeight, uint32 level, size_t bytesPerTexel)
    {
        return static_cast<size_t>(MipExtent(baseWidth, level)) * MipExtent(baseHeight, level) * bytesPerTexel;
    }

    // What the cook's arena hands out: the chain's levels above the base when they are encoded,
    // and the companion's levels and means. Sized up front, so the arena takes one block.
    size_t CookArenaBytes(const MipLevelView& base, uint32 mipCount, size_t bytesPerTexel, bool chainInArena,
                          uint32 companionLevels)
    {
        size_t bytes = 0;
        for (uint32 level = 1; chainInArena && level < mipCount; ++level)
            bytes += AlignedCookBytes(MipLevelBytes(base.Width, base.Height, level, bytesPerTexel));
        for (uint32 level = 1; level <= companionLevels; ++level)
            bytes += AlignedCookBytes(MipLevelBytes(base.Width, base.Height, level, 4));
        if (companionLevels > 0)
            bytes += 2 * AlignedCookBytes(MipLevelBytes(base.Width, base.Height, 1, sizeof(Vector3)));
        return bytes;
    }

    // Mip `level` of `base` in storage from the cook's arena.
    MipLevelView ArenaMipLevel(Memory::Arena& arena, const MipLevelView& base, uint32 level, size_t bytesPerTexel)
    {
        const size_t bytes = MipLevelBytes(base.Width, base.Height, level, bytesPerTexel);
        return {std::span(static_cast<uint8*>(arena.Allocate(bytes, kCookLevelAlignment)), bytes),
                MipExtent(base.Width, level), MipExtent(base.Height, level)};
    }

    // Builds the BC4 normal-variance companion of `base` and writes its file into `outKtx2Bytes`.
    bool CookNormalVarianceCompanion(const MipLevelView& base, uint32 normalMipCount, Memory::Arena& arena,
                                     std::vector<uint8>& outKtx2Bytes, std::string& outError,
                                     const std::function<bool()>& cancelRequested, TextureCookWorkers* workers)
    {
        MipChainStorage storage;
        const std::span<MipLevelView> levels =
            FillMipChainViews(storage, CompanionLevelCount(normalMipCount),
                              [&arena, &base](uint32 level) { return ArenaMipLevel(arena, base, level + 1, 4); });
        const size_t meanCount = static_cast<size_t>(levels[0].Width) * levels[0].Height;
        const std::span<Vector3> means = arena.AllocateArrayForOverwrite<Vector3>(meanCount);
        const std::span<Vector3> previousMeans = arena.AllocateArrayForOverwrite<Vector3>(meanCount);
        if (!BuildNormalVarianceCompanionLevels(base, levels, means, previousMeans, workers, cancelRequested))
        {
            outError = kTextureCookCancelledError;
            return false;
        }
        Ktx2InPlaceWriter file(outKtx2Bytes);
        return file.Begin(TextureCookOutput::BC4, false, false, levels[0].Width, levels[0].Height,
                          static_cast<uint32>(levels.size()), outError) &&
               EncodeLevels(levels, false, TextureCookOutput::BC4, file, outError, cancelRequested, workers) &&
               file.Finish(outError);
    }

    // A failed cook leaves no bytes in either output. Returns false for the caller to return.
    bool DropCookOutputs(std::vector<uint8>& outKtx2Bytes, std::vector<uint8>* outVarianceCompanionKtx2)
    {
        outKtx2Bytes.clear();
        if (outVarianceCompanionKtx2)
            outVarianceCompanionKtx2->clear();
        return false;
    }
} // namespace

bool CookTexture(const uint8* srcBytes, size_t srcSize, std::string_view sourceName, const std::string& extLower,
                 const TextureCookInputs& inputs, TextureCookOutput output,
                 std::vector<uint8>& outKtx2Bytes, std::string& outError,
                 const std::function<bool()>& cancelRequested,
                 TextureCookWorkers* workers,
                 std::vector<uint8>* outVarianceCompanionKtx2)
{
    const auto isCancelled = [&cancelRequested]
    { return cancelRequested && cancelRequested(); };

    outKtx2Bytes.clear();
    if (!srcBytes || srcSize == 0)
    {
        outError = "empty source";
        return false;
    }

    if (isCancelled())
    {
        outError = kTextureCookCancelledError;
        return false;
    }

#if !defined(GE_TEXTURE_COOK_ENCODER)
    if (output != TextureCookOutput::Uncompressed)
    {
        outError = "BC encoder not compiled in";
        return false;
    }
#endif

    const bool isFloat = extLower == ".hdr";
    const bool srgb = inputs.ColorSpace == TextureColorSpace::SRGB && !isFloat;
    if (!ValidateTextureAlphaCoverageSettings(inputs.Settings, isFloat, outError))
        return false;
    if (inputs.Settings.PreserveAlphaCoverage && output != TextureCookOutput::Uncompressed &&
        output != TextureCookOutput::BC7)
    {
        outError = "alpha coverage output must retain RGBA";
        return false;
    }

    // Decode the source to tightly-packed RGBA (8-bit or float). A source that does
    // not decode fails the cook; the decode's warning names it and says why.
    // The base level is the decoder's own pixels, viewed in place.
    const std::span<const uint8> source(srcBytes, srcSize);
    Image::ImageFloat decodedFloat;
    Image::Image8 decoded8;
    MipLevelView base;
    if (isFloat)
    {
        decodedFloat = Image::DecodeFloat(source, sourceName, Image::DecodeChannels::Rgba);
        if (!decodedFloat.Valid)
        {
            outError = "decode failed";
            return false;
        }
        base = {std::span(reinterpret_cast<uint8*>(decodedFloat.Pixels.data()),
                          decodedFloat.Pixels.size() * sizeof(float)),
                decodedFloat.Width, decodedFloat.Height};
    }
    else
    {
        decoded8 = Image::Decode8(source, sourceName, Image::DecodeChannels::Rgba);
        if (!decoded8.Valid)
        {
            outError = "decode failed";
            return false;
        }
        base = {decoded8.Pixels, decoded8.Width, decoded8.Height};
    }

    // Bake the upload swizzle so block-compressed payloads honor it too (the
    // upload path cannot swizzle blocks; see TextureAsset::IsSwizzleBaked).
    if (!isFloat && inputs.Swizzle[0] != 0)
        ApplySwizzleRGBA8(base.Bytes, inputs.Swizzle);

    const uint32 mipCount = ComputeTextureCookMipCount(base.Width, base.Height, inputs.Settings);
    if (!ValidateMipChainInputs(base, 1, mipCount, isFloat, inputs.Settings, outError))
        return false;
    const bool cookCompanion = outVarianceCompanionKtx2 && !isFloat && HasNormalVarianceCompanion(inputs, output);
    if (outVarianceCompanionKtx2)
        outVarianceCompanionKtx2->clear();

    // An uncompressed chain is built where the file holds it. An encoded chain's levels above
    // the base, and the companion, live in one arena for the cook.
    const size_t bytesPerTexel = isFloat ? 4 * sizeof(float) : 4;
    const bool encoded = output != TextureCookOutput::Uncompressed;
    Memory::Arena arena("Assets.TextureCook", CookArenaBytes(base, mipCount, bytesPerTexel, encoded,
                                                             cookCompanion ? CompanionLevelCount(mipCount) : 0));
    Ktx2InPlaceWriter file(outKtx2Bytes);
    if (!file.Begin(output, srgb, isFloat, base.Width, base.Height, mipCount, outError))
        return DropCookOutputs(outKtx2Bytes, outVarianceCompanionKtx2);
    MipChainStorage storage;
    const std::span<MipLevelView> chain = FillMipChainViews(
        storage, mipCount, [&file, &arena, &base, encoded, bytesPerTexel](uint32 level) {
            if (!encoded)
                return MipLevelView{file.Level(level), MipExtent(base.Width, level), MipExtent(base.Height, level)};
            return level == 0 ? base : ArenaMipLevel(arena, base, level, bytesPerTexel);
        });
    if (!encoded)
    {
        if (chain[0].Bytes.size() != base.Bytes.size())
        {
            outError = "the file's base level does not hold the decoded image";
            return DropCookOutputs(outKtx2Bytes, outVarianceCompanionKtx2);
        }
        std::memcpy(chain[0].Bytes.data(), base.Bytes.data(), base.Bytes.size());
    }
    if (!BuildMipLevels(chain, isFloat, srgb, inputs.Settings))
    {
        outError = "mip downsample failed";
        return DropCookOutputs(outKtx2Bytes, outVarianceCompanionKtx2);
    }

    if (cookCompanion && !CookNormalVarianceCompanion(chain[0], mipCount, arena, *outVarianceCompanionKtx2,
                                                      outError, cancelRequested, workers))
        return DropCookOutputs(outKtx2Bytes, outVarianceCompanionKtx2);
    if (!EncodeLevels(chain, isFloat, output, file, outError, cancelRequested, workers) || !file.Finish(outError))
        return DropCookOutputs(outKtx2Bytes, outVarianceCompanionKtx2);
    return true;
}

#else // !GE_HAVE_KTX

bool CookTexture(const uint8*, size_t, std::string_view, const std::string&, const TextureCookInputs&,
                 TextureCookOutput, std::vector<uint8>&, std::string& outError,
                 const std::function<bool()>&, TextureCookWorkers*, std::vector<uint8>*)
{
    outError = "texture cook requires libktx at build time";
    return false;
}

#endif

} // namespace GameEngine
