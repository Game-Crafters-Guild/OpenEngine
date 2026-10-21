#include "Sky/StarLookPresets.h"

#include "Components/Rendering/PostProcessVolume.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

using Components::StarObserverKind;
using Components::StarShapeKind;

// The night tint's navy, as a picker color (only its hue is used).
constexpr float kNavy[3] = {0.155f, 0.249f, 1.0f};

// Vivid is the component's defaults: SkyEnvironment and PostProcessVolume keep the same values
// (StarLookPresetTests pins them).
constexpr std::array<StarLookPreset, 4> kPresets = {{
    {"Vivid",
     "The default, artistic: a dark site's star count, the brightest stars glowing with four thin "
     "diffraction spikes two to three glow radii long, a tighter core, faint stars fading to white, and the full navy night tint.",
     7.25f, 0.3f, 0.775f, 0.9f, 1.0f, 0.85f, 0.2f, 0.6f, StarShapeKind::Point, StarObserverKind::Eye, 1.0f,
     {kNavy[0], kNavy[1], kNavy[2]}},
    {"Restrained",
     "Artistic: Vivid without the spikes, with the physical core, a softer glow and half the night tint.",
     7.25f, 0.3f, 0.775f, 0.8f, 1.0f, 1.0f, 0.0f, 2.0f, StarShapeKind::Point, StarObserverKind::Eye, 0.5f,
     {kNavy[0], kNavy[1], kNavy[2]}},
    {"Physical",
     "Physical: the sky as a camera records it, with no glare and the catalog color at every magnitude; the "
     "naked eye's star count, real star brightness, the fastest twinkle, no glow, no spikes, no "
     "tint.",
     6.5f, 0.3f, 1.0f, 0.0f, Components::kStarTwinkleRateMax, 1.0f, 0.0f, 2.0f, StarShapeKind::Point,
     StarObserverKind::Camera, 0.0f, {kNavy[0], kNavy[1], kNavy[2]}},
    {"Classic",
     "Artistic: every star a four-pointed sparkle with a soft glow, and half the night tint.",
     7.25f, 0.3f, 0.775f, 0.8f, 1.0f, 1.0f, 0.0f, 2.0f, StarShapeKind::Sparkle, StarObserverKind::Eye, 0.5f,
     {kNavy[0], kNavy[1], kNavy[2]}},
}};

void ApplyStarDials(Components::SkyEnvironment& sky, const StarLookPreset& preset)
{
    sky.StarLimitingMagnitude = preset.LimitingMagnitude;
    sky.StarTwinkleAmount = preset.TwinkleAmount;
    sky.StarHighlightCompression = preset.Compression;
    sky.StarGlow = preset.Glow;
    sky.StarTwinkleRate = preset.TwinkleRate;
    sky.StarCoreScale = preset.CoreScale;
    sky.StarSpikes = preset.Spikes;
    sky.StarSpikeLength = preset.SpikeLength;
    sky.StarShape = preset.Shape;
    sky.StarObserver = preset.Observer;
}

void ApplyNightTint(Components::PostProcessVolume& volume, const StarLookPreset& preset)
{
    volume.NightTintStrength = preset.NightTintStrength;
    volume.NightTintColor[0] = preset.NightTintColor[0];
    volume.NightTintColor[1] = preset.NightTintColor[1];
    volume.NightTintColor[2] = preset.NightTintColor[2];
}

std::vector<ECS::EntityHandle> GlobalVolumes(ECS::World& world)
{
    std::vector<ECS::EntityHandle> volumes;
    world.Query<ECS::Read<Components::PostProcessVolume>>().Each(
        [&volumes](ECS::EntityHandle entity, const Components::PostProcessVolume& volume)
        {
            if (volume.IsGlobal)
                volumes.push_back(entity);
        });
    return volumes;
}

} // namespace

std::span<const StarLookPreset> StarLookPresets()
{
    return kPresets;
}

void ApplyStarLookPreset(const StarLookPreset& preset, ECS::World& world, ECS::EntityHandle sky,
                        Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo)
{
    const std::string label = std::string("Star look: ") + preset.Name;
    if (undo)
        undo->BeginCompound(label);
    InspectorDrag::CommitComponentWithUndo<Components::SkyEnvironment>(
        &world, sky, notifications, undo, label, [&preset](Components::SkyEnvironment& u) { ApplyStarDials(u, preset); });
    const std::vector<ECS::EntityHandle> volumes = GlobalVolumes(world);
    for (const ECS::EntityHandle volume : volumes)
        InspectorDrag::CommitComponentWithUndo<Components::PostProcessVolume>(
            &world, volume, notifications, undo, label,
            [&preset](Components::PostProcessVolume& u) { ApplyNightTint(u, preset); });
    if (undo)
        undo->EndCompound();
}

bool MatchesStarLook(const Components::SkyEnvironment& sky, const StarLookPreset& preset)
{
    constexpr float kTolerance = 1e-3f;
    const auto near = [](float a, float b) { return std::abs(a - b) < kTolerance; };
    return near(sky.StarLimitingMagnitude, preset.LimitingMagnitude) && near(sky.StarTwinkleAmount, preset.TwinkleAmount) &&
           near(sky.StarHighlightCompression, preset.Compression) && near(sky.StarGlow, preset.Glow) &&
           near(sky.StarTwinkleRate, preset.TwinkleRate) && near(sky.StarCoreScale, preset.CoreScale) &&
           near(sky.StarSpikes, preset.Spikes) && near(sky.StarSpikeLength, preset.SpikeLength) &&
           sky.StarShape == preset.Shape && sky.StarObserver == preset.Observer;
}

bool HasGlobalPostProcessVolume(ECS::World& world)
{
    return !GlobalVolumes(world).empty();
}

} // namespace GameEngine
