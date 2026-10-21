#pragma once

#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECS.h"

#include <span>

namespace GameEngine
{
namespace Editor
{
class EditorChangeNotifications;
class UndoRedoService;
} // namespace Editor

/// One of the night stars' looks: values for the star dials on the Sky Environment and the night tint
/// on every global Post Process Volume. A look never touches the sky's exposure trim or any other sky
/// setting.
struct StarLookPreset
{
    const char* Name;
    const char* Tooltip;
    float LimitingMagnitude;
    float TwinkleAmount;
    float Compression;
    float Glow;
    float TwinkleRate;
    float CoreScale;
    float Spikes;
    float SpikeLength;
    Components::StarShapeKind Shape;
    Components::StarObserverKind Observer;
    float NightTintStrength;
    float NightTintColor[3];
};

/// The looks in the order the Star look row shows them: Vivid (the component's defaults), Restrained,
/// Physical (every artistic dial at its neutral value, the camera observer) and Classic (the sparkle).
std::span<const StarLookPreset> StarLookPresets();

/// Sets the look on the sky and on every global Post Process Volume as one undo step named after the
/// look. Without a global volume (HasGlobalPostProcessVolume) the night tint is not applied.
void ApplyStarLookPreset(const StarLookPreset& preset, ECS::World& world, ECS::EntityHandle sky,
                        Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo);

/// Whether the sky's star dials are the look's (the night tint lives on the volumes and is not
/// compared, so a hand-tuned tint keeps the look selected).
bool MatchesStarLook(const Components::SkyEnvironment& sky, const StarLookPreset& preset);

/// Whether the world has a global Post Process Volume for the looks' night tint.
bool HasGlobalPostProcessVolume(ECS::World& world);

} // namespace GameEngine
