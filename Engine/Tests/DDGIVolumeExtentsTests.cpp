#include "Engine/Rendering/DDGIProbeFeature.h"
#include "Mathematics/Angle.h"
#include "Mathematics/MatrixOps.h"

#include <gtest/gtest.h>

namespace GameEngine::Engine::Renderer
{
// Each half-extent is half the length of the matching basis column, so a
// rotation does not leak one axis's scale into another.
TEST(DDGIVolumeExtents, HalfExtentsAreHalfEachColumnLengthUnderRotation)
{
    using namespace Mathematics;
    const Matrix4x4 world = MakeTranslation(Vector3(5.0f, -3.0f, 8.0f)) * MakeRotationY(ToRadians(30.0f)) *
                            MakeRotationX(ToRadians(50.0f)) * MakeScale(Vector3(2.0f, 4.0f, 6.0f));

    const Vector3 halfExtents = DDGIProbeFeature::ComputeVolumeHalfExtents(world);

    EXPECT_NEAR(halfExtents.x, 1.0f, 1.0e-5f);
    EXPECT_NEAR(halfExtents.y, 2.0f, 1.0e-5f);
    EXPECT_NEAR(halfExtents.z, 3.0f, 1.0e-5f);
}
}  // namespace GameEngine::Engine::Renderer
