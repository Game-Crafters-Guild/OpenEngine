#include "Terrain/TerrainColorZoneInspector.h"

#include "InspectorRegistry.h"

#include "Components/Terrain/TerrainModifiers.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/TextField.h"

#include <algorithm>
#include <string>

namespace GameEngine
{

void RegisterTerrainColorZoneInspector()
{
    using namespace InspectorDrag;
    using ColorZone = Components::TerrainColorZone;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;
        const auto* zone = ctx.World->GetComponent<ColorZone>(ctx.Entity);
        if (!zone)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        const ColorZone defaults{};

        AddComponentFloatRowWithDrag<ColorZone>(ctx.Parent, "Extent X", zone->ExtentX, w, e, n, undo,
            "Change Color Zone Extent X", [](ColorZone& u, float v) { u.ExtentX = std::max(0.0f, v); },
            defaults.ExtentX,
            "Half the zone's width along its local X axis, in meters, before the entity's scale. The "
            "painted color spans the whole width.", {}, 0.0f);
        AddComponentFloatRowWithDrag<ColorZone>(ctx.Parent, "Extent Z", zone->ExtentZ, w, e, n, undo,
            "Change Color Zone Extent Z", [](ColorZone& u, float v) { u.ExtentZ = std::max(0.0f, v); },
            defaults.ExtentZ,
            "Half the zone's depth along its local Z axis, in meters, before the entity's scale.", {}, 0.0f);
        AddComponentFloatRowWithDrag<ColorZone>(ctx.Parent, "Falloff", zone->Falloff, w, e, n, undo,
            "Change Color Zone Falloff", [](ColorZone& u, float v) { u.Falloff = std::max(0.0f, v); },
            defaults.Falloff,
            "Meters beyond the zone's edge over which its color fades out. 0 is a hard edge.", {}, 0.0f);
        AddComponentFloatRowWithDrag<ColorZone>(ctx.Parent, "Priority", zone->Priority, w, e, n, undo,
            "Change Color Zone Priority", [](ColorZone& u, float v) { u.Priority = v; }, defaults.Priority,
            "Order among color zones: a higher priority composes later, over the lower ones and over "
            "the soil the grass writes.");

        const std::string payloadText =
            zone->PayloadRef.IsNull() ? std::string("None") : zone->PayloadRef.ToGuid().ToString();
        auto* payloadField = InspectorUI::AddTextRow(ctx.Parent, "Painted Color", payloadText,
            "The color and weight painted per texel, stored in the zone's .tzone file beside the scene.");
        InspectorUI::DisableRowOfControl(payloadField,
            "The painted color is stored in the zone's .tzone file beside the scene, not edited here.");
    };

    InspectorRegistry::Get().RegisterComponentInspector<ColorZone>(std::move(fn));
}

} // namespace GameEngine
