#pragma once

#include "Types/Types.h"
#include "Rendering/RenderLayerMask.h"
#include "Components/Rendering/Look.h"

namespace GameEngine {
namespace Components {

enum class PostProcessVolumeShape : int32 {
    Box      = 0,
    Sphere   = 1,
    Capsule  = 2,
    Cylinder = 3,
};

// How the scene exposure is driven. Fixed = the legacy linear multiplier (Exposure). Manual =
// an absolute photographic EV100 (ManualExposureEV) anchored to 203-nit reference white. Physical =
// the EV100 is computed from camera Aperture/ShutterTime/Iso (sunny-16 = f/16, 1/100 s, ISO 100).
// Auto = metered (ExposureMeter) with eye adaptation (AutoExposureNode).
enum class ExposureMode : int32 {
    Fixed    = 0,
    Manual   = 1,
    Physical = 2,
    Auto     = 3,
};

// What Auto exposure meters. The Min/Max EV clamps, the adaptation speeds and the compensation apply
// to both meters alike (one resolve, auto_exposure_resolve.comp).
enum class ExposureMeter : int32 {
    // The frame's pixels: geometry and sky log-luminance histograms, the sky a ceiling.
    Picture = 0,
    // The light falling on a horizontal surface from the shaded directional lights and the sky
    // (Engine::Renderer::IncidentLightEv100); never the pixels.
    Lights  = 1,
};

// The EV100 a manual exposure falls back to when nothing has authored one.
// Chosen so an authored base colour reads back close to what the colour picker showed: swept
// against pure R/G/B under the default sky and scored as CIE76 dE against the authored sRGB, the
// error bottoms at 15.4. Tied to the default Khronos PBR Neutral tonemap — under an identity curve
// the optimum is 15.0.
inline constexpr float32 kDefaultManualExposureEv = 15.4f;

// The darkest scene EV100 Auto exposure fully exposes by default (the max-gain clamp), for a game camera
// and the Scene View alike. Low enough that a moonlit night meters as read and a moonless night (the
// Lights meter reads about -12) is held a few stops above its reading; the exposure compensation curve
// (DefaultExposureCompensationCurve) keeps either one dark.
inline constexpr float32 kDefaultAutoExposureMinEv = -7.0f;

struct PostProcessVolume {
    float32 Weight {1.0f};
    int32 Priority {0};

    // The cameras this volume affects: those whose Components::Camera::VolumeLayers
    // intersect these. Default: every camera.
    Rendering::RenderLayerMask VolumeLayers = Rendering::RenderLayerMask::All();

    // Spatial extent. When IsGlobal is true the volume affects the whole world
    // regardless of camera position. When false, the volume is a unit shape
    // (cube or sphere of diameter 1) transformed by the entity's
    // WorldTransform — the transform's scale defines the dimensions and
    // rotation orients it. Non-uniform scale on a Sphere shape produces an
    // oriented ellipsoid. BlendDistance is the soft fade region around the
    // shape, in world units.
    bool IsGlobal {true};
    PostProcessVolumeShape Shape {PostProcessVolumeShape::Box};
    float32 BlendDistance {1.0f};

    // Exposure is NOT a volume property. It lives on Components::Camera (the sensor) and, for views
    // without a camera (the editor Scene View), on the world default. The ExposureMode enum above is
    // shared by Camera.h / Exposure.h. A volume entity may carry an ExposureAdjustmentEffect —
    // composable modifiers (± stop compensation, narrow-only adaptation clamps) that stack on the
    // camera's result, never replace it.

    // The display transform. Dominant across volumes: a local volume's look applies wherever it
    // dominates and never crossfades.
    Components::Look Look {Components::Look::Neutral};

    // Artistic: the night tint, the film convention of a blue night. A hue given to the dark field of a
    // night image on the display, after the look, so the field shows exactly this hue. It follows the
    // exposure's adaptation: in full when the exposure adapts below 0.005 cd/m2, gone above 3 cd/m2, so
    // a day image is untouched. It spares what is bright on screen (above 0.08 display luminance: the
    // moon, lit windows) and the stars with their glow: a pixel 1.1 times the darkest of four neighbours
    // a star glow away (about 3 % of the image height) is partly spared, 1.5 times fully
    // (Includes/night_tint.glsl). The default strength 1 is the Vivid look's, an artistic choice.
    // NightTintStrength: 0 is physical, 1 gives the dark field exactly the color's hue.
    // NightTintColor: the hue the dark field takes, linear Rec.709, each channel 0 to 1 (what a color
    // picker shows); its brightness is not used, the renderer keeps the image's luminance.
    float32 NightTintStrength {1.0f};
    float32 NightTintColor[3] {0.155f, 0.249f, 1.0f};

};

} // namespace Components
} // namespace GameEngine
