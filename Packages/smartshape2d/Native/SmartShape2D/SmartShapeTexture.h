#pragma once

#include "Components/AssetRef.h"
#include "Mathematics/Vector2.h"

namespace GameEngine::Components
{
/// One entry in an unbounded texture set, authored as a child of the shape.
/// PointIndex selects a segment; -1 supplies the default set for all segments.
struct SmartShapeTexture
{
    int32 PointIndex = -1;
    uint32 Index = 0;
    uint32 TextureSet = 0;
    MaterialRef Material;
    MaterialRef InnerCornerMaterial;
    MaterialRef OuterCornerMaterial;
    MaterialRef StartTaperMaterial;
    MaterialRef EndTaperMaterial;
    MaterialRef SharpStartTaperMaterial;
    MaterialRef SharpEndTaperMaterial;
    float TileLength = 1.0f;
    float WidthScale = 1.0f;
    Mathematics::Vector2 StartTaperSize{1.0f, 1.0f};
    Mathematics::Vector2 EndTaperSize{1.0f, 1.0f};
    bool operator==(const SmartShapeTexture&) const = default;
};
}
