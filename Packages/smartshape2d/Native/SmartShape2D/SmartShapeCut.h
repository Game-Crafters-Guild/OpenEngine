#pragma once

#include "Mathematics/Vector2.h"

namespace GameEngine::Components
{
/// An explosion footprint parented to its terrain. Axes preserve a world-space
/// circle under nonuniform terrain transforms and keep damage attached to it.
struct SmartShapeCut
{
    Mathematics::Vector2 Center;
    Mathematics::Vector2 AxisX{1.0f, 0.0f};
    Mathematics::Vector2 AxisY{0.0f, 1.0f};
    bool operator==(const SmartShapeCut&) const = default;
};
}
