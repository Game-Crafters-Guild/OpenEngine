#pragma once

#include "Components/AssetRef.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplinePlacement.h"
#include "Components/Spline/SplineWall.h"
#include "ECS/Entity.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// What a member spline does to a paving. The mode follows what the member is: a closed
// spline that is not a Road takes Exclude, a Road takes Ignore.
enum class SplinePavingMemberMode : uint8
{
    // The closed spline's area is cut out of the paving and its border, such as a planting bed.
    Exclude = 0,
    // The road stays unpaved on this paving, on every arm where it meets it.
    Ignore = 1,
};

// How a paving meets a road where the road enters its outline.
enum class SplinePavingMouth : uint8
{
    // The road's sides meet the outline at a sharp corner.
    Straight = 0,
    // The outline turns into the road's sides on an arc of MouthRadius, so the mouth opens toward the paving.
    Flared = 1,
};

// How a road's paving meets the paving it yields to at a junction: the road that runs through, a road
// that ranks higher, or a square's own pattern.
enum class SplinePavingSeam : uint8
{
    // The two patterns meet on a bare line.
    HardCut = 0,
    // The road's pattern turns into the other pattern over a band of SeamBandTiles on the road's side.
    Blend = 1,
    // A thin joint of SeamCourseMaterial runs along the seam.
    JointLine = 2,
    // A course of stones of SeamCourseMaterial runs along the seam.
    StoneCourse = 3,
};

// One spline a paving excludes or ignores.
struct SplinePavingMember
{
    // A spline entity. A handle that names no live spline is skipped and reported. Undo of a
    // delete gives back the same handle.
    ECS::EntityHandle Entity{};
    SplinePavingMemberMode Mode = SplinePavingMemberMode::Exclude;
    uint8 _Pad[3] = {};  // @ge-hidden

    bool operator==(const SplinePavingMember&) const = default;
};

// The most members a paving holds.
inline constexpr uint32 kMaxPavingMembers = 16;

// Paves the junctions on its entity's network spline: an Area's outline with every road that enters
// it, or each place another road crosses or ends on a Road; overlaps pave once. Authored data only:
// SplinePavingController builds the chunks, and the scene saves this recipe, never the mesh.
struct SplinePaving
{
    // Members in [0, MemberCount); never above kMaxPavingMembers, and never the entity itself.
    uint32 MemberCount = 0;
    SplinePavingMember Members[kMaxPavingMembers] = {};
    // @ge-tooltip The surface drawn on the paving, at the material's own tiling in meters; empty draws it flat gray.
    MaterialRef Material;
    // @ge-tooltip Any whole number: each value slides the paving material's pattern to another place on the ground. The outline and the curb stay as they are.
    uint32 Seed = 0;
    // @ge-label Width (m)
    // @ge-tooltip How wide the curb is along every edge where no road enters; 0 builds no curb.
    // @ge-range 0 10
    float32 BorderWidth = 0.3f;
    // @ge-label Height (m)
    // @ge-tooltip How high the curb stands above the paving.
    // @ge-range 0.01 10
    float32 BorderHeight = 0.12f;
    // @ge-label Radius (m)
    // @ge-tooltip The radius of the arc at each side of a road in a Flared mouth. Where two roads enter close together, or a road enters at a shallow angle, the arc gets smaller or is left out.
    // @ge-range 0.1 10
    float32 MouthRadius = 1.5f;
    // @ge-label Material
    // @ge-tooltip The surface drawn on the curb; empty uses the paving's Material.
    MaterialRef BorderMaterial;
    // @ge-label Corner
    // @ge-tooltip How the curb turns where the edge turns: on one sharp edge (Miter) or on a rounded outside (Round).
    SplineWallCorner BorderCorner = SplineWallCorner::Miter;
    // @ge-tooltip What the paving does to the terrain under it. Auto is Owns: the terrain is graded to the rim, its grass is cleared and the ground is painted Dirt, and the ground is claimed, so other grading stops at its edge. On a sloped rim the ground steps along the plaza's diagonals. Grades grades the terrain and does not claim it. None leaves the terrain as it is and lays the paving on it. A change removes only the effects that this field added, and you can undo it.
    SplineGroundAuthority GroundAuthority = SplineGroundAuthority::Auto;
    // @ge-tooltip What the paving lies on: the terrain only, so props on the square never lift it (Terrain Only), or any surface, such as a bridge deck (Scene).
    SplineConformTarget ConformTarget = SplineConformTarget::TerrainOnly;
    // @ge-label Shape
    // @ge-tooltip How the paving meets a road that enters it: its edge curves into the road (Flared) or meets the road's sides at a square corner (Straight).
    SplinePavingMouth Mouth = SplinePavingMouth::Flared;
    // @ge-tooltip The seam is the line where two pavings meet at a junction: this road's paving and the paving it gives way to, a road that runs through, a wider road or a square. This sets how they meet there. Hard Cut stops the pattern on a bare line. Blend turns this road's pattern into the other pattern over a band. Joint Line draws a thin joint along the seam. Stone Course draws a row of stones along the seam. A road with no Spline Paving of its own takes the setting of the paving that paves the junction.
    SplinePavingSeam Seam = SplinePavingSeam::Blend;
    // @ge-label Blend Width (tiles)
    // @ge-tooltip How far into this road the Blend reaches, in tiles of its pattern. Where the patterns line up, the tile lines bend over the whole band, and a wider band bends them more gently. Elsewhere the pattern fades over half of the band.
    // @ge-range 0.5 8
    float32 SeamBandTiles = 2.0f;
    // @ge-label Course Width (m)
    // @ge-tooltip How wide the course is: the joint of a Joint Line or the row of stones of a Stone Course. 0 uses 3 cm for a Joint Line and half a tile of the other pattern for a Stone Course.
    // @ge-range 0 2
    float32 SeamCourseWidth = 0.0f;
    // @ge-label Course Material
    // @ge-tooltip The surface drawn on the course: the joint of a Joint Line or the row of stones of a Stone Course. One texture tile is as wide as the course and repeats along the seam. Empty draws the paving's Material at the paving's tile size: darker for a Joint Line; for a Stone Course, the paving's stones turned across the seam and a little brighter.
    MaterialRef SeamCourseMaterial;
    // @ge-tooltip Whether the paving and the curb cast shadows onto the scene.
    bool CastShadows = true;
    // @ge-tooltip Whether shadows fall on the paving and the curb.
    bool ReceiveShadows = true;

    // Memberwise: the controller compares sanitized recipes (SanitizePavingRecipe), so a NaN
    // never re-arms a rebuild.
    bool operator==(const SplinePaving&) const = default;
};

// The authority `paving` carries, with Auto resolved: a paved area owns its ground, as a road does.
// Never Auto.
[[nodiscard]] constexpr SplineGroundAuthority ResolveGroundAuthority(const SplinePaving& paving)
{
    return paving.GroundAuthority == SplineGroundAuthority::Auto ? SplineGroundAuthority::Owns : paving.GroundAuthority;
}

static_assert(sizeof(SplinePavingMember) == 8, "SplinePavingMember is a 4-byte handle and a mode, padded");
static_assert(std::is_trivially_copyable_v<SplinePaving>, "SplinePaving must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<SplinePaving>, "SplinePaving must be standard layout for ECS storage");

} // namespace GameEngine::Components
