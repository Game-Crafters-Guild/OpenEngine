#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowWorldPass.h"

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Mathematics/Vector3.h"
#include "Types/StringId.h"
#include <cmath>
#include <optional>

namespace GameEngine::Engine::Renderer
{
namespace
{
constexpr float kMinLightDirectionLength = 1e-6f;
constexpr StringId kShadowConstantsBindingId = HashStringId("ShadowData");

// Returns the unit direction toward the world's primary directional light, when that light casts
// shadows from a finite direction.
std::optional<Mathematics::Vector3> FindTowardShadowCastingSun(RenderServices& services, uint64_t worldId)
{
    const auto* sun = SelectPrimaryDirectional(services.GetWorldLights(worldId));
    if (!sun || sun->castsShadows == 0)
        return std::nullopt;
    const Mathematics::Vector3 towardLight(-sun->directionWS[0], -sun->directionWS[1], -sun->directionWS[2]);
    const float length = towardLight.Length();
    if (!std::isfinite(length) || length <= kMinLightDirectionLength)
        return std::nullopt;
    return towardLight / length;
}
}

bool TakesScreenSpaceShadowMask(RenderServices& services, uint64_t worldId, const ResolvedShadowSettings& settings,
                                bool transmissiveOnly, Rendering::MaterialKeyword keywords)
{
    return settings.ScreenSpaceShadows && !transmissiveOnly &&
           Rendering::HasKeyword(keywords, Rendering::MaterialKeyword::Shadows) &&
           FindTowardShadowCastingSun(services, worldId).has_value();
}

Rendering::MaterialKeyword ContributeScreenSpaceShadows(
    RenderServices& services, Rendering::RenderGraph::RGFrame& frame,
    const ScreenSpaceShadowView& view, std::unique_ptr<ScreenSpaceShadowPasses>& passes,
    Rendering::RenderGraph::RGTexture& mask, Rendering::MaterialKeyword keywords)
{
    using Rendering::MaterialKeyword;
    // A keyword is permission to sample a mask produced in this view/frame,
    // not a persisted pipeline option or merely an enabled volume setting.
    keywords = keywords & ~MaterialKeyword::ScreenSpaceShadows;
    if (!TakesScreenSpaceShadowMask(services, view.WorldId, view.Settings, view.TransmissiveOnly, keywords))
        return keywords;

    const bool canDeclareMask = view.CanDeclare && !mask.IsValid() && view.Depth.IsValid() && view.Camera;
    if (const auto towardLight = canDeclareMask ? FindTowardShadowCastingSun(services, view.WorldId) : std::nullopt)
    {
        if (!passes)
            passes = std::make_unique<ScreenSpaceShadowPasses>(services.GetDevice());
        ScreenSpaceShadowFilter filter{};
        const auto shadowMap = services.GetShadowMapArrayRG(frame, view.ViewId);
        // Match the current per-view PCF footprint, not a second fit.
        // Only the fixed grid kernels have one: a ray-traced mask and the
        // blocker-dependent filters (PCSS, DPCF, MSM) have no footprint to
        // project, and asking for one would schedule a pass that copies.
        const bool gridFilter =
            view.Settings.Filter == Components::DirectionalShadowFilter::Grid5x5 ||
            view.Settings.Filter == Components::DirectionalShadowFilter::Grid3x3;
        if (gridFilter && !view.HasRayTracedMask && shadowMap.IsValid())
        {
            if (auto* instance = services.Spine().PipelineInstanceForFrame(frame))
                if (const auto* resources = instance->FrameResourcesFor(&frame))
                    resources->ForEachBufferBinding(view.ViewId,
                        [&](StringId nameId, const Pipeline::PipelineBufferBindingRG& binding)
                        {
                            if (nameId == kShadowConstantsBindingId)
                            {
                                filter.Constants = binding.Buffer;
                                filter.Offset = binding.Offset;
                                filter.Bytes = binding.Size;
                                filter.Resolution = frame.Graph().ResourceDesc(shadowMap.Id).Width;
                                filter.Quality = static_cast<uint32_t>(view.Settings.Filter);
                            }
                        });
        }
        mask = passes->DeclareMaskPass(frame, view.ViewId, view.Depth, *view.Camera,
            towardLight->v, view.Settings.ScreenSpaceShadowThickness, &filter);
    }
    return mask.IsValid() ? keywords | MaterialKeyword::ScreenSpaceShadows : keywords;
}
} // namespace GameEngine::Engine::Renderer
