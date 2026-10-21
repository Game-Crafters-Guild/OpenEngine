#pragma once

#include "Rendering/Materials/ShaderVariantKey.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;

/// The HeightFog pass keyword a view's colour passes draw with: set when the view's effective
/// height fog is active (PostProcessSettings::IsHeightFogActive) and its render graph published
/// the HeightFogParams buffer before the asking pass (the HeightFogParamsUpload node). Every pass
/// that shades colour through the forward adapter, and the sky pass, asks it, so the fog is applied
/// once, in every surface's own shader.
Rendering::MaterialKeyword HeightFogPassKeyword(ViewDeclare& d);

/// The VolumetricFog pass keyword: set when the view's volumetric fog published its parameters and
/// its froxel volume before the asking pass (the VolumetricFog node). The glass and late-transparent
/// passes ask it; it is set only for one that draws after the composite (in the shipped graph, the
/// late transparents), whose blended surfaces then take the volumetric fog by their own distance.
Rendering::MaterialKeyword VolumetricFogPassKeyword(ViewDeclare& d);

/// The VolumetricClouds pass keyword: set when the view's volumetric clouds are active this frame
/// (PostProcessSettings::IsVolumetricCloudsActive) and `cloudsRef`, the graph's resolved cloud
/// texture, resolves. Asked only by a pass that draws after the clouds composite.
Rendering::MaterialKeyword VolumetricCloudsPassKeyword(ViewDeclare& d, const std::string& cloudsRef);
} // namespace GameEngine::Engine::Renderer::Pipeline
