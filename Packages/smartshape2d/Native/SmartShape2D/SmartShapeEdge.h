#pragma once

#include "Components/AssetRef.h"

namespace GameEngine::Components
{
/// Authored on a child of a SmartShape entity. Overrides the segment leaving PointIndex.
struct SmartShapeEdge
{
    uint32 PointIndex = 0;
    bool UseNormalRange = false;
    float NormalStart = 45.0f;
    float NormalSweep = 90.0f;
    int32 Priority = 0;
    MaterialRef Material;
    float WidthScale = 1.0f;
    bool Flip = false;
    bool Visible = true;
    bool operator==(const SmartShapeEdge&) const = default;
};
}
