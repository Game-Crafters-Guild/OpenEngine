#pragma once

#include "Mathematics/Matrix4x4.h"
#include "SmartShape2D/SmartShapeAnchor.h"

namespace GameEngine::Spline { class SplineData; }

namespace GameEngine::SmartShape2D
{
/// Returns the anchor's local transform, compensating parent scale unless requested.
Mathematics::Matrix4x4 EvaluateAnchor(const Spline::SplineData& spline,
    const Components::SmartShapeAnchor& anchor, const Mathematics::Matrix4x4& parent);
}
