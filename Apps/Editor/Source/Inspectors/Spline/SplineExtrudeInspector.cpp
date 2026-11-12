#include "Inspectors/Spline/SplineInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorComponent/InspectorComponentSection.h"
#include "Inspectors/InspectorOverrideRows.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Toggle.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Spline/SplineExtrude.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "SplineRecipes/RecipeControllers.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
constexpr std::array<std::string_view, 1> kVergeMaterialField = {"VergeMaterial"};
constexpr const char* kVergeOffReason = "Turn on Build Verge to build a curb along the road.";
constexpr const char* kVergeMaterialEmptyText = "The road's material";
constexpr const char* kSurfaceMaterialEmptyText = "The built-in material";
// The verge's width and height reach 1 m on the slider; a typed value takes the field's whole range.
constexpr float kVergeSliderMaxMeters = 1.0f;
constexpr const char* kUndoPrefix = "Change Spline Extrude ";
constexpr std::array<std::string_view, 1> kSurfaceFields = {"Material"};
constexpr std::array<std::string_view, 5> kPlacementFields = {"ConformMode", "ConformTarget", "LateralOffset",
                                                              "VerticalOffset", "GroundAuthority"};
constexpr std::array<std::string_view, 2> kShadowFields = {"CastShadows", "ReceiveShadows"};

// The values that decide which rows show and which are active. A change of any rebuilds the inspector.
struct RowLayout
{
    SplineExtrudeWidthMode WidthMode = SplineExtrudeWidthMode::Channel;
    SplineExtrudeProfile Profile = SplineExtrudeProfile::Bevel;
    bool Verge = false;

    bool operator==(const RowLayout&) const = default;
};

RowLayout LayoutOf(const SplineExtrude& recipe)
{
    return {recipe.WidthMode, recipe.Profile, recipe.Verge};
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

// Sets Verge on the selection as one undo step. A verge switched on with an empty Material takes the
// curb material of the first paving along its road.
void SetVerge(const InspectorContext& context, bool on)
{
    ECS::World* world = context.World;
    const std::vector<ECS::EntityHandle> others = InspectorDrag::GetAdditionalEntities(context);
    const std::string name = std::string(kUndoPrefix) + "Build Verge";
    std::optional<Editor::UndoRedoService::InteractiveEdit> edit;
    if (context.Undo)
        edit.emplace(context.Undo->BeginInteractiveEdit(
            name, InspectorDrag::MakeMultiComponentSnapshotTarget<SplineExtrude>(world, context.Entity, others,
                                                                  context.ChangeNotifications, name)));
    const SplineRecipes::RecipeControllers* controllers = RunningRecipeControllers();
    std::vector<ECS::EntityHandle> roads = {context.Entity};
    roads.insert(roads.end(), others.begin(), others.end());
    for (ECS::EntityHandle road : roads)
    {
        auto apply = [world, road, on, controllers](SplineExtrude& recipe)
        {
            recipe.Verge = on;
            if (on && controllers)
                controllers->PrefillVergeMaterial(*world, road, recipe);
        };
        InspectorDrag::ApplyComponentEdit<SplineExtrude>(world, road, context.ChangeNotifications, edit.has_value(), apply);
    }
    if (edit)
        edit->Commit();
}

// The Verge section: the Build Verge switch, then the verge's width, height and material, inactive
// with the switch off. With the switch on, an empty Material reads as the road's material, which it draws.
void AddVergeSection(const InspectorContext& context, const SplineExtrude& shown)
{
    InspectorContext rows = context;
    rows.Parent = InspectorUI::AddComponentSection(context.Parent, "SplineExtrude/Verge", "Verge")->GetContentContainer();
    const ECS::ComponentTypeId typeId = ECS::GetComponentTypeId<SplineExtrude>();
    const ECS::FieldInfo* vergeField = ECS::ComponentFieldRegistry::FindField(typeId, "Verge");
    if (!vergeField)
        return;
    Toggle* toggle = InspectorDrag::AddToggleRow(
        rows.Parent, FieldDisplayName(*vergeField), shown.Verge,
        [context](bool on) { SetVerge(context, on); }, InspectorDrag::ReflectedTooltip(typeId, "Verge"));
    BindFieldRow(context, typeId, InspectorUI::RowOfControl(toggle), "Verge");
    std::vector<UIElement*> sized = {
        InspectorDrag::AddReflectedLengthSliderRow(rows, "VergeWidth", &SplineExtrude::VergeWidth, shown, kUndoPrefix,
                                                   kVergeSliderMaxMeters),
        InspectorDrag::AddReflectedLengthSliderRow(rows, "VergeHeight", &SplineExtrude::VergeHeight, shown,
                                                   kUndoPrefix, kVergeSliderMaxMeters)};
    RenderReflectedFieldRows(rows, typeId, kVergeMaterialField);
    sized.push_back(ShowAssetFieldEmptyAs(*rows.Parent, shown.Verge ? kVergeMaterialEmptyText : ""));
    if (shown.Verge)
        return;
    for (UIElement* row : sized)
        InspectorUI::DisableRow(row, kVergeOffReason);
}

// The rows of every width mode, in the sections and order of the paving's inspector.
void AddSharedSections(const InspectorContext& context)
{
    const ECS::ComponentTypeId typeId = ECS::GetComponentTypeId<SplineExtrude>();
    InspectorContext rows = context;
    rows.Parent = InspectorUI::AddComponentSection(context.Parent, "SplineExtrude/Surface", "Surface")->GetContentContainer();
    RenderReflectedFieldRows(rows, typeId, kSurfaceFields);
    ShowAssetFieldEmptyAs(*rows.Parent, kSurfaceMaterialEmptyText);
    rows.Parent =
        InspectorUI::AddComponentSection(context.Parent, "SplineExtrude/Placement", "Placement")->GetContentContainer();
    RenderReflectedFieldRows(rows, typeId, kPlacementFields);
    rows.Parent = InspectorUI::AddComponentSection(context.Parent, "SplineExtrude/Shadows", "Shadows")->GetContentContainer();
    RenderReflectedFieldRows(rows, typeId, kShadowFields);
}

// Fit To Banks fills a region, so the cross-section rows and the verge do not apply; each profile
// shows only its own shape rows.
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
    InspectorContext shape = context;
    shape.Parent = InspectorUI::AddComponentSection(context.Parent, "SplineExtrude/Shape", "Shape")->GetContentContainer();
    RenderReflectedFieldRows(shape, typeId, kWidthModeField);
    if (layout.WidthMode == SplineExtrudeWidthMode::FitToBanks)
    {
        RenderReflectedFieldRows(shape, typeId, kFillFields);
        RenderReflectedFieldRows(shape, typeId, kEdgeDropField);
    }
    else
    {
        RenderReflectedFieldRows(shape, typeId, kCrossSectionFields);
        if (layout.Profile == SplineExtrudeProfile::Crown)
            RenderReflectedFieldRows(shape, typeId, kCrownFields);
        else
            RenderReflectedFieldRows(shape, typeId, kBevelFields);
        RenderReflectedFieldRows(shape, typeId, kChannelEndFields);
        AddVergeSection(context, *recipe);
    }
    AddSharedSections(context);
    AddLayoutFollower(context, layout);
}

} // namespace

void RegisterSplineExtrudeInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<SplineExtrude>(BuildSplineExtrudeInspector);
}

} // namespace GameEngine
