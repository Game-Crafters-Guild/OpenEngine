#include "Engine/Rendering/Pipeline/ForwardFogKeywords.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine::Engine::Renderer::Pipeline
{

Rendering::MaterialKeyword HeightFogKeyword(const PostProcessSettings& settings, bool heightFogParamsPublished)
{
    if (!heightFogParamsPublished || !settings.IsHeightFogActive())
        return Rendering::MaterialKeyword::None;
    return Rendering::MaterialKeyword::HeightFog;
}

Rendering::MaterialKeyword HeightFogPassKeyword(ViewDeclare& d)
{
    return HeightFogKeyword(d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId),
                            d.ResolveBuffer(Names::Res::HeightFogParams).IsValid());
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
