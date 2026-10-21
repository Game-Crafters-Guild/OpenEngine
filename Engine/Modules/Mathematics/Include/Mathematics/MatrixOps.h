#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Mathematics/Types.h"

namespace GameEngine {
namespace Mathematics {

// Common matrix construction helpers built on top of GLM, unified here so the
// engine can share a single set of conventions (left-handed, Z+ forward).

inline Matrix4x4 MakeTranslation(const Vector3& translation)
{
    glm::vec3 t(translation.x, translation.y, translation.z);
    glm::mat4 m(1.0f);
    m = glm::translate(m, t);
    return Matrix4x4(m);
}

inline Matrix4x4 MakeScale(const Vector3& scale)
{
    glm::vec3 s(scale.x, scale.y, scale.z);
    glm::mat4 m(1.0f);
    m = glm::scale(m, s);
    return Matrix4x4(m);
}

// Rotation around principal axes (angles in radians).
inline Matrix4x4 MakeRotationX(float angle)
{
    return Matrix4x4(glm::rotate(glm::mat4(1.0f), angle, glm::vec3(1.0f, 0.0f, 0.0f)));
}

inline Matrix4x4 MakeRotationY(float angle)
{
    return Matrix4x4(glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f)));
}

inline Matrix4x4 MakeRotationZ(float angle)
{
    return Matrix4x4(glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 0.0f, 1.0f)));
}

// Left-handed, Z+ forward view matrix.
inline Matrix4x4 MakeLookAtLH(const Vector3& eye,
                              const Vector3& target,
                              const Vector3& up)
{
    glm::vec3 e(eye.x, eye.y, eye.z);
    glm::vec3 c(target.x, target.y, target.z);
    glm::vec3 u(up.x, up.y, up.z);
    return Matrix4x4(glm::lookAtLH(e, c, u));
}

// Left-handed, depth [0,1] forward-Z perspective projection.
// NOTE: the engine is reverse-Z by default. Forward-Z combined with the default
// CompareOp::GreaterOrEqual depth state will reject every fragment. Use
// MakePerspectiveLH_ZO_ReverseZ unless you have a deliberate reason to deviate.
[[deprecated("Engine defaults to reverse-Z; use MakePerspectiveLH_ZO_ReverseZ.")]]
inline Matrix4x4 MakePerspectiveLH_ZO(float fovYRadians,
                                      float aspect,
                                      float zNear,
                                      float zFar)
{
    return Matrix4x4(glm::perspectiveLH_ZO(fovYRadians, aspect, zNear, zFar));
}

// Left-handed, depth [1,0] reverse-Z perspective projection.
// Maps zNear -> NDC depth 1.0, zFar -> NDC depth 0.0. Combined with a float
// depth buffer this gives near-uniform depth precision across the frustum.
inline Matrix4x4 MakePerspectiveLH_ZO_ReverseZ(float fovYRadians,
                                               float aspect,
                                               float zNear,
                                               float zFar)
{
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    const float invRange = 1.0f / (zFar - zNear);
    glm::mat4 m(0.0f);
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][2] = -zNear * invRange;        // -near/(far-near)
    m[2][3] = 1.0f;                     // LH: w_clip = z_view
    m[3][2] = (zNear * zFar) * invRange;
    return Matrix4x4(m);
}

// Left-handed, depth [0,1] forward-Z orthographic projection.
// NOTE: the engine is reverse-Z by default. Use MakeOrthographicLH_ZO_ReverseZ
// unless you have a deliberate reason to deviate.
[[deprecated("Engine defaults to reverse-Z; use MakeOrthographicLH_ZO_ReverseZ.")]]
inline Matrix4x4 MakeOrthographicLH_ZO(float left,
                                       float right,
                                       float bottom,
                                       float top,
                                       float zNear,
                                       float zFar)
{
    return Matrix4x4(glm::orthoLH_ZO(left, right, bottom, top, zNear, zFar));
}

// Left-handed, depth [1,0] reverse-Z orthographic projection.
// Maps zNear -> NDC depth 1.0, zFar -> NDC depth 0.0. Hand-built because
// glm::orthoLH_ZO does not produce a reverse-Z matrix when its near/far args
// are swapped (the resulting matrix is mathematically wrong).
inline Matrix4x4 MakeOrthographicLH_ZO_ReverseZ(float left,
                                                float right,
                                                float bottom,
                                                float top,
                                                float zNear,
                                                float zFar)
{
    glm::mat4 m(1.0f);
    m[0][0] = 2.0f / (right - left);
    m[1][1] = 2.0f / (top - bottom);
    m[2][2] = -1.0f / (zFar - zNear);
    m[3][0] = -(right + left) / (right - left);
    m[3][1] = -(top + bottom) / (top - bottom);
    m[3][2] = zFar / (zFar - zNear);
    m[3][3] = 1.0f;
    return Matrix4x4(m);
}

// Basic linear algebra helpers.
inline Matrix4x4 Transpose(const Matrix4x4& m)
{
    return Matrix4x4(glm::transpose(m.GetGLM()));
}

inline Matrix4x4 Inverse(const Matrix4x4& m)
{
    return Matrix4x4(glm::inverse(m.GetGLM()));
}

// 2D affine maps, as homogeneous 3x3 matrices acting on (x, y, 1) (Matrix3x3).
// Angles are in radians. The UI's y axis points down, so there a positive
// rotation turns clockwise on screen, as CSS rotate() does.

inline Matrix3x3 MakeTranslation2D(const Vector2& translation)
{
    glm::mat3 m(1.0f);
    m[2] = glm::vec3(translation.x, translation.y, 1.0f);
    return Matrix3x3(m);
}

inline Matrix3x3 MakeScale2D(const Vector2& scale)
{
    glm::mat3 m(1.0f);
    m[0][0] = scale.x;
    m[1][1] = scale.y;
    return Matrix3x3(m);
}

// x' = cos(a) x - sin(a) y, y' = sin(a) x + cos(a) y.
inline Matrix3x3 MakeRotation2D(float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    glm::mat3 m(1.0f);
    m[0] = glm::vec3(c, s, 0.0f);
    m[1] = glm::vec3(-s, c, 0.0f);
    return Matrix3x3(m);
}

// x' = x + tan(angleX) y, y' = tan(angleY) x + y (CSS skew(angleX, angleY)).
inline Matrix3x3 MakeSkew2D(float angleX, float angleY)
{
    glm::mat3 m(1.0f);
    m[0][1] = std::tan(angleY);
    m[1][0] = std::tan(angleX);
    return Matrix3x3(m);
}

// The map x' = a x + c y + e, y' = b x + d y + f: the argument order of CSS
// matrix(a, b, c, d, e, f) and of canvas setTransform.
inline Matrix3x3 MakeAffine2D(float a, float b, float c, float d, float e, float f)
{
    glm::mat3 m(1.0f);
    m[0] = glm::vec3(a, b, 0.0f);
    m[1] = glm::vec3(c, d, 0.0f);
    m[2] = glm::vec3(e, f, 1.0f);
    return Matrix3x3(m);
}

// The inverse of a 2D affine map. False, with `out` untouched, when the map is
// singular (it collapses the plane onto a line or a point, as a zero scale does).
inline bool TryInverseAffine2D(const Matrix3x3& m, Matrix3x3& out)
{
    constexpr float kMinDeterminant = 1e-12f;
    const glm::mat3& g = m.GetGLM();
    const float det = g[0][0] * g[1][1] - g[1][0] * g[0][1];
    if (std::abs(det) < kMinDeterminant)
        return false;
    const float invDet = 1.0f / det;
    glm::mat3 r(1.0f);
    r[0][0] = g[1][1] * invDet;
    r[0][1] = -g[0][1] * invDet;
    r[1][0] = -g[1][0] * invDet;
    r[1][1] = g[0][0] * invDet;
    r[2][0] = -(r[0][0] * g[2][0] + r[1][0] * g[2][1]);
    r[2][1] = -(r[0][1] * g[2][0] + r[1][1] * g[2][1]);
    out = Matrix3x3(r);
    return true;
}

// The axis-aligned bounds of a rectangle after a 2D affine map: the smallest
// rectangle that holds all four mapped corners.
inline Rect TransformRectBounds2D(const Matrix3x3& m, const Rect& r)
{
    const Vector2 corners[4] = {m.TransformPoint(Vector2(r.X, r.Y)), m.TransformPoint(Vector2(r.Right(), r.Y)),
                                m.TransformPoint(Vector2(r.Right(), r.Bottom())),
                                m.TransformPoint(Vector2(r.X, r.Bottom()))};
    float left = corners[0].x;
    float top = corners[0].y;
    float right = corners[0].x;
    float bottom = corners[0].y;
    for (const Vector2& c : corners)
    {
        left = std::min(left, c.x);
        top = std::min(top, c.y);
        right = std::max(right, c.x);
        bottom = std::max(bottom, c.y);
    }
    return Rect::FromEdges(left, top, right, bottom);
}

} // namespace Mathematics
} // namespace GameEngine

