
#pragma once

#include <glm/glm.hpp>

#include "Mathematics/Vector2.h"

namespace GameEngine {
namespace Mathematics {

// Lightweight 3x3 matrix wrapper over glm::mat3 (column-major, column vectors).
// As a 2D affine map it acts on (x, y, 1): the first two columns are the linear
// part and the third column is the translation. MatrixOps.h builds such maps.
struct Matrix3x3
{
private:
    glm::mat3 m_Matrix;

public:
    Matrix3x3() : m_Matrix(1.0f) {}
    explicit Matrix3x3(const glm::mat3& m) : m_Matrix(m) {}

    static Matrix3x3 Identity() { return Matrix3x3(glm::mat3(1.0f)); }

    glm::mat3& GetGLM() { return m_Matrix; }
    const glm::mat3& GetGLM() const { return m_Matrix; }

    operator glm::mat3&() { return m_Matrix; }
    operator const glm::mat3&() const { return m_Matrix; }

    float* Data() { return &m_Matrix[0][0]; }
    const float* Data() const { return &m_Matrix[0][0]; }

    glm::vec3& operator[](int column) { return m_Matrix[column]; }
    const glm::vec3& operator[](int column) const { return m_Matrix[column]; }

    /// `this * rhs` applies `rhs` first, then `this`.
    Matrix3x3 operator*(const Matrix3x3& rhs) const { return Matrix3x3(m_Matrix * rhs.m_Matrix); }

    /// The 2D affine map applied to a point: the linear part plus the translation.
    Vector2 TransformPoint(const Vector2& p) const
    {
        const glm::vec3 r = m_Matrix * glm::vec3(p.x, p.y, 1.0f);
        return Vector2(r.x, r.y);
    }
};

} // namespace Mathematics
} // namespace GameEngine
