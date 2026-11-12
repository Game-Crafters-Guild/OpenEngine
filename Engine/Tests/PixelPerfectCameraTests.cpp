#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Camera/CameraPixelGrid.h"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

TEST(PixelPerfectCamera, OrthoDisplayGridTracksCompensatedRasterAcrossPanning)
{
    CameraData camera{};
    camera.view = camera.proj = Mathematics::Matrix4x4::Identity();
    camera.proj[0][0] = camera.proj[1][1] = 0.02f;
    constexpr uint32_t width = 161, height = 91;
    for (float pan : {-1.25f, -0.35f, 0.0f, 0.25f, 0.4f, 1.75f})
    {
        camera.view[3][0] = pan;
        camera.view[3][1] = -pan;
        Mathematics::Vector4 uv;
        const auto snapped = StabilizeCameraToPixelGrid(camera, width, height, 10, uv);
        const Mathematics::Vector4 grid{1.0f / width, 1.0f / height, -uv[2] / uv[0], -uv[3] / uv[1]};
        for (int axis = 0; axis < 2; ++axis)
        {
            const float size = float(axis == 0 ? width : height);
            // The texel is a sum of order-one float terms scaled by (size + 2), so its rounding error is a
            // few float epsilons of that scale (at most 1.6 measured at 161 px, constant across widths).
            const float tolerance = 4.0f * std::numeric_limits<float>::epsilon() * (size + 2);
            for (float screen : {0.0f, 0.001f, 0.37f, 0.999f, 1.0f})
            {
                const float cell = std::floor((screen - grid[axis + 2]) / grid[axis]);
                const float center = (cell + 0.5f) * grid[axis] + grid[axis + 2];
                const float texel = (center * uv[axis] + 0.5f * (1 - uv[axis]) + uv[axis + 2]) * (size + 2);
                EXPECT_NEAR(texel, cell + 1.5f, tolerance);
            }
        }
        const float rasterU = snapped.viewProj[3][0] * 0.5f + 0.5f;
        const float displayU = (rasterU - 0.5f * (1 - uv[0]) - uv[2]) / uv[0];
        const float cell = std::floor((displayU - grid[2]) / grid[0]);
        const float displayCenter = (cell + 0.5f) * grid[0] + grid[2];
        const float worldSample = ((displayCenter - 0.5f) * 2.0f) / camera.proj[0][0] - camera.view[3][0];
        EXPECT_NEAR(worldSample, 0.0f, 2e-5f);
    }
}

TEST(PixelPerfectCamera, ActiveCameraSnapsInScreenAxesAndPreservesTransform)
{
    RenderServices services;
    const auto cameraId = services.Views().AllocateCamera("PixelPerfect.Test");
    const auto viewId = services.Views().AllocateView("PixelPerfect.Test", cameraId);
    Engine::Renderer::Camera camera;
    camera.params.Perspective = false;
    camera.params.PixelPerfect = true;
    camera.params.PixelPerfectPixelSnap = true;
    camera.params.PixelPerfectPixelsPerUnit = 10;
    camera.params.PixelPerfectReferenceWidth = 161;
    camera.params.PixelPerfectReferenceHeight = 91;
    for (float angle : {0.0f, 0.7f, 1.5707963f})
    {
        Mathematics::Matrix4x4 basis;
        float* rotation = basis.Data();
        rotation[0] = rotation[5] = std::cos(angle);
        rotation[1] = std::sin(angle);
        rotation[4] = -std::sin(angle);
        for (float pan : {-0.17f, -0.04f, 0.0f, 0.04f, 0.17f})
        {
            camera.SetPose(basis, Mathematics::WorldPosition{pan, -pan, 0.0});
            const float* matrix = camera.WorldTransform().Data();
            ApplyActiveCameraAspect(services, viewId, cameraId, camera, 483, 273);
            const auto* actual = services.Views().FindCameraData(cameraId);
            ASSERT_NE(actual, nullptr);
            const auto state = services.Views().GetViewPixelPerfect(viewId);
            ASSERT_TRUE(state.Active);
            const auto view = camera.ComputeViewMatrix();
            for (int axis = 0; axis < 2; ++axis)
            {
                const float original = view.Data()[12 + axis];
                const float snapped = std::round(original * 10.0f) / 10.0f;
                EXPECT_NEAR(actual->viewProj[3][axis], snapped * actual->proj[axis][axis], 1e-5f);
                const float fraction = axis == 0 ? state.FracX : state.FracY;
                EXPECT_NEAR(fraction, (snapped - original) * 10.0f, 1e-5f);
            }
            EXPECT_FLOAT_EQ(matrix[12], pan);
            EXPECT_FLOAT_EQ(matrix[13], -pan);
        }
    }
}

TEST(PixelPerfectCamera, DisabledPixelSnapPreservesProjectionAndZeroCompensation)
{
    RenderServices services;
    const auto cameraId = services.Views().AllocateCamera("PixelPerfect.Disabled");
    const auto viewId = services.Views().AllocateView("PixelPerfect.Disabled", cameraId);
    Engine::Renderer::Camera camera;
    camera.params.Perspective = false;
    camera.params.PixelPerfect = true;
    camera.params.PixelPerfectPixelSnap = false;
    camera.params.PixelPerfectPixelsPerUnit = 10;
    camera.SetPose(Mathematics::Matrix4x4{}, Mathematics::WorldPosition{0.037f, 0.0, 0.0});
    ApplyActiveCameraAspect(services, viewId, cameraId, camera, 640, 360);
    const auto state = services.Views().GetViewPixelPerfect(viewId);
    const auto expected = camera.ToCameraData(float(state.PaddedWidth) / state.PaddedHeight,
                                               float(state.PaddedHeight) / 10.0f);
    const auto* actual = services.Views().FindCameraData(cameraId);
    ASSERT_NE(actual, nullptr);
    EXPECT_FLOAT_EQ(state.FracX, 0.0f);
    EXPECT_FLOAT_EQ(state.FracY, 0.0f);
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            EXPECT_FLOAT_EQ(actual->viewProj[column][row], expected.viewProj[column][row]);
}

// Past 512 m the vertex stage projects through viewProjRel, so it must carry the snap of viewProj.
TEST(PixelPerfectCamera, RebasedViewProjectionCarriesTheSnapPastTheFirstSector)
{
    RenderServices services;
    const auto cameraId = services.Views().AllocateCamera("PixelPerfect.Far");
    const auto viewId = services.Views().AllocateView("PixelPerfect.Far", cameraId);
    Engine::Renderer::Camera camera;
    camera.params.Perspective = false;
    camera.params.PixelPerfect = true;
    camera.params.PixelPerfectPixelSnap = true;
    camera.params.PixelPerfectPixelsPerUnit = 10;
    constexpr float kEyeX = 2000.17f; // sector 2, 0.3 px off the pixel grid
    camera.SetPose(Mathematics::Matrix4x4{}, Mathematics::WorldPosition{kEyeX, 0.0, 0.0});
    ApplyActiveCameraAspect(services, viewId, cameraId, camera, 483, 273);
    const auto* actual = services.Views().FindCameraData(cameraId);
    ASSERT_NE(actual, nullptr);
    ASSERT_EQ(actual->renderOriginSector[0], 2);
    const Mathematics::Matrix4x4 expected = actual->proj * actual->viewRel;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            EXPECT_NEAR(actual->viewProjRel[column][row], expected[column][row], 1e-5f);
}
