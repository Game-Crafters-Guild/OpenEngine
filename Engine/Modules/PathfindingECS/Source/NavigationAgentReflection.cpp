// Reflection for NavigationAgent: the Add Component entry, the inspector's reflected rows, the scene
// file and set_component all read this table. The component lives under the module's own include
// tree, which the build-time component scanner does not read, so it is registered here.

#include "PathfindingECS/Components/NavigationAgent.h"

#include "Components/ComponentRegistration.h"

#include <limits>

namespace
{
constexpr auto kRuntimeField = ::GameEngine::ECS::FieldFlags::Transient | ::GameEngine::ECS::FieldFlags::Hidden;
} // namespace

GE_REGISTER_COMPONENT_BEGIN(GameEngine::Components::NavigationAgent)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationAgent, Surface)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationAgent, Speed)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationAgent, Acceleration)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationAgent, StoppingDistance)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationAgent, Radius)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationAgent, Height)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationAgent, SmoothPaths)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationAgent, AvoidanceQuality)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationAgent, SeparationWeight)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationAgent, ReplanInterval)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationAgent, AgentId)
GE_REGISTER_COMPONENT_END(GameEngine::Components::NavigationAgent)

GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationAgent, Speed, 0.0f, std::numeric_limits<float>::infinity())
GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationAgent, Acceleration, 0.0f, std::numeric_limits<float>::infinity())
GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationAgent, StoppingDistance, 0.0f,
                       std::numeric_limits<float>::infinity())
GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationAgent, Radius, 0.01f, std::numeric_limits<float>::infinity())
GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationAgent, Height, 0.01f, std::numeric_limits<float>::infinity())
GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationAgent, AvoidanceQuality, 0.0f, 3.0f)
GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationAgent, SeparationWeight, 0.0f,
                       std::numeric_limits<float>::infinity())
GE_REFLECT_FIELD_RANGE(GameEngine::Components::NavigationAgent, ReplanInterval, 0.0f,
                       std::numeric_limits<float>::infinity())

GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationAgent, Surface,
                         "The entity whose Navigation Surface the agent walks. Empty: the only surface in the scene.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationAgent, Speed, "Meters per second at full speed.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationAgent, Acceleration,
                         "Meters per second squared: how fast the agent reaches its speed and turns.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationAgent, StoppingDistance,
                         "Meters. The agent counts a corner of its path as reached this close to it.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationAgent, Radius,
                         "Meters. The agent's size for avoidance and for the cells it holds on a grid.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationAgent, Height,
                         "Meters. How far above and below the agent a path looks for the surface.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationAgent, SmoothPaths,
                         "Cut the corners of a grid path where the way between them is clear.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationAgent, AvoidanceQuality,
                         "0 to 3. Higher looks at more ways around other agents and costs more.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationAgent, SeparationWeight,
                         "How strongly the agent keeps apart from agents near it.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationAgent, ReplanInterval,
                         "Seconds between solves of a path the agent walks, so it follows changes on the way. 0: "
                         "solve once.")

GE_REFLECT_FIELD_FLAGS(GameEngine::Components::NavigationAgent, AgentId, kRuntimeField)
