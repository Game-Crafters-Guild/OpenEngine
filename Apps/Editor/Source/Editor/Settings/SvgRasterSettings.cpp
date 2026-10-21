#include "Editor/Settings/SvgRasterSettings.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/SvgRasterizer.h"
#include "Core/Engine.h"
#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace GameEngine::Editor
{
namespace
{
constexpr int kMinRasterExp = 4;
constexpr int kMaxRasterExp = 14;
constexpr int kDefaultRasterExp = 8;
constexpr const char* kTextureSvgRasterExpPref = "import.svgTextureRasterExp";

int PixelsToExponent(uint32_t pixels)
{
    pixels = std::clamp(pixels, kMinSvgRasterSize, kMaxSvgRasterSize);
    int exponent = kMinRasterExp;
    while (exponent < kMaxRasterExp && (1u << exponent) < pixels)
        ++exponent;
    return exponent;
}

// Reloads the loaded SVG textures of every project source. Editor-source SVGs
// are drawn at their on-screen size and do not follow this default.
void ReloadProjectSvgAssets()
{
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    auto& registry = assets.GetRegistry();
    for (const AssetSourceDesc& source : assets.GetRegisteredSources())
    {
        if (source.Alias == kAssetSourceAliasEditor)
            continue;

        for (const GUID& guid : registry.GetAssetsForSource(source.Alias))
        {
            AssetMetadata metadata{};
            if (!registry.TryGetAssetMetadata(guid, metadata))
                continue;
            std::string extension = metadata.Path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".svg")
                assets.RequestAsyncReload(guid);
        }
    }
}
} // namespace

uint32_t SvgRasterSettings::Load(const std::filesystem::path& workspaceRoot, std::string* outError)
{
    double storedExponent = static_cast<double>(kDefaultRasterExp);
    std::string error;
    SettingsStore project = OpenProjectSettings(workspaceRoot);
    project.Load(&error);
    project.TryGetDouble(kTextureSvgRasterExpPref, storedExponent);

    if (outError)
        *outError = error;
    const int exponent = std::clamp(static_cast<int>(std::round(storedExponent)),
                                    kMinRasterExp, kMaxRasterExp);
    return 1u << exponent;
}

bool SvgRasterSettings::SaveAndApply(const std::filesystem::path& workspaceRoot,
                                     uint32_t pixels,
                                     bool reloadAssets,
                                     std::string* outError)
{
    const int exponent = PixelsToExponent(pixels);
    const uint32_t normalizedPixels = 1u << exponent;
    SettingsStore settings = OpenProjectSettings(workspaceRoot);
    std::string error;
    settings.Load(&error);
    settings.SetDouble(kTextureSvgRasterExpPref, static_cast<double>(exponent));
    const bool saved = settings.Save(&error);
    if (!saved)
    {
        if (outError)
            *outError = error;
        return false;
    }

    SetSvgTextureRasterizerDefaultSize(static_cast<float32>(normalizedPixels));
    if (reloadAssets)
        ReloadProjectSvgAssets();
    if (outError)
        outError->clear();
    return true;
}

void SvgRasterSettings::ApplySavedDefault(const std::filesystem::path& workspaceRoot)
{
    std::string error;
    SetSvgTextureRasterizerDefaultSize(static_cast<float32>(Load(workspaceRoot, &error)));
    if (!error.empty())
        Logger::Log::Warning("SvgRasterSettings: failed to load project setting: {}", error);
}

} // namespace GameEngine::Editor
