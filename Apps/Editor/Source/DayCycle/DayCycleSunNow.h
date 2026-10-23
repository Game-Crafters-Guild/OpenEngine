#pragma once

#include "ECS/Entity.h"
#include "Rendering/Sky/SolarPath.h"

namespace GameEngine
{
namespace Components
{
struct SkyEnvironment;
}
namespace ECS
{
class World;
}
namespace Engine::Renderer
{
class RenderServices;
class SkyEnvironmentSystem;
} // namespace Engine::Renderer
} // namespace GameEngine

namespace GameEngine::Editor
{

/// Where the open scene's sun stands now, as the Day Cycle surfaces draw their playhead: the hour
/// the sky renders (the preview hour while one is set), the sun's elevation above the apparent
/// horizon and its half of the day from the sun state the sky published, and the sky's path.
struct DayCycleSunNow
{
    bool HasSky = false;
    float Hours = 12.0f;
    float ElevationDegrees = 90.0f;
    bool DuskHalf = false;
    /// The sky's hour places its sun: false while the sun is set by hand or follows a linked light,
    /// when the hour is only a stored number.
    bool HourPlacesSun = true;
    Rendering::SolarFrame Path{};
};

/// The light `sky` follows: its linked directional light while the light places the sun (the sky's
/// Time of Day does not drive it). Invalid otherwise.
ECS::EntityHandle DayCycleFollowedLight(const ECS::World& world, const Components::SkyEnvironment& sky);

/// The sun now in `world`. `render` and `sky` may be null (a host with no renderer): the elevation
/// and the half then come from the sky's path at its hour.
DayCycleSunNow ReadDayCycleSunNow(ECS::World& world, const Engine::Renderer::RenderServices* render,
                                  const Engine::Renderer::SkyEnvironmentSystem* sky);

} // namespace GameEngine::Editor
