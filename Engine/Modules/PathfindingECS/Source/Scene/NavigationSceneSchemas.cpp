#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"

#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationDebugSettings.h"
#include "PathfindingECS/Components/NavigationObstacle.h"
#include "ECS/ECSTemplates.h"
#include "PathfindingECS/NavigationService.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

namespace GameEngine::Scene
{
namespace
{

// ---------------------------------------------------------------------------
// Helpers (same as BuiltInSceneSchemas.cpp local helpers)
// ---------------------------------------------------------------------------

static std::string FormatFloat(float v)
{
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

static bool ParseU32(std::string_view value, uint32_t& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Int)
    {
        if (v.IntValue < 0 || v.IntValue > 0xFFFFFFFFll)
        {
            if (outError)
                *outError = "Value out of range for uint32";
            return false;
        }
        out = static_cast<uint32_t>(v.IntValue);
        return true;
    }
    if (v.Kind == SceneValueKind::Float)
    {
        if (v.FloatValue < 0.0 || v.FloatValue > 4294967295.0)
        {
            if (outError)
                *outError = "Value out of range for uint32";
            return false;
        }
        out = static_cast<uint32_t>(v.FloatValue);
        return true;
    }
    if (outError)
        *outError = "Expected an integer value";
    return false;
}

static bool ParseF32(std::string_view value, float32& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Float)
    {
        out = (float32)v.FloatValue;
        return true;
    }
    if (v.Kind == SceneValueKind::Int)
    {
        out = (float32)v.IntValue;
        return true;
    }
    if (outError)
        *outError = "Expected a numeric value";
    return false;
}

static bool ParseU8(std::string_view value, uint8& out, std::string* outError)
{
    uint32_t tmp = 0;
    if (!ParseU32(value, tmp, outError))
        return false;
    if (tmp > 0xFFu)
    {
        if (outError)
            *outError = "Value out of range for uint8";
        return false;
    }
    out = (uint8)tmp;
    return true;
}

static bool ParseBool(std::string_view value, bool& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind != SceneValueKind::Bool)
    {
        if (outError)
            *outError = "Expected true/false";
        return false;
    }
    out = v.BoolValue;
    return true;
}

// ---------------------------------------------------------------------------
// NavigationAgentSchema
// ---------------------------------------------------------------------------

class NavigationAgentSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "NavigationAgent"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::NavigationAgent>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("NavigationAgent.speed = ") + FormatFloat(c->Speed));
        outLines.push_back(std::string("NavigationAgent.acceleration = ") + FormatFloat(c->Acceleration));
        outLines.push_back(std::string("NavigationAgent.stoppingDistance = ") + FormatFloat(c->StoppingDistance));
        outLines.push_back(std::string("NavigationAgent.radius = ") + FormatFloat(c->Radius));
        outLines.push_back(std::string("NavigationAgent.height = ") + FormatFloat(c->Height));
        outLines.push_back(std::string("NavigationAgent.smoothPaths = ") + (c->SmoothPaths ? "true" : "false"));
        outLines.push_back(std::string("NavigationAgent.avoidanceEnabled = ") + (c->AvoidanceEnabled ? "true" : "false"));
        outLines.push_back(std::string("NavigationAgent.avoidanceQuality = ") + std::to_string(c->AvoidanceQuality));
        outLines.push_back(std::string("NavigationAgent.separationWeight = ") + FormatFloat(c->SeparationWeight));
        outLines.push_back(std::string("NavigationAgent.replanInterval = ") + FormatFloat(c->ReplanInterval));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::NavigationAgent c{};
        if (auto* existing = world.GetComponent<Components::NavigationAgent>(entity))
            c = *existing;

        // Clear runtime state on load.
        c.NavMapIndex = 0;
        c.NavMapGeneration = 0;
        c.AgentId = 0;

        if (property == "speed")
        {
            if (!ParseF32(value, c.Speed, outError))
                return false;
        }
        else if (property == "acceleration")
        {
            if (!ParseF32(value, c.Acceleration, outError))
                return false;
        }
        else if (property == "stoppingdistance")
        {
            if (!ParseF32(value, c.StoppingDistance, outError))
                return false;
        }
        else if (property == "radius")
        {
            if (!ParseF32(value, c.Radius, outError))
                return false;
        }
        else if (property == "height")
        {
            if (!ParseF32(value, c.Height, outError))
                return false;
        }
        else if (property == "smoothpaths")
        {
            if (!ParseBool(value, c.SmoothPaths, outError))
                return false;
        }
        else if (property == "avoidanceenabled")
        {
            if (!ParseBool(value, c.AvoidanceEnabled, outError))
                return false;
        }
        else if (property == "avoidancequality")
        {
            if (!ParseU8(value, c.AvoidanceQuality, outError))
                return false;
        }
        else if (property == "separationweight")
        {
            if (!ParseF32(value, c.SeparationWeight, outError))
                return false;
        }
        else if (property == "replaninterval")
        {
            if (!ParseF32(value, c.ReplanInterval, outError))
                return false;
        }
        else
        {
            return ReportUnknownField(ctx, GetComponentName(), property, outError);
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::NavigationAgent>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::NavigationAgent{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::NavigationAgent>(entity);
        return true;
    }
};

// ---------------------------------------------------------------------------
// NavigationObstacleSchema
// ---------------------------------------------------------------------------

class NavigationObstacleSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "NavigationObstacle"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::NavigationObstacle>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("NavigationObstacle.radius = ") + FormatFloat(c->Radius));
        outLines.push_back(std::string("NavigationObstacle.height = ") + FormatFloat(c->Height));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::NavigationObstacle c{};
        if (auto* existing = world.GetComponent<Components::NavigationObstacle>(entity))
            c = *existing;

        if (property == "radius")
        {
            if (!ParseF32(value, c.Radius, outError))
                return false;
        }
        else if (property == "height")
        {
            if (!ParseF32(value, c.Height, outError))
                return false;
        }
        else
        {
            return ReportUnknownField(ctx, GetComponentName(), property, outError);
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::NavigationObstacle>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::NavigationObstacle{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::NavigationObstacle>(entity);
        return true;
    }
};

// ---------------------------------------------------------------------------
// NavigationDebugSettingsSchema
// ---------------------------------------------------------------------------

class NavigationDebugSettingsSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "NavigationDebugSettings"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::NavigationDebugSettings>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("NavigationDebugSettings.showGrid = ") + (c->ShowGrid ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.showNavMesh = ") + (c->ShowNavMesh ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.showPaths = ") + (c->ShowPaths ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.showAgentRadii = ") + (c->ShowAgentRadii ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.showCellCosts = ") + (c->ShowCellCosts ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.showReservations = ") + (c->ShowReservations ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.gridLineThickness = ") + FormatFloat(c->GridLineThickness));
        outLines.push_back(std::string("NavigationDebugSettings.pathLineThickness = ") + FormatFloat(c->PathLineThickness));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::NavigationDebugSettings c{};
        if (auto* existing = world.GetComponent<Components::NavigationDebugSettings>(entity))
            c = *existing;

        if (property == "showgrid")
        {
            if (!ParseBool(value, c.ShowGrid, outError))
                return false;
        }
        else if (property == "shownavmesh")
        {
            if (!ParseBool(value, c.ShowNavMesh, outError))
                return false;
        }
        else if (property == "showpaths")
        {
            if (!ParseBool(value, c.ShowPaths, outError))
                return false;
        }
        else if (property == "showagentradii")
        {
            if (!ParseBool(value, c.ShowAgentRadii, outError))
                return false;
        }
        else if (property == "showcellcosts")
        {
            if (!ParseBool(value, c.ShowCellCosts, outError))
                return false;
        }
        else if (property == "showreservations")
        {
            if (!ParseBool(value, c.ShowReservations, outError))
                return false;
        }
        else if (property == "gridlinethickness")
        {
            if (!ParseF32(value, c.GridLineThickness, outError))
                return false;
        }
        else if (property == "pathlinethickness")
        {
            if (!ParseF32(value, c.PathLineThickness, outError))
                return false;
        }
        else
        {
            return ReportUnknownField(ctx, GetComponentName(), property, outError);
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::NavigationDebugSettings>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::NavigationDebugSettings{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::NavigationDebugSettings>(entity);
        return true;
    }
};

GE_REGISTER_SCENE_SCHEMA(NavigationAgentSchema)
GE_REGISTER_SCENE_SCHEMA(NavigationObstacleSchema)
GE_REGISTER_SCENE_SCHEMA(NavigationDebugSettingsSchema)

} // namespace
} // namespace GameEngine::Scene
