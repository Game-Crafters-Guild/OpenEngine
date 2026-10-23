#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"

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
        outLines.push_back(std::string("NavigationObstacle.carve = ") + (c->Carve ? "true" : "false"));
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
        else if (property == "carve")
        {
            if (!ParseBool(value, c.Carve, outError))
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

GE_REGISTER_SCENE_SCHEMA(NavigationObstacleSchema)
GE_REGISTER_SCENE_SCHEMA(NavigationDebugSettingsSchema)

} // namespace
} // namespace GameEngine::Scene
