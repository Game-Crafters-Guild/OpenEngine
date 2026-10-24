#pragma once

#include "Rendering/RenderLayerMask.h"
#include "Types/Types.h"

namespace GameEngine {
namespace Components {

enum class ReflectionProbeUpdateMode : uint32 {
    Once = 0,
    Realtime = 1, // @ge-label Realtime (on change)
};

struct ReflectionProbe {
    // Local influence volume. Like PostProcessVolume, the entity transform owns
    // the probe range: a unit box/sphere is scaled and oriented by WorldTransform.
    // @ge-tooltip Fade width near the influence edge. Visible when the camera is near the probe boundary.
    float32 BlendDistance = 2.0f;
    // @ge-tooltip Tie-breaker when more than one probe influences the camera.
    uint32 Priority = 0;

    // Godot-style local probe box. When disabled, the same transform range is
    // treated as a sphere/ellipsoid influence without parallax box projection.
    // @ge-tooltip Use the transform-scaled local box for probe selection and parallax-corrected specular reflections. When disabled, the transform range is treated as an ellipsoid.
    bool BoxProjection = true;
    // @ge-tooltip Offset of the capture and projection origin along the probe's local X axis.
    float32 OriginOffsetX = 0.0f;
    // @ge-tooltip Offset of the capture and projection origin along the probe's local Y axis.
    float32 OriginOffsetY = 0.0f;
    // @ge-tooltip Offset of the capture and projection origin along the probe's local Z axis.
    float32 OriginOffsetZ = 0.0f;

    // Dynamic multiplier applied through EnvData, so changing it does not force a
    // cubemap rebake.
    // @ge-tooltip Runtime IBL multiplier. Updates immediately without rebaking the cubemap.
    float32 Intensity = 1.0f;
    // @ge-tooltip Exposure applied when baking the probe capture.
    float32 ExposureEV = 0.0f;
    // @ge-tooltip Yaw rotation applied to sky/HDRI probe lighting during bake.
    float32 RotationDegrees = 0.0f;
    // @ge-tooltip Darkens lighting sampled from below the horizon. 0 keeps it bright; 1 makes it black.
    float32 IblLowerHemisphereDarkness = 0.3f;

    // When true the six-face capture also gathers environment reflections
    // (the sky/HDRI and any lower-priority probe). Default false: the capture
    // sees direct light, emission and the diffuse GI field only. A probe that
    // sampled its OWN environment cube while baking would feed its output back
    // into its next bake; in a closed, high-albedo room with no dominant fixed
    // sky that loop diverges to Inf and the whole view tonemaps to black, so
    // the safe default breaks the recursion. Turn it on only for an open scene
    // where the environment is dominated by a fixed sky.
    bool CaptureEnvironment = false;

    // Scene capture behavior. Probes use the active sky/HDRI as background
    // when present, then capture nearby scene content.
    // @ge-tooltip Cubemap face resolution for the scene capture. Higher values sharpen mirror-like reflections but make probe bakes more expensive.
    uint32 CaptureResolution = 256;
    // @ge-tooltip When the probe re-bakes. Once: only when the probe's own settings, the sky or the geometry in its world are added or removed. Realtime (on change): also whenever its world changes: lights, the sky, moved or edited renderables, material edits, and animated or depth-writing content such as skinned characters, terrain, an animated ocean or grass. Shader-time material animation, playing video textures and particles do not re-bake it; set the interval to 0 to capture them every frame.
    ReflectionProbeUpdateMode UpdateMode = ReflectionProbeUpdateMode::Realtime;
    // Shortest time between realtime captures while the probe's world
    // changes (SceneProbeWorldEpochs: lights, renderables, dynamic depth,
    // material edits; not shader-time animation, video textures or
    // particles); a world at rest is not recaptured. 0 captures every frame:
    // a full six-face bake per frame, no time slicing and no rebake throttle.
    // @ge-tooltip Realtime (on change): the fastest the probe may re-bake while the world changes, in seconds; 0 = every frame.
    float32 RealtimeUpdateInterval = 0.5f; // @ge-range 0
    // @ge-tooltip Far clip distance for scene capture. 0 uses the probe volume.
    float32 MaxDistance = 0.0f;
    // @ge-tooltip The render layers the probe's capture draws.
    Rendering::RenderLayerMask CapturedLayers = Rendering::RenderLayerMask::All();
};

} // namespace Components
} // namespace GameEngine
