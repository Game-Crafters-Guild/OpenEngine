#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{
/// Positions this child on its parent shape's outgoing spline segment.
struct SmartShapeAnchor
{
    uint32 PointIndex = 0;
    float PointOffset = 0.0f;
    float Rotation = 0.0f;
    bool UseShapeScale = false;
    bool operator==(const SmartShapeAnchor&) const = default;
};
}
