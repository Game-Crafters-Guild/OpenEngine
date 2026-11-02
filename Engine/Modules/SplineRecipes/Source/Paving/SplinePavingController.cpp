#include "SplineRecipes/Paving/SplinePavingController.h"

#include "Assets/ModelAsset.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Geometry.h"
#include "PavingMesh.h"
#include "PavingRoadUV.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "SplineGeometry/AreaGround.h"
#include "SplineGeometry/PavedBorder.h"
#include "SplineLayout/PieceEntity.h"
#include "SplineRecipes/GeneratedSplineMesh.h"
#include "SplineRecipes/Paving/SplinePavingSanitize.h"
#include "SplineRecipes/RecipeBuildStates.h"
#include "SplineRecipes/RecipeValidation.h"
#include "SplineRecipes/SplineExtrudeSanitize.h"
#include "TerrainECS/TerrainModifierSystem.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector2;
using Mathematics::Vector3;

// The built-in paving's linear base color: a flat mid gray, so a paving with no material
// renders as paving on the first try.
constexpr float32 kDefaultPavingGray = 0.5f;

GUID DefaultPavingMaterialGuid()
{
    return GUID::Derive(GUID::Null(), "engine/material/spline-paving-default");
}

// The key spaces an owner's lid and border meshes re-register under, one key per region slot.
GUID LidKeySpace(uint32 ownerId)
{
    return GUID::Derive(GUID::Null(), "engine/mesh/spline-paving-lid/" + std::to_string(ownerId));
}

GUID BorderKeySpace(uint32 ownerId)
{
    return GUID::Derive(GUID::Null(), "engine/mesh/spline-paving-border/" + std::to_string(ownerId));
}

// Appends a swept border run's mesh to `out`, from world space into the owner's space.
void AppendInOwnerSpace(const SG::SplineStripMesh& strip, const Mathematics::Matrix4x4& toLocal, PavingLidMesh& out)
{
    const uint32 base = static_cast<uint32>(out.Vertices.size());
    const Vector3 origin = toLocal.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));
    for (SG::SplineVertex vertex : strip.Vertices)
    {
        vertex.Position = toLocal.TransformPoint(vertex.Position);
        vertex.Normal = (toLocal.TransformPoint(vertex.Normal) - origin).NormalizeOrZero();
        const Vector3 tangent =
            (toLocal.TransformPoint(Vector3(vertex.Tangent.x, vertex.Tangent.y, vertex.Tangent.z)) - origin).NormalizeOrZero();
        vertex.Tangent = Mathematics::Vector4(tangent.x, tangent.y, tangent.z, vertex.Tangent.w);
        if (out.Vertices.empty())
        {
            out.MinBounds = vertex.Position;
            out.MaxBounds = vertex.Position;
        }
        out.MinBounds = Vector3(std::min(out.MinBounds.x, vertex.Position.x), std::min(out.MinBounds.y, vertex.Position.y),
                                std::min(out.MinBounds.z, vertex.Position.z));
        out.MaxBounds = Vector3(std::max(out.MaxBounds.x, vertex.Position.x), std::max(out.MaxBounds.y, vertex.Position.y),
                                std::max(out.MaxBounds.z, vertex.Position.z));
        out.Vertices.push_back(vertex);
    }
    for (const uint32 index : strip.Indices)
        out.Indices.push_back(base + index);
}

// The verge of each footprint arm's road, by arm index (Components::BuildsVerge).
std::vector<SG::PavedVerge> ArmVerges(const PavingPlan& plan, std::span<const PavingSplineObservation> splines)
{
    std::vector<SG::PavedVerge> verges(plan.ArmRoads.size());
    for (uint32 a = 0; a < plan.ArmRoads.size(); ++a)
    {
        const auto found = std::find_if(splines.begin(), splines.end(), [&](const PavingSplineObservation& spline)
                                        { return spline.Entity == plan.ArmRoads[a]; });
        if (found == splines.end() || !found->Strip)
            continue;
        const Components::SplineExtrude strip = SanitizeExtrudeRecipe(*found->Strip);
        if (Components::BuildsVerge(strip))
            verges[a] = SG::PavedVerge{true, strip.VergeWidth, strip.VergeHeight};
    }
    return verges;
}

// The trims of every arm a region takes over, keyed by road; the Open edges at the paving's
// heights under each region's owner (`groundAt`, by owner index), curbed when the owner builds a curb.
PavingRoadTrims RoadTrimsOf(const PavingPlan& plan, std::span<const SG::GroundSampler> groundAt,
                            std::span<const PavingCandidate> owners)
{
    PavingRoadTrims trims;
    for (uint32 a = 0; a < plan.Footprint.Arms.size(); ++a)
    {
        const SG::PavedArmStop& stop = plan.Footprint.Arms[a];
        if (stop.Region == SG::kNoPavedRegion)
            continue;
        const SG::GroundSampler& ground = groundAt[plan.RegionOwners[stop.Region]];
        PavingRoadTrim trim;
        trim.NodeDistance = stop.NodeDistance;
        trim.StopDistance = stop.StopDistance;
        trim.Open = stop.Open;
        trim.Curbed = stop.Open && owners[plan.RegionOwners[stop.Region]].Recipe.BorderWidth > 0.0f;
        if (stop.Open)
        {
            trim.OpenLeft = Vector3(stop.OpenLeft.x, ground(stop.OpenLeft), stop.OpenLeft.y);
            trim.OpenRight = Vector3(stop.OpenRight.x, ground(stop.OpenRight), stop.OpenRight.y);
        }
        trims.GetOrInsert(plan.ArmRoads[a]).push_back(trim);
    }
    return trims;
}

// The approaches of the regions that an Area owns: each arm from where it leaves the outline to its stop,
// on the ground under the owner (`groundAt`, by owner index). An arm whose stop is not open and not at its
// road's end lies in another arm's band and gets no approach.
std::vector<PavingApproach> ApproachesOf(const PavingPlan& plan, std::span<const SG::GroundSampler> groundAt,
                                         std::span<const PavingCandidate> owners, std::span<const PavingSource> sources)
{
    std::vector<PavingApproach> approaches;
    for (uint32 a = 0; a < plan.Footprint.Arms.size(); ++a)
    {
        const SG::PavedArmStop& stop = plan.Footprint.Arms[a];
        const SG::PavedArm& arm = plan.Arms[a];
        const bool deadEnd = arm.EndsRoad && !arm.Stations.empty()
                             && std::abs(stop.StopDistance - arm.Stations.back().Distance) <= SG::kPavedTouchMeters;
        if (stop.Region == SG::kNoPavedRegion || plan.Footprint.Regions[stop.Region].Outlines.empty()
            || (!stop.Open && !deadEnd))
            continue;
        const uint32 owner = plan.RegionOwners[stop.Region];
        // The curb and the mouth's arcs are the paving's too.
        const float32 edge = sources[owner].BorderWidth + sources[owner].MouthRadius;
        PavingApproach approach =
            BuildPavingApproach(arm, stop, edge, groundAt[owner], sources[owner].Outline);
        if (approach.Points.size() < 2)
            continue;
        approach.Owner = owners[owner].Entity;
        approaches.push_back(std::move(approach));
    }
    return approaches;
}

// The ids of every road in `previous` or `next`, sorted and unique, into `out`.
void HoldRoads(const PavingRoadTrims& previous, const PavingRoadTrims& next, std::vector<uint32>& out)
{
    out.clear();
    for (const auto& [roadId, trims] : previous)
        out.push_back(roadId);
    for (const auto& [roadId, trims] : next)
        out.push_back(roadId);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

// Who paves each junction of source `s`: nobody when its road is left out, else the owner of the
// region its first paved arm reached.
std::vector<PavingJunctionStatus> ResolveJunctions(const PavingWorld& paved, const PavingPlan& plan, uint32 s)
{
    const PavingSource& source = paved.Sources[s];
    std::vector<PavingJunctionStatus> junctions = paved.Candidates[s].Junctions;
    for (uint32 j = 0; j < junctions.size(); ++j)
    {
        PavingJunctionStatus& junction = junctions[j];
        if (junction.State != PavingJunctionState::Paved)
            continue;
        if (std::find(source.UnpavedRoads.begin(), source.UnpavedRoads.end(), junction.Road.id) !=
            source.UnpavedRoads.end())
        {
            junction.State = PavingJunctionState::Ignored;
            continue;
        }
        junction.State = PavingJunctionState::TooNarrow;
        for (uint32 a = 0; a < source.ArmJunctions.size(); ++a)
        {
            if (source.ArmJunctions[a] != j)
                continue;
            const uint32 footprintArm = plan.SourceArms[s][a];
            if (footprintArm == kNoPavedArm)
                continue;
            const uint32 region = plan.Footprint.Arms[footprintArm].Region;
            if (region == SG::kNoPavedRegion)
                continue;
            const uint32 owner = plan.RegionOwners[region];
            junction.State = owner == s ? PavingJunctionState::Paved : PavingJunctionState::PavedByOther;
            if (owner != s)
                junction.PavedBy = paved.Candidates[owner].Entity;
            break;
        }
    }
    return junctions;
}

std::string_view ReasonFor(PavingRole role)
{
    switch (role)
    {
    case PavingRole::PavedBy:
        return kPavedByAnotherReason;
    case PavingRole::NothingToPave:
        return kNothingToPaveReason;
    case PavingRole::Owns:
        break;
    }
    return std::string_view();
}

bool HoldsCandidate(const PavingWorld& paved, uint32 entityId)
{
    return std::any_of(paved.Candidates.begin(), paved.Candidates.end(),
                       [entityId](const PavingCandidate& candidate) { return candidate.Entity.id == entityId; });
}

// One paving's height at a point: the ground under its conform target, or its entity's height where
// no ray hits, plus kGroundSurfaceLiftMeters.
struct LiftedGround
{
    ECS::World* World = nullptr;
    const std::vector<ECS::EntityHandle>* Ignore = nullptr;
    ConformMissTally* Misses = nullptr;
    float32 CeilingY = 0.0f;
    float32 FallbackY = 0.0f;
    Components::SplineConformTarget Target = Components::SplineConformTarget::TerrainOnly;

    float32 operator()(const Vector2& xz) const
    {
        Vector3 hit;
        if (ConformRayDown(*World, Vector3(xz.x, CeilingY, xz.y), *Ignore, hit, nullptr, Misses, Target))
            return hit.y + kGroundSurfaceLiftMeters;
        return FallbackY + kGroundSurfaceLiftMeters;
    }
};

} // namespace

void SplinePavingController::Update(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                       float32 deltaSeconds, RecipeBuildStates& states)
{
    // A world reset destroys every chunk with no per-entity event: drop the bookkeeping first.
    m_HeldRoads.clear();
    const uint64 resetGeneration = world.GetLifecycleResetGeneration();
    if (resetGeneration != m_LastWorldResetGeneration)
    {
        m_LastWorldResetGeneration = resetGeneration;
        for (auto& [ownerId, owner] : m_Owners)
        {
            owner.Lid.DropAfterWorldReset(&renderServices.GetMeshGPURegistry());
            owner.Border.DropAfterWorldReset(&renderServices.GetMeshGPURegistry());
        }
        m_Owners.Clear();
        m_RoadTrims.Clear();
        m_Approaches.clear();
        ++m_ApproachesVersion;
        m_Gate.MarkUnbuilt();
    }

    // Each frame reads what the gate compares; a rebuild reads the curves.
    const PavingWorld observed = GatherPavings(world, PavingGatherDepth::Observe);
    if (observed.Candidates.empty())
    {
        if (RetireAll(world, &renderServices.GetMeshGPURegistry()))
            world.ProcessCommands();
        m_RoadTrims.Clear();
        m_Approaches.clear();
        ++m_ApproachesVersion;
        m_Statuses.Clear();
        m_LastStates.Clear();
        m_LoggedNotices.Clear();
        m_Gate.MarkUnbuilt();
        return;
    }

    ObservedState current;
    for (const PavingCandidate& candidate : observed.Candidates)
        current.Recipes.push_back({candidate.Entity.id, PavingGeometryKey(candidate.Recipe)});
    current.Splines = observed.Splines;
    current.Junctions = observed.Junctions;
    // Every paving reads the ground: the lid lies on it.
    current.SurfaceRevision = ObservedSurfaceRevision(true, ConformSurfaceRevision(world, ResolveTerrainModifierSystem()));

    bool outputsAlive = true;
    for (const auto& [ownerId, owner] : m_Owners)
        outputsAlive &= owner.Lid.ChunksAlive(world) && owner.Border.ChunksAlive(world);
    const ConformSurfaceReadiness readiness = ConformSurfaceState(world, observed.Bounds);
    const uint32 firstEntity = observed.Candidates.front().Entity.id;
    if (!m_Gate.ShouldRebuild(current, outputsAlive, readiness, true, deltaSeconds, firstEntity))
    {
        const BuildState waiting = StateWhileGateWaits(m_Gate, current);
        for (const PavingCandidate& candidate : observed.Candidates)
        {
            const auto* last = m_LastStates.Find(candidate.Entity.id);
            if (waiting == BuildState::Built && last)
                states.Set(candidate.Entity, last->first, last->second);
            else
                states.Set(candidate.Entity, waiting, waiting == BuildState::Held ? kHeldForTerrainReason : std::string_view());
        }
        Rebind(world, renderServices, observed);
        return;
    }

    m_Hold.Held = false;
    const bool worldDirty = Rebuild(world, renderServices, GatherPavings(world, PavingGatherDepth::Full), states);
    if (!m_Hold.Held)
        m_Gate.MarkBuilt(current);
    if (worldDirty)
        world.ProcessCommands();
}

bool SplinePavingController::Rebuild(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                        const PavingWorld& paved, RecipeBuildStates& states)
{
    const PavingPlan plan = PlanPavings(paved.Sources);
    const std::vector<ECS::EntityHandle> ignore = CollectConformRayIgnoreList(world);
    ConformMissTally misses;

    std::vector<SG::GroundSampler> groundUnder;
    for (const PavingCandidate& candidate : paved.Candidates)
    {
        const float32 fallbackY = candidate.WorldMatrix.TransformPoint(Vector3(0.0f, 0.0f, 0.0f)).y;
        groundUnder.push_back(
            LiftedGround{&world, &ignore, &misses, paved.CeilingY, fallbackY, candidate.Recipe.ConformTarget});
    }

    const std::vector<SG::PavedVerge> armVerges = ArmVerges(plan, paved.Splines);
    // Per owner, its regions' lids and borders in region order.
    FlatMap<uint32, std::pair<std::vector<Mesh>, std::vector<Mesh>>> built;
    for (uint32 r = 0; r < plan.Footprint.Regions.size(); ++r)
    {
        const SG::PavedRegion& region = plan.Footprint.Regions[r];
        const PavingCandidate& owner = paved.Candidates[plan.RegionOwners[r]];
        const Components::SplinePaving& recipe = owner.Recipe;
        const SG::GroundSampler& groundAt = groundUnder[plan.RegionOwners[r]];

        const std::string name = "SplinePaving_" + std::to_string(owner.Entity.id) + "_" + std::to_string(r);
        const SG::AreaGround ground = SG::BuildAreaGround(region.Pieces, SG::AreaGroundSettings{}, groundAt);
        const std::vector<PavingRoadUV> roads = RegionRoadUVs(plan, r, paved.Splines, paved.RoadRanks);
        std::vector<SG::AreaRing> outlines;
        for (const uint32 outline : region.Outlines)
            outlines.push_back(plan.Outlines[outline]);
        const PavingLidMesh lid =
            BuildPavingLidMesh(ground, roads, outlines, owner.WorldMatrix, PavingPatternPhase(recipe.Seed));

        PavingLidMesh border;
        if (recipe.BorderWidth > 0.0f)
        {
            SG::PavedBorderSettings settings;
            settings.Width = recipe.BorderWidth;
            settings.Height = recipe.BorderHeight;
            settings.Corner = recipe.BorderCorner == Components::SplineWallCorner::Round ? SG::SplineCornerStyle::Round
                                                                                          : SG::SplineCornerStyle::Miter;
            const SG::SplineProfile profile = SG::BuildPavedBorderProfile(settings);
            const Mathematics::Matrix4x4 toLocal = SplineLayout::InvertPlacerWorld(owner.WorldMatrix.Data());
            for (const SG::PavedBorderRun& run : SG::BuildPavedBorder(region.Runs, settings, groundAt, armVerges))
                AppendInOwnerSpace(SG::BuildSplineStrip(profile, run.Stations, run.Strip), toLocal, border);
        }

        auto& [lids, borders] = built.GetOrInsert(owner.Entity.id);
        lids.push_back(ToEngineMesh(lid, name + "_Lid"));
        borders.push_back(ToEngineMesh(border, name + "_Border"));
    }
    PavingRoadTrims trims = RoadTrimsOf(plan, groundUnder, paved.Candidates);
    if (HoldForPendingBvh(misses, paved.Candidates.front().Entity.id, m_Hold))
    {
        HoldRoads(m_RoadTrims, trims, m_HeldRoads);
        return false;
    }
    m_RoadTrims = std::move(trims);
    m_Approaches = ApproachesOf(plan, groundUnder, paved.Candidates, paved.Sources);
    ++m_ApproachesVersion;
    ReportConformMisses(misses, paved.Candidates.front().Entity.id, m_LoggedConformMisses);

    bool worldDirty = false;
    for (auto& [ownerId, owner] : m_Owners)
        owner.Built = false;
    m_Statuses.Clear();
    const auto removed = [&paved](uint32 entityId, const auto&) { return !HoldsCandidate(paved, entityId); };
    m_LastStates.EraseIf(removed);
    m_LoggedNotices.EraseIf(removed);
    for (uint32 s = 0; s < paved.Candidates.size(); ++s)
    {
        const PavingCandidate& candidate = paved.Candidates[s];
        const auto* nameComponent = world.GetComponent<Components::Name>(candidate.Entity);
        const std::string_view name = nameComponent ? nameComponent->View() : std::string_view();
        std::vector<std::string> notices = candidate.Notices;
        if (plan.Roles[s] == PavingRole::NothingToPave && paved.Sources[s].Outline.empty())
            notices.push_back(NothingToPaveNotice(name.empty() ? std::string_view("This road") : name));
        ReportRecipeValidation(notices, name, candidate.Entity.id, m_LoggedNotices.GetOrInsert(candidate.Entity.id));
        PavingStatus& status = m_Statuses.GetOrInsert(candidate.Entity.id);
        status.Entity = candidate.Entity;
        status.Role = plan.Roles[s];
        status.PavedBy = plan.Roles[s] == PavingRole::PavedBy ? paved.Candidates[plan.PavedBy[s]].Entity
                                                                 : ECS::EntityHandle{};
        status.Junctions = ResolveJunctions(paved, plan, s);
        status.Notices = std::move(notices);

        std::pair<BuildState, std::string_view>& last = m_LastStates.GetOrInsert(candidate.Entity.id);
        last = {plan.Roles[s] == PavingRole::NothingToPave ? BuildState::Failed : BuildState::Built,
                ReasonFor(plan.Roles[s])};
        states.Set(candidate.Entity, last.first, last.second);
        if (const auto* meshes = built.Find(candidate.Entity.id))
        {
            OwnerState& owner = m_Owners.GetOrInsert(candidate.Entity.id);
            owner.Built = true;
            worldDirty |= CommitOwner(world, renderServices, candidate, owner, meshes->first, meshes->second);
        }
    }

    // An owner that owns no region now gives its chunks up.
    m_Owners.EraseIf([&](uint32, OwnerState& owner)
    {
        if (owner.Built)
            return false;
        worldDirty |= owner.Lid.Retire(world, &renderServices.GetMeshGPURegistry());
        worldDirty |= owner.Border.Retire(world, &renderServices.GetMeshGPURegistry());
        return true;
    });
    return worldDirty;
}

bool SplinePavingController::CommitOwner(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                            const PavingCandidate& owner, OwnerState& state,
                                            std::span<const Mesh> lids, std::span<const Mesh> borders)
{
    const Components::SplinePaving& recipe = owner.Recipe;
    state.Look = {recipe.Material, recipe.BorderMaterial, recipe.CastShadows, recipe.ReceiveShadows};
    const Components::MaterialRef borderMaterial = recipe.BorderMaterial.IsNull() ? recipe.Material : recipe.BorderMaterial;
    Rendering::MeshGPURegistry& registry = renderServices.GetMeshGPURegistry();
    bool worldDirty = state.Lid.CommitSlots(world, registry, owner.Entity, LidKeySpace(owner.Entity.id), lids,
                                            RendererFor(renderServices, recipe.Material, state.Look), recipe.CastShadows);
    worldDirty |= state.Border.CommitSlots(world, registry, owner.Entity, BorderKeySpace(owner.Entity.id), borders,
                                           RendererFor(renderServices, borderMaterial, state.Look), recipe.CastShadows);
    return worldDirty;
}

void SplinePavingController::Rebind(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                       const PavingWorld& paved)
{
    for (const PavingCandidate& candidate : paved.Candidates)
    {
        OwnerState* owner = m_Owners.Find(candidate.Entity.id);
        const Components::SplinePaving& recipe = candidate.Recipe;
        const OwnerLook look{recipe.Material, recipe.BorderMaterial, recipe.CastShadows, recipe.ReceiveShadows};
        if (!owner || owner->Look == look)
            continue;
        owner->Look = look;
        const Components::MaterialRef borderMaterial = recipe.BorderMaterial.IsNull() ? recipe.Material : recipe.BorderMaterial;
        owner->Lid.Rebind(world, RendererFor(renderServices, recipe.Material, look), recipe.CastShadows);
        owner->Border.Rebind(world, RendererFor(renderServices, borderMaterial, look), recipe.CastShadows);
    }
}

Components::MeshRenderer SplinePavingController::RendererFor(Engine::Renderer::RenderServices& renderServices,
                                                                const Components::MaterialRef& material,
                                                                const OwnerLook& look)
{
    if (!m_DefaultMaterialRegistered && !renderServices.Materials().Registry().Find(DefaultPavingMaterialGuid()))
    {
        MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Spline Paving");
        doc.properties["baseColor"] = std::vector<float>{kDefaultPavingGray, kDefaultPavingGray, kDefaultPavingGray, 1.0f};
        renderServices.RegisterAndPrewarmMaterial(DefaultPavingMaterialGuid(), doc);
    }
    m_DefaultMaterialRegistered = true;

    // Null and unrenderable materials both take the built-in one (SplinePieceMaterial.h).
    Components::MaterialRef bound = material;
    if (!EnsureOverrideMaterialRegistered(renderServices, material, m_OverrideMaterials))
        bound.Set(DefaultPavingMaterialGuid());

    Components::MeshRenderer renderer{};
    renderer.materialAssetGuid = bound;
    renderer.shadowCasting = look.CastShadows ? Components::ShadowCasting::On : Components::ShadowCasting::Off;
    renderer.receiveShadows = look.ReceiveShadows;
    // Vertices replaced under a stable handle have no previous-frame match.
    renderer.motionVectors = false;
    return renderer;
}

const PavingStatus* SplinePavingController::StatusOf(ECS::EntityHandle entity) const
{
    const PavingStatus* status = m_Statuses.Find(entity.id);
    return status && status->Entity == entity ? status : nullptr;
}

void SplinePavingController::PrefillVergeMaterial(const ECS::World& world, ECS::EntityHandle road,
                                                  Components::SplineExtrude& recipe) const
{
    if (!recipe.VergeMaterial.IsNull())
        return;
    const Components::SplinePaving* first = nullptr;
    float32 firstMeters = 0.0f;
    for (const auto& [id, status] : m_Statuses)
    {
        const auto* paving = world.GetComponent<Components::SplinePaving>(status.Entity);
        if (!paving)
            continue;
        for (const PavingJunctionStatus& junction : status.Junctions)
        {
            if (junction.Road != road || junction.State != PavingJunctionState::Paved)
                continue;
            if (!first || junction.RoadMeters < firstMeters)
            {
                first = paving;
                firstMeters = junction.RoadMeters;
            }
        }
    }
    if (first)
        recipe.VergeMaterial = first->BorderMaterial.IsNull() ? first->Material : first->BorderMaterial;
}

bool SplinePavingController::RetireAll(ECS::World& world, Rendering::MeshGPURegistry* registry)
{
    bool worldDirty = false;
    for (auto& [ownerId, owner] : m_Owners)
    {
        worldDirty |= owner.Lid.Retire(world, registry);
        worldDirty |= owner.Border.Retire(world, registry);
    }
    m_Owners.Clear();
    return worldDirty;
}

void SplinePavingController::Shutdown(ECS::World& world, Engine::Renderer::RenderServices* renderServices)
{
    if (RetireAll(world, renderServices ? &renderServices->GetMeshGPURegistry() : nullptr))
        world.ProcessCommands();
    m_LoggedNotices.Clear();
    m_LastStates.Clear();
    m_RoadTrims.Clear();
    m_Approaches.clear();
    ++m_ApproachesVersion;
    m_Statuses.Clear();
    m_Gate.MarkUnbuilt();
}

} // namespace GameEngine::SplineRecipes
