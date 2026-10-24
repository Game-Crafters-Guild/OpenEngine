/**
 * @file Math.h
 * @brief Rendering's names for the Mathematics vector and matrix types.
 *
 * Scalar helpers (angles, interpolation) live in Mathematics/Angle.h and
 * Mathematics/Interpolation.h; the matrix conversion for a shader upload in
 * Rendering/Common/MatrixUtils.h.
 */

#pragma once

#include <cmath>
#include <cstring>

// Canonical engine-wide math (GLM-based) used as the underlying implementation
// for rendering math. Rendering keeps its own lightweight row-major types for
// public API and GPU conversion, but delegates construction to Mathematics.
#include "Mathematics/Types.h"
#include "Mathematics/MatrixOps.h"

namespace GameEngine {
namespace Rendering {
    	// Rendering's vector types are the Mathematics types.
	    using Vector3 = GameEngine::Mathematics::Vector3;
	    using Vector4 = GameEngine::Mathematics::Vector4;

	    // Rendering's matrix is the Mathematics matrix (column-major).
	    using Matrix4x4 = GameEngine::Mathematics::Matrix4x4;

} // namespace Rendering
} // namespace GameEngine
