#pragma once

#include "Engine/Rendering/ResolvedShadowSettings.h"
#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowPasses.h"
#include "Rendering/Materials/Shader/ShaderVariantKey.h"

namespace GameEngine::Engine::Renderer
{
class RenderServices;

// Frame-local inputs to the shadow feature's world-pass contribution. GPU
// resources stay in the pass bundle; view outputs stay in the frame registry.
struct ScreenSpaceShadowView
{
    uint32_t ViewId;
    uint64_t WorldId;
    Rendering::RenderGraph::RGTexture Depth;
    const Rendering::CameraData* Camera;
    const ResolvedShadowSettings& Settings;
    bool CanDeclare;
    bool TransmissiveOnly;
    bool HasRayTracedMask;
};

// Whether a world pass with `keywords` samples a screen-space shadow mask once its view has the prepass
// depth and a camera: the settings ask for the mask, the pass draws more than transmissive surfaces, it
// takes the Shadows keyword and the world's primary directional light casts shadows.
bool TakesScreenSpaceShadowMask(RenderServices& services, uint64_t worldId, const ResolvedShadowSettings& settings,
                                bool transmissiveOnly, Rendering::MaterialKeyword keywords);

Rendering::MaterialKeyword ContributeScreenSpaceShadows(
    RenderServices& services, Rendering::RenderGraph::RGFrame& frame,
    const ScreenSpaceShadowView& view, std::unique_ptr<ScreenSpaceShadowPasses>& passes,
    Rendering::RenderGraph::RGTexture& mask, Rendering::MaterialKeyword keywords);
} // namespace GameEngine::Engine::Renderer
