#include "Terrain/TerrainInspector.h"

#include "InspectorRegistry.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Rendering/Sky/SkyPlanet.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "UI/Controls/EnumField.h"
#include "UI/Controls/FloatField.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

namespace GameEngine
{
namespace
{

constexpr float kMinShare = 0.001f;
constexpr float kMinHeightMeters = 0.01f;
constexpr float kNoDefault = std::numeric_limits<float>::quiet_NaN();
constexpr int kCaptionSignificantDigits = 3;
constexpr int kAutoHeightDecimalPlaces = 1;

float CurrentHeightMeters(ECS::World& world, ECS::EntityHandle entity, const Components::Terrain& terrain)
{
    return TerrainECS::AtmosphereHeightMeters(terrain, world.GetComponent<Components::TerrainPlanetRelief>(entity));
}

// A caption number to three significant digits. Whole numbers from 100 up keep every digit, grouped as
// the fields group them: 31.4, 0.424, 848, 67 804.
std::string FormatCaptionNumber(float value)
{
    const float wholeFrom = std::pow(10.0f, static_cast<float>(kCaptionSignificantDigits - 1));
    const float magnitude = std::fabs(value);
    if (magnitude >= wholeFrom || magnitude == 0.0f)
        return FormatFixedAdaptive(std::round(value));
    const float scale = wholeFrom / std::pow(10.0f, std::floor(std::log10(magnitude)));
    return FormatFixedAdaptive(std::round(value * scale) / scale);
}

// The height in meters, as the caption beside a share.
void ShowMeters(FloatField* field, float meters)
{
    field->SetSuffix("= " + FormatCaptionNumber(meters) + " m");
}

// The share of the radius, as the caption beside a height in meters.
void ShowShare(FloatField* field, float meters, float planetRadius)
{
    const float share = planetRadius > 0.0f ? meters / planetRadius : 0.0f;
    field->SetSuffix("= " + FormatCaptionNumber(share) + " of radius");
}

// Sets the mode and stores the current height in the new mode's unit, so the sky does not change.
void SwitchAtmosphereMode(Components::Terrain& terrain, Components::TerrainAtmosphereMode mode, float heightMeters)
{
    terrain.AtmosphereMode = mode;
    if (mode == Components::TerrainAtmosphereMode::ShareOfRadius && terrain.PlanetRadius > 0.0f)
        terrain.AtmosphereShare = std::clamp(heightMeters / terrain.PlanetRadius, kMinShare, 1.0f);
    if (mode == Components::TerrainAtmosphereMode::HeightMeters)
        terrain.AtmosphereHeight = std::max(heightMeters, kMinHeightMeters);
}

void OnAtmosphereModeChosen(const InspectorContext& ctx, Components::TerrainAtmosphereMode chosen)
{
    const auto* terrain = ctx.World->GetComponent<Components::Terrain>(ctx.Entity);
    if (!terrain || terrain->AtmosphereMode == chosen)
        return;
    const float heightMeters = CurrentHeightMeters(*ctx.World, ctx.Entity, *terrain);
    InspectorDrag::CommitComponentWithUndo<Components::Terrain>(
        ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, "Change Atmosphere Mode",
        [chosen, heightMeters](Components::Terrain& t) { SwitchAtmosphereMode(t, chosen, heightMeters); });
    if (ctx.RequestInspectorRefresh)
        ctx.RequestInspectorRefresh();
}

constexpr EnumEntry<Components::TerrainAtmosphereMode> kAtmosphereModes[] = {
    {Components::TerrainAtmosphereMode::Auto, "Auto"},
    {Components::TerrainAtmosphereMode::ShareOfRadius, "Share of Radius"},
    {Components::TerrainAtmosphereMode::HeightMeters, "Height in Meters"},
};

void AddModeRow(UIElement* parent, const InspectorContext& ctx, Components::TerrainAtmosphereMode mode)
{
    EnumField<Components::TerrainAtmosphereMode>* modes = InspectorUI::AddEnumRow(parent, "Atmosphere Height",
        kAtmosphereModes, mode,
        "How the atmosphere height is set. Auto sets it from the planet's highest ground: the air is about 11 "
        "times as tall as the highest peak, and never thinner, for its size, than Earth's air. "
        "Share of Radius keeps the look when the radius changes. Height in Meters sets the height directly.");
    modes->SetOnValueChanged([ctx](Components::TerrainAtmosphereMode chosen) { OnAtmosphereModeChosen(ctx, chosen); });
}

// The share drives: the field is the share and the caption the meters it gives.
void AddShareRow(UIElement* parent, const InspectorContext& ctx, const Components::Terrain& terrain)
{
    auto field = std::make_shared<FloatField*>(nullptr);
    auto [preview, commit] = InspectorDrag::MakeComponentInteractiveHandlers<Components::Terrain, float>(
        ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, "Change Atmosphere Height",
        [](Components::Terrain& t, float share) { t.AtmosphereShare = share; }, {});
    const float radius = terrain.PlanetRadius;
    *field = InspectorDrag::AddFloatRowWithDrag(parent, "Height (share of radius)", terrain.AtmosphereShare,
        [preview, field, radius](float share) { preview(share); ShowMeters(*field, share * radius); },
        [commit, field, radius](float share) { commit(share); ShowMeters(*field, share * radius); },
        Rendering::SkyPlanet::kEarthAtmosphereShare,
        "Height of the atmosphere as a share of the planet's radius. 0.0157 is Earth's (100 km on 6360 km). "
        "The sky looks the same on a planet of any radius. Keep it above the terrain's highest point.",
        kMinShare, 1.0f);
    (*field)->SetSuffixRole(SuffixRole::Caption);
    ShowMeters(*field, terrain.AtmosphereShare * radius);
}

// The meters drive: the field is the height in meters and the caption the share it is.
void AddMetersRow(UIElement* parent, const InspectorContext& ctx, const Components::Terrain& terrain)
{
    auto field = std::make_shared<FloatField*>(nullptr);
    auto [preview, commit] = InspectorDrag::MakeComponentInteractiveHandlers<Components::Terrain, float>(
        ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, "Change Atmosphere Height",
        [](Components::Terrain& t, float meters) { t.AtmosphereHeight = meters; }, {});
    const float radius = terrain.PlanetRadius;
    *field = InspectorDrag::AddFloatRowWithDrag(parent, "Height (m)", terrain.AtmosphereHeight,
        [preview, field, radius](float meters) { preview(meters); ShowShare(*field, meters, radius); },
        [commit, field, radius](float meters) { commit(meters); ShowShare(*field, meters, radius); },
        kNoDefault,
        "Height of the atmosphere's top above the planet's radius, in meters. Keep it above the terrain's "
        "highest point.",
        kMinHeightMeters, std::max(kMinHeightMeters, radius));
    (*field)->SetSuffixRole(SuffixRole::Caption);
    ShowShare(*field, terrain.AtmosphereHeight, radius);
}

// Auto drives: the height is derived, so the field is read-only.
void AddAutoRow(UIElement* parent, const InspectorContext& ctx, const Components::Terrain& terrain)
{
    const float meters = CurrentHeightMeters(*ctx.World, ctx.Entity, terrain);
    FloatField* field = InspectorDrag::AddFloatRowWithDrag(parent, "Height (m)", meters,
        [](float) {}, [](float) {}, kNoDefault,
        "Set from the planet's highest ground: the air is about 11 times as tall as the highest peak. Pick "
        "Share of Radius or Height in Meters to set it yourself.");
    field->SetEnabled(false);
    field->SetFixedDecimalPlaces(kAutoHeightDecimalPlaces);
    field->SetSuffixRole(SuffixRole::Caption);
    ShowShare(field, meters, terrain.PlanetRadius);
}

} // namespace

void AddPlanetAtmosphereRows(UIElement* parent, const InspectorContext& ctx)
{
    const auto* terrain = ctx.World ? ctx.World->GetComponent<Components::Terrain>(ctx.Entity) : nullptr;
    if (!parent || !terrain)
        return;
    AddModeRow(parent, ctx, terrain->AtmosphereMode);
    switch (terrain->AtmosphereMode)
    {
    case Components::TerrainAtmosphereMode::Auto:
        AddAutoRow(parent, ctx, *terrain);
        break;
    case Components::TerrainAtmosphereMode::ShareOfRadius:
        AddShareRow(parent, ctx, *terrain);
        break;
    case Components::TerrainAtmosphereMode::HeightMeters:
        AddMetersRow(parent, ctx, *terrain);
        break;
    }
}

} // namespace GameEngine
