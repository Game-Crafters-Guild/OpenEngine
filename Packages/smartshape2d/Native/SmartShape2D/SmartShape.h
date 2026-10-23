#pragma once

#include "Components/AssetRef.h"
#include "Mathematics/Vector2.h"

namespace GameEngine::Components
{

enum class ShapeCollisionMode : uint32 { Filled, RenderedEdges, Hollow };

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
    bool FillAbsolutePosition = false;
    bool FillAbsoluteRotation = false;
    float FillDepth = 0.0f;
    float FillMeshOffset = 0.0f;
    float EdgeWidth = 0.5f;
    float EdgeOffset = 0.0f;
    float TileLength = 1.0f;
    float EdgeDepth = -0.001f;
    uint32 CurveSubdivisions = 8;
    float CollisionDepth = 1.0f;
    float CollisionWidth = 0.5f;
    float CollisionOffset = 0.0f;
    ShapeCollisionMode CollisionMode = ShapeCollisionMode::Filled;
    bool CollisionEnabled = true;
    bool Destructible = false;
    bool FillEnabled = true;
    bool EdgesEnabled = true;
    bool StretchToFit = true;
    bool FlipEdges = false;
    bool RandomizeTextures = false;
    uint32 TextureSeed = 0;
    bool WeldEdges = true;
    bool UseCornerTextures = true;
    bool UseTaperTextures = true;
    bool TaperSharpCorners = false;
    Mathematics::Vector2 StartTaperSize{1.0f, 1.0f};
    Mathematics::Vector2 EndTaperSize{1.0f, 1.0f};

    bool operator==(const SmartShape&) const = default;
};

} // namespace GameEngine::Components
