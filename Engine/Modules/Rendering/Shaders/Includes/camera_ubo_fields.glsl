// CameraUBO std140 field list — the single GLSL source for the per-view camera
// block bound at set 0, binding 5 (352 B). Included INSIDE the block body so
// each consumer keeps its own instance name:
//
//     layout(set = 0, binding = 5) uniform CameraUBO {
//     #include "Includes/camera_ubo_fields.glsl"   // from Shaders/*.vert
//     } Cam;
//
// (Adapters in Shaders/Adapters/ include it as "../Includes/camera_ubo_fields.glsl".)
//
// The device-diagnostic fixtures ubo_nudge.vert / ubo_sample.frag deliberately
// keep a standalone copy — they are isolated UBO-binding smoke tests, not part
// of the render path this block feeds.
    mat4 uV;
    mat4 uP;
    mat4 uVP;
    vec4 uCameraPos; // xyz = world-space camera position (full world — fragment/lighting/culling stay world-space)
    // View and view-proj relative to the render origin, the eye's own sector, built on the
    // CPU from the double eye. The vertex stage projects origin-relative positions through
    // them; the fragment stage reads the full-world fields above. C++ mirror: Rendering::CameraData.
    mat4 uVRel;
    mat4 uVPRel;
    ivec4 uRenderOriginSector; // xyz = render origin sector; w = sector size (meters)
