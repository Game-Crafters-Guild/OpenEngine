#include "Terrain/TerrainGrassRegionRows.h"

#include "InspectorRegistry.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "ECS/World.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Terrain/TerrainInteractiveModifierEdit.h"
#include "UI/Controls/InspectorNotice.h"

#include <cstdio>
#include <memory>

namespace GameEngine
{

TerrainGrassBase ResolveTerrainGrassBase(const ECS::World& world)
{
    TerrainGrassBase base;
    uint32 count = 0;
    const_cast<ECS::World&>(world)
        .Query<ECS::Read<Components::Terrain>, ECS::Read<Components::TerrainGrass>>()
        .Each([&](const Components::Terrain&, const Components::TerrainGrass& grass)
        {
            ++count;
            base.BladeHeight = grass.TextureGrass ? grass.TextureSize : grass.BladeHeight;
            base.BladesPerSquareMeter =
                grass.TextureGrass ? grass.TextureCardsPerSquareMeter : grass.BladesPerSquareMeter;
            base.GroundDarkening = grass.GroundDarkening;
        });
    if (count != 1)
        return {}; // Ambiguous: publish nothing rather than whichever terrain the query saw last.
    base.Known = true;
    return base;
}

std::string GrassMultiplierLabel(const char* name, float32 multiplier, float32 base, const char* unit)
{
    if (base <= 0.0f)
        return name;
    char text[64];
    std::snprintf(text, sizeof(text), "%s (%g %s)", name,
                  static_cast<double>(multiplier * base), unit);
    return text;
}

void AddGrassRegionDarkenGroundRow(const InspectorContext& ctx, const Components::TerrainGrassEffect& effect,
                                   const TerrainGrassBase& base)
{
    using namespace InspectorDrag;
    using Components::TerrainGrassEffect;
    // The colour map recomposes the whole terrain when the value changes, so a drag holds the
    // terrain's interactive-edit signal, which the colour map waits out, and the map follows once.
    auto arm = std::make_shared<Editor::TerrainInteractiveModifierEditArm>();
    AddComponentMultiplierPercentRow<TerrainGrassEffect>(
        ctx.Parent, effect.GroundDarkeningScale, ctx.World, ctx.Entity, ctx.ChangeNotifications,
        [](TerrainGrassEffect& u, float v) { u.GroundDarkeningScale = Components::ClampTerrainGrassEffectScale(v); },
        [base](float v) { return GrassMultiplierLabel("Darken Ground", v, base.GroundDarkening * 100.0f, "%"); },
        "How much of the terrain's Darken Ground this volume keeps: the soil the grass writes into "
        "the color map under its blades. 100% leaves it alone, 0% keeps the soil out of the volume, "
        "for a path through a field. Applies at the next bake.",
        {}, [arm](bool previewing) { previewing ? arm->Arm() : arm->Release(); });
    if (base.Known && base.GroundDarkening <= 0.0f)
        ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
            "Darken Ground on the terrain is 0%, so this row has no effect. Set Darken Ground in "
            "the terrain's Grass settings."));
}

} // namespace GameEngine
