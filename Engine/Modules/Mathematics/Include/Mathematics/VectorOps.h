#pragma once

#include "Mathematics/MulAdd.h"

#include <cmath>

namespace GameEngine {
namespace Mathematics {

// Common 3D vector helpers for float[3] arrays used across Engine and Editor.
// These are intentionally lightweight and header-only so they can be used in
// hot paths (e.g., gizmos, picking, camera setup) without introducing a hard
// dependency on a particular math library.

inline constexpr float Pi = 3.14159265358979323846f;

inline void Sub3(const float a[3], const float b[3], float r[3])
{
    r[0] = a[0] - b[0];
    r[1] = a[1] - b[1];
    r[2] = a[2] - b[2];
}

inline void Add3(const float a[3], const float b[3], float r[3])
{
    r[0] = a[0] + b[0];
    r[1] = a[1] + b[1];
    r[2] = a[2] + b[2];
}

inline void MulAdd3(const float a[3], float s, const float b[3], float r[3])
{
    r[0] = Math::MulAdd(s, b[0], a[0]);
    r[1] = Math::MulAdd(s, b[1], a[1]);
    r[2] = Math::MulAdd(s, b[2], a[2]);
}

inline float Dot3(const float a[3], const float b[3])
{
    return Math::MulAdd(a[2], b[2], Math::MulAdd(a[0], b[0], a[1] * b[1]));
}

inline void Cross3(const float a[3], const float b[3], float r[3])
{
    r[0] = Math::MulAdd(a[1], b[2], -(a[2] * b[1]));
    r[1] = Math::MulAdd(a[2], b[0], -(a[0] * b[2]));
    r[2] = Math::MulAdd(a[0], b[1], -(a[1] * b[0]));
}

inline void Normalize3(float v[3])
{
    const float lenSq = Math::MulAdd(v[2], v[2], Math::MulAdd(v[0], v[0], v[1] * v[1]));
    if (lenSq <= 0.0f)
        return;

    const float invLen = 1.0f / std::sqrt(lenSq);
    v[0] *= invLen;
    v[1] *= invLen;
    v[2] *= invLen;
}

} // namespace Mathematics
} // namespace GameEngine
