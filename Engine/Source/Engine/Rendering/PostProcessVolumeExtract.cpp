#include "Engine/Rendering/PostProcessVolumeExtract.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"

#include <cstring>

namespace GameEngine {
namespace Engine {
namespace Renderer {
namespace {

struct WorldTimeOfDayState
{
    float hours = 12.0f;
    float dayKeyTimesHours[4] = {0.0f, 6.0f, 12.0f, 18.0f};
};

WorldTimeOfDayState FindWorldTimeOfDayState(GameEngine::ECS::World& world)
{
    WorldTimeOfDayState state{};
    bool found = false;
    world.Query<
             GameEngine::ECS::Read<GameEngine::Components::SkyEnvironment>>()
        .Each([&](GameEngine::ECS::EntityHandle,
                  const GameEngine::Components::SkyEnvironment& sky)
              {
                  if (found)
                      return;
                  state.hours = sky.TimeOfDayHours;
                  state.dayKeyTimesHours[0] = sky.DayKeyTimesHours.Midnight;
                  state.dayKeyTimesHours[1] = sky.DayKeyTimesHours.Dawn;
                  state.dayKeyTimesHours[2] = sky.DayKeyTimesHours.Midday;
                  state.dayKeyTimesHours[3] = sky.DayKeyTimesHours.Sunset;
                  found = true; });
    return state;
}

} // namespace

PostProcessExtractContext MakePostProcessExtractContext(ECS::World& world, RenderServices* services)
{
    const WorldTimeOfDayState worldTimeOfDay = FindWorldTimeOfDayState(world);
    PostProcessExtractContext context{};
    context.Services = services;
    context.TimeOfDayHours = worldTimeOfDay.hours;
    std::memcpy(context.DayKeyTimesHours, worldTimeOfDay.dayKeyTimesHours, sizeof(context.DayKeyTimesHours));
    const auto* sky = services ? services->GetFeature<SkyRenderFeature>() : nullptr;
    if (sky && sky->HasActiveSettings())
        context.SunState = sky->GetSunState();
    return context;
}

} // namespace Renderer
} // namespace Engine
} // namespace GameEngine
