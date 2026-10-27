#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::Rendering::Passes
{

/// Declares a thread-safe, brightness-weighted resolve of multisampled scene color, so a bright HDR edge blends as after
/// the tonemap. The pass writes every pixel: an earlier unread write is culled. `multisampled` is a ShaderResource texture.
/// @param resolved A single-sample, one-mip, one-layer render target the size of `multisampled`.
/// @param bloomSampleDelta Invalid, or a target like `resolved` that gets the plain sample mean minus the weighted mean.
/// @return An invalid pass, with an error in the log, when a resolve shader package does not load.
RenderGraph::RGPass AddMsaaColorResolvePassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture multisampled,
                                              RenderGraph::RGTexture resolved, RenderGraph::RGTexture bloomSampleDelta,
                                              const char* passName);

} // namespace GameEngine::Rendering::Passes
