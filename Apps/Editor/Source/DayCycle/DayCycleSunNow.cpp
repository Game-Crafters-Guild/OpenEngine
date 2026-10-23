#include "DayCycle/DayCycleSunNow.h"

#include "Assets/DayCycleProfile.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "DayCycle/DayCycleAxis.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"

#include <numbers>

namespace GameEngine::Editor
{

ECS::EntityHandle DayCycleFollowedLight(const ECS::World& world, const Components::SkyEnvironment& sky)
{
    if (sky.TimeOfDayDrivesSunLight || !world.IsValid(sky.SunLight))
        return {};
    const auto* light = world.GetComponent<Components::Light>(sky.SunLight);
    return light && light->Type == Components::LightType::Directional ? sky.SunLight : ECS::EntityHandle{};
}

DayCycleSunNow ReadDayCycleSunNow(ECS::World& world, const Engine::Renderer::RenderServices* render,
                                  const Engine::Renderer::SkyEnvironmentSystem* sky)
{
    DayCycleSunNow now;
    const ECS::EntityHandle skyEntity = Components::SkySunIlluminance::RenderedSky(world);
    const auto* component = skyEntity.IsValid() ? world.GetComponent<Components::SkyEnvironment>(skyEntity) : nullptr;
    now.HasSky = component != nullptr;
    now.HourPlacesSun = component && component->AutoSunMoon && !DayCycleFollowedLight(world, *component).IsValid();
    const Components::SkyEnvironment path = component ? *component : Components::SkyEnvironment{};
    now.Path = DayCyclePathOf(path);
    const std::optional<float> preview = sky ? sky->GetPreviewHour(world.GetWorldId()) : std::nullopt;
    now.Hours = preview.value_or(path.TimeOfDayHours);

    const auto* feature = render ? render->GetFeature<Engine::Renderer::SkyRenderFeature>() : nullptr;
    if (now.HasSky && feature && feature->HasActiveSettings())
    {
        const Rendering::SkySunState& state = feature->GetSunState();
        now.ElevationDegrees = state.SunApparentElevation * (180.0f / std::numbers::pi_v<float>);
        now.DuskHalf = IsDuskHalf(state);
        return now;
    }
    now.ElevationDegrees = ElevationDegreesAtHour(now.Path, now.Hours);
    now.DuskHalf = IsDuskHour(now.Path, now.Hours);
    return now;
}

} // namespace GameEngine::Editor
