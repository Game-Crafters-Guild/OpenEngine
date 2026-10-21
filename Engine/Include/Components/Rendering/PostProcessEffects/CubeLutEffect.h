#pragma once

#include "Components/AssetRef.h"
#include "Types/Types.h"

#include <cstdint>
#include <string_view>

namespace GameEngine {
namespace Components {

/// Where in the image chain a volume's Cube LUT runs. Stored in a scene by name
/// (`CubeLutEffect.stage = Grade`), never by number.
enum class CubeLutStage : uint32
{
    /// After the look: the LUT grades the look's display-referred output inside the Tonemap pass,
    /// before the HDR highlight extension and every display-referred effect. The default.
    Finish = 0,
    /// Before the look: the LUT is the grade's last stage (the HDRColorFx pass), on the exposed
    /// scene-linear image; the look then renders its output.
    Grade = 1,
};

inline constexpr uint32 kCubeLutStageCount = 2;

/// The one name table for the stages: the scene format and the loader's message read it.
inline constexpr std::string_view kCubeLutStageNames[kCubeLutStageCount] = {"Finish", "Grade"};

constexpr std::string_view CubeLutStageName(CubeLutStage stage)
{
    return kCubeLutStageNames[stage == CubeLutStage::Grade ? 1 : 0];
}

// Resolve-style .cube color LUT (1D, 3D, or 1D shaper + 3D). Attach to the same entity as
// PostProcessVolume. The LUT asset declares the encoding its input is authored in (the asset's
// inspector); the effect chooses the stage it runs at.
struct CubeLutEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    // 0 = bypass, 1 = full LUT blend.
    float32 Intensity{1.0f};
    // Where the LUT runs: after the look (Finish) or before it (Grade).
    CubeLutStage Stage{CubeLutStage::Finish};
    // The .cube color LUT asset.
    AssetRef<AssetType::CubeLut> LutAssetGuid{};
};

} // namespace Components
} // namespace GameEngine
