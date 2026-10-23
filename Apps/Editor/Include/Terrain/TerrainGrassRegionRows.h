#pragma once

#include "Types/Types.h"

#include <string>

namespace GameEngine
{
namespace ECS
{
class World;
}
namespace Components
{
struct TerrainGrassEffect;
}
struct InspectorContext;

// What a regional grass multiplier is a percentage OF: the grass settings on the terrain the
// region sits on. Zero means there is no single answer - with no enabled terrain carrying grass,
// or with several that disagree, the absolute value a multiplier produces is ambiguous, so the
// resolver publishes nothing and the inspector shows the percentage alone.
struct TerrainGrassBase
{
    float32 BladeHeight = 0.0f;
    float32 BladesPerSquareMeter = 0.0f;
    float32 GroundDarkening = 0.0f; // [0, 1]
    bool Known = false;             // exactly one grass terrain: the values above are its own
};

// Pure read: no component column is write-stamped, so building the inspector cannot trip the
// modifier bake's change gate.
TerrainGrassBase ResolveTerrainGrassBase(const ECS::World& world);

// A multiplier row's label, carrying the absolute value that multiplier lands on when the base is
// known ("Height (0.36 m)"). The absolute rides the label rather than the field's unit because a
// unit long enough to hold it does not fit beside the number, and it is bracketed rather than
// dash-separated because a spaced hyphen beside a number reads as a minus sign - the same stack
// holds a Height Offset effect whose value really is signed. Recomputed on every change.
std::string GrassMultiplierLabel(const char* name, float32 multiplier, float32 base, const char* unit);

// The Grass Area region's Darken Ground row: the share of the terrain's Darken Ground the region
// keeps, labelled with the absolute it lands on, and a notice when the terrain's amount is 0, which
// leaves the row without effect.
void AddGrassRegionDarkenGroundRow(const InspectorContext& ctx, const Components::TerrainGrassEffect& effect,
                                   const TerrainGrassBase& base);

} // namespace GameEngine
