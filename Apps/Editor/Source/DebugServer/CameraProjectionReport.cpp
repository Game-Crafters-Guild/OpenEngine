#include "DebugServer/CameraProjectionReport.h"

#include "Engine/Rendering/CameraUtils.h"
#include "Engine/Rendering/Exposure.h"
#include "Mathematics/Angle.h"
#include "Mathematics/MatrixOps.h"

#include <cmath>

namespace GameEngine::Editor
{
namespace
{

// Both builders produce strictly positive diagonal scales: cot(fovY/2) for a
// FOV in (0, pi), and 2/(right-left), 2/(top-bottom) for a non-inverted ortho
// box. A negative one is a matrix this code cannot describe, and the arctangent
// would answer with a negative angle — a number, wrong, and indistinguishable
// from a real reading. Absence, not a guess.
bool UsableScale(float v)
{
    return std::isfinite(v) && v > 1e-6f;
}

// 2 * atan(1 / cot(half)) in degrees. Both angles fall straight out of the
// matrix this way: the vertical from m11, the horizontal from m00 — deriving
// the horizontal from the vertical and the aspect instead would multiply two
// recovered quantities and their error.
float FullAngleDegFromCotHalf(float cotHalfAngle)
{
    return Mathematics::ToDegrees(2.0f * std::atan(1.0f / cotHalfAngle));
}

} // namespace

nlohmann::json DescribeCameraProjection(const Mathematics::Matrix4x4& proj,
                                        std::uint32_t viewportWidthPx,
                                        std::uint32_t viewportHeightPx)
{
    using nlohmann::json;

    const float scaleX = proj[0][0]; // perspective: cot(fovY/2) / aspect   ortho: 2/(right-left)
    const float scaleY = proj[1][1]; // perspective: cot(fovY/2)            ortho: 2/(top-bottom)
    if (!UsableScale(scaleX) || !UsableScale(scaleY))
        return json(nullptr);

    // Both projections scale X by width and Y by height, so the ratio of the
    // two diagonal terms is the aspect in both cases.
    const float aspect = scaleY / scaleX;

    json out{
        {"aspect", aspect},
        {"viewportWidthPx", viewportWidthPx},
        {"viewportHeightPx", viewportHeightPx}
    };

    if (Engine::Renderer::IsOrthographicProjectionLH_ZO(proj))
    {
        out["type"] = "orthographic";
        out["orthoHeight"] = 2.0f / scaleY;
        return out;
    }

    out["type"] = "perspective";
    out["fovVerticalDeg"] = FullAngleDegFromCotHalf(scaleY);
    out["fovHorizontalDeg"] = FullAngleDegFromCotHalf(scaleX);
    return out;
}

nlohmann::json DescribeGameViewCamera(const Rendering::CameraData& camera,
                                      Components::ExposureMode mode,
                                      Components::ExposureMeter meter,
                                      std::uint32_t viewportWidthPx,
                                      std::uint32_t viewportHeightPx)
{
    const auto world = Mathematics::Inverse(camera.view);
    auto transform = nlohmann::json::array();
    for (int index = 0; index < 16; ++index)
        transform.push_back(world.Data()[index]);
    const char* modeName = "unknown";
    switch (mode)
    {
    case Components::ExposureMode::Fixed: modeName = "fixed"; break;
    case Components::ExposureMode::Manual: modeName = "manual"; break;
    case Components::ExposureMode::Physical: modeName = "physical"; break;
    case Components::ExposureMode::Auto: modeName = "auto"; break;
    }
    nlohmann::json meterName = nullptr;
    if (mode == Components::ExposureMode::Auto)
        meterName = meter == Components::ExposureMeter::Lights ? "lights" : "picture";
    return {{"position", {camera.cameraPos.x, camera.cameraPos.y, camera.cameraPos.z}},
            {"worldTransform", std::move(transform)},
            {"projection", DescribeCameraProjection(camera.proj, viewportWidthPx, viewportHeightPx)},
            {"exposure", {{"mode", modeName}, {"meter", std::move(meterName)}, {"metered", nullptr}}}};
}

nlohmann::json DescribeMeteredExposure(float linearScale, std::uint64_t frameIndex)
{
    if (!std::isfinite(linearScale) || linearScale <= 0.0f)
        return nullptr;
    return {{"linearScale", linearScale},
            {"ev100", Rendering::LinearExposureToEv(linearScale)},
            {"frameIndex", frameIndex}};
}

} // namespace GameEngine::Editor
