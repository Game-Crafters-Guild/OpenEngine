#pragma once

#include "Components/AssetRef.h"
#include "Mathematics/Vector2.h"

namespace GameEngine::Components
{

/// A textured shape in the local XY plane, following the entity's SplineComponent.
/// Radius on each spline point multiplies EdgeWidth. All dimensions are world units.
struct SmartShape
{
    MaterialRef FillMaterial;
    MaterialRef TopMaterial;
    MaterialRef RightMaterial;
    MaterialRef BottomMaterial;
    MaterialRef LeftMaterial;
    MaterialRef InnerCornerMaterial;
    MaterialRef OuterCornerMaterial;
    MaterialRef StartCapMaterial;
    MaterialRef EndCapMaterial;
    Mathematics::Vector2 FillScale{1.0f, 1.0f};
    Mathematics::Vector2 FillOffset{};
    float FillRotation = 0.0f;
    float EdgeWidth = 0.5f;
    float EdgeOffset = 0.0f;
    float TileLength = 1.0f;
    float EdgeDepth = -0.001f;
    float MiterLimit = 4.0f;
    uint32 CurveSubdivisions = 8;
    float CollisionDepth = 1.0f;
    bool CollisionEnabled = true;
    bool FillEnabled = true;
    bool EdgesEnabled = true;
    bool StretchToFit = true;
    bool FlipEdges = false;

    bool operator==(const SmartShape&) const = default;
};

} // namespace GameEngine::Components
