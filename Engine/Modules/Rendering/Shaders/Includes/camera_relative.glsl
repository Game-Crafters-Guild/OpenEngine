// Camera-relative rendering: shared vertex helpers for the standard mesh path
// (adapter_vertex.glsl) and the depth-only / shadow path (shadow_depth_shared*.vert).
// Needs the CameraUBO instance `Cam` (camera_ubo_fields.glsl) in scope.
//
// The render origin is the eye's integer sector. Every view projects a position
// relative to it through the rebased view-proj (Cam.uVRel / Cam.uVPRel), so the
// clip position stays fp32-precise at any distance. At sector (0,0,0) the rebased
// matrices equal the full-world ones.

#ifndef GE_CAMERA_RELATIVE_GLSL
#define GE_CAMERA_RELATIVE_GLSL

#include "instance_sector.glsl"

// World-space position of the render origin (originSector * sector size). Passes that
// own their world position (terrain, procedural, ocean) subtract it to feed shadow_sampling
// a render-origin-relative receiver position that matches the rebased ge_shadowVP.
vec3 GE_RenderOriginWorld()
{
    return vec3(Cam.uRenderOriginSector.xyz) * GE_SECTOR_SIZE;
}

// Project a render-origin-relative position through the rebased view / view-proj.
// The planet branch (uCameraPos.w < 0) builds a reverse-Z depth from view-space
// distance for the auto-extended far plane.
vec4 GE_ProjectDepthPositionRel(vec3 relWorldPos)
{
    if (Cam.uCameraPos.w < 0.0)
    {
        float farPlane = max(-Cam.uCameraPos.w, 0.001);
        vec3 localPos = (Cam.uVRel * vec4(relWorldPos, 1.0)).xyz;
        float dist = length(localPos);
        vec3 dir = dist > 1e-6 ? localPos / dist : vec3(0.0, 0.0, 1.0);
        vec2 ndc = dir.xy / max(1.0 + abs(dir.z), 1e-6);
        float reverseDepth = clamp(1.0 - dist / farPlane, 0.0, 1.0);
        return vec4(ndc, reverseDepth, 1.0);
    }
    return Cam.uVPRel * vec4(relWorldPos, 1.0);
}

// Project a sector-local world position (model matrix applied) of an instance in
// integer sector `instSector`. outFullWorld receives the absolute world position for
// the fragment stage (lighting, fog); outRelWorld the render-origin-relative position
// for the fragment shadow receiver (small at any distance). Returns the clip position.
vec4 GE_ClipFromSectorLocal(vec3 sectorLocalWorldPos, ivec3 instSector,
                            out vec3 outFullWorld, out vec3 outRelWorld)
{
    ivec3 originSector = Cam.uRenderOriginSector.xyz;
    vec3 relWorldPos = vec3(instSector - originSector) * GE_SECTOR_SIZE + sectorLocalWorldPos;
    outFullWorld = relWorldPos + vec3(originSector) * GE_SECTOR_SIZE;
    outRelWorld = relWorldPos;
    return GE_ProjectDepthPositionRel(relWorldPos);
}

// Project a full-world position of geometry that owns its world coordinates and has
// no per-instance sector (CBT terrain, ocean, procedural surfaces). It subtracts the
// render origin and projects the small relative position. outRel receives the
// render-origin-relative position for the fragment shadow receiver.
vec4 GE_ClipFromWorld(vec3 worldPos, out vec3 outRel)
{
    vec3 relWorldPos = worldPos - GE_RenderOriginWorld();
    outRel = relWorldPos;
    return GE_ProjectDepthPositionRel(relWorldPos);
}

#endif // GE_CAMERA_RELATIVE_GLSL
