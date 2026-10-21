#include "Engine/Rendering/TextureMipUpload.h"

#include "Assets/TextureCook.h"
#include "Logger/Logger.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include <string>
#include <vector>

namespace GameEngine
{

Rendering::TextureHandle UploadRgba8WithMips(Rendering::IDevice* device,
                                             const uint8_t* rgba, uint32_t width, uint32_t height,
                                             Rendering::TextureFormat gpuFormat,
                                             const TextureCookSettings& cookSettings,
                                             bool srgb, const char* debugName,
                                             const char* stagingName, uint32_t& outLevels)
{
    std::vector<TextureCookMipLevel> mipChain;
    {
        TextureCookMipLevel level0;
        level0.Width = width;
        level0.Height = height;
        level0.Bytes.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
        mipChain.push_back(std::move(level0));
    }
    std::string mipError;
    if (!BuildTextureCookMipChain(mipChain, ComputeTextureCookMipCount(width, height, cookSettings),
                                  /*isFloat*/ false, srgb, cookSettings, &mipError))
    {
        if (cookSettings.PreserveAlphaCoverage)
        {
            Logger::Log::Error("Texture upload '{}': alpha coverage mip build failed: {}", debugName, mipError);
            return {};
        }
        mipChain.resize(1);
    }
    outLevels = static_cast<uint32_t>(mipChain.size());

    Rendering::TextureDesc td{};
    td.width = width;
    td.height = height;
    td.mipLevels = outLevels;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(gpuFormat);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource)
             | static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = debugName;
    Rendering::TextureHandle tex = device->CreateTexture(td);
    if (!tex.IsValid())
        return {};
    std::vector<Rendering::TextureMipUploadEntry> mipEntries(mipChain.size());
    for (size_t level = 0; level < mipChain.size(); ++level)
        mipEntries[level] = {mipChain[level].Bytes.data(), mipChain[level].Bytes.size(),
                             mipChain[level].Width, mipChain[level].Height};
    Rendering::UploadTextureMips(device, tex, mipEntries.data(),
                                 static_cast<uint32_t>(mipEntries.size()), gpuFormat, stagingName);
    return tex;
}

} // namespace GameEngine
