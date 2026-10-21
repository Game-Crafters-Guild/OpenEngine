#pragma once

#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"

#include <cstdint>

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;

/// Bytes of a view's ExposureHistory state: the adapted exposure the auto-exposure resolve writes
/// and the pre-exposure pair the PreExposureSnapshot pass writes (ExposureReadbackFeature::State).
inline constexpr uint32_t kViewExposureHistoryBytes = 16u;

/// The view's ExposureHistory buffer, for a pass that reads the view's exposure or the pre-exposure
/// P its scene colour is stored at. Resolves the render graph's "ExposureHistory" resource; a graph
/// that declares none gets a persistent per-view buffer, zero-filled on creation, imported and
/// published under the same name, so every pass of one view binds one buffer. A buffer no snapshot
/// writes holds a pre-exposure of 0, which every reader takes as 1 (absolute scene colour).
/// The caller declares its read of the returned Graph buffer, which orders it after the snapshot.
/// Invalid only when the import fails.
PipelineBufferBindingRG ResolveViewExposureHistory(ViewDeclare& d);
} // namespace GameEngine::Engine::Renderer::Pipeline
