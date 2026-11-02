#include "SplineRecipes/SplineExtrudeController.h"

#include "Assets/ModelAsset.h"
#include "Core/Engine.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Paving/PavingRoadUV.h"
#include "Scene/SceneTlas.h"
#include "SplineLayout/CenterlineSampling.h"
#include "SplineRecipes/GeneratedSplineMesh.h"
#include "SplineRecipes/Paving/PavingRoadTrim.h"
#include "SplineLayout/PieceEntity.h"
#include "SplineRecipes/SplineFillRebuild.h"
#include "SplineRecipes/SplineChunkCommit.h"
#include "SplineRecipes/SplineChunkSlots.h"
#include "SplineRecipes/RecipeValidation.h"
#include "SplineRecipes/SplineExtrudeSanitize.h"
#include "SplineRecipes/SplineSurfaceConform.h"
#include "SplineRecipes/SplineSweepStations.h"
#include "SplineRecipes/RecipeBuildStates.h"
#include "RoadVerge.h"
#include "SplineLayout/TileLayout.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "SplineGeometry/SplineEndTaper.h"
#include "SplineGeometry/SplineStripBuilder.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace GameEngine::SplineRecipes
{
namespace
{

using Mathematics::Vector3;
namespace SG = GameEngine::SplineGeometry;

// Chunk length in world metres. Chunking is what sets culling granularity for a
// long run, and it is what makes an incremental rebuild possible later without
// reshaping the keys. Chunks share their boundary station, so the two meshes
// meet on identical rings and there is no crack between them.
constexpr float32 kChunkLengthMetres = 50.0f;

// Ceiling on chunks per recipe, so a hostile spline cannot mint meshes without
// bound. At the chunk length above this is a 10 km run.
constexpr uint32 kMaxChunks = 200;

GUID DefaultExtrudeMaterialGuid()
{
    return GUID::Derive(GUID::Null(), "engine/material/spline-extrude-default");
}

// The key space a recipe's chunk meshes re-register under, one key per chunk
// slot. Registration is an in-place UPDATE when the key already exists, so a
// stable key is what keeps the handle, the GPUScene row and the spawned
// entity's meshGpuHandleId valid across rebuilds.
GUID ChunkKeySpace(uint32 entityId)
{
    return GUID::Derive(GUID::Null(), "engine/mesh/spline-extrude/" + std::to_string(entityId));
}

// The key space of a recipe's verge meshes, one key per chunk slot, as ChunkKeySpace.
GUID VergeKeySpace(uint32 entityId)
{
    return GUID::Derive(GUID::Null(), "engine/mesh/spline-extrude-verge/" + std::to_string(entityId));
}

Rendering::MeshGPURegistry* MeshRegistryOf(Engine::Renderer::RenderServices* renderServices)
{
    return renderServices ? &renderServices->GetMeshGPURegistry() : nullptr;
}

SG::SplineProfile BuildRecipeProfile(const Components::SplineExtrude& recipe)
{
    SG::SplineProfileParams params;
    switch (recipe.Profile)
    {
    case Components::SplineExtrudeProfile::Crown:
        params.Shape = SG::SplineProfileShape::Crown;
        break;
    case Components::SplineExtrudeProfile::Bevel:
        params.Shape = SG::SplineProfileShape::Bevel;
        break;
    }
    params.Width = recipe.Width;
    params.EdgeDrop = recipe.EdgeDrop;
    params.EdgeInset = recipe.EdgeInset;
    params.CrownRise = recipe.CrownRise;
    params.ShoulderWidth = recipe.ShoulderWidth;
    params.ShoulderDrop = recipe.ShoulderDrop;
    return SG::BuildProfile(params);
}

// The reason a build that made no chunk gives. A fill names why it found no water.
std::string_view EmptyBuildReason(const Components::SplineExtrude& recipe, WaterFillOutcome fillOutcome)
{
    if (recipe.WidthMode != Components::SplineExtrudeWidthMode::FitToBanks)
        return kBuiltNothingReason;
    switch (fillOutcome)
    {
    case WaterFillOutcome::NoTerrain:
        return kNoTerrainToFillReason;
    case WaterFillOutcome::SampleWindowTooLarge:
        return kFillWindowTooLargeReason;
    case WaterFillOutcome::OverBudget:
        return kFillOverBudgetReason;
    case WaterFillOutcome::NoRegion:
    case WaterFillOutcome::Built:
        break;
    }
    return kNoWaterToFillReason;
}

// The recipe with its look fields at their defaults: what the geometry is built from.
Components::SplineExtrude WithoutLook(Components::SplineExtrude recipe)
{
    const Components::SplineExtrude defaults{};
    recipe.Material = defaults.Material;
    recipe.VergeMaterial = defaults.VergeMaterial;
    recipe.CastShadows = defaults.CastShadows;
    recipe.ReceiveShadows = defaults.ReceiveShadows;
    return recipe;
}

// The recipe as its verge's chunks draw it: the verge's material, else the strip's.
Components::SplineExtrude VergeLook(Components::SplineExtrude recipe)
{
    if (!recipe.VergeMaterial.IsNull())
        recipe.Material = recipe.VergeMaterial;
    return recipe;
}

// Whether one end of a verge stretch gets a cap: the end of a run, and not on an Open edge the
// paving's curb reaches. On a closed road a run end on no Open edge is the loop's seam.
bool CapsVergeEnd(bool runEnd, bool closedRoad, const TrimmedRunEdge* edge)
{
    if (!runEnd)
        return false;
    if (edge && edge->Open)
        return !edge->Curbed;
    return !closedRoad;
}

// A conformed road's strip sits the ground lift above the ground, level with the paving it meets.
bool LiftsOffTheGround(const ECS::World& world, ECS::EntityHandle entity, const Components::SplineExtrude& recipe)
{
    const auto* member = world.GetComponent<Components::SplineNetworkMember>(entity);
    return recipe.ConformMode != Components::SplinePlacementConform::None && member &&
           member->Kind == Components::SplineNetworkMemberKind::Road;
}

SG::SplineProfileScale MapWidthScale(Components::SplineExtrudeWidthScale mode)
{
    switch (mode)
    {
    case Components::SplineExtrudeWidthScale::Uniform:
        return SG::SplineProfileScale::Uniform;
    case Components::SplineExtrudeWidthScale::None:
        return SG::SplineProfileScale::None;
    case Components::SplineExtrudeWidthScale::LateralOnly:
        break;
    }
    return SG::SplineProfileScale::LateralOnly;
}

} // namespace

SplineExtrudeController::Candidate SplineExtrudeController::MakeCandidate(ECS::World& world, ECS::EntityHandle entity,
                                                                          const Components::SplineComponent& spline, const Components::SplineExtrude& recipe,
                                                                          const Components::WorldTransform& transform)
{
    Candidate candidate;
    candidate.Entity = entity;
    // Sanitize once, here: everything downstream, the settle comparison included,
    // sees only finite floats, so a poisoned field cannot make a recipe unequal to itself.
    candidate.Recipe = SanitizeExtrudeRecipe(recipe);
    candidate.RetiredProfile = CarriesRetiredWallProfile(world, entity);
    candidate.SplineDataIndex = spline.SplineDataIndex;
    candidate.SplineDataGeneration = spline.SplineDataGeneration;
    candidate.WorldMatrix = Mathematics::Matrix4x4::FromColumnMajor(transform.matrix);
    return candidate;
}

void SplineExtrudeController::Update(ECS::World& world,
                                     Engine::Renderer::RenderServices& renderServices,
                                     float32 deltaSeconds,
                                     RecipeBuildStates& states,
                                     const PavingRoadTrims& roadTrims,
                                     std::span<const uint32> heldRoads)
{
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!splineService)
        return;

    // A world reset destroys every generated chunk with no per-entity Removed
    // event and restarts entity versions, so the cached handles now alias
    // unrelated entities — the retire sweep below would destroy those. The
    // scene-swap sites release this controller beforehand, but World::Clear is
    // also reached through World::DeserializeWorld (play-mode exit, undo
    // snapshot restore), which no swap-site hook can cover. Drop the
    // bookkeeping here, before the query, so state rebuilt for the new world is
    // untouched.
    const uint64 resetGeneration = world.GetLifecycleResetGeneration();
    if (resetGeneration != m_LastWorldResetGeneration)
    {
        m_LastWorldResetGeneration = resetGeneration;
        DropStateAfterWorldReset(&renderServices);
    }

    ++m_VisitStamp;
    m_PendingTargetsCollected = false;

    std::vector<Candidate> candidates;
    world.Query<ECS::Read<Components::SplineComponent>,
                ECS::Read<Components::SplineExtrude>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle e, const Components::SplineComponent& spline, const Components::SplineExtrude& recipe,
                  const Components::WorldTransform& xf)
              { candidates.push_back(MakeCandidate(world, e, spline, recipe, xf)); });

    bool worldDirty = false;

    // One reading of the ground per frame, shared by every candidate that reads
    // it: it is a property of the world, not of a spline. A scene where no
    // extrude recipe reads the ground never scans the terrains for it.
    //
    // Same predicate as the observation below — a candidate that fits to banks
    // without conforming reads the ground too, and skipping the scan for it
    // would hand it a zero digest to observe.
    const bool anyReadsSurface =
        std::any_of(candidates.begin(), candidates.end(),
                    [](const Candidate& c) { return Components::ReadsSurface(c.Recipe); });
    const uint64 surfaceRevision = anyReadsSurface ? ConformSurfaceRevision(world, ResolveTerrainModifierSystem()) : 0u;
    // One request of the scene TLAS per frame, shared by every candidate that conforms to the scene.
    const bool anyConformsToScene =
        std::any_of(candidates.begin(), candidates.end(), [](const Candidate& c)
                    { return c.Recipe.ConformMode != Components::SplinePlacementConform::None &&
                             c.Recipe.ConformTarget == Components::SplineConformTarget::Scene; });
    Scene::SceneTlas* sceneTlas = anyConformsToScene ? &Scene::GetSceneTlas(world) : nullptr;
    if (sceneTlas)
        sceneTlas->RequestCurrent(world);

    for (const Candidate& candidate : candidates)
    {
        ExtrudeState& state = m_States.GetOrInsert(candidate.Entity.id);
        state.VisitStamp = m_VisitStamp;

        if (candidate.RetiredProfile)
        {
            worldDirty |= RetireChunks(world, &renderServices.GetMeshGPURegistry(), state);
            state.Gate.MarkUnbuilt();
            const auto* entityName = world.GetComponent<Components::Name>(candidate.Entity);
            ReportRecipeValidation({RetiredWallProfileValidation()},
                                   entityName ? entityName->View() : std::string_view(),
                                   candidate.Entity.id, state.LoggedValidation);
            states.Set(candidate.Entity, BuildState::Failed, RetiredWallProfileValidation());
            continue;
        }

        ObservedState current;
        current.Spline = ObserveSpline(*splineService, candidate.SplineDataIndex,
                                       candidate.SplineDataGeneration, candidate.WorldMatrix);
        current.Recipe = WithoutLook(candidate.Recipe);
        current.LiftsOffTheGround = LiftsOffTheGround(world, candidate.Entity, candidate.Recipe);
        const ExtrudeLook look{candidate.Recipe.Material, candidate.Recipe.VergeMaterial, candidate.Recipe.CastShadows,
                               candidate.Recipe.ReceiveShadows};
        // Fitting rings to the banks reads the ground whatever ConformMode says,
        // so this recipe's ground-reader predicate is ReadsSurface, not "conforms".
        const bool readsSurface = Components::ReadsSurface(candidate.Recipe);
        current.SurfaceRevision = ObservedSurfaceRevision(readsSurface, surfaceRevision);
        if (const std::vector<PavingRoadTrim>* trims = roadTrims.Find(candidate.Entity.id))
            current.Trims = *trims;

        if (!current.Spline.Valid)
        {
            worldDirty |= RetireChunks(world, &renderServices.GetMeshGPURegistry(), state);
            state.Gate.MarkUnbuilt();
            states.Set(candidate.Entity, BuildState::Failed, kSplineTooShortReason);
            continue;
        }

        // Only the terrains near this run can hold its first build. A Scene conform also reads
        // the meshes under the run, so they join its surface term.
        const Mathematics::AABB runBounds =
            readsSurface ? RunWorldBounds(*splineService, candidate.SplineDataIndex,
                                          candidate.SplineDataGeneration, candidate.WorldMatrix)
                         : Mathematics::AABB::Empty();
        const ConformSurfaceReadiness surfaceReadiness =
            readsSurface ? ConformSurfaceState(world, runBounds) : ConformSurfaceReadiness::NoSurface;
        if (candidate.Recipe.ConformMode != Components::SplinePlacementConform::None &&
            candidate.Recipe.ConformTarget == Components::SplineConformTarget::Scene)
        {
            RefreshSceneConformDigest(world, *sceneTlas, runBounds, state.SceneDigest);
            current.SurfaceRevision = Hashing::Fnv1a64(&state.SceneDigest.Value, sizeof(state.SceneDigest.Value),
                                                       current.SurfaceRevision);
        }
        bool releaseWaitingChunks = false;
        if (!state.Models.Bounds.empty())
        {
            state.Models.HeldSeconds += deltaSeconds;
            releaseWaitingChunks = ChunkModelHoldReleases(state.Models, PendingTargets(world));
        }
        // A region that rebuilt this update moved the trims: the strip follows now, not after a settle.
        const bool trimsMoved = state.Gate.AppliedOnce() && current.Trims != state.AppliedTrims;
        if (!releaseWaitingChunks && !trimsMoved &&
            !state.Gate.ShouldRebuild(current, state.Chunks.ChunksAlive(world) && state.VergeChunks.ChunksAlive(world),
                                      surfaceReadiness, readsSurface,
                                      deltaSeconds, candidate.Entity.id))
        {
            if (state.Gate.AppliedOnce() && !(look == state.AppliedLook))
            {
                state.Chunks.Rebind(world, ChunkRenderer(renderServices, candidate.Recipe),
                                    candidate.Recipe.CastShadows);
                state.VergeChunks.Rebind(world, ChunkRenderer(renderServices, VergeLook(candidate.Recipe)),
                                         candidate.Recipe.CastShadows);
                state.AppliedLook = look;
            }
            const BuildState waiting = StateWhileGateWaits(state.Gate, current);
            if (waiting == BuildState::Built && !state.Models.Bounds.empty())
                states.Set(candidate.Entity, BuildState::Held, state.Models.HeldReason);
            else if (waiting == BuildState::Built && !state.Models.FailedReason.empty())
                states.Set(candidate.Entity, BuildState::Failed, state.Models.FailedReason);
            else
                states.Set(candidate.Entity, waiting,
                           waiting == BuildState::Held    ? kHeldForTerrainReason
                           : waiting == BuildState::Built ? state.EmptyReason
                                                          : std::string_view());
            continue;
        }

        // The region this road enters waits for picking data: the strip keeps its pieces and its
        // trims until the region builds, so both change in one update.
        if (std::binary_search(heldRoads.begin(), heldRoads.end(), candidate.Entity.id))
        {
            states.Set(candidate.Entity, BuildState::Held, kHeldForPickingDataReason);
            continue;
        }

        state.Hold.Held = false;

        if (Rebuild(world, renderServices, candidate, current.Trims, state))
            worldDirty = true;

        if (!state.Hold.Held)
        {
            state.Gate.MarkBuilt(current);
            state.AppliedTrims = current.Trims;
            state.EmptyReason = state.Chunks.HasChunks() ? std::string_view()
                                                         : EmptyBuildReason(candidate.Recipe, state.LastFillOutcome);
        }
        if (state.Hold.Held)
            states.Set(candidate.Entity, BuildState::Held, kHeldForPickingDataReason);
        else if (!state.Models.Bounds.empty())
            states.Set(candidate.Entity, BuildState::Held, state.Models.HeldReason);
        else if (!state.Models.FailedReason.empty())
            states.Set(candidate.Entity, BuildState::Failed, state.Models.FailedReason);
        else
            states.Set(candidate.Entity, BuildState::Built, state.EmptyReason);
    }

    m_States.EraseIf([&](uint32, ExtrudeState& state)
    {
        if (state.VisitStamp == m_VisitStamp)
            return false;
        worldDirty |= RetireChunks(world, &renderServices.GetMeshGPURegistry(), state);
        return true;
    });

    if (worldDirty)
        world.ProcessCommands();
}

std::span<const PendingConformTarget> SplineExtrudeController::PendingTargets(ECS::World& world)
{
    if (!m_PendingTargetsCollected)
    {
        m_PendingTargets = CollectPendingConformTargets(world);
        m_PendingTargetsCollected = true;
    }
    return m_PendingTargets;
}

void SplineExtrudeController::DropStateAfterWorldReset(
    Engine::Renderer::RenderServices* renderServices)
{
    for (auto& [entityId, state] : m_States)
    {
        state.Chunks.DropAfterWorldReset(MeshRegistryOf(renderServices));
        state.VergeChunks.DropAfterWorldReset(MeshRegistryOf(renderServices));
    }
    m_States.Clear();
}

void SplineExtrudeController::Shutdown(ECS::World& world,
                                       Engine::Renderer::RenderServices* renderServices)
{
    bool worldDirty = false;
    for (auto& [entityId, state] : m_States)
        worldDirty |= RetireChunks(world, MeshRegistryOf(renderServices), state);
    m_States.Clear();
    if (worldDirty)
        world.ProcessCommands();
}

bool SplineExtrudeController::Rebuild(ECS::World& world,
                                      Engine::Renderer::RenderServices& renderServices,
                                      const Candidate& candidate, std::span<const PavingRoadTrim> trims,
                                      ExtrudeState& state)
{
    auto* splineService = SplineECS::SplineService::TryGet();
    const SplineECS::SplineHandle handle(candidate.SplineDataIndex,
                                         candidate.SplineDataGeneration);

    // Self-heal a stale arc-length LUT (a points writer that forgot RebuildCache).
    if (const auto* dirtyData = splineService->GetSplineData(handle); dirtyData && dirtyData->Dirty)
        splineService->RebuildCache(handle);

    const Spline::SplineData* data = splineService->GetSplineData(handle);
    if (!data || !data->IsValid() || data->TotalArcLength <= 0.0f)
        return RetireChunks(world, &renderServices.GetMeshGPURegistry(), state);

    const Components::SplineExtrude& recipe = candidate.Recipe;
    const SG::SplineProfile profile = BuildRecipeProfile(recipe);
    if (!profile.IsValid())
        return RetireChunks(world, &renderServices.GetMeshGPURegistry(), state);

    const Mathematics::Matrix4x4& worldM = candidate.WorldMatrix;
    const Vector3 worldOrigin = worldM.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));
    const float32 axisScale = SplineLayout::LargestAxisScale(worldM);

    // Geometry is generated in the PLACER'S LOCAL SPACE so a chunk entity can be
    // a child carrying an identity transform: the hierarchy system then makes
    // its world transform exactly the placer's, and the vertices land where they
    // were conformed. Building in world space instead would need the placer's
    // transform decomposed out of every chunk pose.
    const Mathematics::Matrix4x4 invPlacerWorld = SplineLayout::InvertPlacerWorld(candidate.WorldMatrix.Data());

    const float32 localArcLength = data->TotalArcLength;
    const float32 worldArcLength = SplineLayout::WorldCenterlineLength(localArcLength, worldM);
    const bool overCenterlineBudget = worldArcLength > SplineLayout::kMaxCenterlineLengthMetres;
    if (overCenterlineBudget && !state.CenterlineBudgetLogged)
    {
        Logger::Log::Warning(
            "SplineExtrude: entity {} has a {:.0f} m centerline, past the {:.0f} m drape "
            "budget — the {:.2f} m sampling step coarsens beyond it, so the generated "
            "surface follows the ground only approximately there",
            candidate.Entity.id, worldArcLength, SplineLayout::kMaxCenterlineLengthMetres,
            SplineLayout::kCenterlineStepMetres);
    }
    state.CenterlineBudgetLogged = overCenterlineBudget;

    const bool fitToBanks =
        recipe.WidthMode == Components::SplineExtrudeWidthMode::FitToBanks;
    const SweepShape shape = SweepShapeForExtrude();
    const SweepSamples samples =
        SampleSweepCenterline(*data, SplineLayout::CenterlineSampleCount(worldArcLength), worldM, shape);
    if (samples.Frames.size() < 2u)
        return RetireChunks(world, &renderServices.GetMeshGPURegistry(), state);

    const std::vector<ECS::EntityHandle> ignore = CollectConformRayIgnoreList(world);
    ConformMissTally conformMisses;
    const bool conform = recipe.ConformMode != Components::SplinePlacementConform::None;
    const Vector3 worldUp(0.0f, 1.0f, 0.0f);

    // ---- Draped centerline, in world space (where "down" is unambiguous) ----
    std::vector<SplineLayout::CenterSample> center;
    center.reserve(samples.Frames.size());
    for (size_t i = 0; i < samples.Frames.size(); ++i)
    {
        const Spline::SplineFrame& frame = samples.Frames[i];
        Vector3 pos = worldM.TransformPoint(frame.Position);
        const Vector3 worldTangent = SplineLayout::NormalizedOrFallback(
            worldM.TransformPoint(frame.Forward) - worldOrigin, Vector3(0.0f, 0.0f, 1.0f));
        const Vector3 travel = SplineLayout::NormalizedOrFallback(Vector3(worldTangent.x, 0.0f, worldTangent.z),
                                                    Vector3(0.0f, 0.0f, 1.0f));
        if (recipe.LateralOffset != 0.0f)
        {
            // Cross(worldUp, travel) is the RIGHT of travel in this LH +Y-up
            // engine — the same convention the placement recipes use, so a
            // generated ribbon and a placed path agree about sides.
            const Vector3 right = SplineLayout::NormalizedOrFallback(Vector3::Cross(worldUp, travel), Vector3(1.0f, 0.0f, 0.0f));
            pos = pos + right * recipe.LateralOffset;
        }

        SplineLayout::CenterSample sample;
        sample.Pos = pos;
        sample.Normal = worldUp;
        // An inserted sample casts no ray: DrapeInsertedSamples below gives it
        // the drape of the probed samples either side.
        if (conform && samples.Probed[i])
        {
            Vector3 hit;
            Vector3 hitNormal = worldUp;
            if (ConformRayDown(world, pos, ignore, hit, &hitNormal, &conformMisses,
                               recipe.ConformTarget))
            {
                sample.Pos.y = hit.y;
                sample.Normal = hitNormal;
            }
            else
                sample.SurfaceValid = false; // keep an unmeasured sample out of the length
        }
        center.push_back(sample);
    }
    if (conform)
        DrapeInsertedSamples(samples, worldM, center);
    SplineLayout::HoldSurfaceAcrossGaps(center);
    if (HoldForPendingBvh(conformMisses, candidate.Entity.id, state.Hold))
        return false;
    ReportConformMisses(conformMisses, candidate.Entity.id, state.LoggedConformMisses);

    const bool lifts = LiftsOffTheGround(world, candidate.Entity, recipe);
    const float32 lift = recipe.VerticalOffset + (lifts ? kGroundSurfaceLiftMeters : 0.0f);
    for (SplineLayout::CenterSample& sample : center)
        sample.Pos.y += lift;

    // ---- Stations: one per draped sample, walked by true 3-D length ----
    //
    // FitToBanks does not sweep a cross-section at all: it floods the region the
    // water occupies over the terrain. It needs its stations in WORLD space,
    // because the terrain lattice it reads exists in no other space, so the two
    // modes take two station streams off the same draped centreline.
    SweepStationStream stream =
        BuildSweepStations(*data, samples, center, invPlacerWorld, profile.NominalHalfWidth,
                           MapWidthScale(recipe.WidthScale), fitToBanks, shape);
    std::vector<SG::SplineStripStation>& stations = stream.Local;
    const std::vector<float32>& worldDistance = stream.WorldDistance;

    // ---- FitToBanks: flood the region, mesh it, and leave ----
    //
    // Structurally separate rather than a variation on the sweep: this path has
    // no profile, no half-width and no end taper. EndTaperMetres is IGNORED here
    // by design — a region's end is already the rim of the bowl its bed was
    // carved into, and pinching it to a point would put back the very mismatch
    // the fill exists to remove.
    if (fitToBanks)
    {
        state.Models = {};
        const WaterFillRun fill = BuildWaterFillRun(world, stream.World, recipe,
                                                    candidate.WorldMatrix, ResolveTerrainModifierSystem());
        ReportWaterFillDiagnostics(fill, candidate.Entity.id, state.LastFillDiagnostics,
                                   state.LastFillOutcome);
        const bool vergeRetired = state.VergeChunks.Retire(world, &renderServices.GetMeshGPURegistry());
        return CommitChunkMeshes(world, renderServices, candidate, state, fill.Chunks) || vergeRetired;
    }

    // The ends stop somewhere whatever sized the rings, so the taper runs after
    // the width mode has committed its half-widths. TaperMetres is walked
    // against the stations' WORLD distances; the drop moves a LOCAL position, so
    // it is the profile's own EdgeDrop rather than a world metre count.
    SG::SplineEndTaperParams taperParams;
    taperParams.TaperMetres = std::max(0.0f, recipe.EndTaperMetres);
    taperParams.HeightDrop = std::max(0.0f, recipe.EdgeDrop);
    taperParams.ClosedLoop = data->IsEffectivelyClosed();
    SG::ApplyEndTaper(stations, taperParams);

    SG::SplineStripParams stripParams;
    stripParams.WidthScale = MapWidthScale(recipe.WidthScale);
    stripParams.TilesPerMetreU = recipe.TilesPerMetreU;
    // V is measured on the LOCAL cross-section; scaling by the placer's
    // LARGEST axis scale keeps it world metres like U. Under a non-uniform
    // placer scale this over-tiles edges that run along the smaller axes — the
    // same accepted degradation as the frame mapping above.
    stripParams.TilesPerMetreV = recipe.TilesPerMetreV * axisScale;
    // Stations carry GLOBAL run distances, and chunks share their boundary
    // stations: one U origin for the whole run is what makes the shared ring
    // get the same U from both sides.
    stripParams.UOriginMetres = worldDistance.front();
    // A closed loop's whole-tile snap is computed ONCE over the full run and
    // pre-multiplied into the density. The builder's own SnapUForClosedLoop
    // stays off: it rounds against the span it is given, which per chunk would
    // drift the density and leave a partial tile at the loop seam.
    if (data->IsEffectivelyClosed() && std::isfinite(stripParams.TilesPerMetreU) &&
        stripParams.TilesPerMetreU > 0.0f)
    {
        const float32 runLength = worldDistance.back() - worldDistance.front();
        if (runLength > 0.0f)
        {
            const float32 wholeTiles =
                std::max(1.0f, std::round(runLength * stripParams.TilesPerMetreU));
            stripParams.TilesPerMetreU = wholeTiles / runLength;
        }
    }

    // ---- Paved regions: the strip runs around the arc length they take over ----
    std::span<const SG::SplineStripStation> sweptStations = stations;
    std::span<const SplineLayout::CenterSample> sweptCenter = center;
    TrimmedSweepStations trimmed;
    std::vector<SplineLayout::CenterSample> trimmedCenter;
    const bool trimming = !trims.empty() && samples.LocalDistance.size() == stations.size();
    if (trimming)
    {
        const float32 arcScale = RoadArcScale(worldM);
        std::vector<float32> roadDistance;
        roadDistance.reserve(samples.LocalDistance.size());
        for (const float32 local : samples.LocalDistance)
            roadDistance.push_back(local * arcScale);
        const float32 loopMeters = data->IsEffectivelyClosed() ? localArcLength * arcScale : 0.0f;
        trimmed = TrimSweepStations(stations, roadDistance, loopMeters, trims, invPlacerWorld);
        for (const uint32 source : trimmed.Source)
            trimmedCenter.push_back(center[source]);
        sweptStations = trimmed.Stations;
        sweptCenter = trimmedCenter;
    }

    // ---- Chunking: one mesh per ~50 m of run, never across a paved region ----
    std::vector<std::pair<uint32, uint32>> chunkRanges;
    // Per chunk, its run's index into trimmed.Runs; the whole run when not trimming.
    std::vector<uint32> chunkRuns;
    if (!trimming)
    {
        chunkRanges = CarveChunkRanges(worldDistance, kChunkLengthMetres, kMaxChunks);
        chunkRuns.assign(chunkRanges.size(), 0u);
    }
    for (uint32 run = 0; run < trimmed.Runs.size(); ++run)
    {
        const auto [first, last] = trimmed.Runs[run];
        std::vector<float32> runDistance;
        for (uint32 i = first; i <= last; ++i)
            runDistance.push_back(trimmed.Stations[i].Distance);
        for (const auto& [low, high] : CarveChunkRanges(runDistance, kChunkLengthMetres, kMaxChunks))
        {
            chunkRanges.emplace_back(first + low, first + high);
            chunkRuns.push_back(run);
        }
    }
    if (chunkRanges.empty())
        return RetireChunks(world, &renderServices.GetMeshGPURegistry(), state);

    // A chunk that waits for a model near it builds empty. Only a Scene conform reads models.
    std::vector<bool> waits(chunkRanges.size(), false);
    if (conform && recipe.ConformTarget == Components::SplineConformTarget::Scene)
        waits = HoldChunksForPendingModels(sweptCenter, chunkRanges, PendingTargets(world), candidate.Entity.id,
                                           state.Models);
    else
        state.Models = {};

    // Build every chunk's geometry first: the slot plan needs to know which
    // stretches of the run produced nothing.
    std::vector<SG::SplineStripMesh> strips(chunkRanges.size());
    for (uint32 c = 0; c < chunkRanges.size(); ++c)
    {
        if (waits[c])
            continue;
        const auto [firstStation, lastStation] = chunkRanges[c];
        const std::span<const SG::SplineStripStation> span = sweptStations.subspan(firstStation,
                                                                                   lastStation - firstStation + 1u);
        strips[c] = SG::BuildSplineStrip(profile, span, stripParams);
    }

    if (!Components::BuildsVerge(recipe))
    {
        const bool vergeRetired = state.VergeChunks.Retire(world, &renderServices.GetMeshGPURegistry());
        return CommitChunkMeshes(world, renderServices, candidate, state, strips) || vergeRetired;
    }

    // The verge follows each chunk of the strip; a chunk end inside a run is welded to the next chunk.
    const bool closedRoad = data->IsEffectivelyClosed();
    std::vector<SG::SplineStripMesh> verges(chunkRanges.size());
    for (uint32 c = 0; c < chunkRanges.size(); ++c)
    {
        if (waits[c])
            continue;
        const auto [firstStation, lastStation] = chunkRanges[c];
        const auto [runFirst, runLast] =
            trimming ? trimmed.Runs[chunkRuns[c]] : std::pair<uint32, uint32>(0u, static_cast<uint32>(stations.size()) - 1u);
        const TrimmedRunEdge* startEdge =
            trimming && firstStation == runFirst ? &trimmed.RunEdges[chunkRuns[c]].first : nullptr;
        const TrimmedRunEdge* endEdge =
            trimming && lastStation == runLast ? &trimmed.RunEdges[chunkRuns[c]].second : nullptr;
        const RoadVergeEnd start{CapsVergeEnd(firstStation == runFirst, closedRoad, startEdge), startEdge};
        const RoadVergeEnd end{CapsVergeEnd(lastStation == runLast, closedRoad, endEdge), endEdge};
        verges[c] = BuildRoadVerge(recipe, sweptStations.subspan(firstStation, lastStation - firstStation + 1u), start,
                                   end, worldDistance.front());
    }
    const bool stripDirty = CommitChunkMeshes(world, renderServices, candidate, state, strips);
    return CommitVergeMeshes(world, renderServices, candidate, state, verges) || stripDirty;
}

bool SplineExtrudeController::RetireChunks(ECS::World& world, Rendering::MeshGPURegistry* registry,
                                           ExtrudeState& state)
{
    const bool strip = state.Chunks.Retire(world, registry);
    const bool verge = state.VergeChunks.Retire(world, registry);
    return strip || verge;
}

bool SplineExtrudeController::CommitVergeMeshes(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                                const Candidate& candidate, ExtrudeState& state,
                                                const std::vector<SG::SplineStripMesh>& vergeMeshes)
{
    std::vector<Mesh> meshes(vergeMeshes.size());
    for (size_t c = 0; c < vergeMeshes.size(); ++c)
    {
        if (vergeMeshes[c].IsValid())
            meshes[c] = ToEngineMesh(vergeMeshes[c], "SplineExtrude_" + std::to_string(candidate.Entity.id) + "_Verge_" +
                                                         std::to_string(c));
    }
    return state.VergeChunks.CommitSlots(world, renderServices.GetMeshGPURegistry(), candidate.Entity,
                                         VergeKeySpace(candidate.Entity.id), meshes,
                                         ChunkRenderer(renderServices, VergeLook(candidate.Recipe)),
                                         candidate.Recipe.CastShadows);
}

float32 SplineExtrudeController::StripTopHalfWidth(const Components::SplineExtrude& authored, float32 stationHalfWidth)
{
    const Components::SplineExtrude recipe = SanitizeExtrudeRecipe(authored);
    if (recipe.WidthMode == Components::SplineExtrudeWidthMode::FitToBanks)
        return stationHalfWidth;
    const SG::SplineProfile profile = BuildRecipeProfile(recipe);
    // The walkable surface of every preset is at y = 0 and above; the edge drop and the shoulders are below it.
    float32 top = 0.0f;
    for (const SG::SplineProfilePoint& point : profile.Points)
    {
        if (point.Position.y >= 0.0f)
            top = std::max(top, std::abs(point.Position.x));
    }
    if (!profile.IsValid() || !(top > 0.0f))
        return stationHalfWidth;
    const float32 placed =
        recipe.WidthScale == Components::SplineExtrudeWidthScale::None ? profile.NominalHalfWidth : stationHalfWidth;
    return top / profile.NominalHalfWidth * placed;
}

std::optional<PavingCapUV> StripCapUV(const Components::SplineExtrude& authored,
                                      const Mathematics::Matrix4x4& worldMatrix, float32 topHalfLeft,
                                      float32 topHalfRight, float32 roadLoopMeters)
{
    const Components::SplineExtrude recipe = SanitizeExtrudeRecipe(authored);
    if (recipe.WidthMode == Components::SplineExtrudeWidthMode::FitToBanks)
        return std::nullopt;
    const SG::SplineProfile profile = BuildRecipeProfile(recipe);
    if (!profile.IsValid() || profile.Closed)
        return std::nullopt;
    const float32 axisScale = SplineLayout::LargestAxisScale(worldMatrix);
    if (!(axisScale > 0.0f))
        return std::nullopt;

    // The top's ends in the profile: the outermost points at y = 0 and above on each side.
    float32 topLeftX = 0.0f;
    float32 topRightX = 0.0f;
    for (const SG::SplineProfilePoint& point : profile.Points)
    {
        if (point.Position.y < 0.0f)
            continue;
        topLeftX = std::max(topLeftX, -point.Position.x);
        topRightX = std::max(topRightX, point.Position.x);
    }
    if (!(topLeftX > 0.0f) || !(topRightX > 0.0f))
        return std::nullopt;

    // V is the scaled profile's perimeter from its first point; the top starts at the first point
    // at y = 0 or above. The cap is flat, so every height above zero scales to zero.
    const SG::SplineProfileScale mode = MapWidthScale(recipe.WidthScale);
    SG::SplineStripScale scale;
    if (mode != SG::SplineProfileScale::None)
    {
        scale.Left = topHalfLeft / axisScale / topLeftX;
        scale.Right = topHalfRight / axisScale / topRightX;
        scale.ScaleVertical = mode == SG::SplineProfileScale::Uniform;
    }
    scale.Rise = 0.0f;
    std::vector<Mathematics::Vector2> scaledPoints(profile.Points.size());
    std::vector<float32> pointV(profile.Points.size());
    SG::StripProfileV(profile, scale, scaledPoints, pointV);
    const auto top = std::find_if(profile.Points.begin(), profile.Points.end(),
                                  [](const SG::SplineProfilePoint& point) { return point.Position.y >= 0.0f; });
    const float32 leftV = pointV[static_cast<size_t>(top - profile.Points.begin())];

    PavingCapUV uv;
    uv.TilesPerMeterU = recipe.TilesPerMetreU;
    // A closed road rounds its length to whole tiles, as the sweep does.
    if (roadLoopMeters > 0.0f && recipe.TilesPerMetreU > 0.0f)
        uv.TilesPerMeterU = std::max(1.0f, std::round(roadLoopMeters * recipe.TilesPerMetreU)) / roadLoopMeters;
    uv.TilesPerMeterV = recipe.TilesPerMetreV;
    uv.LeftV = leftV * recipe.TilesPerMetreV * axisScale;
    return uv;
}

Components::MeshRenderer SplineExtrudeController::ChunkRenderer(Engine::Renderer::RenderServices& renderServices,
                                                                const Components::SplineExtrude& recipe)
{
    // Mint a default material once, so dropping the recipe on a spline renders
    // something on the first try rather than nothing.
    if (!m_DefaultMaterialRegistered &&
        !renderServices.Materials().Registry().Find(DefaultExtrudeMaterialGuid()))
    {
        MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Spline Extrude");
        renderServices.RegisterAndPrewarmMaterial(DefaultExtrudeMaterialGuid(), doc);
    }
    m_DefaultMaterialRegistered = true;

    // Null and unrenderable recipe materials both take the minted default:
    // binding an unregistered GUID would not retexture the chunks, it would
    // freeze them at their last registered material (SplinePieceMaterial.h).
    const bool overrideRenderable =
        EnsureOverrideMaterialRegistered(renderServices, recipe.Material, m_OverrideMaterials);
    Components::MaterialRef material = recipe.Material;
    if (!overrideRenderable)
        material.Set(DefaultExtrudeMaterialGuid());

    Components::MeshRenderer renderer{};
    renderer.materialAssetGuid = material;
    renderer.shadowCasting = recipe.CastShadows ? Components::ShadowCasting::On : Components::ShadowCasting::Off;
    renderer.receiveShadows = recipe.ReceiveShadows;
    // A chunk whose vertices are wholesale replaced under a stable handle has
    // no previous-frame correspondence, so motion vectors would ghost for one
    // frame after every rebuild.
    renderer.motionVectors = false;

    return renderer;
}

template <typename MeshT>
bool SplineExtrudeController::CommitChunkMeshes(ECS::World& world,
                                                Engine::Renderer::RenderServices& renderServices,
                                                const Candidate& candidate, ExtrudeState& state,
                                                const std::vector<MeshT>& chunkMeshes)
{
    const Components::SplineExtrude& recipe = candidate.Recipe;
    if (chunkMeshes.empty())
        return RetireChunks(world, &renderServices.GetMeshGPURegistry(), state);

    const Components::MeshRenderer renderer = ChunkRenderer(renderServices, recipe);
    state.AppliedLook = {recipe.Material, recipe.VergeMaterial, recipe.CastShadows, recipe.ReceiveShadows};

    // An invalid chunk stays an empty mesh: its slot is a hole.
    std::vector<Mesh> meshes(chunkMeshes.size());
    for (size_t c = 0; c < chunkMeshes.size(); ++c)
    {
        if (chunkMeshes[c].IsValid())
        {
            meshes[c] = ToEngineMesh(chunkMeshes[c], "SplineExtrude_" +
                                                         std::to_string(candidate.Entity.id) + "_" +
                                                         std::to_string(c));
        }
    }
    return state.Chunks.CommitSlots(world, renderServices.GetMeshGPURegistry(), candidate.Entity,
                                    ChunkKeySpace(candidate.Entity.id), meshes, renderer,
                                    recipe.CastShadows);
}

} // namespace GameEngine::SplineRecipes
