#pragma once

#include "AssetCore/AssetTypes.h"
#include "Components/AssetRef.h"
#include "Types/Color.h"

namespace GameEngine {
namespace Components {

// Attaches a lens flare to an entity (typically a light or the sun). Positional
// flares use the entity's world position; SunMode projects a directional light
// as an infinite source. The referenced LensFlareDefinition supplies the
// elements, atlas, fades, and dynamic boost.
struct LensFlareSource {
    AssetRef<AssetType::LensFlareDefinition> Flare;

    // When attached to (or placed directly on) a directional light, project
    // the flare from the light direction instead of its world position.
    // @ge-tooltip For a directional light, project the flare from the light direction as an infinite sun source. Positional distance and source-angle falloff are ignored.
    bool SunMode = false;

    // Per-instance multipliers on top of the definition's globals.
    // @ge-tooltip Brightness multiplier on top of the flare definition's global brightness.
    // @ge-range 0 10
    float Intensity = 1.0f;
    // @ge-tooltip Size multiplier on top of the flare definition's global scale.
    // @ge-range 0 10
    float Scale     = 1.0f;

    // @ge-tooltip Per-source tint multiplied into every flare element's color.
    ColorLinear Tint{1.0f, 1.0f, 1.0f, 1.0f};

    // @ge-tooltip Fade the flare when scene geometry covers the source (depth probe).
    bool Occlude = true;

    // @ge-tooltip When above zero, replaces the definition's max distance for this source (distance fade, size falloff, and cull range).
    // @ge-range 0 1000
    float MaxDistanceOverride = 0.0f;
};

} // namespace Components
} // namespace GameEngine
