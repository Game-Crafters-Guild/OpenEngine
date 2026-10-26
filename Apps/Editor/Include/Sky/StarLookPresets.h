#pragma once

#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECS.h"
#include "Types/Color.h"

#include <span>

namespace GameEngine
{
namespace Editor
{
class EditorChangeNotifications;
class UndoRedoService;
} // namespace Editor

/// One of the night stars' looks: values for the star dials on the Sky Environment and the night tint
/// on every global Post Process Volume. A look never touches who sees the stars (the observer), the
/// sky's exposure trim or any other sky setting.
struct StarLookPreset
{
    const char* Name;
    /// One line under the name in the Preset list: what the look does.
    const char* Description;
    /// The CSS class of the look's picture in the Preset list (theme/option-pictures.css).
    const char* PictureClass;
    float LimitingMagnitude;
    float TwinkleAmount;
    float Compression;
    float Glow;
    float TwinkleRate;
    float CoreScale;
    float Spikes;
    float SpikeLength;
    Components::StarShapeKind Shape;
    float NightTintStrength;
    /// The night tint's hue, linear Rec.709 (alpha unused).
    ColorLinear NightTintColor;
};

/// The looks in the order the Preset list shows them: Vivid (the component's defaults), Restrained,
/// Classic (the sparkle) and Physical (every artistic dial at its neutral value).
std::span<const StarLookPreset> StarLookPresets();

/// Sets the look on the sky and on every global Post Process Volume as one undo step named after the
/// look. Without a global volume (HasGlobalPostProcessVolume) the night tint is not applied.
void ApplyStarLookPreset(const StarLookPreset& preset, ECS::World& world, ECS::EntityHandle sky,
                        Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo);

/// Whether the sky's star dials are the look's. The observer is not part of a look, so it is not
/// compared; the night tint lives on the volumes (FindStarLook compares it).
bool MatchesStarLook(const Components::SkyEnvironment& sky, const StarLookPreset& preset);

/// The look the sky's star dials and every global volume's night tint match, or null when they match
/// none (the Preset row reads Custom), so a hand-tuned tint reads Custom and picking the look resets
/// it. Without a global volume only the dials are compared.
const StarLookPreset* FindStarLook(const Components::SkyEnvironment& sky, ECS::World& world);

/// Sets who sees the stars (Seen by: the eye or a camera) as one undo step, and nothing else.
void ApplyStarObserver(Components::StarObserverKind observer, ECS::World& world, ECS::EntityHandle sky,
                       Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo);

/// Whether the world has a global Post Process Volume for the looks' night tint.
bool HasGlobalPostProcessVolume(ECS::World& world);

} // namespace GameEngine
