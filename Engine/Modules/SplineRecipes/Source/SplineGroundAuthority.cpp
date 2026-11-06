#include "SplineRecipes/SplineGroundAuthority.h"

#include "SplineRecipes/SplineExtrudeSanitize.h"

#include "SplineRecipes/RecipeEditJournal.h"
#include "SplineRecipes/SplineGroundEdit.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Spline/SplinePaving.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Terrain/TerrainMaterialRecord.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::SplineRecipes
{
namespace
{

using Components::SplineGroundAuthority;
using Components::TerrainVolumeShape;

// The paint layer that a road or a paved area on Owns paints under itself. It shows in the fade
// ring past the edge.
constexpr uint32 kOwnedGroundPaintLayer = 2;
static_assert(std::string_view(Terrain::kTerrainLayerRoleNames[kOwnedGroundPaintLayer]) == "Dirt",
              "Owned ground paints the Dirt layer.");

// What one authority resolves to, as components. Written out as data rather than
// as four branches at each write site, so "a road claims and does not defer"
// exists once and the tests can read the same table the controller writes from.
struct GroundPlan
{
    bool GradesGround = false;
    bool Claim = false;
    const char* Group = "";     // empty = the shared pool
    bool RespectClaims = true;
    float32 Priority = kGradedRunPriority;
    TerrainVolumeShape Shape = TerrainVolumeShape::SplinePath;
    // A grass effect that clears the grass, and a paint effect.
    bool Grass = false;
    bool Paint = false;
};

GroundPlan PlanFor(SplineGroundAuthority authority, TerrainVolumeShape shape)
{
    switch (authority)
    {
    case SplineGroundAuthority::Grades:
        return {true, false, "", true, kGradedRunPriority, shape, false, false};
    case SplineGroundAuthority::Owns:
        // Its own pool, below the runs that stop at it, and no deferral to claims: a run that
        // deferred would defer to its own claim. Owned ground also clears its grass and paints itself.
        return {true, true, kOwnedRunGroupName, false, kOwnedRunPriority, shape, true, true};
    case SplineGroundAuthority::Auto:
    case SplineGroundAuthority::None:
        break;
    }
    return {};
}

void WritePoolGroup(Components::TerrainFlattenEffect& fx, const char* group)
{
    std::memset(fx.PoolGroup, 0, sizeof(fx.PoolGroup));
    const std::size_t length = std::strlen(group);
    std::memcpy(fx.PoolGroup, group, std::min(sizeof(fx.PoolGroup) - 1, length));
}

const char* AuthorityName(SplineGroundAuthority authority)
{
    switch (authority)
    {
    case SplineGroundAuthority::Auto:   return "Auto";
    case SplineGroundAuthority::None:   return "None";
    case SplineGroundAuthority::Grades: return "Grades";
    case SplineGroundAuthority::Owns:   return "Owns";
    }
    return "?";
}

// Writes the SplineGroundProvisioned marker back to the entity: present while any piece is
// recipe-created, absent otherwise, so a scene never carries an all-false
// marker block.
void WriteMarker(ECS::World& world, ECS::EntityHandle entity,
                 const Components::SplineGroundProvisioned& owned)
{
    const bool any = owned.Flatten || owned.Claim || owned.Volume || owned.Grass || owned.Paint;
    const auto* existing = world.GetComponent<Components::SplineGroundProvisioned>(entity);
    if (any)
    {
        if (!existing || !(*existing == owned))
            world.AddComponentImmediate(entity, owned);
    }
    else if (existing)
    {
        world.RemoveComponentImmediate<Components::SplineGroundProvisioned>(entity);
    }
}

// Removes `Piece` from `entity` when the recipe made it (`owned`) and it is there; adds `name` to
// `removed` when it does.
template <typename Piece>
bool RemoveOwned(ECS::World& world, ECS::EntityHandle entity, bool owned, std::string_view name,
                 std::vector<std::string_view>& removed)
{
    if (!owned || world.GetComponent<Piece>(entity) == nullptr)
        return false;
    world.RemoveComponentImmediate<Piece>(entity);
    removed.push_back(name);
    return true;
}

// Hands what one sweep did to `entity` to the undo journal, so one undo reverts it with the edit
// that caused it.
void Record(ECS::World& world, RecipeEditJournal* journal, ECS::EntityHandle entity,
            const SplineGroundPieces& before)
{
    const SplineGroundEdit edit{.Entity = entity, .Before = before, .After = SplineGroundPieces::Read(world, entity)};
    if (journal && !edit.Empty())
        journal->RecordGroundEdit(world, edit);
}

} // namespace

void SplineGroundAuthorityController::SweepRecipe(ECS::World& world, const Pending& recipe,
                                                  std::vector<Pending>& pending)
{
    const ECS::EntityHandle entity = recipe.Entity;
    const SplineGroundAuthority resolved = recipe.Authority;
    const bool firstSight = !m_Provisioned.Contains(entity);
    Provisioned& record = m_Provisioned.GetOrInsert(entity);
    record.SeenGeneration = m_SweepGeneration;

    // A loaded scene has its effects already: the first sweep records the authority and writes no
    // column, so a scene open does not re-bake every road. The pieces a recipe made are on the
    // entity's SplineGroundProvisioned marker, which the scene saves.
    if (firstSight && world.GetComponent<Components::TerrainFlattenEffect>(entity) != nullptr)
    {
        record.Authority = resolved;
        return;
    }
    if (record.Authority == resolved)
        return; // Steady state: no write, so no re-bake.

    pending.push_back(recipe);
}

void SplineGroundAuthorityController::SweepExtrude(ECS::World& world, ECS::EntityHandle entity,
                                                   const Components::SplineExtrude& recipe,
                                                   std::vector<Pending>& pending)
{
    // A run saved with the retired Rectangle profile reads as a path here. Its author drew a wall,
    // so the ground under it stays as it is.
    if (CarriesRetiredWallProfile(world, entity))
        return;
    const bool fixedWidthBevelOnAuto = recipe.Profile == Components::SplineExtrudeProfile::Bevel
                                       && recipe.WidthMode == Components::SplineExtrudeWidthMode::Channel
                                       && recipe.GroundAuthority == SplineGroundAuthority::Auto;
    SweepRecipe(world,
                {entity, Components::ResolveGroundAuthority(recipe), TerrainVolumeShape::SplinePath,
                 fixedWidthBevelOnAuto},
                pending);
}

void SplineGroundAuthorityController::SweepPaving(ECS::World& world, ECS::EntityHandle entity,
                                                  const Components::SplinePaving& paving,
                                                  const Components::SplineNetworkMember& member,
                                                  std::vector<Pending>& pending)
{
    // A paving on a Road paves the road's junctions, and the road's extrude owns that ground. An
    // entity that carries an extrude as well keeps the extrude's authority: one volume per entity.
    if (member.Kind != Components::SplineNetworkMemberKind::Area
        || world.GetComponent<Components::SplineExtrude>(entity) != nullptr)
        return;
    SweepRecipe(world, {entity, Components::ResolveGroundAuthority(paving), TerrainVolumeShape::SplineArea},
                pending);
}

void SplineGroundAuthorityController::Update(ECS::World& world, RecipeEditJournal* journal)
{
    if (journal)
        journal->BeginGroundUpdate(world, *this);
    ++m_SweepGeneration;

    std::vector<Pending> pending;
    world.Query<ECS::Read<Components::SplineComponent>,
                ECS::Read<Components::SplineExtrude>>()
        .Each([this, &world, &pending](ECS::EntityHandle entity, const Components::SplineComponent&,
                                       const Components::SplineExtrude& recipe)
              { SweepExtrude(world, entity, recipe, pending); });
    world.Query<ECS::Read<Components::SplineComponent>, ECS::Read<Components::SplinePaving>,
                ECS::Read<Components::SplineNetworkMember>>()
        .Each([this, &world, &pending](ECS::EntityHandle entity, const Components::SplineComponent&,
                                       const Components::SplinePaving& paving,
                                       const Components::SplineNetworkMember& member)
              { SweepPaving(world, entity, paving, member, pending); });

    for (const Pending& p : pending)
    {
        const GroundPlan plan = PlanFor(p.Authority, p.Shape);
        const SplineGroundPieces before = SplineGroundPieces::Read(world, p.Entity);

        // A Bevel on the Channel path is a FOOTPATH as far as the recipe can
        // tell, and so is a stream authored at a fixed width. The second one gets
        // graded to its own surface without a word, which is the failure an
        // author cannot see: the water still renders and the bed beneath it is
        // simply gone. Reported where the decision is actually made rather than
        // guessed at in ResolveGroundAuthority. Fires when a run is PROVISIONED,
        // so it lands as the author creates the run and never on scene load,
        // which adopts.
        if (p.Authority == SplineGroundAuthority::Grades && p.FixedWidthBevelOnAuto
            && m_FixedWidthBevelWarned.Insert(p.Entity))
        {
            Logger::Log::Info(
                "SplineGroundAuthority: entity {} is a fixed-width Bevel run, so Auto grades the "
                "terrain to its own heights, like a path. If this run is WATER rather than a path, "
                "set its Ground Authority to None -- a fixed-width stream left on Auto has its bed "
                "graded away to the water surface.",
                p.Entity.id);
        }

        // The SplineGroundProvisioned marker records which pieces the recipe made. It, not this
        // controller's session record, separates them from the author's, and the scene saves it.
        const auto* markerNow =
            world.GetComponent<Components::SplineGroundProvisioned>(p.Entity);
        Components::SplineGroundProvisioned owned =
            markerNow ? *markerNow : Components::SplineGroundProvisioned{};

        if (!plan.GradesGround)
        {
            // The recipe does not touch the ground, so the pieces that it made come off, and the
            // log names them. A piece that the author placed by hand stays.
            const bool hadMarker = markerNow != nullptr;
            std::vector<std::string_view> removed;
            RemoveOwned<Components::TerrainFlattenEffect>(world, p.Entity, owned.Flatten, "flatten", removed);
            RemoveOwned<Components::TerrainGroundClaimEffect>(world, p.Entity, owned.Claim, "ground claim", removed);
            RemoveOwned<Components::TerrainModifierVolume>(world, p.Entity, owned.Volume, "volume", removed);
            RemoveOwned<Components::TerrainGrassEffect>(world, p.Entity, owned.Grass, "grass", removed);
            RemoveOwned<Components::TerrainPaintLayerEffect>(world, p.Entity, owned.Paint, "paint", removed);
            if (hadMarker)
                world.RemoveComponentImmediate<Components::SplineGroundProvisioned>(p.Entity);
            if (!removed.empty())
                Logger::Log::Info(
                    "SplineGroundAuthority: entity {} no longer grades (authority {}); removed "
                    "the recipe-created {}. Undo restores them; hand-authored components were "
                    "not touched.",
                    p.Entity.id, AuthorityName(p.Authority), Join(removed, ", "));

            const bool handPiecesRemain =
                (!owned.Flatten
                 && world.GetComponent<Components::TerrainFlattenEffect>(p.Entity) != nullptr)
                || (!owned.Claim
                    && world.GetComponent<Components::TerrainGroundClaimEffect>(p.Entity)
                           != nullptr)
                || (!owned.Volume
                    && world.GetComponent<Components::TerrainModifierVolume>(p.Entity) != nullptr)
                || (!owned.Grass && world.GetComponent<Components::TerrainGrassEffect>(p.Entity) != nullptr)
                || (!owned.Paint
                    && world.GetComponent<Components::TerrainPaintLayerEffect>(p.Entity) != nullptr);
            if (handPiecesRemain && m_HandAuthoredConflictWarned.Insert(p.Entity))
                Logger::Log::Info(
                    "SplineGroundAuthority: entity {}'s authority is {} but it still carries "
                    "terrain effects the recipe did not create (hand-authored, or from a scene "
                    "saved before provenance was recorded). They keep grading; remove them by "
                    "hand if unwanted.",
                    p.Entity.id, AuthorityName(p.Authority));

            Record(world, journal, p.Entity, before);
            m_Provisioned.GetOrInsert(p.Entity).Authority = p.Authority;
            continue;
        }

        // The volume: created with the run's fade and priority, then left alone.
        // A run's swept half-width comes from the spline's width channel, so the
        // volume carries no width of its own to fall out of step with the mesh.
        bool handVolumeConflict = false;
        if (!world.GetComponent<Components::TerrainModifierVolume>(p.Entity))
        {
            Components::TerrainModifierVolume volume{};
            volume.Shape = plan.Shape;
            volume.Falloff = kProvisionedRunFade;
            volume.Priority = plan.Priority;
            world.AddComponentImmediate<Components::TerrainModifierVolume>(p.Entity, volume);
            owned.Volume = true;
        }
        else if (owned.Volume)
        {
            // The authority moved, and the priority is what makes a claim reach
            // the pools that must stop at it — so on the recipe's own volume this
            // one field IS the authority and is rewritten with it. The fade is
            // the author's and is not.
            if (auto* volume =
                    world.GetComponentForWrite<Components::TerrainModifierVolume>(p.Entity))
                volume->Priority = plan.Priority;
        }
        else
        {
            // A hand-authored volume keeps its priority: the recipe does not
            // rewrite what it did not create, even the field it would normally
            // own. Reported below, because claim ordering rides on priority.
            handVolumeConflict = true;
        }

        // Blend travels with the group and the claim flag rather than being left
        // to the author — on the recipe's OWN flatten: Average is what makes the
        // flatten POOL at all, so it is the structural half of the authority.
        // A hand-authored flatten is the author's whole component, blend
        // included; the recipe never rewrites it, and says so instead.
        //
        // The height fields are the author's on either kind, so they are never
        // rewritten and at creation come from the component's own defaults:
        // UseVolumeHeight grades to the spline's own Y at each station, and
        // TargetHeight adds no offset to it. The station spacing that samples
        // those heights is the VOLUME's and defaults with it.
        bool handFlattenConflict = false;
        if (world.GetComponent<Components::TerrainFlattenEffect>(p.Entity) != nullptr)
        {
            if (owned.Flatten)
            {
                if (auto* flatten =
                        world.GetComponentForWrite<Components::TerrainFlattenEffect>(p.Entity))
                {
                    flatten->Blend = Components::TerrainModifierBlend::Average;
                    WritePoolGroup(*flatten, plan.Group);
                    flatten->RespectClaims = plan.RespectClaims;
                }
            }
            else
            {
                handFlattenConflict = true;
            }
        }
        else
        {
            Components::TerrainFlattenEffect fx{};
            fx.Blend = Components::TerrainModifierBlend::Average;
            WritePoolGroup(fx, plan.Group);
            fx.RespectClaims = plan.RespectClaims;
            world.AddComponentImmediate<Components::TerrainFlattenEffect>(p.Entity, fx);
            owned.Flatten = true;
        }

        if (plan.Claim
            && world.GetComponent<Components::TerrainGroundClaimEffect>(p.Entity) == nullptr)
        {
            world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(
                p.Entity, Components::TerrainGroundClaimEffect{});
            owned.Claim = true;
        }

        // The grass and the paint: created once with their defaults for owned ground, then the
        // author's to tune. A hand-authored one stays as it is.
        if (plan.Grass && world.GetComponent<Components::TerrainGrassEffect>(p.Entity) == nullptr)
        {
            Components::TerrainGrassEffect grass{};
            grass.HeightScale = 0.0f;
            grass.DensityScale = 0.0f;
            grass.GroundDarkeningScale = 0.0f;
            world.AddComponentImmediate<Components::TerrainGrassEffect>(p.Entity, grass);
            owned.Grass = true;
        }
        if (plan.Paint && world.GetComponent<Components::TerrainPaintLayerEffect>(p.Entity) == nullptr)
        {
            Components::TerrainPaintLayerEffect paint{};
            paint.LayerIndex = kOwnedGroundPaintLayer;
            paint.Replace = true;
            paint.RespectClaims = plan.RespectClaims;
            world.AddComponentImmediate<Components::TerrainPaintLayerEffect>(p.Entity, paint);
            owned.Paint = true;
        }

        WriteMarker(world, p.Entity, owned);

        // Recipe-created pieces the new plan does not want: the claim, the grass and the paint.
        const bool handClaimConflict = !plan.Claim && !owned.Claim
                                       && world.GetComponent<Components::TerrainGroundClaimEffect>(p.Entity) != nullptr;
        Components::SplineGroundProvisioned after = owned;
        std::vector<std::string_view> droppedNames;
        if (!plan.Claim
            && RemoveOwned<Components::TerrainGroundClaimEffect>(world, p.Entity, owned.Claim, "ground claim",
                                                                droppedNames))
            after.Claim = false;
        if (!plan.Grass
            && RemoveOwned<Components::TerrainGrassEffect>(world, p.Entity, owned.Grass, "grass", droppedNames))
            after.Grass = false;
        if (!plan.Paint
            && RemoveOwned<Components::TerrainPaintLayerEffect>(world, p.Entity, owned.Paint, "paint", droppedNames))
            after.Paint = false;
        if (!droppedNames.empty())
        {
            WriteMarker(world, p.Entity, after);
            owned = after;
            Logger::Log::Info(
                "SplineGroundAuthority: entity {} moved to authority {}, which keeps the "
                "grade but does not own the ground; removed the recipe-created {}. Undo "
                "restores them.",
                p.Entity.id, AuthorityName(p.Authority), Join(droppedNames, ", "));
        }

        if ((handFlattenConflict || handVolumeConflict || handClaimConflict)
            && m_HandAuthoredConflictWarned.Insert(p.Entity))
        {
            std::vector<std::string_view> pieces;
            if (handFlattenConflict)
                pieces.push_back("the flatten keeps its authored blend and pool group (a pooling run "
                                 "wants Blend=Average)");
            if (handVolumeConflict)
                pieces.push_back("the volume keeps its authored priority (claim ordering rides on it)");
            if (handClaimConflict)
                pieces.push_back("the ground claim stays");
            Logger::Log::Info(
                "SplineGroundAuthority: entity {}'s authority is {} but some of its terrain "
                "components are hand-authored, and the recipe does not rewrite what it did not "
                "create: {}. Tune the fields yourself, or remove the components to let the "
                "recipe provision its own.",
                p.Entity.id, AuthorityName(p.Authority), Join(pieces, "; "));
        }

        Record(world, journal, p.Entity, before);
        m_Provisioned.GetOrInsert(p.Entity).Authority = p.Authority;
    }

    // Entities that went away between sweeps. Their components went with them, so
    // there is nothing to undo — only the record to drop, or the map grows for
    // the lifetime of the editor session.
    m_Provisioned.EraseIf([this](ECS::EntityHandle, const Provisioned& record)
                          { return record.SeenGeneration != m_SweepGeneration; });
    if (journal)
        journal->EndGroundUpdate();
}

void SplineGroundAuthorityController::Shutdown()
{
    m_Provisioned.Clear();
    m_FixedWidthBevelWarned.Clear();
    m_HandAuthoredConflictWarned.Clear();
}

} // namespace GameEngine::SplineRecipes
