#pragma once

#include "Components/Transform.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3F64.h"
#include "Mathematics/Vector3Int.h"
#include "Mathematics/WorldPosition.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/GPUScene.h"
#include "Types/Types.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace GameEngine::Engine::Renderer
{

// Camera-relative rendering ("Earth-scale" precision). The render origin is an
// integer sector index (exact, large scale) plus an fp32 local remainder (fine
// scale) — a "manual double" for the one coordinate that must stay precise at
// planetary distance. The vertex stage reconstructs each instance's
// camera-relative position from a per-view origin uniform, so a moving camera
// re-uploads only the CameraUBO, never the persistent, change-tracked instance
// buffer (the #405/#490 quiescence law). Authoring space stays plain fp32
// Transforms; the sector is derived automatically from the camera each frame.
//
// GLSL mirror: Shaders/Includes/instance_sector.glsl (ge_UnpackInstanceSector)
// and Shaders/Includes/camera_ubo_fields.glsl (uRenderOriginSector). The pack /
// unpack round-trip is locked by RenderOriginTests.

// The world position of the origin of a sector, sector * kWorldSectorSize. Exact in fp32 for every
// 21-bit sector: the product of a 21-bit integer and a power of two fits the fp32 mantissa.
inline Mathematics::Vector3 SectorToWorld(const Mathematics::Vector3Int& sector)
{
    constexpr float32 kSectorSize = static_cast<float32>(Mathematics::kWorldSectorSize);
    return Mathematics::Vector3{static_cast<float32>(sector.x) * kSectorSize,
                                static_cast<float32>(sector.y) * kSectorSize,
                                static_cast<float32>(sector.z) * kSectorSize};
}

// The render origin of an eye: the eye's own sector (the WorldPosition split), clamped to the
// packed 21-bit range. Every view renders relative to it, at any distance.
inline Mathematics::Vector3Int ComputeRenderOriginSector(const Mathematics::WorldPosition& eye)
{
    // A NaN or infinite eye is an upstream bug: assert, and keep the origin at sector 0.
    if (!std::isfinite(eye.X) || !std::isfinite(eye.Y) || !std::isfinite(eye.Z))
    {
        assert(false && "render-origin camera position is not finite");
        return Mathematics::Vector3Int{0, 0, 0};
    }
    return eye.ToSectorClamped(Rendering::kSectorAxisMin, Rendering::kSectorAxisMax);
}

// The #660 double-rebase core, shared by the camera (ComputeRebasedView) and
// the light VP build (ComputeRebasedLightVP): rebuild a view
// matrix's translation from its own 3x3 and the eye position, with the origin
// exact in double from the integer sector and (eye − origin) a difference of
// fp32-representable values — exact in double. The result is −R3x3·(eye − origin),
// kept in double; the caller rounds to fp32 once, on store. The legacy fp32
// column rebase (translation + M3x3·origin, a big+big sum whose small result
// inherits ULP(|origin|) rounding) re-quantized the relative translation every
// frame the matrices changed — mm wobble at |origin| 5e4, meters at Earth
// radius.
inline Mathematics::Vector3F64 ComputeRebasedViewTranslation(const Mathematics::Matrix4x4& view,
                                                             const Mathematics::WorldPosition& eye,
                                                             const Mathematics::Vector3Int& sector)
{
    const float64 sectorSize = Mathematics::kWorldSectorSize;
    const float64 lx = eye.X - static_cast<float64>(sector.x) * sectorSize;
    const float64 ly = eye.Y - static_cast<float64>(sector.y) * sectorSize;
    const float64 lz = eye.Z - static_cast<float64>(sector.z) * sectorSize;
    return Mathematics::Vector3F64(-(static_cast<float64>(view[0][0]) * lx + static_cast<float64>(view[1][0]) * ly +
                                     static_cast<float64>(view[2][0]) * lz),
                                   -(static_cast<float64>(view[0][1]) * lx + static_cast<float64>(view[1][1]) * ly +
                                     static_cast<float64>(view[2][1]) * lz),
                                   -(static_cast<float64>(view[0][2]) * lx + static_cast<float64>(view[1][2]) * ly +
                                     static_cast<float64>(view[2][2]) * lz));
}

// A light's view-projection for positions relative to the origin of `sector`: proj · [R | −R·(eye − origin)]
// in double, R from rotView, one fp32 rounding per element. worldVP is never rebased: its translation column
// holds the rounding of the large eye. In sector (0,0,0) the result is a bit copy of worldVP.
inline Mathematics::Matrix4x4 ComputeRebasedLightVP(const Mathematics::Matrix4x4& rotView,
                                                    const Mathematics::Matrix4x4& projection,
                                                    const Mathematics::Matrix4x4& worldVP,
                                                    const Mathematics::WorldPosition& eye,
                                                    const Mathematics::Vector3Int& sector)
{
    if (sector.x == 0 && sector.y == 0 && sector.z == 0)
        return worldVP;

    const Mathematics::Vector3F64 t = ComputeRebasedViewTranslation(rotView, eye, sector);

    // [R | t], indexed [column][row].
    float64 viewRel[4][4];
    for (int32 c = 0; c < 3; ++c)
        for (int32 k = 0; k < 4; ++k)
            viewRel[c][k] = static_cast<float64>(rotView[c][k]);
    viewRel[3][0] = t.X;
    viewRel[3][1] = t.Y;
    viewRel[3][2] = t.Z;
    viewRel[3][3] = static_cast<float64>(rotView[3][3]);

    Mathematics::Matrix4x4 out;
    for (int32 c = 0; c < 4; ++c)
    {
        for (int32 r = 0; r < 4; ++r)
        {
            float64 acc = 0.0;
            for (int32 k = 0; k < 4; ++k)
                acc += static_cast<float64>(projection[k][r]) * viewRel[c][k];
            out[c][r] = static_cast<float32>(acc);
        }
    }
    return out;
}

// Moves a retained origin-relative matrix from sector `from` to sector `to`:
// out = m * translate((to - from) * kWorldSectorSize), in double. Writes only the translation
// column (12..15): the caller copies the other columns into `out`, and copies m itself when from == to.
inline void RebaseTranslationColumnBySectorDelta(const float32 m[16], const int32 from[3],
                                                 const int32 to[3], float32 out[16])
{
    const float64 size = Mathematics::kWorldSectorSize;
    const float64 dx = static_cast<float64>(to[0] - from[0]) * size;
    const float64 dy = static_cast<float64>(to[1] - from[1]) * size;
    const float64 dz = static_cast<float64>(to[2] - from[2]) * size;
    for (int32 r = 0; r < 4; ++r)
    {
        const float64 acc = static_cast<float64>(m[0 * 4 + r]) * dx +
                            static_cast<float64>(m[1 * 4 + r]) * dy +
                            static_cast<float64>(m[2 * 4 + r]) * dz +
                            static_cast<float64>(m[12 + r]);
        out[12 + r] = static_cast<float32>(acc);
    }
}

// Moves a sector-local model matrix from sector `from` to sector `to`:
// translate((from - to) * kWorldSectorSize) * matrix, in double, rounded once.
inline Mathematics::Matrix4x4 PremultiplyTranslationBySectorDelta(const Mathematics::Matrix4x4& matrix,
                                                                  const Mathematics::Vector3Int& from,
                                                                  const Mathematics::Vector3Int& to)
{
    const float64 size = Mathematics::kWorldSectorSize;
    const float64 w = static_cast<float64>(matrix[3][3]);
    Mathematics::Matrix4x4 out = matrix;
    out[3][0] = static_cast<float32>(static_cast<float64>(matrix[3][0]) + static_cast<float64>(from.x - to.x) * size * w);
    out[3][1] = static_cast<float32>(static_cast<float64>(matrix[3][1]) + static_cast<float64>(from.y - to.y) * size * w);
    out[3][2] = static_cast<float32>(static_cast<float64>(matrix[3][2]) + static_cast<float64>(from.z - to.z) * size * w);
    return out;
}

// An instance's render-origin sector and its sector-local model matrix, from the one split of the
// exact world position: the float translation column stays inside [-512, 512) at any distance.
inline Mathematics::Matrix4x4 SectorLocalMatrix(const ::GameEngine::Components::WorldTransform& transform,
                                                Mathematics::Vector3Int& outSector)
{
    Mathematics::Vector3 local;
    transform.GetPosition().ToSector(outSector, local);
    return transform.ToMatrix4x4(local);
}

// The world model matrix of a GPUInstance: the sector-local `matrix` with the packed sector's
// origin added in float, so the translation has the float spacing of the world position (about
// 31 mm at 300 km). DDGI and the ray-traced shadow mask drop an instance past kMaxSaneWorldUnits.
inline Mathematics::Matrix4x4 ComposeWorldModelMatrix(const Mathematics::Matrix4x4& matrix, uint32 sectorP0,
                                                      uint32 sectorP1)
{
    Mathematics::Vector3Int sector;
    Rendering::UnpackSector(sectorP0, sectorP1, sector.x, sector.y, sector.z);
    const Mathematics::Vector3 origin = SectorToWorld(sector);
    Mathematics::Matrix4x4 out = matrix;
    out[3][0] = matrix[3][0] + origin.x;
    out[3][1] = matrix[3][1] + origin.y;
    out[3][2] = matrix[3][2] + origin.z;
    return out;
}

// Fill the camera-relative fields (viewRel, viewProjRel, sector) from the full-world view,
// proj and viewProj and the eye. The rebased matrices take positions relative to the eye's
// sector. At sector (0,0,0) they are bit copies of view and viewProj.
// The translation is -M3x3 * (eye - origin), built in double from the double eye and rounded
// to fp32 once, so the eye moves by its exact step at any distance. viewProjRel is
// proj * viewRel accumulated in double.
inline void ComputeRebasedView(const Mathematics::Matrix4x4& view, const Mathematics::Matrix4x4& proj,
                               const Mathematics::Matrix4x4& viewProj, const Mathematics::WorldPosition& eye,
                               Mathematics::Matrix4x4& outViewRel, Mathematics::Matrix4x4& outViewProjRel,
                               int32 outSector[4])
{
    const Mathematics::Vector3Int origin = ComputeRenderOriginSector(eye);
    outSector[0] = origin.x;
    outSector[1] = origin.y;
    outSector[2] = origin.z;
    outSector[3] = static_cast<int32>(Mathematics::kWorldSectorSize);

    outViewRel = view;
    outViewProjRel = viewProj;
    if (origin.x == 0 && origin.y == 0 && origin.z == 0)
        return; // sector (0,0,0): bit copies

    // The origin in double, straight from the integer sector.
    const Mathematics::Vector3F64 t = ComputeRebasedViewTranslation(view, eye, origin);
    outViewRel[3][0] = static_cast<float32>(t.X);
    outViewRel[3][1] = static_cast<float32>(t.Y);
    outViewRel[3][2] = static_cast<float32>(t.Z);
    outViewRel[3][3] = view[3][3];

    for (int32 c = 0; c < 4; ++c)
    {
        for (int32 r = 0; r < 4; ++r)
        {
            float64 acc = 0.0;
            for (int32 k = 0; k < 4; ++k)
                acc += static_cast<float64>(proj[k][r]) * static_cast<float64>(outViewRel[c][k]);
            outViewProjRel[c][r] = static_cast<float32>(acc);
        }
    }
}

/// Set a camera's eye: the fp32 cameraPos, the render origin sector and the rebased matrices.
/// Call it after view, proj and viewProj are set. The eye keeps its double precision only here.
inline void SetCameraEye(Rendering::CameraData& camera, const Mathematics::WorldPosition& eye)
{
    camera.cameraPos.x = static_cast<float32>(eye.X);
    camera.cameraPos.y = static_cast<float32>(eye.Y);
    camera.cameraPos.z = static_cast<float32>(eye.Z);
    ComputeRebasedView(camera.view, camera.proj, camera.viewProj, eye, camera.viewRel, camera.viewProjRel,
                       camera.renderOriginSector);
}

/// The eye a camera draws from. At sector (0,0,0) the view is the fp32 view, so the eye is cameraPos.
/// Else it is the origin plus -R^T * (viewRel translation), in double: about 1e-4 m of error in the
/// sector, against the 7.8 mm step of an fp32 eye at 125 km.
inline Mathematics::WorldPosition CameraEye(const Rendering::CameraData& camera)
{
    const int32* sector = camera.renderOriginSector;
    if (sector[0] == 0 && sector[1] == 0 && sector[2] == 0)
        return Mathematics::WorldPosition::FromVector3(
            Mathematics::Vector3{camera.cameraPos.x, camera.cameraPos.y, camera.cameraPos.z});
    // viewRel is column-major: view[c] is column c, so view[axis][k] is R[k][axis] and view[3] is t.
    const Mathematics::Matrix4x4& view = camera.viewRel;
    const float64 tx = view[3][0];
    const float64 ty = view[3][1];
    const float64 tz = view[3][2];
    const float64 size = Mathematics::kWorldSectorSize;
    return Mathematics::WorldPosition{
        static_cast<float64>(sector[0]) * size - (view[0][0] * tx + view[0][1] * ty + view[0][2] * tz),
        static_cast<float64>(sector[1]) * size - (view[1][0] * tx + view[1][1] * ty + view[1][2] * tz),
        static_cast<float64>(sector[2]) * size - (view[2][0] * tx + view[2][1] * ty + view[2][2] * tz)};
}

/// The render origin sector of a camera, the one SetCameraEye stored. A CPU pass reads it here,
/// so the CPU and the GPU use one sector.
inline Mathematics::Vector3Int CameraRenderOriginSector(const Rendering::CameraData& camera)
{
    assert(camera.renderOriginSector[3] != 0 && "the camera has no eye: call SetCameraEye");
    return Mathematics::Vector3Int{camera.renderOriginSector[0], camera.renderOriginSector[1],
                                   camera.renderOriginSector[2]};
}

/// A world position relative to the origin of a camera's render origin sector, the frame of viewRel and of
/// CameraEyeRelativeToOrigin. It subtracts in double and rounds once. At sector (0,0,0) it is the position
/// rounded to fp32, bit for bit.
inline Mathematics::Vector3 ViewRelativePoint(const Rendering::CameraData& camera,
                                              const Mathematics::WorldPosition& position)
{
    return position -
           Mathematics::WorldPosition::FromSector(CameraRenderOriginSector(camera), Mathematics::Vector3{});
}

/// The eye of a camera relative to the origin of its render origin sector, the frame of viewRel and of a
/// center composed by SectorLocalPointRelativeTo. At sector (0,0,0) it is cameraPos, bit for bit.
inline Mathematics::Vector3 CameraEyeRelativeToOrigin(const Rendering::CameraData& camera)
{
    return ViewRelativePoint(camera, CameraEye(camera));
}

} // namespace GameEngine::Engine::Renderer
