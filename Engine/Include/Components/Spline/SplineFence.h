#pragma once

#include "Components/AssetRef.h"
#include "Components/Spline/SplinePlacement.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// The most parts a fence can list. 32 parts keep SplineFence trivially
// copyable and in its 768-byte budget. The inspector disables Add Part at 32.
inline constexpr uint32 kSplineFenceMaxParts = 32;

// Span overrides a recipe can carry. Fixed capacity for the same reason the
// parts list has one: the component stays trivially copyable and chunk-resident.
inline constexpr uint32 kSplineFenceMaxSpanOverrides = 16;

// How a span meets the grade between the two stations it joins.
enum class SplineSpanGrade : int32
{
    // The span pitches to follow the line between station bases — rails,
    // hedges, most game fences.
    Racked = 0,
    // The span stays level at the LOWER of its two station altitudes, so the
    // height jumps at stations and no span floats off its downhill end —
    // brick walls, panel fences, castle walls on steep ground.
    Stepped = 1,
    // The span is SHEARED to the grade rather than rotated onto it: its
    // along-run axis follows the line between station bases while its up axis
    // stays with the stations, so a panel's moulded-in posts and pickets stand
    // plumb while its rails climb. Both ends meet their own station's altitude,
    // which is what Stepped cannot do — a level panel buries its uphill end and
    // steps against its neighbour.
    //
    // This is the joinery a real fence uses on a slope, and the trade builders
    // name "racking". The enumerator is not called that here because Racked is
    // already taken by the mode that rotates the whole piece, which leans the
    // posts with it.
    //
    // Right for a piece carrying vertical detail (panels, pickets, balusters);
    // wrong for one whose whole silhouette should follow the ground, which is
    // what Racked is for.
    Sheared = 2,
};

// The role of one part of a fence. The parts of one kind are that role's pool:
// the fence picks among them by Seed, in list order.
enum class SplineFencePartKind : uint8
{
    // An empty slot of the parts list.
    None = 0,
    // Planted at each station.
    Post = 1,
    // Stretched between two stations.
    Span = 2,
    // Stands in a span that an override makes a gate.
    Gate = 3,
    // Laid along the tops of the spans at Crest Pitch.
    Crest = 4,
    // Closes a gap at the end of a crest row.
    Cap = 5,
};

// One part of a fence: a mesh and the role it plays. A part without a mesh
// places nothing.
//
// @ge-no-add
struct SplineFencePart
{
    ModelRef Mesh;
    // @ge-tooltip What the part is. Post: stands between two spans. A fence whose spans carry their own posts needs no Post part. Span: fills the gap between two posts. Gate: stands in a span that a span override makes a gate. Crest: laid along the tops of the spans at Crest Pitch, such as battlements. Cap: closes the end of a row of crest parts. The fence picks among the parts of one kind by Seed.
    SplineFencePartKind Kind = SplineFencePartKind::None;

    bool operator==(const SplineFencePart&) const = default;
};

static_assert(sizeof(SplineFencePart) == 17, "SplineFencePart has no padding bytes");
static_assert(std::is_trivially_copyable_v<SplineFencePart>);
static_assert(std::is_standard_layout_v<SplineFencePart>);

// What an override substitutes for the span it names.
enum class SplineSpanOverrideKind : uint8
{
    // An empty slot of the override table: it names no span.
    None = 0,
    // Draw from the Gate parts instead of the Span parts. With no Gate part the
    // opening stays empty, which is itself useful (a gap in a hedge).
    Gate = 1,
    // Draw one Span part in this span instead of the part the Seed picks.
    ExplicitPart = 2,
};

// One authored "this span is a gate" or "this span is that part". It names the span
// by its authored point and its place in that segment, not by distance, so it
// survives a knot drag. Overrides live on the fence, not on the spline.
//
// @ge-no-add
struct SplineSpanOverride
{
    uint32 PointIndex = 0;  // authored point that opens the segment
    uint16 SpanOrdinal = 0; // which span of that segment, counted from 0 at the point
    SplineSpanOverrideKind Kind = SplineSpanOverrideKind::None;
    uint8 PoolSlot = 0; // the n-th Span part (ExplicitPart) or Gate part (Gate) with a mesh

    bool operator==(const SplineSpanOverride&) const = default;
};

static_assert(sizeof(SplineSpanOverride) == 8, "SplineSpanOverride must stay 8 bytes");
static_assert(std::is_trivially_copyable_v<SplineSpanOverride>);
static_assert(std::is_standard_layout_v<SplineSpanOverride>);

// Builds a post-and-span structure — fence, hedge, wall — along the sibling
// SplineComponent's curve. Sibling of SplinePlacement, not a mode inside it:
// the two families share the centerline machinery and almost no recipe fields.
//
// Pure authored data, trivially copyable, serialized by the scene; the
// editor-side SplineFenceController samples, conforms and spawns runtime-only
// piece entities. Authored points are mandatory STATIONS: a fence turns a
// corner by planting a post, never by bending a rail.
struct SplineFence
{
    // The parts the fence builds from, in list order. A slot whose Kind is None
    // is empty. The parts of one kind with a mesh are the pool of that role. A
    // fence builds when it has a Post or a Span part with a mesh.
    SplineFencePart Parts[kSplineFenceMaxParts] = {{ModelRef{}, SplineFencePartKind::Span}};
    // Target metres between stations within a run, along the draped
    // centerline. Runs bend it slightly so their stations land on both
    // authored endpoints — the trade FitToLength already makes for tiles.
    float32 PostPitch = 2.4f;
    // Metres between crest cells, measured from each run's opening authored
    // point so the row keeps its phase when the spline is edited elsewhere.
    // 0 takes the longest active crest piece's own length, which meets flush;
    // a smaller value is raised to it, because overlapping crest pieces are
    // never right.
    float32 CrestPitch = 0.0f;
    // Stations conform; spans derive their frame from the stations and cast no
    // rays of their own.
    // @ge-tooltip How the posts meet the surface under the spline. None keeps the frame of the spline. Height puts each post on the surface. Height And Slope also tilts it with the surface. Spans take their frame from the posts.
    SplinePlacementConform ConformMode = SplinePlacementConform::HeightAndSlope;
    // What the conform ray may land on. Scene (the default) is every pickable
    // surface; TerrainOnly makes scenery transparent so stations do not ride
    // the roofs of the props they pass under.
    //
    // A fence is more exposed to this than a tile path. A run's length is the
    // 3D length of its DRAPED polyline, so a prop overhanging the line does not
    // merely lift the stations beneath it: it lengthens the run, re-solves how
    // many spans fill it, and so moves every station in that run.
    // @ge-tooltip What the posts stand on. Scene takes the nearest surface of any kind, so a fence under a tree or an awning stands on top of it, and the longer path over it can take more spans. Terrain Only ignores scenery, so the posts follow the ground under it. With Terrain Only the drawn line of the spline can still ride over scenery that the parts pass under.
    SplineConformTarget ConformTarget = SplineConformTarget::Scene;
    // 0 = plumb posts (what a real fence does), 1 = posts lean into the slope.
    float32 SlopeBlend = 0.0f;
    // Cap on a span's length scale (and, reciprocally, its floor) before the
    // run re-walks with one more or one fewer station. Protects proportion and
    // silhouette: a 2x stretched plank reads instantly as wrong.
    float32 SpanMaxStretch = 1.25f;
    // @ge-tooltip Which height of a part meets the ground. Pivot Plane puts the height of the mesh's pivot on the ground, so a part modeled to reach below its pivot sinks into the ground as it was made to. Bounds Min lifts the part until its lowest point touches the ground, so a part like that floats.
    SplinePlantMode PlantMode = SplinePlantMode::PivotPlane;
    // @ge-tooltip How a span follows a slope. Racked turns the whole part onto the slope, so its posts lean (rails, hedges). Stepped keeps the part level at the lower post, so the height steps at each post (brick walls, castle walls). Sheared slants the part and keeps its posts upright, so both ends meet their own post: the joint a real fence uses on a slope.
    SplineSpanGrade SpanGrade = SplineSpanGrade::Racked;
    // Authored span exceptions: "this span is a gate", "this span is that
    // piece". Every slot whose Kind is not None is one override, in any order;
    // an untouched table writes nothing to the scene, and a filled slot writes
    // one line per member ("Overrides3.SpanOrdinal = 2").
    SplineSpanOverride Overrides[kSplineFenceMaxSpanOverrides];
    // Salts the deterministic per-station, per-span and per-crest-cell pool
    // picks; same Seed + same ordinal = same pick on every rebuild. Caps are
    // chosen by length, not by the seed.
    uint32 Seed = 0;
    // Replaces the material on every emitted post, span, crest piece and cap.
    // Null (the default) keeps each mesh's own embedded model material. Kit FBX
    // commonly embed a bare untextured Lambert, or name source textures the
    // project never imported, so the embedded material is often not the one
    // the scene wants; the override retextures a fence without touching the
    // shared meshes. A GUID the runtime material registry cannot resolve is
    // not bound: the binder logs once and keeps the embedded material rather
    // than letting render extraction drop the pieces.
    MaterialRef OverrideMaterial;

    // Memberwise (arrays element-wise): the rebuild scheduler compares whole
    // recipes, so a hand-written comparison that missed a field would silently
    // stop rebuilds for edits to that field. Defaulted == cannot miss one.
    //
    // A NaN float would make a recipe unequal to ITSELF and re-arm the settle
    // rebuild forever, so the controller compares sanitized recipes only —
    // SanitizeFenceRecipe (Placement/FenceLayout.h) is the single gate.
    bool operator==(const SplineFence&) const = default;
};

static_assert(std::is_trivially_copyable_v<SplineFence>,
              "SplineFence must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<SplineFence>,
              "SplineFence must be standard layout for ECS storage");

} // namespace GameEngine::Components
