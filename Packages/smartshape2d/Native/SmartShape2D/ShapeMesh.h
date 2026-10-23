#pragma once

#include "Assets/ModelAsset.h"
#include "Mathematics/Vector2.h"

namespace GameEngine::SmartShape2D
{
void AppendShapeVertex(Mesh& mesh, Mathematics::Vector2 position, Mathematics::Vector2 uv, float depth);
void AppendShapeQuad(Mesh& mesh, Mathematics::Vector2 a, Mathematics::Vector2 b, Mathematics::Vector2 c,
    Mathematics::Vector2 d, float startU, float endU, float depth, bool flip);
void ComputeShapeBounds(Mesh& mesh);
}
