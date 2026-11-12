#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"

#include <gtest/gtest.h>

using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector3;

TEST(Matrix4x4Tests, FromColumnMajorReadsTranslationFromTheLastColumn)
{
    // Uniform scale 2, translation (5, 6, 7), column-major.
    const float columnMajor[16] = {
        2.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 2.0f, 0.0f,
        5.0f, 6.0f, 7.0f, 1.0f};
    const Vector3 p = Matrix4x4::FromColumnMajor(columnMajor).TransformPoint(Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_FLOAT_EQ(p.x, 7.0f);
    EXPECT_FLOAT_EQ(p.y, 10.0f);
    EXPECT_FLOAT_EQ(p.z, 13.0f);
}

TEST(Matrix4x4Tests, FromBasisPutsTheBasisInTheFirstThreeColumnsAndTheTranslationInTheFourth)
{
    GameEngine::Mathematics::Matrix3x3 basis;
    basis[0] = glm::vec3(0.0f, 0.0f, -2.0f);
    basis[1] = glm::vec3(0.0f, 3.0f, 0.0f);
    basis[2] = glm::vec3(4.0f, 0.0f, 0.0f);
    const Matrix4x4 m = Matrix4x4::FromBasis(basis, Vector3(5.0f, 6.0f, 7.0f));
    const glm::mat4 expected(glm::vec4(0.0f, 0.0f, -2.0f, 0.0f), glm::vec4(0.0f, 3.0f, 0.0f, 0.0f),
                             glm::vec4(4.0f, 0.0f, 0.0f, 0.0f), glm::vec4(5.0f, 6.0f, 7.0f, 1.0f));
    EXPECT_TRUE(m.GetGLM() == expected);
}

// Element (0, 0) is a0 * b0 + a4 * b1 + a8 * b2 + a12 * b3, fused from the left as Vector3::Dot is.
// (1 + 2^-12)^2 - (1 + 2^-11) is 2^-24 exactly, and 0 if the product rounds first.
TEST(Matrix4x4Tests, ColumnMajorMultiplyRoundsEachFusedProductOnce)
{
    float a[16] = {};
    float b[16] = {};
    a[0] = 0x1.001p0f;
    b[0] = 0x1.001p0f;
    a[4] = -0x1.002p0f;
    b[1] = 1.0f;
    float out[16] = {};
    GameEngine::Mathematics::MultiplyColumnMajor4x4(a, b, out);
    EXPECT_EQ(out[0], 0x1p-24f);
}
