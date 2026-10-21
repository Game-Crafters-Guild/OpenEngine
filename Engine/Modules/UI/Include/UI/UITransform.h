#pragma once

#include "Mathematics/Matrix3x3.h"
#include "Mathematics/Rect.h"

namespace GameEngine
{
struct VisualStyle;
}

namespace GameEngine::UI
{

/// The element's own CSS transform: the 2D affine map from its untransformed
/// layout space into the space its parent paints in, pivoting about
/// transform-origin. `borderBox` is the layout rect in CSS logical px.
/// `unitsPerCssPx` is the output unit per CSS px: the content scale for
/// physical pixels (paint), 1 for logical pixels (hit testing and events).
/// Percentages in translate() and transform-origin resolve against the border
/// box. False, with `out` untouched, for `transform: none`.
bool ComputeElementTransform(const VisualStyle& vs, const Mathematics::Rect& borderBox, float unitsPerCssPx,
                             Mathematics::Matrix3x3& out);

} // namespace GameEngine::UI
