#include "BackgroundImageUpload.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Engine/Rendering/TextureMipUpload.h"
#include "Image/Decode.h"
#include "Logger/Logger.h"
#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
namespace UI
{

// Largest image edge, in pixels, that a UI upload builds a mip chain for. Every editor icon
// is at or below it (the largest are 512 px PNGs); above it sit texture previews, whose
// chain would cost the UI thread a full CPU resample on every upload.
namespace
{
constexpr uint32_t kMaxMippedUiImageEdge = 1024;

// Decode an ordinary image file to RGBA8 for a preview. Full resolution: the
// caller caches the upload per asset, and a UI background is scaled by the
// style, not by the payload.
bool DecodeSourceImageToRgba8(const std::filesystem::path& path,
                                     std::vector<unsigned char>& outRgba,
                                     uint32_t& outWidth, uint32_t& outHeight)
{
    Vector<uint8> bytes;
    if (!ReadFileBytesShared(path, bytes) || bytes.empty())
        return false;

    const Image::Image8 decoded = Image::Decode8(bytes, path.string(), Image::DecodeChannels::Rgba);
    if (!decoded.Valid || decoded.Width == 0 || decoded.Height == 0)
        return false;

    outRgba = std::vector<unsigned char>(decoded.Pixels.begin(), decoded.Pixels.end());
    outWidth = decoded.Width;
    outHeight = decoded.Height;
    return true;
}
} // namespace

Rendering::TextureHandle UploadBackgroundImageFromAsset(Rendering::IDevice* device, const TextureAsset* texA,
                                                        uint32_t& outMipLevels)
{
    if (!device || !texA)
        return Rendering::TextureHandle{};

    uint32_t width = texA->GetWidth();
    uint32_t height = texA->GetHeight();
    if (!width || !height)
        return Rendering::TextureHandle{};

    const auto fmt = texA->GetFormat();
    const uint32_t channels = texA->GetChannels();
    const uint64 dataSize = texA->GetDataSize();
    const uint8* pixelsTyped = texA->GetPixelData();
    if (!pixelsTyped || dataSize == 0)
        return Rendering::TextureHandle{};

    const unsigned char* src = reinterpret_cast<const unsigned char*>(pixelsTyped);
    const uint64 pixelCount = static_cast<uint64>(width) * static_cast<uint64>(height);
    if (pixelCount == 0)
        return Rendering::TextureHandle{};

    const void* uploadData = src;
    uint64 uploadSize = dataSize;
    std::vector<unsigned char> converted;

    // A block-compressed payload (a cooked artifact, a package's payload, or an authored
    // Basis container transcoded to BC7) is not per-pixel addressable: decode its level 0.
    // BC6H has no CPU decode, so its source image answers instead (the editor keeps the
    // source; a package does not, and that preview stays empty).
    bool decodedFromContainer = false;
    if (texA->IsBlockCompressed())
    {
        const auto& mips = texA->GetMipChain();
        const uint64 level0Offset = mips.empty() ? 0 : mips[0].Offset;
        const uint64 level0Size = mips.empty() ? dataSize : mips[0].Size;
        const std::filesystem::path& assetPath = texA->GetPath();
        const bool decoded =
            DecodeBlockPayloadRGBA8(fmt, width, height, pixelsTyped + level0Offset, level0Size, converted) ||
            DecodeSourceImageToRgba8(assetPath, converted, width, height);
        if (!decoded)
        {
            static bool sWarnedBlockCompressedOnce = false;
            if (!sWarnedBlockCompressedOnce)
            {
                Logger::Log::Warning("UIManager: failed to re-decode block-compressed texture '{}' for UI preview.",
                                     assetPath.string());
                sWarnedBlockCompressedOnce = true;
            }
            return Rendering::TextureHandle{};
        }
        uploadData = converted.data();
        uploadSize = static_cast<uint64>(converted.size());
        decodedFromContainer = true;
    }

    auto isSupported8Bit = [](GameEngine::TextureFormat f) -> bool
    {
        switch (f)
        {
        case GameEngine::TextureFormat::R8:
        case GameEngine::TextureFormat::RG8:
        case GameEngine::TextureFormat::RGB8:
        case GameEngine::TextureFormat::RGBA8:
            return true;
        default:
            return false;
        }
    };

    auto isSupportedFloat = [](GameEngine::TextureFormat f) -> bool
    {
        switch (f)
        {
        case GameEngine::TextureFormat::R32F:
        case GameEngine::TextureFormat::RG32F:
        case GameEngine::TextureFormat::RGB32F:
        case GameEngine::TextureFormat::RGBA32F:
            return true;
        default:
            return false;
        }
    };

    if (decodedFromContainer)
    {
        // RGBA8 level-0 data is already in `converted`; skip format handling.
    }
    else if (isSupportedFloat(fmt))
    {
        if (dataSize < pixelCount * channels * sizeof(float))
            return Rendering::TextureHandle{};

        auto toByte = [](float v) -> unsigned char
        {
            if (!std::isfinite(v))
                v = 0.0f;
            v = std::max(0.0f, v);
            v = v / (1.0f + v);
            v = std::pow(v, 1.0f / 2.2f);
            v = Math::Clamp01(v);
            return static_cast<unsigned char>(v * 255.0f + 0.5f);
        };

        auto alphaToByte = [](float v) -> unsigned char
        {
            if (!std::isfinite(v))
                v = 1.0f;
            v = Math::Clamp01(v);
            return static_cast<unsigned char>(v * 255.0f + 0.5f);
        };

        converted.resize(static_cast<size_t>(pixelCount * 4u));
        const float* srcIt = reinterpret_cast<const float*>(pixelsTyped);
        unsigned char* dst = converted.data();
        for (uint64 i = 0; i < pixelCount; ++i)
        {
            const float r = channels >= 1 ? srcIt[0] : 0.0f;
            const float g = channels >= 2 ? srcIt[1] : r;
            const float b = channels >= 3 ? srcIt[2] : g;
            const float a = channels >= 4 ? srcIt[3] : 1.0f;
            dst[0] = toByte(r);
            dst[1] = toByte(g);
            dst[2] = toByte(b);
            dst[3] = alphaToByte(a);
            srcIt += channels;
            dst += 4;
        }

        uploadData = converted.data();
        uploadSize = static_cast<uint64>(converted.size());
    }
    else if (!isSupported8Bit(fmt))
    {
        static bool sWarnedOnce = false;
        if (!sWarnedOnce)
        {
            Logger::Log::Warning("UIManager: background image texture '{}' has unsupported format for UI backgrounds; expected 8-bit LDR or HDR float texture. Background will not render.", texA->GetPath().string());
            sWarnedOnce = true;
        }
        return Rendering::TextureHandle{};
    }

    // Fast path: already RGBA8 with four channels, upload directly.
    if (!decodedFromContainer &&
        isSupported8Bit(fmt) && !(fmt == GameEngine::TextureFormat::RGBA8 && channels == 4))
    {
        // Convert to RGBA8 with opaque alpha so we can upload as UI texture.
        converted.resize(static_cast<size_t>(pixelCount * 4u));
        unsigned char* dst = converted.data();

        if (fmt == GameEngine::TextureFormat::RGB8 && channels >= 3)
        {
            const unsigned char* srcIt = src;
            for (uint64 i = 0; i < pixelCount; ++i)
            {
                dst[0] = srcIt[0];
                dst[1] = srcIt[1];
                dst[2] = srcIt[2];
                dst[3] = 255u;
                srcIt += channels;
                dst += 4;
            }
        }
        else if (fmt == GameEngine::TextureFormat::R8 && channels >= 1)
        {
            const unsigned char* srcIt = src;
            for (uint64 i = 0; i < pixelCount; ++i)
            {
                unsigned char v = srcIt[0];
                dst[0] = v;
                dst[1] = v;
                dst[2] = v;
                dst[3] = 255u;
                srcIt += channels;
                dst += 4;
            }
        }
        else if (fmt == GameEngine::TextureFormat::RG8 && channels >= 2)
        {
            const unsigned char* srcIt = src;
            for (uint64 i = 0; i < pixelCount; ++i)
            {
                unsigned char r = srcIt[0];
                unsigned char g = srcIt[1];
                dst[0] = r;
                dst[1] = g;
                dst[2] = g;
                dst[3] = 255u;
                srcIt += channels;
                dst += 4;
            }
        }
        else
        {
            // Unexpected combination; fail gracefully.
            return Rendering::TextureHandle{};
        }

        uploadData = converted.data();
        uploadSize = static_cast<uint64>(converted.size());
    }

    if (uploadSize < pixelCount * 4u)
        return Rendering::TextureHandle{};

    // A UI image is often drawn smaller than its pixels (a raster icon shown below its size,
    // or an SVG asset drawn from its own raster), so it gets the full trilinear chain the UI
    // sampler reads. An SVG drawn at its baked size uploads level 0 only
    // (BackgroundImageCache::CompleteSvgSize).
    TextureCookSettings mipSettings;
    mipSettings.MipsEnabled = std::max(width, height) <= kMaxMippedUiImageEdge;
    const bool srgb = texA->GetColorSpace() == TextureColorSpace::SRGB;
    const Rendering::TextureFormat gpuFormat =
        srgb ? Rendering::TextureFormat::RGBA8_SRGB : Rendering::TextureFormat::RGBA8_UNORM;
    return UploadRgba8WithMips(device, static_cast<const uint8_t*>(uploadData), width, height, gpuFormat,
                               mipSettings, srgb, "UI_BG", "UI_BG_Staging", outMipLevels);
}

} // namespace UI
} // namespace GameEngine
