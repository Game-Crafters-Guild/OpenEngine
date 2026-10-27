#include "Inspectors/Spline/SplineInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"

#include "Components/Spline/SplineExtrude.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <array>
#include <string_view>

namespace GameEngine
{
namespace
{

using Components::SplineExtrude;
using Components::SplineExtrudeProfile;
using Components::SplineExtrudeWidthMode;

constexpr std::array<std::string_view, 1> kWidthModeField = {"WidthMode"};
constexpr std::array<std::string_view, 3> kCrossSectionFields = {"Profile", "Width", "WidthScale"};
constexpr std::array<std::string_view, 2> kBevelFields = {"EdgeDrop", "EdgeInset"};
// Edge Drop also sinks the tapered ends of a crown.
constexpr std::array<std::string_view, 4> kCrownFields = {"EdgeDrop", "CrownRise", "ShoulderWidth", "ShoulderDrop"};
constexpr std::array<std::string_view, 3> kChannelEndFields = {"EndTaperMetres", "TilesPerMetreU", "TilesPerMetreV"};
constexpr std::array<std::string_view, 2> kFillFields = {"MaxHalfWidth", "FloorHeight"};
constexpr std::array<std::string_view, 1> kEdgeDropField = {"EdgeDrop"};
constexpr std::array<std::string_view, 8> kSharedFields = {"ConformMode",   "ConformTarget", "LateralOffset",
                                                           "VerticalOffset", "Material",     "GroundAuthority",
                                                           "CastShadows",   "ReceiveShadows"};

// The two values that decide which rows show. A change of either rebuilds the inspector.
struct RowLayout
{
    SplineExtrudeWidthMode WidthMode = SplineExtrudeWidthMode::Channel;
    SplineExtrudeProfile Profile = SplineExtrudeProfile::Bevel;

    bool operator==(const RowLayout&) const = default;
};

RowLayout LayoutOf(const SplineExtrude& recipe)
{
    return {recipe.WidthMode, recipe.Profile};
}

void AddLayoutFollower(const InspectorContext& context, RowLayout built)
{
    if (!context.SimulationRefreshCallbacks)
        return;
    context.SimulationRefreshCallbacks->push_back(
        [world = context.World, entity = context.Entity, refresh = context.RequestInspectorRefresh, built,
         requested = false]() mutable
        {
            const auto* recipe = world->GetComponent<SplineExtrude>(entity);
            if (!recipe || requested || !refresh || LayoutOf(*recipe) == built)
                return;
            requested = true;
            refresh();
        });
}

// Fit To Banks fills a region, so the cross-section rows do not apply; each profile shows only
// its own shape rows.
void BuildSplineExtrudeInspector(const InspectorContext& context)
{
    if (!context.World)
        return;
    const auto* recipe = context.World->GetComponent<SplineExtrude>(context.Entity);
    if (!recipe)
        return;
    const RowLayout layout = LayoutOf(*recipe);
    const ECS::ComponentTypeId typeId = ECS::GetComponentTypeId<SplineExtrude>();

    AddSplineRecipeBuildStateNotice(context);
    RenderReflectedFieldRows(context, typeId, kWidthModeField);
    if (layout.WidthMode == SplineExtrudeWidthMode::FitToBanks)
    {
        RenderReflectedFieldRows(context, typeId, kFillFields);
        RenderReflectedFieldRows(context, typeId, kEdgeDropField);
    }
    else
    {
        RenderReflectedFieldRows(context, typeId, kCrossSectionFields);
        if (layout.Profile == SplineExtrudeProfile::Crown)
            RenderReflectedFieldRows(context, typeId, kCrownFields);
        else
            RenderReflectedFieldRows(context, typeId, kBevelFields);
        RenderReflectedFieldRows(context, typeId, kChannelEndFields);
    }
    RenderReflectedFieldRows(context, typeId, kSharedFields);
    AddLayoutFollower(context, layout);
}

} // namespace

void RegisterSplineExtrudeInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<SplineExtrude>(BuildSplineExtrudeInspector);
}

} // namespace GameEngine
