#include "Rendering/Utils/CubeLutGpuUpload.h"

#include "Rendering/Utils/TextureUploadHelpers.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace GameEngine::Rendering
{
namespace
{
void PackRgbToRgba32f(const std::vector<float32>& rgb, std::vector<float32>& rgba)
{
    rgba.resize((rgb.size() / 3u) * 4u);
    for (size_t i = 0; i < rgb.size() / 3u; ++i)
    {
        rgba[i * 4u + 0u] = rgb[i * 3u + 0u];
        rgba[i * 4u + 1u] = rgb[i * 3u + 1u];
        rgba[i * 4u + 2u] = rgb[i * 3u + 2u];
        rgba[i * 4u + 3u] = 1.0f;
    }
}

uint16_t FloatToHalf(float value)
{
    if (std::isnan(value))
        return 0x7E00u;

    const bool sign = std::signbit(value);
    float v = std::fabs(value);
    uint16_t signBits = sign ? 0x8000u : 0u;

    if (std::isinf(v))
        return static_cast<uint16_t>(signBits | 0x7C00u);
    if (v == 0.0f)
        return signBits;

    if (v > 65504.0f)
        return static_cast<uint16_t>(signBits | 0x7BFFu);
    if (v < 5.960464477539063e-8f)
        return signBits;

    int exp = 0;
    float mant = std::frexp(v, &exp);
    if (exp < -13)
    {
        const float scaled = std::ldexp(v, 24);
        return static_cast<uint16_t>(signBits | static_cast<uint16_t>(std::lround(scaled)));
    }

    const uint16_t halfExp = static_cast<uint16_t>(exp + 14);
    uint16_t halfMant = static_cast<uint16_t>(std::lround((mant * 2.0f - 1.0f) * 1024.0f));
    if (halfMant == 1024u)
    {
        halfMant = 0u;
        return static_cast<uint16_t>(signBits | ((halfExp + 1u) << 10u));
    }
    return static_cast<uint16_t>(signBits | (halfExp << 10u) | halfMant);
}

void PackRgba32fToRgba16f(const std::vector<float32>& rgba32, std::vector<uint16_t>& rgba16)
{
    rgba16.resize(rgba32.size());
    for (size_t i = 0; i < rgba32.size(); ++i)
        rgba16[i] = FloatToHalf(rgba32[i]);
}

TextureFormat NormalizeLutTextureFormat(TextureFormat format)
{
    return format == TextureFormat::R16G16B16A16_FLOAT
        ? TextureFormat::R16G16B16A16_FLOAT
        : TextureFormat::R32G32B32A32_FLOAT;
}
} // namespace

bool CreateCubeLutGpuTextures(IDevice* device,
                              const CubeLutParseResult& parsed,
                              TextureFormat textureFormat,
                              CubeLutGpuTextures& outTextures,
                              std::string* outError)
{
    outTextures = CubeLutGpuTextures{};
    if (!device)
    {
        if (outError)
            *outError = "No device.";
        return false;
    }
    if (!parsed.Ok)
    {
        if (outError)
            *outError = "Parsed LUT is not valid.";
        return false;
    }

    auto cleanupOnFailure = [&]()
    {
        if (outTextures.Lut3D.IsValid())
            device->DestroyTexture(outTextures.Lut3D);
        if (outTextures.Lut1DStrip.IsValid())
            device->DestroyTexture(outTextures.Lut1DStrip);
        outTextures = CubeLutGpuTextures{};
    };

    if (parsed.Has3D)
    {
        std::vector<float32> rgba;
        PackRgbToRgba32f(parsed.Lut3DRgb, rgba);
        const uint32_t n = parsed.Size3D;
        const TextureFormat gpuFormat = NormalizeLutTextureFormat(textureFormat);
        TextureDesc td{};
        td.width = n;
        td.height = n;
        td.depth = n;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.format = static_cast<uint32_t>(gpuFormat);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferDst);
        td.persistent = true;
        td.debugName = "CubeLut3D";
        outTextures.Lut3D = device->CreateTexture(td);
        if (!outTextures.Lut3D.IsValid())
        {
            if (outError)
                *outError = "Failed to create 3D LUT texture.";
            cleanupOnFailure();
            return false;
        }
        if (gpuFormat == TextureFormat::R16G16B16A16_FLOAT)
        {
            std::vector<uint16_t> rgba16;
            PackRgba32fToRgba16f(rgba, rgba16);
            const size_t rowPitch = static_cast<size_t>(n) * 4u * sizeof(uint16_t);
            UploadTexture3D(device, outTextures.Lut3D, rgba16.data(), n, n, n, rowPitch, 0, "CubeLut3DStaging");
        }
        else
        {
            const size_t rowPitch = static_cast<size_t>(n) * 4u * sizeof(float32);
            UploadTexture3D(device, outTextures.Lut3D, rgba.data(), n, n, n, rowPitch, 0, "CubeLut3DStaging");
        }
    }

    if (parsed.Has1D)
    {
        const uint32_t n = parsed.Size1D;
        const TextureFormat gpuFormat = NormalizeLutTextureFormat(textureFormat);
        std::vector<float32> strip(static_cast<size_t>(n) * 3u * 4u);
        for (uint32_t i = 0; i < n; ++i)
        {
            const float32 r = parsed.Lut1DRgb[static_cast<size_t>(i) * 3u + 0u];
            const float32 g = parsed.Lut1DRgb[static_cast<size_t>(i) * 3u + 1u];
            const float32 b = parsed.Lut1DRgb[static_cast<size_t>(i) * 3u + 2u];
            strip[(static_cast<size_t>(0) * n + i) * 4u + 0u] = r;
            strip[(static_cast<size_t>(0) * n + i) * 4u + 3u] = 1.0f;
            strip[(static_cast<size_t>(1) * n + i) * 4u + 0u] = g;
            strip[(static_cast<size_t>(1) * n + i) * 4u + 3u] = 1.0f;
            strip[(static_cast<size_t>(2) * n + i) * 4u + 0u] = b;
            strip[(static_cast<size_t>(2) * n + i) * 4u + 3u] = 1.0f;
        }

        TextureDesc td1{};
        td1.width = n;
        td1.height = 3u;
        td1.depth = 1;
        td1.mipLevels = 1;
        td1.arrayLayers = 1;
        td1.format = static_cast<uint32_t>(gpuFormat);
        td1.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
                    static_cast<uint32_t>(TextureUsage::TransferDst);
        td1.persistent = true;
        td1.debugName = "CubeLut1DStrip";
        outTextures.Lut1DStrip = device->CreateTexture(td1);
        if (!outTextures.Lut1DStrip.IsValid())
        {
            if (outError)
                *outError = "Failed to create 1D LUT strip texture.";
            cleanupOnFailure();
            return false;
        }
        if (gpuFormat == TextureFormat::R16G16B16A16_FLOAT)
        {
            std::vector<uint16_t> strip16;
            PackRgba32fToRgba16f(strip, strip16);
            const size_t rowPitch = static_cast<size_t>(n) * 4u * sizeof(uint16_t);
            UploadTexture2D(device, outTextures.Lut1DStrip, strip16.data(), n, 3u, rowPitch, "CubeLut1DStaging");
        }
        else
        {
            const size_t rowPitch = static_cast<size_t>(n) * 4u * sizeof(float32);
            UploadTexture2D(device, outTextures.Lut1DStrip, strip.data(), n, 3u, rowPitch, "CubeLut1DStaging");
        }
    }

    return true;
}

CubeLutGpuTextures CreateIdentityCubeLutTextures(IDevice* device)
{
    CubeLutGpuTextures out{};
    if (!device)
        return out;
    constexpr uint16_t kHalfZero = 0x0000u;
    constexpr uint16_t kHalfOne = 0x3C00u;
    constexpr uint32_t n3 = 2u;
    std::vector<uint16_t> rgba3(static_cast<size_t>(n3) * n3 * n3 * 4u);
    for (uint32_t b = 0; b < n3; ++b)
    {
        for (uint32_t g = 0; g < n3; ++g)
        {
            for (uint32_t r = 0; r < n3; ++r)
            {
                const size_t idx = ((static_cast<size_t>(b) * n3 + g) * n3 + r) * 4u;
                rgba3[idx + 0u] = r != 0u ? kHalfOne : kHalfZero;
                rgba3[idx + 1u] = g != 0u ? kHalfOne : kHalfZero;
                rgba3[idx + 2u] = b != 0u ? kHalfOne : kHalfZero;
                rgba3[idx + 3u] = kHalfOne;
            }
        }
    }

    TextureDesc td3{};
    td3.width = n3;
    td3.height = n3;
    td3.depth = n3;
    td3.mipLevels = 1;
    td3.arrayLayers = 1;
    td3.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    td3.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
              | static_cast<uint32_t>(TextureUsage::TransferDst);
    td3.persistent = true;
    td3.debugName = "IdentityLut3D";
    out.Lut3D = device->CreateTexture(td3);
    if (out.Lut3D.IsValid())
    {
        const size_t rowPitch3 = static_cast<size_t>(n3) * 4u * sizeof(uint16_t);
        UploadTexture3D(device, out.Lut3D, rgba3.data(), n3, n3, n3, rowPitch3, 0, "IdentityLut3DStaging");
    }

    constexpr uint32_t n1 = 2u;
    std::vector<uint16_t> rgba1(static_cast<size_t>(n1) * 3u * 4u, kHalfZero);
    for (uint32_t row = 0; row < 3u; ++row)
    {
        rgba1[(static_cast<size_t>(row) * n1 + 0u) * 4u + 3u] = kHalfOne;
        rgba1[(static_cast<size_t>(row) * n1 + 1u) * 4u + 0u] = kHalfOne;
        rgba1[(static_cast<size_t>(row) * n1 + 1u) * 4u + 3u] = kHalfOne;
    }

    TextureDesc td1{};
    td1.width = n1;
    td1.height = 3u;
    td1.depth = 1;
    td1.mipLevels = 1;
    td1.arrayLayers = 1;
    td1.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    td1.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
              | static_cast<uint32_t>(TextureUsage::TransferDst);
    td1.persistent = true;
    td1.debugName = "IdentityLut1DStrip";
    out.Lut1DStrip = device->CreateTexture(td1);
    if (out.Lut1DStrip.IsValid())
    {
        const size_t rowPitch1 = static_cast<size_t>(n1) * 4u * sizeof(uint16_t);
        UploadTexture2D(device, out.Lut1DStrip, rgba1.data(), n1, 3u, rowPitch1, "IdentityLut1DStaging");
    }
    return out;
}

CubeLutGpuBindingState IdentityCubeLutBinding(const CubeLutGpuTextures& identity)
{
    CubeLutGpuBindingState out{};
    out.Lut3D = identity.Lut3D;
    out.Lut1DStrip = identity.Lut1DStrip;
    return out;
}

} // namespace GameEngine::Rendering
