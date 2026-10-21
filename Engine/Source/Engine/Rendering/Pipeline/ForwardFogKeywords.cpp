#include "Engine/Rendering/Pipeline/ForwardFogKeywords.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine::Engine::Renderer::Pipeline
{

Rendering::MaterialKeyword HeightFogPassKeyword(ViewDeclare& d)
{
    if (!d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId).IsHeightFogActive())
        return Rendering::MaterialKeyword::None;
    if (!d.ResolveBuffer(Names::Res::HeightFogParams).IsValid())
        return Rendering::MaterialKeyword::None;
    return Rendering::MaterialKeyword::HeightFog;
}

Rendering::MaterialKeyword VolumetricFogPassKeyword(ViewDeclare& d)
{
    if (!d.ResolveBuffer(Names::Res::VolumetricFogParams).IsValid() ||
        !d.ResolveTexture(Names::Res::VolumetricFog).IsValid())
        return Rendering::MaterialKeyword::None;
    return Rendering::MaterialKeyword::VolumetricFog;
}

Rendering::MaterialKeyword VolumetricCloudsPassKeyword(ViewDeclare& d, const std::string& cloudsRef)
{
    if (cloudsRef.empty() ||
        !d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId).IsVolumetricCloudsActive())
        return Rendering::MaterialKeyword::None;
    if (!d.ResolveTexture(cloudsRef).IsValid())
        return Rendering::MaterialKeyword::None;
    return Rendering::MaterialKeyword::VolumetricClouds;
}

} // namespace GameEngine::Engine::Renderer::Pipeline
