#include "Assets/CubeLutColorSettings.h"

#include "Assets/AssetRegistry.h"
#include "Assets/RuntimeAssetMetadata.h"

#include <string>

namespace GameEngine
{

CubeLutColorSettings ResolveCubeLutColorSettings(const AssetRegistry& registry, const std::filesystem::path& assetPath,
                                                 const Rendering::CubeLutColorProposal& proposal)
{
    CubeLutColorSettings settings{};
    settings.InputEncoding = proposal.InputEncoding;

    std::string value;
    if (registry.TryGetMetaValue(assetPath, kCubeLutInputEncodingMetaKey, value))
    {
        if (const auto chosen = Rendering::ParseCubeLutInputEncodingKey(value))
        {
            settings.InputEncoding = *chosen;
            settings.InputEncodingChosen = true;
        }
    }
    return settings;
}

} // namespace GameEngine
