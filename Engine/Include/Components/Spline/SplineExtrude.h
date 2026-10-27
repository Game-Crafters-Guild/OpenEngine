#pragma once

#include "Components/AssetRef.h"
#include "Components/Spline/SplinePlacement.h"
#include "Types/Types.h"

#include <limits>
#include <type_traits>

namespace GameEngine::Components
{

// Which generated cross-section is swept. A sibling of the placement recipe's
// mesh pools, and the same kind of authored choice: the shape decides whether
// the run reads as a path or a road. A wall is its own recipe (SplineWall).
//
// The geometry module also supports a Custom point list, which has no authoring
// surface yet and so carries no field here.
enum class SplineExtrudeProfile : int32
{
    // path/water: an open strip that hugs the ground. EdgeDrop and EdgeInset
    // both zero collapse it to a flat ribbon.
    Bevel = 0,
    // road: a cambered carriageway with shoulders.
    Crown = 1,
};

// How many profiles exist, maintained by hand.
//
// NOT an enumerator: this enum is reflected into the inspector's dropdown, and a
// `Count` member would offer itself there as a profile an author could pick —
// which is why the ResolvedEffect::Kind::Count idiom does not transfer here.
//
// BE CLEAR ABOUT WHAT THIS DOES AND DOES NOT DO. It is a hand-written number, so
// adding an enumerator does NOT move it and does NOT stop the build; the
// static_assert in ResolveGroundAuthority is a tripwire for someone editing this
// file, not a forcing function. The gate that actually cannot be forgotten is
// SplineGroundAuthority.EveryDeclaredProfileDecidesWhatItDoesToTheGround, which
// walks the enum table the build-time scanner emits — that table grows on its
// own — and fails naming the new profile and the value it silently resolved to.
// Verified by adding a fourth profile: the build stayed green and the test
// failed on both the count and the name.
inline constexpr int32 kSplineExtrudeProfileCount = 2;

// How the spline's width channel scales the cross-section. A road that widens
// should not get taller, but a rampart that widens probably should.
enum class SplineExtrudeWidthScale : int32
{
    LateralOnly = 0,
    Uniform = 1,
    None = 2,
};

// Where each ring's half-widths come from.
//
// Channel is an AUTHORED width: the spline's width channel sets both sides
// equally, so the run is as wide as the curve says wherever it goes.
//
// FitToBanks has no width at all: it FILLS. The region the water occupies is
// flood-filled over the terrain from the centreline's own stations, bounded by
// where the ground rises to meet the waterline, and meshed as an area rather
// than swept as a cross-section.
//
// That is a structural difference, not a better measurement. A swept strip is a
// thickened curve, and the shape water takes routinely is not one: it folds
// through itself at a tight bend, it reaches into pockets that lie on no
// station's perpendicular, and a hairpin's two legs over one basin are a single
// pool rather than two overlapping surfaces. A region has none of those failure
// modes available to it — connectivity decides the extent, and the ground
// decides connectivity.
//
// The curve therefore says only WHERE the water is and HOW HIGH it stands. The
// waterline elevation is the station height, and the station height is the
// draped-or-authored altitude plus VerticalOffset — the fill reads that height,
// never writes it. Carving the bed deeper widens the region it finds without
// moving the water surface down with it.
//
// The cross-section fields (Width, Profile, EdgeInset, WidthScale) and
// EndTaperMetres have no meaning for a region and are ignored in this mode;
// they remain Channel's controls on a shared component.
enum class SplineExtrudeWidthMode : int32
{
    Channel = 0,
    FitToBanks = 1,
};

// "No floor height" for SplineExtrude::FloorHeight. See that field.
inline constexpr float32 kNoFloorHeight = std::numeric_limits<float32>::lowest();

// What an extrude does to the terrain under it. The recipe declares it, and the editor makes the
// terrain effects that apply it: Grades is a pooled flatten effect, and Owns adds the ground claim
// on the volume that a hand-authored route carries.
enum class SplineGroundAuthority : int32
{
    // Read the choice from the cross-section, where the author already says what kind of extrude
    // this is (ResolveGroundAuthority below).
    Auto = 0,
    // Leave the terrain alone; the extrude lies on the ground it finds.
    None = 1,
    // The terrain follows the heights of the extrude and fades out to each side.
    Grades = 2,
    // Grades, and owns the ground it graded, so other routes stop at its edge. It does not defer to
    // claims: its pool reads ownership at the flush, after its own claim is in place, so a road
    // that defers would hold itself off its own route.
    Owns = 3,
};


// Generates one continuous mesh along the sibling SplineComponent's curve.
//
// Pure authored data, exactly as its siblings: the editor-side
// SplineExtrudeController samples the spline, conforms against the scene,
// generates the geometry and spawns runtime-only chunk entities — the scene
// serializes this recipe, never the generated mesh.
//
// Where placement instances a sculpted kit piece, this generates the surface
// outright. That is the whole difference in behaviour: rings share their
// vertices, so a run has no piece ends to bury in a slope, no footprints to
// overlap, and no quantised count to jump when its length changes.
struct SplineExtrude
{
    // @ge-tooltip The shape of the cross-section. Bevel is a flat strip whose edges drop, for a path or a stream. Crown is a road that is higher at the center, with a shoulder at each side.
    SplineExtrudeProfile Profile = SplineExtrudeProfile::Bevel;
    // Lateral extent of the cross-section in metres, before the width channel
    // scales it.
    // @ge-tooltip The width of the cross-section in meters, before the spline's width channel scales it.
    float32 Width = 2.0f;
    // How far the outer edge drops below the walkable surface. It is also how
    // far a tapered end and the rim of a filled region sink. EdgeInset, Bevel
    // only: how far in from that edge the surface stays flat.
    // @ge-tooltip How far the outer edge drops below the surface, in meters. A tapered end sinks by the same distance. With Fit To Banks, how far the edge of the water sinks into the bank.
    float32 EdgeDrop = 0.1f;
    // @ge-tooltip How far in from the edge the surface stays flat, in meters.
    float32 EdgeInset = 0.15f;
    // Crown only: camber rise at the centreline, and the shoulder beyond Width.
    // @ge-tooltip How much higher the center line is than the edges, in meters.
    float32 CrownRise = 0.12f;
    // @ge-tooltip The width of the shoulder at each side of the road, in meters.
    float32 ShoulderWidth = 0.5f;
    // @ge-tooltip How far each shoulder drops below the edge of the road, in meters.
    float32 ShoulderDrop = 0.2f;

    // @ge-tooltip What the spline's width channel changes. Lateral Only makes the cross-section wider and keeps its height. Uniform scales its width and height. None ignores the width channel.
    SplineExtrudeWidthScale WidthScale = SplineExtrudeWidthScale::LateralOnly;
    // @ge-tooltip Channel sweeps the cross-section along the spline. Fit To Banks fills the area that the ground holds at the height of the spline, for water such as a river or a lake.
    SplineExtrudeWidthMode WidthMode = SplineExtrudeWidthMode::Channel;
    // FitToBanks only: the search radius around the spline, in metres. The fill
    // stamps and floods only corners within this distance of a station, so it
    // bounds the cost (the reach is a band along the route, not the whole
    // terrain) and how far the water may reach.
    //
    // It bounds the SEARCH and is never a wall. Water that reaches the edge of
    // that reach over ground below itself is left OUT rather than cut off in
    // mid-air:
    // nothing inside the reach shows that anything holds it there. Those samples
    // are reported as UnseenBankCorners — raise this until the banks are inside
    // it — which is a different instruction from a channel that is simply too
    // shallow, so the fill counts the two separately.
    // @ge-label Search Reach
    // @ge-tooltip How far from the spline the fill looks for banks, in meters. Increase it when the build reports banks it cannot see.
    float32 MaxHalfWidth = 12.0f;
    // FitToBanks only: an altitude the water surface never falls below, in world
    // metres. A river mouth reaching a sea at a known level then merges into the
    // shoreline instead of interpenetrating it.
    //
    // OPTIONAL, and disabled by default. The disabled value is the lowest finite
    // float, chosen because it is the ARITHMETIC IDENTITY of the clamp that
    // applies it (the waterline takes the max of itself and this): the disabled
    // state therefore needs no branch and is never compared for equality, so it
    // cannot be broken by a scene round-trip. A NaN sentinel would be far worse
    // than useless here — see the memberwise == note below.
    // @ge-tooltip The lowest world height of the filled surface, in meters. Set it where a river meets a sea or a lake of known height, so that the two surfaces join. None means that the surface has no floor.
    float32 FloorHeight = kNoFloorHeight;
    // Metres of run at EACH open end over which the ring pinches smoothly to
    // nothing and sinks EdgeDrop below the surface, so a run ends in a buried
    // point rather than a square cut across its full cross-section.
    //
    // CHANNEL ONLY. A region fill has no cross-section to pinch, and its end is
    // already the shape the ground gives it — the rim of the bowl the bed was
    // carved into, whose radius is the bowl's rather than a value authored
    // twice. Applying a taper there would reintroduce the very mismatch the fill
    // removes, so FitToBanks IGNORES this field rather than honouring it.
    //
    // Zero (the default) is off and leaves the geometry untouched. A closed
    // loop has no end and ignores it. The tuck depth IS EdgeDrop, so a profile
    // with none — the flat water ribbon — pinches in the surface plane and does
    // not bury its point.
    // @ge-label End Taper
    // @ge-tooltip The length in meters over which each open end narrows to a point and sinks by Edge Drop. 0 keeps the ends square.
    float32 EndTaperMetres = 0.0f;
    // Height-only by default: the sweep already pitches with the draped grade,
    // so the per-station surface normal that a rigid tile needs for its lean is
    // not what orients a ring.
    // @ge-tooltip How the extrude meets the surface under the spline. None keeps the heights of the spline. Height puts the extrude on the surface. Height And Slope also tilts it with the surface.
    SplinePlacementConform ConformMode = SplinePlacementConform::Height;
    // What the conform ray may land on. Scene (the default) is every pickable
    // surface; TerrainOnly makes scenery transparent so a swept ribbon follows
    // the ground under the props it passes rather than their roofs.
    // @ge-tooltip What the conform ray may land on. Scene takes the nearest surface of any kind, so a ribbon passing under a tree or a bridge is swept over its roof; TerrainOnly makes scenery transparent so the ribbon follows the ground beneath it. Under TerrainOnly the drawn drape line can still ride scene props while the generated geometry follows the terrain under them; the line does not read this setting yet.
    SplineConformTarget ConformTarget = SplineConformTarget::Scene;
    // Metres to the right of travel; negative = left.
    // @ge-tooltip Moves the extrude to the right of the direction of travel, in meters. A negative value moves it to the left.
    float32 LateralOffset = 0.0f;
    // Metres above the conformed surface. A water ribbon wants a small positive
    // lift so it reads as sitting in its bed rather than z-fighting the bank.
    // @ge-tooltip Raises the extrude above the surface it follows, in meters. A water surface needs a small lift to sit in its bed without flicker.
    float32 VerticalOffset = 0.0f;

    // Texture density in world metres, never normalized: U runs along the
    // draped run and V across the cross-section, so a 10 m and a 100 m road get
    // different numbers of paving stones rather than differently-shaped ones.
    //
    // CHANNEL ONLY, for the same reason EndTaperMetres is. A region fill carries
    // a MEASUREMENT in uv0 -- arc along the flow and still-water depth, both in
    // metres -- rather than a texture chart, because a water shader needs to
    // know how deep the water is and no chart can tell it. Texture density for
    // a filled surface is the MATERIAL's to set, in tiles per metre, against
    // those metres. FitToBanks therefore IGNORES these two rather than honouring
    // them.
    // @ge-label Tiles Per Meter U
    // @ge-tooltip How many times the texture repeats per meter along the extrude.
    float32 TilesPerMetreU = 1.0f;
    // @ge-label Tiles Per Meter V
    // @ge-tooltip How many times the texture repeats per meter across the extrude.
    float32 TilesPerMetreV = 1.0f;

    // Replaces the generated geometry's default material. Null keeps the
    // built-in one, so dropping the recipe on a spline renders something on the
    // first try rather than nothing.
    // @ge-tooltip The material of the generated surface. Empty uses the built-in material.
    MaterialRef Material;

    // What this run does to the terrain under it. Auto — the default — reads the
    // answer off Profile and WidthMode, so dropping a road recipe on a spline
    // grades and claims the ground without anyone opening the terrain effects at
    // all. See ResolveGroundAuthority below.
    // @ge-tooltip What the extrude does to the terrain under it. Auto reads it from the cross-section: a path grades the terrain to its heights, a road grades and owns its ground so other routes stop at its edge, and a filled water region leaves the terrain alone. Another value adds its terrain effects to this entity, and you can tune their fields. None removes only the effects that this field added, and you can undo it. While the extrude grades, it adds again a Flatten or Ground Claim of its own that you delete.
    SplineGroundAuthority GroundAuthority = SplineGroundAuthority::Auto;

    // @ge-tooltip The generated surface casts shadows.
    bool CastShadows = true;
    // @ge-tooltip Shadows fall on the generated surface.
    bool ReceiveShadows = true;

    // Memberwise: the rebuild scheduler compares whole recipes, so a
    // hand-written comparison that missed a field would silently stop rebuilds
    // for edits to that field. Defaulted == cannot miss one.
    //
    // A NaN float would make a recipe unequal to ITSELF and re-arm the settle
    // rebuild forever, so the controller compares sanitized recipes only —
    // SanitizeExtrudeRecipe (SplineRecipes/SplineExtrudeSanitize.h) is the single
    // gate.
    bool operator==(const SplineExtrude&) const = default;
};

// True when the recipe measures the ground for ANY reason.
//
// There are two such reasons and they are independent: draping the stations
// (ConformMode) and filling to the banks (WidthMode). Reading only the first
// is the trap this predicate exists to close — a ConformMode=None FitToBanks
// recipe still reads the heightfield over its whole search radius to flood the
// water region, so treating "does not conform" as "does not read the ground"
// leaves it observing no surface term and never re-filling when the bed under
// it is carved.
//
// ONE definition, shared by the terrain-scan skip and the rebuild-settle
// observation, so those two cannot drift into disagreeing about which recipes
// care about the ground.
[[nodiscard]] constexpr bool ReadsSurface(const SplineExtrude& recipe)
{
    return recipe.ConformMode != SplinePlacementConform::None ||
           recipe.WidthMode == SplineExtrudeWidthMode::FitToBanks;
}

// The authority `recipe` actually carries, with Auto resolved. Never Auto.
//
// ONE definition, shared by the provisioning controller, the inspector row that
// shows what Auto came out as, and the tests that hold those two together — a
// second copy of the mapping is how they stop agreeing.
[[nodiscard]] constexpr SplineGroundAuthority ResolveGroundAuthority(const SplineExtrude& recipe)
{
    if (recipe.GroundAuthority != SplineGroundAuthority::Auto)
        return recipe.GroundAuthority;

    // A filled water region READS the ground and must never write it. It is a
    // Bevel exactly as a path is, so the profile ALONE would grade a river's bed
    // to its own waterline — flattening away the basin the fill needs in order to
    // find where the water reaches at all. WidthMode is checked first for that
    // reason, not as a special case bolted onto the profile switch.
    //
    // THE GUARD IS WIDTHMODE AND ONLY WIDTHMODE, which is exact for the water
    // that exists (every Bevel run authored today fills to its banks) and is NOT
    // exact in general: a stream authored at a FIXED width is a Bevel on the
    // Channel path and is indistinguishable here from a footpath, so it resolves
    // to Grades and would be graded to its own surface. Nothing in the recipe
    // separates those two intents, so this does not guess — the provisioning
    // controller warns once when it happens, and the author sets the authority
    // explicitly. Do not "fix" this with a heuristic on materials or offsets.
    if (recipe.WidthMode == SplineExtrudeWidthMode::FitToBanks)
        return SplineGroundAuthority::None;

    // Tripwire, not a gate: it fires for someone who bumps the count above, and
    // stays silent for someone who only adds an enumerator. The test named beside
    // that constant is what catches the second case.
    static_assert(kSplineExtrudeProfileCount == 2,
                  "A new SplineExtrudeProfile must decide what it does to the ground in the "
                  "switch below. Falling through resolves to None, which grades nothing and "
                  "says nothing — the one outcome an author cannot debug.");

    switch (recipe.Profile)
    {
    // road: a carriageway owns the ground it is cut into.
    case SplineExtrudeProfile::Crown:
        return SplineGroundAuthority::Owns;
    // path: grades to its own heights, and gives way to anything that claims.
    case SplineExtrudeProfile::Bevel:
        return SplineGroundAuthority::Grades;
    }
    return SplineGroundAuthority::None;
}

static_assert(std::is_trivially_copyable_v<SplineExtrude>,
              "SplineExtrude must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<SplineExtrude>,
              "SplineExtrude must be standard layout for ECS storage");

// Which of the terrain components under a generated run the recipe's ground
// authority created (SplineGroundAuthorityController). Cleanup on an authority
// change removes exactly the pieces marked here and never one an author placed
// by hand: the mark is set when the controller creates a piece, is NEVER adopted
// onto a component that already existed, and is serialized with the scene so
// provenance survives a reload. An entity with no recipe-created pieces carries
// no marker at all. A pre-provenance scene has no marker either, so its effects
// read as hand-authored and cleanup leaves them in place — the conservative
// direction: an orphaned effect is visible and deletable, a deleted authored one
// is gone.
// @ge-tooltip The terrain effects of this extrude that Ground Authority added. A change of Ground Authority removes only these. Effects that you add by hand stay.
struct SplineGroundProvisioned
{
    bool Flatten = false;
    bool Claim = false;
    bool Volume = false;

    bool operator==(const SplineGroundProvisioned&) const = default;
};

static_assert(std::is_trivially_copyable_v<SplineGroundProvisioned>,
              "SplineGroundProvisioned must be trivially copyable for ECS storage");

} // namespace GameEngine::Components
