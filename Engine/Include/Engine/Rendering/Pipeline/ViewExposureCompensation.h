#pragma once

#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"

#include <cstdint>

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;

/// Bytes of a view's exposure compensation state (Includes/exposure_compensation.glsl): the stops the
/// auto exposure resolve took from the compensation curve at the frame's reading, and a word that is
/// 1 once it has written them.
inline constexpr uint32_t kViewExposureCompensationBytes = 16u;

/// The view's exposure compensation buffer: persistent, one per view, zero-filled on creation. The
/// auto exposure resolve writes it; the star pass reads the previous frame's value to undo the
/// compensation for the stars. Every pass of one view gets the same buffer. The caller declares its
/// access to the returned Graph buffer. Invalid only when the import fails.
PipelineBufferBindingRG ResolveViewExposureCompensation(ViewDeclare& d);
} // namespace GameEngine::Engine::Renderer::Pipeline
