#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::Rendering::Passes
{

/// Declares a thread-safe, brightness-weighted resolve of multisampled scene color, so a bright HDR
/// edge blends as after the tonemap. The pass writes every pixel: an earlier unread write is culled.
/// @param multisampled The samples; its texture has TextureUsage::ShaderResource.
/// @param resolved A single-sample, one-mip, one-layer render target of the same size.
/// @return An invalid pass, with an error in the log, when Shaders/msaa_color_resolve.shaderpkg does not load.
RenderGraph::RGPass AddMsaaColorResolvePassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture multisampled,
                                              RenderGraph::RGTexture resolved, const char* passName);

} // namespace GameEngine::Rendering::Passes
