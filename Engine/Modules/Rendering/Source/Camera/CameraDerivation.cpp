#include "Rendering/Camera/CameraDerivation.h"

#include <cassert>
#include <cmath>
#include <cstring>

namespace GameEngine
{
namespace Rendering
{

CameraDerivedData DeriveCameraData(const CameraData& source, const CameraRanges& ranges)
{
    CameraDerivedData out{};

    // CameraData stores column-major matrices compatible with Matrix4x4's
    // internal layout, so a raw memcpy lands the matrices in canonical form.
    out.ViewMatrix = source.view;
    out.ProjMatrix = source.proj;
    out.ViewProjMatrix = out.ProjMatrix * out.ViewMatrix;

    // Camera position = inverse(view) * origin.
    const Matrix4x4 worldFromCamera = GameEngine::Mathematics::Inverse(out.ViewMatrix);
    out.Position = worldFromCamera.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));

    // Camera forward = inverse(view) * +Z, then normalise. This engine is
    // LEFT-HANDED with +Z forward (MakeLookAtLH), so camera-space forward is
    // +Z — the previous -Z here was the right-handed convention and pointed
    // this vector AWAY from the view, silently negating every consumer's
    // view-depth dot (caught by the sorted transparent drain's per-run depth
    // log: dot(centre - camPos, Forward) came out negative for content dead
    // ahead, inverting the blend sort front-to-back).
    const Vector4 forward4 = worldFromCamera.Transform(Vector4(0.0f, 0.0f, 1.0f, 0.0f));
    out.Forward = Vector3(forward4.x, forward4.y, forward4.z).Normalize();

    // A camera with data and zero or inverted ranges is an owner that passed bad ranges.
    // An unset camera (zero projection) has neither.
    [[maybe_unused]] const bool unsetCamera = source.proj[0][0] == 0.0f && source.proj[1][1] == 0.0f;
    assert(unsetCamera || (ranges.Near > 0.0f && ranges.CullingRange > ranges.Near));
    out.NearPlane = ranges.Near;
    out.FarPlane = ranges.CullingRange;

    // Orthographic detection + screen-space slope recovery.
    //   perspective: proj[3][3] = 0, proj[1][1] = 1 / tan(fovY/2)
    //     -> ScreenScale = tan(fovY/2)
    //   ortho:       proj[3][3] = 1, proj[1][1] = 2 / (top - bottom)
    //     -> ScreenScale = orthoHalfHeight
    out.IsOrthographic = std::fabs(out.ProjMatrix[3][3] - 1.0f) < 1e-4f;
    const float m11 = out.ProjMatrix[1][1];
    if (std::fabs(m11) > 1e-6f)
        out.ScreenScale = 1.0f / m11;

    return out;
}

} // namespace Rendering
} // namespace GameEngine
