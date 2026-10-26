#pragma once

#include "Components/Spline/SplineExtrude.h"
#include "ECS/Entity.h"
#include "Types/FlatMap.h"
#include "Types/FlatSet.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::ECS { class World; }

namespace GameEngine::SplineRecipes
{
class RecipeEditJournal;

// Keeps the terrain effects under a generated run in step with what the run's
// recipe SAYS about the ground (Components::SplineGroundAuthority).
//
// A road that grades the ground needs a spline volume with the Spline Path
// shape, a flatten with the Average blend, a ground claim, and a priority below
// every pool the claim holds back. The recipe says it is a road; this
// controller adds those components.
//
// OWNERSHIP, stated because it is the only surprising part: the recipe owns the
// pieces IT CREATED — recorded per piece on the entity's SplineGroundProvisioned
// marker at creation time, serialized with the scene — and nothing else. On a
// recipe-created flatten it owns the fields that encode the authority itself
// (the blend that makes the flatten pool at all, the pool group name, and
// whether the flatten defers to claims), and on a recipe-created volume the
// priority. It does NOT own the fields an author tunes afterwards (the volume's
// fade width and station spacing, the flatten's height offset, the claim's
// strength) — written once at provisioning and never again — and it NEVER
// writes, adopts or removes a component the author placed by hand: a
// hand-authored flatten keeps its blend, and survives every authority toggle.
// When a hand-authored piece occupies a slot the authority wants, the
// controller says so in the log instead of taking it.
//
// Removal is scoped by the same ledger: an authority that stops grading removes
// exactly the recipe-created pieces, reports what it removed, and commits the
// removal to the undo journal, so Ctrl+Z restores the pieces and their marker.
// DELETING a recipe-created Flatten or Ground Claim by hand still does not
// stick — the recipe still says the run grades, so it provisions the missing
// piece again. Setting GroundAuthority to None is the removal that holds. Said
// on the recipe field's own tooltip as well, which is where an author is
// standing when they try it.
//
// Ticked once per frame by RecipeControllers after the controllers that
// generate geometry, and deliberately NOT part of SplineExtrudeController: that
// one owns generated geometry, and terrain components are not geometry.
class SplineGroundAuthorityController
{
public:
    // Provisions, updates or removes the terrain effects for every
    // (SplineComponent, SplineExtrude) entity whose resolved authority changed.
    // Idempotent: an unchanged scene writes no component and stamps no column,
    // which matters because a write here would wake the terrain modifier system's
    // change gate and re-bake the region every frame.
    //
    // `journal` receives the destructive transitions (recipe-created cleanup);
    // null applies them with no journal but never skips the ledger, so a
    // process with no editor still gets provenance-scoped removal.
    void Update(ECS::World& world, RecipeEditJournal* journal);

    // Forgets every per-entity record. The records are scoped to the open scene:
    // a swap clears the world and restarts entity versions, so a surviving record
    // matches whichever incoming entity recycles its handle, and that entity is
    // then rewritten instead of adopted. Run it at the swap.
    void Shutdown();

private:
    // The authority this controller last provisioned for an entity, so a change
    // is detectable without reading the terrain components back (an author may
    // have retuned them, and re-deriving "what did I provision" from tuned state
    // is guesswork). Entities that disappear are dropped by the same sweep that
    // reads them.
    struct Provisioned
    {
        Components::SplineGroundAuthority Authority = Components::SplineGroundAuthority::None;
        uint64 SeenGeneration = 0;
    };
    // A recipe whose ground authority changed. The sweep collects these first and applies them
    // after the query: a component add or remove moves the entity to another archetype.
    struct Pending
    {
        ECS::EntityHandle Entity;
        Components::SplineGroundAuthority Authority;
        Components::SplineExtrude Recipe;
    };

    // Records the authority of one recipe, and adds it to `pending` when the authority changed.
    void SweepRecipe(ECS::World& world, ECS::EntityHandle entity, const Components::SplineExtrude& recipe,
                     std::vector<Pending>& pending);

    FlatMap<ECS::EntityHandle, Provisioned> m_Provisioned;
    uint64 m_SweepGeneration = 0;

    // Entities already told that a fixed-width Bevel grades like a path. The
    // recipe cannot tell a footpath from a fixed-width stream, so this is a
    // breadcrumb at the moment of authoring rather than a refusal. Once per
    // entity per session, the same shape as the terrain gather's warn sets.
    FlatSet<ECS::EntityHandle> m_FixedWidthBevelWarned;

    // Entities already told that a hand-authored component occupies a slot the
    // authority wants (a flatten whose blend the recipe may not rewrite, a claim
    // it may not remove). Once per entity per session.
    FlatSet<ECS::EntityHandle> m_HandAuthoredConflictWarned;
};

// The pool group name an owning run's flatten carries, and the reason a road is
// not simply in the shared pool: a run that owns its ground must not average its
// grade with the paths that stop at it, or the carriageway wanders toward
// whatever crosses it.
//
// A fixed name rather than one generated per entity: the group is serialised
// TEXT that an author reads in the inspector, and an id minted from an entity
// handle is neither stable across a scene reload nor meaningful to read.
inline constexpr const char* kOwnedRunGroupName = "Roads";

// Priorities the provisioning writes. A claim only holds ground against pools
// that flush AFTER it, so every owning run has to sort below the graded runs
// that must stop at it — which is why these are two named constants and not one
// default. Both sit above the 0 a hand-placed volume defaults to, so authored
// terrain shaping stays underneath the routes.
inline constexpr float32 kOwnedRunPriority = 10.0f;
inline constexpr float32 kGradedRunPriority = 20.0f;

// Fade width, in metres, of the graded region a run provisions: how far past the
// run's own swept edge the terrain keeps following it before returning to the
// ground that was there. The run's half-width itself comes from the spline's
// width channel, which is the same channel the extruded cross-section is scaled
// by — so the graded region is exactly as wide as the thing standing on it,
// with no second width to keep in step.
inline constexpr float32 kProvisionedRunFade = 3.0f;

} // namespace GameEngine::SplineRecipes
