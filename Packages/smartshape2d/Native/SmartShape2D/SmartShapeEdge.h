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
    uint32 TextureIndex = 0;
    uint32 TextureSet = 0;
    bool OverrideTextureSettings = false;
    bool RandomizeTextures = false;
    bool StretchToFit = true;
    bool UseCornerTextures = true;
    bool UseTaperTextures = true;
    float Offset = 0.0f;
    float DepthOffset = 0.0f;
    bool Weld = true;
    bool TaperSharpCorners = false;
    bool operator==(const SmartShapeEdge&) const = default;
};
}
