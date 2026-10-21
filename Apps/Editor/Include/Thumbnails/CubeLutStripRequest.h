#pragma once

#include "AssetCore/GUID.h"
#include "Rendering/Passes/CubeLutPreviewSources.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace GameEngine
{

/// What a Cube LUT preview texture shows of its source (the exposure ramp or the saturated probes).
enum class CubeLutStripContent : uint8_t
{
    LookAlone, ///< the source through the look: the band to compare against
    WithLut,   ///< the source through the look and the LUT at its stage and intensity
    Sheet,     ///< the asset thumbnail: the ramp and the probes, each the look alone over the look
               ///< with the LUT, the ramp pair above the probe pair with a black gap between
};

/// A Cube LUT preview texture: a scene-linear source through the real passes (Tonemap, and the
/// HDRColorFx pass first at the Grade stage). The request is its own name (MakeCubeLutStripKey):
/// two requests that differ in nothing are one texture, and whoever holds the name can have it
/// rendered.
struct CubeLutStripRequest
{
    GUID Lut{};
    /// The field showing it: 0 for an asset thumbnail (its name never changes), otherwise an
    /// inspector's entity id + 1. A new name for the same owner, content, source and size replaces
    /// the old one.
    uint32_t Owner = 0;
    float Intensity = 1.0f; ///< [0, 1]; named in thousandths
    int32_t Look = 0;       ///< Components::Look
    uint32_t Stage = 0;     ///< Components::CubeLutStage: 0 = Finish (after the look), 1 = Grade
    Rendering::Passes::CubeLutPreviewSource Source = Rendering::Passes::CubeLutPreviewSource::Ramp;
    uint32_t Width = 128;
    uint32_t Height = 12;
    CubeLutStripContent Content = CubeLutStripContent::WithLut;
};

/// The UI texture key of a request ("cubelut-strip-..."; the thumbnail service's engine names
/// prefix it with "engine:"). Empty for a request with no LUT.
std::string MakeCubeLutStripKey(const CubeLutStripRequest& request);

/// The request a key names, or nothing for a key MakeCubeLutStripKey did not make. Accepts the key
/// with or without the "engine:" prefix.
std::optional<CubeLutStripRequest> ParseCubeLutStripKey(std::string_view key);

} // namespace GameEngine
