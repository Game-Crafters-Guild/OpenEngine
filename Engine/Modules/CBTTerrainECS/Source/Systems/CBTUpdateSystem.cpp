#include "CBTTerrainECS/Systems/CBTUpdateSystem.h"

#include "CBTTerrainECS/CBTRenderFeature.h"

#include "CBTTerrain/CBTInstance.h"    // CBTClassifyDesc
#include "CBTTerrain/CBTLayout.h"      // kClassifyScreenSpace, kFocusRootAll
#include "CBTTerrain/CBTSphereFaceMap.h" // SphereEditRegions (per-face dirty drain)
#include "CBTTerrainECS/TerrainProvisioning.h" // ResolveTerrainMaxDepth (auto MaxDepth)
#include "Components/Terrain/Terrain.h"    // Components::Terrain (domain + tuning + handles)
#include "Components/Terrain/TerrainPlanetRelief.h" // Components::TerrainPlanetRelief (base relief)
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Interpolation.h"
#include "Terrain/TerrainTypes.h" // TerrainNeedsTiling (a tiled terrain keeps its lattice cap)
#include "TerrainECS/Planet/PlanetPageRequester.h"
#include "TerrainECS/TerrainRenderFeature.h" // unified-height dirty rect (tiled streaming)
#include "TerrainECS/TerrainService.h" // TerrainService, TerrainData, DirtyRegionLog

#include "ECS/ECS.h"
#include "ECS/Query.h"
#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"

#include <algorithm> // std::clamp
#include <bit>       // std::bit_cast (provision-signal dedupe key)
#include <cstdlib>   // std::getenv / std::atoi (the GE_CBT_* A/B toggles)

namespace GameEngine::CBTTerrainECS
{

// Frames of full-pool VertexEval to force after each sphere-sculpt version change (the flatten-
// move-crack fix). Covers the frame the edit lands PLUS a short settle so the final edit is
// re-evaluated against its own uploaded sculpt even under the one-frame CBTUpdate/TerrainModifiers
// extraction-order lag; the CBT vertex cache is single-buffered, so a couple of settle frames is
// ample. Larger only lengthens the (cheap) full re-eval tail after the last edit.
constexpr uint32 kSphereEditForceEvalFrames = 3u;

// Above the occupancy ceiling, the near-field force-split stops, so the pool keeps headroom and a
// static-camera edit can still re-tessellate. On by default; GE_CBT_NEARFIELD_GATE=0 turns it off.
bool NearFieldGateEnabled()
{
    static const bool enabled = [] {
        if (const char* v = std::getenv("GE_CBT_NEARFIELD_GATE"))
            return std::atoi(v) != 0;
        return true;
    }();
    return enabled;
}

// Planar pool-pressure scale (CBTClassifyDesc::PoolPressure). Ships default-ON; GE_CBT_POOL_PRESSURE=0
// holds the scale at 1x so the pinned regime can be measured on the same build. An A/B arm, not a
// setting: a pool that pins is never the better default.
bool PoolPressureEnabled()
{
    static const bool enabled = [] {
        if (const char* v = std::getenv("GE_CBT_POOL_PRESSURE"))
            return std::atoi(v) != 0;
        return true;
    }();
    return enabled;
}

// What the classify keeps of the ground wholly behind the eye (CBTClassifyDesc::BehindEye).
CBTTerrain::BehindEyeRelease ToBehindEyeRelease(Components::TerrainBehindCameraDetail detail)
{
    switch (detail)
    {
    case Components::TerrainBehindCameraDetail::AlwaysKeep:
        return CBTTerrain::BehindEyeRelease::AlwaysKeep;
    case Components::TerrainBehindCameraDetail::AlwaysRelease:
        return CBTTerrain::BehindEyeRelease::AlwaysRelease;
    case Components::TerrainBehindCameraDetail::ReleaseWhenNeeded:
        break;
    }
    return CBTTerrain::BehindEyeRelease::ReleaseWhenNeeded;
}

// The imported heightmap's sample grid when one drives the terrain's heights
// (ResolveTerrainMaxDepth caps at its spacing); {0, 0} when the terrain's own lattice is its
// source, when the terrain tiles (ResolveTerrainMaxDepth keeps a tiled terrain's lattice cap: its
// CBT spans the tile grid, which can overhang the footprint the import is measured on, #2609), or
// when the asset did not decode (the base fill then reads flat). The tiling check comes first, so
// the cap does not look the import up for a terrain it would ignore it on.
ImportedHeightSource ResolveImportedHeightSource(const Components::Terrain& terrain)
{
    if (terrain.BaseSource != Components::TerrainBaseSource::HeightmapAsset ||
        terrain.TerrainAssetGuid.IsNull() ||
        Terrain::TerrainNeedsTiling(terrain.SizeX, terrain.SizeZ, terrain.SamplesPerMeter))
        return {};
    auto* service = TerrainECS::TerrainService::TryGet();
    const std::shared_ptr<const Terrain::HeightfieldData> source =
        service ? service->ResolveHeightmapAsset(terrain.TerrainAssetGuid.ToGuid()) : nullptr;
    if (source == nullptr || source->IsEmpty())
        return {};
    return ImportedHeightSource{source->GetWidth(), source->GetHeight()};
}

void CBTUpdateSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    GE_CPU_PROFILE_SCOPE("Terrain.CBTUpdateSystem");
    if (!m_RenderServices)
        return;

    auto& feature = m_RenderServices->EnsureFeature<CBTRenderFeature>();

    // CBT is the terrain renderer (unconditional post-cutover): active whenever an
    // enabled, live Terrain entity exists.
    const bool active = ComputeTerrainActive(world);

    CBTTerrain::CBTClassifyDesc classify{};
    classify.Mode = CBTTerrain::kClassifyScreenSpace; // C4 production metric
    classify.FocusRoot = CBTTerrain::kFocusRootAll;   // unused under screen-space
    classify.NearFieldGate = NearFieldGateEnabled() ? 1u : 0u;
    classify.PoolPressure = PoolPressureEnabled() ? 1u : 0u;

    // The domain + refinement tuning come straight off the Terrain component now
    // (post-merge), via the shared active-terrain resolver so the renderer and the
    // editor brush never disagree about which terrain is active. A default-constructed
    // component supplies fallbacks when none is live. The base relief is a companion
    // component (TerrainPlanetRelief) resolved off the SAME active-terrain entity, so a
    // relief edit drives the exact math the physics collider and far-clip framing read.
    Components::Terrain tuning{};
    Components::TerrainPlanetRelief relief{};
    if (active)
    {
        FindActiveTerrain(world, tuning);
        relief = ResolveActivePlanetRelief(world);
    }
    // The release acts only where the inspector shows the setting: one rule for both.
    const auto* terrainService = TerrainECS::TerrainService::TryGet();
    classify.BehindEye = terrainService && TerrainECS::BehindCameraDetailApplies(tuning, *terrainService)
                             ? ToBehindEyeRelease(tuning.BehindCameraDetail)
                             : CBTTerrain::BehindEyeRelease::AlwaysKeep;

    // C7: the rendering domain + planet tuning. The feature re-seeds the tree on a
    // planar<->spherical switch, a retired or replaced drawn terrain or a lowered depth cap (EnsureSeeded,
    // render-side).
    CBTRenderFeature::DomainConfig domain{};
    const bool spherical = (tuning.Domain == Components::TerrainDomain::Spherical);

    // Reset the planet layers when the active spherical planet changes identity: a different
    // entity, or none (deleted or switched away from the sphere domain). A new planet must not
    // take the old planet's sculpt.
    const ECS::EntityHandle currentPlanet =
        (active && spherical) ? FindActiveTerrainEntity(world) : ECS::EntityHandle{};
    if (currentPlanet != m_ActivePlanetEntity)
    {
        if (m_ActivePlanetEntity.IsValid()) // a planet existed and is now different or gone
            if (auto* service = TerrainECS::TerrainService::TryGet())
                service->ResetPlanetSculpt();
        m_ActivePlanetEntity = currentPlanet;
    }
    // The planet's sculpt comes from its height layer asset: a scene open or a new asset reference loads it.
    if (currentPlanet.IsValid())
        if (auto* service = TerrainECS::TerrainService::TryGet())
        {
            const float32 texel = tuning.SamplesPerMeter > 0.0f ? 1.0f / tuning.SamplesPerMeter : 0.0f;
            service->EnsurePlanetHeightLayerLoaded(currentPlanet.id,
                                                   TerrainECS::PlanetFaceSamples(tuning.PlanetRadius, texel),
                                                   tuning.HeightLayerGuid.ToGuid());
        }

    if (active)
    {
        // (face,rect) region identity (plan §planet-editing): a spherical terrain's edits
        // live in the planet page layers, keyed by (face, face-local rect);
        // a planar terrain's edits live in the TerrainData heightfield's UV DirtyRegionLog.
        // Consume the one that matches the active domain.
        if (spherical)
        {
            // The planet's sculpt and modifier layers live in TerrainService (the brush and
            // TerrainModifierSystem write them directly), so this reads TerrainService.
            auto* service = TerrainECS::TerrainService::TryGet();
            const uint64 version = service ? service->PlanetLayerVersion() : 0u;

            // While the planet layer version advances (a live planet edit) — and for a short
            // settle after — force a full-pool VertexEval instead of trusting the region-scoped
            // MODIFIED re-eval below. A modifier dragged across the surface bumps the version at a
            // new footprint every frame while the per-frame dirty rect trails behind it, so the
            // region path stranded the bisectors the modifier already passed at their stale
            // (flattened) height — the round-8 flatten-move crack trail. A full re-eval every
            // editing frame makes every live bisector match the current sculpt, so no stale trail
            // survives, regardless of how the dirty rect is scoped or which frame it lands. It runs
            // ONLY while editing, so a quiescent planet keeps the gated re-eval (idle cost unchanged).
            if (version != m_LastPlanetLayerVersion)
            {
                m_LastPlanetLayerVersion = version;
                m_SphereForceEvalFrames = kSphereEditForceEvalFrames;
            }
            if (m_SphereForceEvalFrames > 0u)
            {
                --m_SphereForceEvalFrames;
                feature.RequestForceVertexEval();
            }

            CBTTerrain::SphereEditRegions regions{};
            if (service)
                service->ConsumePlanetLayerDirtyRegions(regions); // drains the render dirty union
            ConsumePlanetLayerEdits(feature.GetUpdateRecordCount(), regions, classify);
        }
        else
        {
            // A tiled terrain zeroes its heightfield DirtyRegionLog handle, so the
            // log-based ConsumeDirtyRegion is inert for it — its edits (streaming +
            // modifier re-bakes) land in the unified GPU texture instead. Feed Classify
            // from the feature's unified-height dirty rect on that path.
            const bool tiledActive = tuning.TiledTerrainHandle != 0 ||
                                     tuning.TiledTerrainGeneration != 0;
            if (tiledActive)
            {
                auto* terrainFeature =
                    m_RenderServices->GetFeature<TerrainECS::TerrainRenderFeature>();
                ConsumeUnifiedTiledDirtyRegion(terrainFeature, feature.GetUpdateRecordCount(),
                                               tuning, classify);
            }
            else
            {
                ConsumeDirtyRegion(world, feature.GetUpdateRecordCount(), classify);
            }
        }
    }
    domain.DomainMode = spherical ? CBTTerrain::kDomainSpherical : CBTTerrain::kDomainPlanar;
    domain.PlanetRadius = tuning.PlanetRadius;
    domain.PlanetEntity = currentPlanet.id;
    domain.ReliefAmplitude = relief.Amplitude;
    domain.ReliefFrequency = relief.Frequency;
    // Clamp octaves like MaxDepth: the count drives an unbounded per-vertex/per-pixel GPU loop,
    // and the inspector field is user-editable — an absurd count is TDR-class (thousands of
    // transcendentals/px) and overflows the octave frequency (f*=lacunarity -> inf -> NaN). 8
    // octaves is ample detail; 1 is the base shape.
    const uint32 kMaxReliefOctaves = 8u;
    domain.ReliefOctaves = std::clamp(relief.Octaves, 1u, kMaxReliefOctaves);

    // The refinement depth cap is auto-derived from the domain + size / radius and, for an
    // imported heightmap, the import's own spacing (or the internal MaxDepthOverride),
    // already clamped to the ceiling the domain's gVertex store carries on this arm (SubdivCapFor).
    // Nobody tunes a heap depth: the correct value is computable.
    const ImportedHeightSource imported =
        active ? ResolveImportedHeightSource(tuning) : ImportedHeightSource{};
    classify.TargetDepth =
        ResolveTerrainMaxDepth(tuning, imported, feature.UsesNarrowHeap());

    feature.SetActive(active, classify, tuning.TargetPixelError, tuning.SeaLevel, domain,
                      static_cast<uint32>(tuning.DebugView));

    if (active)
        LogProvisionSignal(tuning, classify.TargetDepth);
}

void CBTUpdateSystem::LogProvisionSignal(const Components::Terrain& tuning, uint32 resolvedMaxDepth)
{
    // Falsifiable provision signal, debounced to the SETTLED config so an inspector
    // drag (radius/size changing every tick) does not spam the log: the Info one-shot
    // fires only once a config has held for a frame, and never re-fires until it
    // changes again. Silent while mid-drag; grep-able on the settled value.
    const bool spherical = (tuning.Domain == Components::TerrainDomain::Spherical);
    const float32 sizeOrRadius = spherical ? tuning.PlanetRadius
                                           : std::max(tuning.SizeX, tuning.SizeZ);
    // Bits don't overlap (depth in 33+, domain in 32, size in 0-31), so any real key is
    // >= 2^33 — the small sentinels the members seed with can never collide with one.
    const uint64 key = (static_cast<uint64>(resolvedMaxDepth) << 33) |
                       (static_cast<uint64>(spherical ? 1u : 0u) << 32) |
                       static_cast<uint64>(std::bit_cast<uint32>(sizeOrRadius));
    if (key == m_ProvisionSettledKey)
        return; // already logged this exact config
    if (key != m_ProvisionPendingKey)
    {
        m_ProvisionPendingKey = key; // changed this frame — wait for it to settle
        return;
    }

    // Held for a second frame: settled. Emit the one-shot Info line.
    m_ProvisionSettledKey = key;
    const char* depthKind = tuning.MaxDepthOverride != 0u ? "override" : "auto";
    if (spherical)
        Logger::Log::Info("Terrain.Provision domain=spherical radius={} maxDepth={}({})",
                          tuning.PlanetRadius, resolvedMaxDepth, depthKind);
    else
        Logger::Log::Info("Terrain.Provision domain=planar size={} maxDepth={}({})",
                          std::max(tuning.SizeX, tuning.SizeZ), resolvedMaxDepth, depthKind);
}

void CBTUpdateSystem::ConsumePlanetLayerEdits(uint64 updateRecordCount, const CBTTerrain::SphereEditRegions& regions,
                                              CBTTerrain::CBTClassifyDesc& classify)
{
    // Ingest the drained dirty union (clear-on-read from TerrainService, so every non-empty call
    // is new content that must not be dropped — this is what preserves several dabs that land
    // between two CBT updates during a fast stroke). Each face's rect is the union since it was
    // last recorded; a face not already pending starts fresh, a pending one grows. A dab/modifier
    // near a cube edge dirties BOTH adjacent faces here — the cross-face-seam fix.
    for (uint32 ri = 0; ri < regions.Count; ++ri)
    {
        const CBTTerrain::SphereFaceUVRect& r = regions.Rects[ri];
        if (r.IsEmpty() || r.Face >= CBTTerrain::kCubeFaceCount)
            continue;
        SphereFaceDirty& f = m_SphereFaces[r.Face];
        if (!f.Pending)
        {
            f = SphereFaceDirty{true, r.MinU, r.MinV, r.MaxU, r.MaxV};
        }
        else
        {
            f.MinU = std::min(f.MinU, r.MinU);
            f.MinV = std::min(f.MinV, r.MinV);
            f.MaxU = std::max(f.MaxU, r.MaxU);
            f.MaxV = std::max(f.MaxV, r.MaxV);
        }
    }

    // An update recorded since the last push: a face whose pushed rect did not grow since is done; a
    // grown one stays pending and goes again at its new extent.
    if (m_SpherePushed.Count != 0u && updateRecordCount != m_SpherePushedRecordCount)
    {
        for (uint32 i = 0; i < m_SpherePushed.Count; ++i)
        {
            const CBTTerrain::SphereFaceUVRect& pushed = m_SpherePushed.Rects[i];
            SphereFaceDirty& f = m_SphereFaces[pushed.Face];
            if (f.MinU == pushed.MinU && f.MinV == pushed.MinV && f.MaxU == pushed.MaxU && f.MaxV == pushed.MaxV)
                f.Pending = false;
        }
        m_SpherePushed = {};
    }

    // Every pending face goes to Classify in this update; the list is rebuilt each update. With no face
    // pending the list is empty, so Classify skips its dirty branch and the work stays quiescent.
    classify.SphereDirty = {};
    for (uint32 face = 0; face < CBTTerrain::kCubeFaceCount; ++face)
    {
        const SphereFaceDirty& f = m_SphereFaces[face];
        if (f.Pending)
            classify.SphereDirty.Rects[classify.SphereDirty.Count++] =
                CBTTerrain::SphereFaceUVRect{face, f.MinU, f.MinV, f.MaxU, f.MaxV};
    }
    if (classify.SphereDirty.Count == 0u)
        return;
    m_SpherePushed = classify.SphereDirty;
    m_SpherePushedRecordCount = updateRecordCount;
}

bool CBTUpdateSystem::ComputeTerrainActive(ECS::World& world)
{
    // A terrain is live when EITHER handle is set. A tiled terrain has its
    // single-terrain handle zeroed (extraction moves it to TiledTerrainHandle), so
    // checking only TerrainDataHandle here made auto-tiled terrains inactive — CBT
    // never ran and the terrain vanished at the tiling flip. Match the "index OR
    // generation non-zero" liveness test extraction uses, for BOTH handles.
    bool active = false;
    world.Query<ECS::Read<Components::Terrain>>().Each(
        [&](const Components::Terrain& terrain)
        {
            const bool singleLive =
                terrain.TerrainDataHandle != 0 || terrain.TerrainDataGeneration != 0;
            const bool tiledLive =
                terrain.TiledTerrainHandle != 0 || terrain.TiledTerrainGeneration != 0;
            if (singleLive || tiledLive)
                active = true;
        });
    return active;
}

void CBTUpdateSystem::ConsumeDirtyRegion(ECS::World& world, uint64 updateRecordCount,
                                         CBTTerrain::CBTClassifyDesc& classify)
{
    // C5 (plan §8): consume CBT's OWN cursor into the drawn terrain's region-dirty
    // log (E0), map the edited texel rect to UV, and hand it to Classify so the
    // overlapping bisectors reclassify this update — the new heights arrive through
    // the region heightmap upload (E2), which the CBT.Update read edge orders before
    // VertexEval. CollectSince never clears the log, so extraction's and physics'
    // cursors are untouched. The CBT draws one terrain, so this tracks the drawn terrain
    // (FindActiveTerrainEntity) when it has a single-terrain handle.
    auto* service = TerrainECS::TerrainService::TryGet();
    if (!service)
        return;

    const ECS::EntityHandle drawn = FindActiveTerrainEntity(world);
    const auto* terrain = drawn.IsValid() ? world.GetComponent<Components::Terrain>(drawn) : nullptr;
    // A terrain is live when index OR generation is non-zero: TerrainService hands the first
    // terrain slot index 0 and generation 1, so a `handle == 0` test misses that valid terrain.
    if (!terrain || (terrain->TerrainDataHandle == 0 && terrain->TerrainDataGeneration == 0))
        return;
    const TerrainECS::TerrainHandle handle{terrain->TerrainDataHandle, terrain->TerrainDataGeneration};

    const TerrainECS::TerrainData* data = service->GetTerrainData(handle);
    if (!data)
    {
        m_RegionCursors.erase(handle.Index); // slot destroyed — drop the stale cursor
        return;
    }

    RegionCursor& cursor = m_RegionCursors[handle.Index];
    if (cursor.Generation != handle.Generation)
        cursor = RegionCursor{handle.Generation}; // slot reused — re-read from the full-dirty entry

    // Confirm: an update pass recorded the rect we pushed last time (the feature's
    // record count moved past what we saw when we pushed). Only then commit the log
    // cursor past that edit — otherwise the edit was never handed to the GPU (a frame
    // with no scene view / instance not ready), so hold the cursor and re-arm below.
    if (cursor.InFlight && updateRecordCount != cursor.InFlightRecordCount)
    {
        cursor.CommittedVersion = cursor.InFlightVersion;
        cursor.InFlight = false;
    }

    TerrainECS::DirtyRegionLog::Region region;
    if (!data->HeightfieldDirtyLog.CollectSince(cursor.CommittedVersion, region))
        return; // nothing newer than the confirmed cursor — quiescent (rect stays empty)

    const uint32 width = data->Heightfield.GetWidth();
    const uint32 height = data->Heightfield.GetHeight();
    if (width == 0u || height == 0u)
        return;

    // Texel rect -> UV [0,1], padded ONE texel each side (like the tiled accumulator): CBT's
    // linear-clamp height sample spreads each texel's influence half a texel past its center,
    // so a bisector in the band adjacent to the raw rect samples edited heights but would not
    // reclassify without the pad. The log's Max is exclusive, so the raw upper edge is MaxX/W.
    const float invW = 1.0f / static_cast<float>(width);
    const float invH = 1.0f / static_cast<float>(height);
    classify.DirtyMinU = Math::Clamp01((static_cast<float>(region.MinX) - 1.0f) * invW);
    classify.DirtyMinV = Math::Clamp01((static_cast<float>(region.MinZ) - 1.0f) * invH);
    classify.DirtyMaxU = Math::Clamp01((static_cast<float>(region.MaxX) + 1.0f) * invW);
    classify.DirtyMaxV = Math::Clamp01((static_cast<float>(region.MaxZ) + 1.0f) * invH);

    // Mark the rect in-flight at the current heightfield version; do NOT commit the
    // cursor until a later frame confirms an update recorded it (above).
    cursor.InFlight = true;
    cursor.InFlightVersion = data->HeightfieldVersion;
    cursor.InFlightRecordCount = updateRecordCount;
}

void CBTUpdateSystem::ConsumeUnifiedTiledDirtyRegion(TerrainECS::TerrainRenderFeature* feature,
                                                     uint64 updateRecordCount,
                                                     const Components::Terrain& tuning,
                                                     CBTTerrain::CBTClassifyDesc& classify)
{
    // The unified textures are keyed by the tiled terrain's GLOBAL GPU handle — the same
    // handle the region uploads (TerrainExtractionSystem) and BuildFrameParams' bound
    // heightmap use, so a rect drained here maps to exactly the texture VertexEval samples.
    auto* service = TerrainECS::TerrainService::TryGet();

    float minU = 0.0f, minV = 0.0f, maxU = 0.0f, maxV = 0.0f;
    bool taken = false;
    if (feature && service)
    {
        TerrainECS::TiledTerrainHandle tiledHandle{tuning.TiledTerrainHandle,
                                                   tuning.TiledTerrainGeneration};
        if (const auto* tiled = service->GetTiledTerrainData(tiledHandle))
        {
            TerrainECS::TerrainHandle globalGpuHandle{tiled->GlobalGpuHandleIndex,
                                                      tiled->GlobalGpuHandleGeneration};
            uint64 version = 0;
            taken = feature->TakeUnifiedHeightDirtyRect(globalGpuHandle, minU, minV, maxU, maxV,
                                                        version);
        }
    }

    // Run the discipline even when nothing was drained (taken == false) so an
    // unconfirmed pending rect from a prior frame re-arms or commits on schedule.
    ConsumeUnifiedDirtyRect(taken, minU, minV, maxU, maxV, updateRecordCount, classify);
}

void CBTUpdateSystem::ConsumeUnifiedDirtyRect(bool takenHasRect, float minU, float minV, float maxU,
                                              float maxV, uint64 updateRecordCount,
                                              CBTTerrain::CBTClassifyDesc& classify)
{
    // Confirm: a CBT.Update recorded the rect we pushed last time (the feature's record
    // count advanced past what we saw), so the streamed region reached the GPU — drop the
    // pending union. Otherwise the update never ran (no scene view / instance not ready):
    // hold the union and re-arm below.
    //
    // Confirmation keys off the update being DECLARED, not the region copy having landed.
    // On a SINGLE-graph frame (the shipping game / editor-scene view) the copy (TerrainUpload)
    // and VertexEval (CBT.Update) run in that one graph, and CBTRenderNode::DeclareForView adds
    // an ordering edge from the copy's pass to the update's, so a committed rect is always
    // sampled post-copy. The phase does not carry that: the copy declares no access the update
    // shares, so it forms a scheduling component of one and without the edge the scheduler is
    // free to emit the update's whole component ahead of it. The one hazard left is a MULTI-
    // graph frame where TerrainUpload's once-per-frame flush is claimed by graph A while
    // CBT.Update is claimed by graph B and B submits before A: VertexEval then samples pre-
    // copy texels yet this commits the (one-shot streamed) rect -> frozen stale. The reviewer
    // could not confirm that interleaving is reachable, it needs no new machinery for the
    // shipping path, and the C5 edit path shares the exact same upload+read-edge machinery
    // (self-healing there only because brush drags re-dirty every frame). The precondition —
    // the upload flush and CBT.Update landing in different graphs — is now caught by a loud
    // tripwire at the CBT.Update claim site (CBTRenderNode::DeclareForView, via
    // TerrainRenderFeature::HeightmapUploadClaimedInOtherGraph), so a future change that
    // splits them fails loudly instead of silently committing against pre-copy texels.
    if (m_UnifiedInFlight && updateRecordCount != m_UnifiedInFlightRecordCount)
    {
        m_UnifiedInFlight = false;
        m_UnifiedPendingHasRect = false;
    }

    // Union this frame's freshly-drained upload rect into the pending region.
    if (takenHasRect)
    {
        if (!m_UnifiedPendingHasRect)
        {
            m_UnifiedPendingMinU = minU;
            m_UnifiedPendingMinV = minV;
            m_UnifiedPendingMaxU = maxU;
            m_UnifiedPendingMaxV = maxV;
            m_UnifiedPendingHasRect = true;
        }
        else
        {
            m_UnifiedPendingMinU = std::min(m_UnifiedPendingMinU, minU);
            m_UnifiedPendingMinV = std::min(m_UnifiedPendingMinV, minV);
            m_UnifiedPendingMaxU = std::max(m_UnifiedPendingMaxU, maxU);
            m_UnifiedPendingMaxV = std::max(m_UnifiedPendingMaxV, maxV);
        }
    }

    // Quiescent: nothing pending -> leave the dirty rect empty so Classify's
    // reclassification branch is skipped (the idle-zero-work invariant).
    if (!m_UnifiedPendingHasRect)
        return;

    classify.DirtyMinU = m_UnifiedPendingMinU;
    classify.DirtyMinV = m_UnifiedPendingMinV;
    classify.DirtyMaxU = m_UnifiedPendingMaxU;
    classify.DirtyMaxV = m_UnifiedPendingMaxV;

    // Mark the rect in-flight at the current record count; hold it until a later frame
    // confirms an update recorded it (above).
    m_UnifiedInFlight = true;
    m_UnifiedInFlightRecordCount = updateRecordCount;
}

} // namespace GameEngine::CBTTerrainECS
