#pragma once

#include "Mathematics/Vector3Int.h"

#include <array>
#include <cstdint>

namespace GameEngine::Mathematics
{

/// Faces of a cube, and corners of a face.
inline constexpr uint32_t kCubeFaceCount = 6u;
inline constexpr uint32_t kCubeFaceCornerCount = 4u;

/// The six cube faces in the order +X, -X, +Y, -Y, +Z, -Z, four corner indices each, wound
/// counterclockwise seen from outside. Corner bit 0 is +X, bit 1 +Y, bit 2 +Z (CubeCorner).
/// CBT_CUBE_FACES in the CBT shaders must equal this table.
inline constexpr int kCubeFaces[kCubeFaceCount * kCubeFaceCornerCount] = {
    1, 3, 7, 5, // +X
    0, 4, 6, 2, // -X
    2, 6, 7, 3, // +Y
    0, 1, 5, 4, // -Y
    4, 5, 7, 6, // +Z
    0, 2, 3, 1  // -Z
};

/// The cube corner of corner index `index`: each component -1 or +1.
constexpr Vector3Int CubeCorner(int index)
{
    return Vector3Int((index & 1) ? 1 : -1, (index & 2) ? 1 : -1, (index & 4) ? 1 : -1);
}

/// A cube face's frame: its outward normal, and the axes from its corner 0 to its corner 1 (X)
/// and to its corner 3 (Z), each a world axis (0 = X, 1 = Y, 2 = Z) with a sign of +1 or -1.
struct CubeFaceAxes
{
    uint8_t NormalAxis = 0;
    int8_t NormalSign = 1;
    uint8_t XAxis = 1;
    int8_t XSign = 1;
    uint8_t ZAxis = 2;
    int8_t ZSign = 1;
};

/// The frame of `face` of kCubeFaces, from its corners 0, 1 and 3.
constexpr CubeFaceAxes CubeFaceAxesOf(uint32_t face)
{
    const Vector3Int origin = CubeCorner(kCubeFaces[face * kCubeFaceCornerCount]);
    const Vector3Int alongX = CubeCorner(kCubeFaces[face * kCubeFaceCornerCount + 1u]);
    const Vector3Int alongZ = CubeCorner(kCubeFaces[face * kCubeFaceCornerCount + 3u]);
    CubeFaceAxes frame;
    for (uint8_t axis = 0; axis < 3u; ++axis)
    {
        if (alongX[axis] != origin[axis])
        {
            frame.XAxis = axis;
            frame.XSign = static_cast<int8_t>(alongX[axis] > origin[axis] ? 1 : -1);
        }
        else if (alongZ[axis] != origin[axis])
        {
            frame.ZAxis = axis;
            frame.ZSign = static_cast<int8_t>(alongZ[axis] > origin[axis] ? 1 : -1);
        }
        else
        {
            frame.NormalAxis = axis;
            frame.NormalSign = static_cast<int8_t>(origin[axis]);
        }
    }
    return frame;
}

/// The frames of kCubeFaces, by face.
inline constexpr std::array<CubeFaceAxes, kCubeFaceCount> kCubeFaceAxes{CubeFaceAxesOf(0), CubeFaceAxesOf(1), CubeFaceAxesOf(2),
                                                          CubeFaceAxesOf(3), CubeFaceAxesOf(4), CubeFaceAxesOf(5)};

} // namespace GameEngine::Mathematics
