#pragma once

#include "Components/AssetRef.h"

namespace GameEngine::Components
{
/// Click-to-explode controller for an orthographic XY demonstration scene.
struct ShapeDestructionDemo
{
    float Radius = 1.25f;
    MaterialRef FlashMaterial;
};
}
