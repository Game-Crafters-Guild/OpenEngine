// Tests for the sun's screen projection (Engine/Rendering/SunScreenProjection.h), which the lens
// flare and the sun glare share to place the sun on screen.

#include <gtest/gtest.h>

#include <cstring>
#include <optional>

#include "Engine/Rendering/SunScreenProjection.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"

using GameEngine::Engine::Renderer::ProjectSunDirectionToNdc;
using GameEngine::Mathematics::MakeLookAtLH;
using GameEngine::Mathematics::MakeOrthographicLH_ZO_ReverseZ;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector3;
using GameEngine::Rendering::CameraData;

// An orthographic view (the editor's 2D views) places the sun at a point far along the
// toward-sun direction from the camera. A sun ahead and to the right lands right of center.
TEST(SunScreenProjection, AnOrthographicViewPlacesTheSunOnTheSideItIsOn)
{
    constexpr float kHalfWidth = 2000.0f;
    const Matrix4x4 view = MakeLookAtLH(Vector3(), Vector3(0.0f, 0.0f, 1.0f), Vector3(0.0f, 1.0f, 0.0f));
    const Matrix4x4 proj =
        MakeOrthographicLH_ZO_ReverseZ(-kHalfWidth, kHalfWidth, -kHalfWidth, kHalfWidth, 0.1f, 5000.0f);
    const Matrix4x4 viewProj = proj * view;

    CameraData camera{};
    std::memcpy(camera.viewProj, viewProj.Data(), sizeof(camera.viewProj));
    camera.cameraPos[3] = 1.0f; // orthographic

    const std::optional<Vector3> ndc = ProjectSunDirectionToNdc(camera, Vector3(0.6f, 0.0f, 0.8f));
    ASSERT_TRUE(ndc.has_value());
    // The sun point is 1000 along the direction: x = 600 of a 2000 half-width.
    EXPECT_NEAR(ndc->x, 0.3f, 1e-5f);
    EXPECT_NEAR(ndc->y, 0.0f, 1e-5f);
    EXPECT_EQ(ndc->z, 0.0f) << "the sun sits on the reverse-Z far plane";
}
