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

using Components::StarShapeKind;

// The night tint's navy, as a picker color (only its hue is used).
const ColorLinear kNavy(0.155f, 0.249f, 1.0f);

// How far a value may sit from a look's and still read as the look: a value typed or dragged to the
// look's number is the look.
constexpr float kMatchTolerance = 1e-3f;

// Vivid is the component's defaults: SkyEnvironment and PostProcessVolume keep the same values
// (StarLookPresetTests pins them).
const std::array<StarLookPreset, 4> kPresets = {{
    {"Vivid", "The default. Bright stars glow, with four thin spikes and the full night tint.", "option-picture--star-look-vivid", 7.25f,
     0.3f, 0.775f, 0.9f, 1.0f, 0.75f, 0.2f, 0.6f, StarShapeKind::Point, 1.0f, kNavy},
    {"Restrained", "A soft round glow around the bright stars, no spikes, half the night tint.", "option-picture--star-look-restrained", 7.25f,
     0.3f, 0.775f, 0.8f, 1.0f, 1.0f, 0.0f, 2.0f, StarShapeKind::Point, 0.5f, kNavy},
    {"Classic", "Every star a four-pointed sparkle, a soft glow, half the night tint.", "option-picture--star-look-classic", 7.25f,
     0.3f, 0.775f, 0.8f, 1.0f, 1.0f, 0.0f, 2.0f, StarShapeKind::Sparkle, 0.5f, kNavy},
    {"Physical", "No glow, no spikes, no tint, fewer faint stars: real star brightness.", "option-picture--star-look-physical", 6.5f,
     0.3f, 1.0f, 0.0f, Components::kStarTwinkleRateMax, 1.0f, 0.0f, 2.0f, StarShapeKind::Point, 0.0f, kNavy},
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
}

void ApplyNightTint(Components::PostProcessVolume& volume, const StarLookPreset& preset)
{
    volume.NightTintStrength = preset.NightTintStrength;
    volume.NightTintColor[0] = preset.NightTintColor.r;
    volume.NightTintColor[1] = preset.NightTintColor.g;
    volume.NightTintColor[2] = preset.NightTintColor.b;
}

bool Near(float a, float b)
{
    return std::abs(a - b) < kMatchTolerance;
}

bool MatchesNightTint(const Components::PostProcessVolume& volume, const StarLookPreset& preset)
{
    return Near(volume.NightTintStrength, preset.NightTintStrength) &&
           Near(volume.NightTintColor[0], preset.NightTintColor.r) &&
           Near(volume.NightTintColor[1], preset.NightTintColor.g) &&
           Near(volume.NightTintColor[2], preset.NightTintColor.b);
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
    return Near(sky.StarLimitingMagnitude, preset.LimitingMagnitude) && Near(sky.StarTwinkleAmount, preset.TwinkleAmount) &&
           Near(sky.StarHighlightCompression, preset.Compression) && Near(sky.StarGlow, preset.Glow) &&
           Near(sky.StarTwinkleRate, preset.TwinkleRate) && Near(sky.StarCoreScale, preset.CoreScale) &&
           Near(sky.StarSpikes, preset.Spikes) && Near(sky.StarSpikeLength, preset.SpikeLength) &&
           sky.StarShape == preset.Shape;
}

const StarLookPreset* FindStarLook(const Components::SkyEnvironment& sky, ECS::World& world)
{
    const std::vector<ECS::EntityHandle> volumes = GlobalVolumes(world);
    for (const StarLookPreset& preset : kPresets)
    {
        if (!MatchesStarLook(sky, preset))
            continue;
        bool tintMatches = true;
        for (const ECS::EntityHandle volume : volumes)
        {
            const auto* values = world.GetComponent<Components::PostProcessVolume>(volume);
            tintMatches = tintMatches && values && MatchesNightTint(*values, preset);
        }
        if (tintMatches)
            return &preset;
    }
    return nullptr;
}

void ApplyStarObserver(Components::StarObserverKind observer, ECS::World& world, ECS::EntityHandle sky,
                       Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo)
{
    const char* label = observer == Components::StarObserverKind::Camera ? "Stars seen by: Camera" : "Stars seen by: Eye";
    InspectorDrag::CommitComponentWithUndo<Components::SkyEnvironment>(
        &world, sky, notifications, undo, label, [observer](Components::SkyEnvironment& u) { u.StarObserver = observer; });
}

bool HasGlobalPostProcessVolume(ECS::World& world)
{
    return !GlobalVolumes(world).empty();
}

} // namespace GameEngine
