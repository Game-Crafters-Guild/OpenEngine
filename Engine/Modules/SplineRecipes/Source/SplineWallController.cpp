#include "SplineRecipes/SplineWallController.h"

#include "Assets/ModelAsset.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "SplineLayout/CenterlineSampling.h"
#include "SplineRecipes/GeneratedSplineMesh.h"
#include "SplineLayout/PieceEntity.h"
#include "SplineRecipes/RecipeValidation.h"
#include "SplineRecipes/SplineChunkSlots.h"
#include "SplineRecipes/SplineSurfaceConform.h"
#include "SplineRecipes/SplineSweepStations.h"
#include "SplineRecipes/SplineWallBuild.h"
#include "SplineRecipes/SplineWallSanitize.h"
#include "SplineRecipes/RecipeBuildStates.h"
#include "SplineLayout/TileLayout.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "SplineGeometry/SplineStripBuilder.h"
#include "TerrainECS/TerrainModifierSystem.h"

#include <algorithm>
#include <string>

namespace GameEngine::SplineRecipes
{
namespace
{

using Mathematics::Vector3;
namespace SG = GameEngine::SplineGeometry;

// Chunk length in world metres, and the ceiling on chunks per wall: the
// extrude's, so a wall and a road of one length cull alike. Chunks share their
// boundary station, so neighbouring meshes meet on identical rings.
constexpr float32 kChunkLengthMetres = 50.0f;
constexpr uint32 kMaxChunks = 200;

// The built-in material's linear base colour: a flat mid grey, so a wall with no
// material assigned renders as a wall rather than a white block.
constexpr float32 kDefaultWallGrey = 0.45f;

const TerrainECS::TerrainModifierSystem* ResolveModifierSystem()
{
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    auto* systems = loop ? loop->GetSystemManager() : nullptr;
    return systems ? systems->GetSystem<TerrainECS::TerrainModifierSystem>() : nullptr;
}

GUID DefaultWallMaterialGuid()
{
    return GUID::Derive(GUID::Null(), "engine/material/spline-wall-default");
}

// The key space a wall's chunk meshes re-register under, one key per chunk slot,
// so the handle, the GPUScene row and the chunk entity survive a rebuild.
GUID ChunkKeySpace(uint32 entityId)
{
    return GUID::Derive(GUID::Null(), "engine/mesh/spline-wall/" + std::to_string(entityId));
}

Rendering::MeshGPURegistry* MeshRegistryOf(Engine::Renderer::RenderServices* renderServices)
{
    return renderServices ? &renderServices->GetMeshGPURegistry() : nullptr;
}

bool ReadsGround(const Components::SplineWall& recipe)
{
    return recipe.ConformMode != Components::SplinePlacementConform::None;
}

} // namespace

SplineWallController::Candidate SplineWallController::MakeCandidate(ECS::EntityHandle entity,
                                                                    const Components::SplineComponent& spline, const Components::SplineWall& recipe,
                                                                    const Components::WorldTransform& transform)
{
    Candidate candidate;
    candidate.Entity = entity;
    candidate.Authored = recipe;
    candidate.Recipe = SanitizeWallRecipe(recipe);
    candidate.SplineDataIndex = spline.SplineDataIndex;
    candidate.SplineDataGeneration = spline.SplineDataGeneration;
    candidate.WorldMatrix = Mathematics::Matrix4x4::FromColumnMajor(transform.matrix);
    return candidate;
}

void SplineWallController::Update(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                  float32 deltaSeconds, RecipeBuildStates& states)
{
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!splineService)
        return;

    // A world reset destroys every chunk with no per-entity Removed event and
    // restarts entity versions, so cached chunk handles would alias unrelated
    // entities: drop the bookkeeping before the query (World::DeserializeWorld —
    // play-mode exit, undo snapshot restore — clears with no swap-site hook).
    const uint64 resetGeneration = world.GetLifecycleResetGeneration();
    if (resetGeneration != m_LastWorldResetGeneration)
    {
        m_LastWorldResetGeneration = resetGeneration;
        DropStateAfterWorldReset(&renderServices);
    }

    ++m_VisitStamp;

    std::vector<Candidate>& candidates = m_Candidates;
    candidates.clear();
    world.Query<ECS::Read<Components::SplineComponent>, ECS::Read<Components::SplineWall>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle e, const Components::SplineComponent& spline, const Components::SplineWall& recipe,
                  const Components::WorldTransform& xf)
              { candidates.push_back(MakeCandidate(e, spline, recipe, xf)); });

    // One reading of the ground per frame, and none when no wall reads it.
    const bool anyReadsGround = std::any_of(candidates.begin(), candidates.end(),
                                            [](const Candidate& c) { return ReadsGround(c.Recipe); });
    const uint64 surfaceRevision =
        anyReadsGround ? ConformSurfaceRevision(world, ResolveModifierSystem()) : 0u;

    bool worldDirty = false;
    for (const Candidate& candidate : candidates)
    {
        WallState& state = m_States.GetOrInsert(candidate.Entity.id);
        state.VisitStamp = m_VisitStamp;

        ObservedState current;
        current.Spline = ObserveSpline(*splineService, candidate.SplineDataIndex,
                                       candidate.SplineDataGeneration, candidate.WorldMatrix);
        current.Recipe = candidate.Recipe;
        const bool readsGround = ReadsGround(candidate.Recipe);
        current.SurfaceRevision = ObservedSurfaceRevision(readsGround, surfaceRevision);

        if (!current.Spline.Valid)
        {
            worldDirty |= state.Chunks.Retire(world, &renderServices.GetMeshGPURegistry());
            state.Gate.MarkUnbuilt();
            states.Set(candidate.Entity, BuildState::Failed, kSplineTooShortReason);
            continue;
        }

        // Only the terrains near this run can hold its first build.
        const ConformSurfaceReadiness surfaceReadiness =
            readsGround ? ConformSurfaceState(world, RunWorldBounds(*splineService, candidate.SplineDataIndex,
                                                               candidate.SplineDataGeneration,
                                                               candidate.WorldMatrix))
                   : ConformSurfaceReadiness::NoSurface;
        if (!state.Gate.ShouldRebuild(current, state.Chunks.ChunksAlive(world), surfaceReadiness, readsGround,
                                      deltaSeconds, candidate.Entity.id))
        {
            const BuildState waiting = StateWhileGateWaits(state.Gate, current);
            states.Set(candidate.Entity, waiting,
                       waiting == BuildState::Held    ? kHeldForTerrainReason
                       : waiting == BuildState::Built ? state.EmptyReason
                                                      : std::string_view());
            continue;
        }

        state.Hold.Held = false;

        if (Rebuild(world, renderServices, candidate, state))
            worldDirty = true;

        if (!state.Hold.Held)
        {
            state.Gate.MarkBuilt(current);
            state.EmptyReason = state.Chunks.HasChunks() ? std::string_view()
                                                         : kBuiltNothingReason;
        }
        states.Set(candidate.Entity, state.Hold.Held ? BuildState::Held : BuildState::Built,
                   state.Hold.Held ? kHeldForPickingDataReason : state.EmptyReason);
    }

    m_States.EraseIf([&](uint32, WallState& state)
    {
        if (state.VisitStamp == m_VisitStamp)
            return false;
        worldDirty |= state.Chunks.Retire(world, &renderServices.GetMeshGPURegistry());
        return true;
    });

    if (worldDirty)
        world.ProcessCommands();
}

void SplineWallController::DropStateAfterWorldReset(Engine::Renderer::RenderServices* renderServices)
{
    for (auto& [entityId, state] : m_States)
        state.Chunks.DropAfterWorldReset(MeshRegistryOf(renderServices));
    m_States.Clear();
}

void SplineWallController::Shutdown(ECS::World& world, Engine::Renderer::RenderServices* renderServices)
{
    bool worldDirty = false;
    for (auto& [entityId, state] : m_States)
        worldDirty |= state.Chunks.Retire(world, MeshRegistryOf(renderServices));
    m_States.Clear();
    if (worldDirty)
        world.ProcessCommands();
}

bool SplineWallController::Rebuild(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                   const Candidate& candidate, WallState& state)
{
    auto* splineService = SplineECS::SplineService::TryGet();
    const SplineECS::SplineHandle handle(candidate.SplineDataIndex, candidate.SplineDataGeneration);
    // Self-heal a stale arc-length table (a points writer that forgot RebuildCache).
    if (const auto* dirtyData = splineService->GetSplineData(handle); dirtyData && dirtyData->Dirty)
        splineService->RebuildCache(handle);

    const Spline::SplineData* data = splineService->GetSplineData(handle);
    if (!data || !data->IsValid() || data->TotalArcLength <= 0.0f)
        return state.Chunks.Retire(world, &renderServices.GetMeshGPURegistry());

    const Components::SplineWall& recipe = candidate.Recipe;
    const SG::SplineProfile profile = BuildWallProfile(recipe);
    if (!profile.IsValid())
        return state.Chunks.Retire(world, &renderServices.GetMeshGPURegistry());

    // Geometry is built in the placer's LOCAL space, so each chunk entity is a
    // child with an identity transform and lands where it was conformed.
    const Mathematics::Matrix4x4& worldM = candidate.WorldMatrix;
    const Mathematics::Matrix4x4 invPlacerWorld = SplineLayout::InvertPlacerWorld(candidate.WorldMatrix.Data());
    const float32 axisScale = SplineLayout::LargestAxisScale(worldM);

    const float32 worldArcLength = SplineLayout::WorldCenterlineLength(data->TotalArcLength, worldM);
    const bool overCenterlineBudget = worldArcLength > SplineLayout::kMaxCenterlineLengthMetres;
    if (overCenterlineBudget && !state.CenterlineBudgetLogged)
    {
        Logger::Log::Warning(
            "SplineWall: entity {} has a {:.0f} m centerline, past the {:.0f} m drape budget — the "
            "{:.2f} m sampling step coarsens beyond it, so the wall follows the ground only "
            "approximately there",
            candidate.Entity.id, worldArcLength, SplineLayout::kMaxCenterlineLengthMetres, SplineLayout::kCenterlineStepMetres);
    }
    state.CenterlineBudgetLogged = overCenterlineBudget;

    const SweepShape shape = SweepShapeForWall(recipe);
    const SweepSamples samples =
        SampleSweepCenterline(*data, SplineLayout::CenterlineSampleCount(worldArcLength), worldM, shape);
    if (samples.Frames.size() < 2u)
        return state.Chunks.Retire(world, &renderServices.GetMeshGPURegistry());

    // ---- Draped centreline, in world space (where "down" is unambiguous) ----
    const std::vector<ECS::EntityHandle> ignore = CollectConformRayIgnoreList(world);
    ConformMissTally conformMisses;
    const bool conform = ReadsGround(recipe);
    const Vector3 worldUp(0.0f, 1.0f, 0.0f);
    std::vector<SplineLayout::CenterSample> center;
    center.reserve(samples.Frames.size());
    for (size_t i = 0; i < samples.Frames.size(); ++i)
    {
        SplineLayout::CenterSample sample;
        sample.Pos = worldM.TransformPoint(samples.Frames[i].Position);
        sample.Normal = worldUp;
        // An inserted sample casts no ray: DrapeInsertedSamples gives it the
        // drape of the probed samples either side.
        if (conform && samples.Probed[i])
        {
            Vector3 hit;
            Vector3 hitNormal = worldUp;
            if (ConformRayDown(world, sample.Pos, ignore, hit, &hitNormal, &conformMisses,
                               recipe.ConformTarget))
            {
                sample.Pos.y = hit.y;
                sample.Normal = hitNormal;
            }
            else
            {
                sample.SurfaceValid = false; // keep an unmeasured sample out of the length
            }
        }
        center.push_back(sample);
    }
    if (conform)
        DrapeInsertedSamples(samples, worldM, center);
    SplineLayout::HoldSurfaceAcrossGaps(center);
    if (HoldForPendingBvh(conformMisses, candidate.Entity.id, state.Hold))
        return false;
    ReportConformMisses(conformMisses, candidate.Entity.id, state.LoggedConformMisses);

    // ---- Stations: corners, weld, grounded base, face U, then the steps ----
    SweepStationStream stream =
        BuildSweepStations(*data, samples, center, invPlacerWorld, profile.NominalHalfWidth,
                           SG::SplineProfileScale::None, false, shape);
    const bool closedLoop = data->IsEffectivelyClosed();
    StepWallTop(stream, recipe, invPlacerWorld, closedLoop);

    std::vector<std::string> validation = WallRecipeValidation(candidate.Authored);
    for (std::string& line : CornerValidation("SplineWall", stream.CornerIssues))
        validation.push_back(std::move(line));
    const auto* entityName = world.GetComponent<Components::Name>(candidate.Entity);
    ReportRecipeValidation(validation,
                           entityName ? entityName->View() : std::string_view(),
                           candidate.Entity.id, state.LoggedValidation);

    // ---- Chunks: one mesh per ~50 m of wall, all on one U axis and V datum ----
    const SG::SplineStripParams stripParams = WallStripParams(stream, closedLoop, axisScale);
    const std::vector<std::pair<uint32, uint32>> chunkRanges =
        CarveChunkRanges(stream.WorldDistance, kChunkLengthMetres, kMaxChunks);
    if (chunkRanges.empty())
        return state.Chunks.Retire(world, &renderServices.GetMeshGPURegistry());

    // An empty chunk stays an empty mesh: its slot is a hole, not a shift.
    std::vector<Mesh> meshes(chunkRanges.size());
    for (uint32 c = 0; c < chunkRanges.size(); ++c)
    {
        const auto [firstStation, lastStation] = chunkRanges[c];
        const std::span<const SG::SplineStripStation> span(stream.Local.data() + firstStation,
                                                           lastStation - firstStation + 1u);
        const SG::SplineStripMesh strip = SG::BuildSplineStrip(profile, span, stripParams);
        if (strip.IsValid())
        {
            meshes[c] = ToEngineMesh(strip, "SplineWall_" + std::to_string(candidate.Entity.id) +
                                                "_" + std::to_string(c));
        }
    }
    return Commit(world, renderServices, candidate, state, meshes);
}

bool SplineWallController::Commit(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                                  const Candidate& candidate, WallState& state,
                                  const std::vector<Mesh>& meshes)
{
    const Components::SplineWall& recipe = candidate.Recipe;

    // Mint the built-in grey once, so a wall with no material renders on the
    // first try.
    if (!m_DefaultMaterialRegistered &&
        !renderServices.Materials().Registry().Find(DefaultWallMaterialGuid()))
    {
        MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Spline Wall");
        doc.properties["baseColor"] =
            std::vector<float>{kDefaultWallGrey, kDefaultWallGrey, kDefaultWallGrey, 1.0f};
        renderServices.RegisterAndPrewarmMaterial(DefaultWallMaterialGuid(), doc);
    }
    m_DefaultMaterialRegistered = true;

    // Null and unrenderable materials both take the built-in one: binding an
    // unregistered GUID would freeze the chunks at their last registered material
    // (SplinePieceMaterial.h).
    Components::MaterialRef material = recipe.Material;
    if (!EnsureOverrideMaterialRegistered(renderServices, recipe.Material, m_OverrideMaterials))
        material.Set(DefaultWallMaterialGuid());

    Components::MeshRenderer renderer{};
    renderer.materialAssetGuid = material;
    renderer.shadowCasting = recipe.CastShadows ? Components::ShadowCasting::On : Components::ShadowCasting::Off;
    renderer.receiveShadows = recipe.ReceiveShadows;
    // Vertices replaced wholesale under a stable handle have no previous-frame
    // correspondence, so motion vectors would ghost for a frame after a rebuild.
    renderer.motionVectors = false;

    return state.Chunks.CommitSlots(world, renderServices.GetMeshGPURegistry(), candidate.Entity,
                                    ChunkKeySpace(candidate.Entity.id), meshes, renderer,
                                    recipe.CastShadows);
}

} // namespace GameEngine::SplineRecipes
