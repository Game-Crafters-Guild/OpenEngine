#include "TerrainECS/Systems/TerrainPhysicsSystem.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainRenderPublication.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/TerrainPages.h"
#include "TerrainECS/Planet/PlanetChart.h"
#include "Planet/PlanetPageBodyFill.h"
#include "TerrainECS/Components/TerrainPageCollider.h"
#include "TerrainECS/Components/TerrainPlanetPageCollider.h"
#include "Components/Terrain/Terrain.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Transform.h"
#include "Core/CpuProfiler.h"
#include "Mathematics/Geometry.h"
#include "ECS/ECS.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "ECS/ECSTemplates.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "PhysicsECS/HeightFieldDataProvider.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace GameEngine::TerrainECS
{

// One planet page body fill on the job pool: what it was started for, its input and its samples. The
// job writes Samples and Filled, then releases Done; the main thread reads them after it acquires Done.
struct PlanetPageFill
{
    ECS::EntityHandle Planet;
    uint64 Epoch = 0;
    PageStreaming::PageAddress Page;
    uint64 LayerVersion = 0;
    // The body the samples refill; an invalid entity makes a new body.
    ECS::EntityHandle Body;
    TerrainService::PagePhysicsHandle Slot;

    PlanetPageBodyInput Input;
    std::vector<float32> Samples;
    bool Filled = false;
    std::atomic<bool> Done{false};

    // The job body. It frees the page copies once the samples are written.
    void Run()
    {
        GE_CPU_PROFILE_SCOPE("Terrain.PlanetBodyFill");
        Filled = FillPlanetPageBody(Input, Samples);
        Input.Pages = {};
        Done.store(true, std::memory_order_release);
    }
};

// The page colliders the sweep tears down, with the page slots to release after their bodies.
struct PageColliderOrphans
{
    std::vector<ECS::EntityHandle> Entities;
    /// Per entity: the built body that takes over its ground, or an invalid entity.
    std::vector<ECS::EntityHandle> Grounds;
    std::vector<TerrainService::PagePhysicsHandle> Slots;
};

// A planet page body the sweep keeps, sorted by its page key.
struct PlanetBodyRecord
{
    uint64 Key = 0;
    ECS::EntityHandle Entity;
    Components::TerrainPlanetPageCollider Tag;
};

// The planet page bodies the sweep found: the ones it keeps, and the ones it tears down with their slots.
struct PlanetBodySweep
{
    std::vector<PlanetBodyRecord> Current;
    std::vector<PlanetBodyRecord> Previous; ///< from an older stream, on a window page
    std::vector<PlanetBodyRecord> BuiltCurrent; ///< the current bodies with a built backend body
    PageColliderOrphans Orphans;

    void Clear()
    {
        Current.clear();
        Previous.clear();
        BuiltCurrent.clear();
        Orphans.Entities.clear();
        Orphans.Grounds.clear();
        Orphans.Slots.clear();
    }
};

// The planet pass's containers, kept across frames and cleared at each use, so a steady frame
// allocates nothing.
struct PlanetColliderScratch
{
    std::vector<uint64> WindowKeys;
    PlanetBodySweep Sweep;
    std::vector<PageStreaming::PageAddress> Reads;
};

namespace
{

// Fills started in one frame, and fills in flight at once: bounds the page copies a camera jump makes.
constexpr size_t kPlanetFillsStartedPerFrame = 4;
constexpr size_t kPlanetFillsInFlight = 16;

// Identity transform translated to `position`.
Components::Transform MakeTranslation(const Mathematics::WorldPosition& position)
{
    Components::Transform t{}; // defaults to identity
    t.Position = position;
    return t;
}

Components::WorldTransform MakeWorldTranslation(const Mathematics::WorldPosition& position)
{
    Components::WorldTransform t{}; // defaults to identity
    t.Position = position;
    return t;
}

// Basis whose columns are a planet chart's frame (TangentX, Up, TangentZ). Local X runs along
// TangentX, local Z along TangentZ and local Y (the heightfield's height axis) along Up.
Mathematics::Matrix3x3 ChartBasis(const PlanetChart& chart)
{
    Mathematics::Matrix3x3 basis;
    basis[0] = glm::vec3(chart.TangentX.x, chart.TangentX.y, chart.TangentX.z);
    basis[1] = glm::vec3(chart.Up.x, chart.Up.y, chart.Up.z);
    basis[2] = glm::vec3(chart.TangentZ.x, chart.TangentZ.y, chart.TangentZ.z);
    return basis;
}

// A planet level-0 page as one sortable value.
uint64 PlanetPageKey(const PageStreaming::PageAddress& page)
{
    return (static_cast<uint64>(page.Face) << 56u) | (static_cast<uint64>(page.Z) << 28u) | page.X;
}

uint64 PlanetPageKey(const Components::TerrainPlanetPageCollider& tag)
{
    return PlanetPageKey(PageStreaming::PageAddress{static_cast<uint8>(tag.Face), 0, tag.PageX, tag.PageZ});
}

bool ContainsKey(const std::vector<uint64>& sortedKeys, uint64 key)
{
    return std::binary_search(sortedKeys.begin(), sortedKeys.end(), key);
}

// Wakes the dynamic bodies over the built footprint of the released heightfield `entity`. A body asleep
// on the ground does not wake when the ground's body goes, so it would stay above the ground that
// replaces it. With a `ground` heightfield, a body that the ground's surface buries moves on top of it.
void SettleBodiesOverReleasedHeightField(const ECS::World& world, Physics::PhysicsWorld& physicsWorld,
                                         ECS::EntityHandle entity, ECS::EntityHandle ground)
{
    const auto* shape = world.GetComponent<Components::HeightFieldColliderShape>(entity);
    const auto* worldTransform = world.GetComponent<Components::WorldTransform>(entity);
    if (!shape || !worldTransform || shape->builtSampleCount < 2u)
        return;
    const int32 last = static_cast<int32>(shape->builtSampleCount - 1u);
    const Mathematics::AABB box = PhysicsECS::HeightFieldRegionWorldBox(
        *worldTransform, *shape, shape->builtSampleCount, 0,
        0, last, last, shape->builtMinHeight, shape->builtMaxHeight);
    const auto* groundShape =
        ground.IsValid() ? world.GetComponent<Components::HeightFieldColliderShape>(ground) : nullptr;
    const auto* groundTransform = ground.IsValid() ? world.GetComponent<Components::WorldTransform>(ground) : nullptr;
    if (groundShape && groundTransform)
        PhysicsECS::SettleBodiesOnHeightField(physicsWorld, *groundTransform, *groundShape, box);
    else
        physicsWorld.ActivateBodiesInAABB(box);
}

// Frees the entity's backend body now and settles the bodies over it on `ground` (an invalid entity only
// wakes them). The entity stays live until the next flush, so its runtime handles are cleared here: nothing
// reads the freed body before the removal, and the OnRemove<PhysicsBody> hook finds them cleared.
void ReleaseColliderBody(ECS::World& world, ECS::EntityHandle entity, ECS::EntityHandle ground = {})
{
    auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
    auto* body = world.GetComponentForWrite<Components::PhysicsBody>(entity);
    if (!pw || !body || !body->initialized)
        return;
    if (body->body.IsValid())
    {
        pw->DestroyBody(body->body);
        SettleBodiesOverReleasedHeightField(world, *pw, entity, ground);
    }
    if (body->shape.IsValid())
        pw->DestroyShape(body->shape);
    for (uint16 i = 0; i < body->childShapeCount; ++i)
        if (body->childShapes[i].IsValid())
            pw->DestroyShape(body->childShapes[i]);
    body->body = {};
    body->shape = {};
    for (uint16 i = 0; i < body->childShapeCount; ++i)
        body->childShapes[i] = {};
    body->childShapeCount = 0;
    body->initialized = false;
}

// Points the collider at the terrain's current data and dimensions and clears its
// build cursors: the version cursors force PhysicsInit's rebuild path, and a zero
// built sample count refuses its in-place update, whose baked scale would be stale.
void ResyncTerrainCollider(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* terrain = world.GetComponent<Components::Terrain>(entity);
    auto* shape = world.GetComponentForWrite<Components::HeightFieldColliderShape>(entity);
    if (!terrain || !shape)
        return;
    shape->dataHandle = terrain->TerrainDataHandle;
    shape->dataGeneration = terrain->TerrainDataGeneration;
    shape->sizeX = terrain->SizeX;
    shape->sizeZ = terrain->SizeZ;
    shape->heightScale = terrain->HeightScale;
    shape->lastBuiltVersion = 0;
    shape->lastSeenVersion = 0;
    shape->rebuildWaitSeconds = 0.0f;
    shape->builtSampleCount = 0;
    shape->inPlaceFailedVersion = 0;
    Logger::Log::Info("TerrainPhysicsSystem: terrain {} changed its data or dimensions; rebuilding its collider",
                      entity.id);
}

// A static heightfield collider entity placed by `transform` (its center) over `sizeX` x `sizeZ` meters, its samples
// resolved through `dataHandle`. CreateEntity allocates the handle immediately; the AddComponent calls
// are deferred. The engine's per-frame ProcessCommands flush runs at the START of the next frame, before
// any system (Engine.cpp), so PhysicsInit sees the fully-formed collider entity on frame N+1, one frame
// after provisioning. (Tests flush explicitly via world.ProcessCommands().) Engine-created runtime
// state, rebuilt from the terrain on load: RuntimeOnlyEntity keeps it out of every scene file, or a
// save during Play would write a ghost collider entity per body.
ECS::EntityHandle CreateTerrainColliderEntity(ECS::World& world, const Components::Transform& transform,
                                              const Components::WorldTransform& worldTransform, uint32 dataHandle,
                                              uint32 dataGeneration, float32 sizeX, float32 sizeZ, float32 heightScale)
{
    ECS::EntityHandle e = world.CreateEntity();
    world.AddComponent(e, transform);
    world.AddComponent(e, worldTransform);

    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Static;
    body.mass = 0.0f;
    body.gravityScale = 0.0f;
    body.allowSleep = true;
    world.AddComponent(e, body);

    Components::PhysicsCollider collider{};
    collider.layer = Physics::Layers::Static;
    world.AddComponent(e, collider);

    Components::HeightFieldColliderShape shape{};
    shape.dataHandle = dataHandle;
    shape.dataGeneration = dataGeneration;
    shape.sizeX = sizeX;
    shape.sizeZ = sizeZ;
    shape.heightScale = heightScale;
    world.AddComponent(e, shape);
    world.AddComponent(e, Components::RuntimeOnlyEntity{});
    return e;
}

// A level-0 page as one sortable value.
uint64 PageKey(uint32 pageX, uint32 pageZ)
{
    return (static_cast<uint64>(pageZ) << 32u) | pageX;
}

// A paged planar terrain (the first entity of its tiled handle) with its window pages sorted for lookup.
struct PagedTerrain
{
    TiledTerrainHandle Tiled;
    SurfaceHeightQuery Query;
    std::span<const PageStreaming::PageAddress> Window;
    std::vector<uint64> SortedWindow;
};

// Adds the terrain unless it is spherical, has no page source yet, or shares the tiled handle of one
// already added (a duplicated entity: the first one's colliders serve it).
void AddPagedTerrain(std::vector<PagedTerrain>& terrains, TerrainService& service, const Components::Terrain& terrain,
                     const Components::WorldTransform& transform)
{
    if (terrain.Domain == Components::TerrainDomain::Spherical)
        return;
    const TiledTerrainHandle tiled{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
    for (const PagedTerrain& paged : terrains)
        if (paged.Tiled == tiled)
            return;
    SurfaceHeightQuery query = service.SurfaceQuery(terrain, transform);
    if (!query.HasPageSource())
        return;
    PagedTerrain& paged = terrains.emplace_back(PagedTerrain{tiled, std::move(query), {}, {}});
    paged.Window = service.FinestWindowPages(terrain);
    for (const PageStreaming::PageAddress& page : paged.Window)
        paged.SortedWindow.push_back(PageKey(page.X, page.Z));
    std::sort(paged.SortedWindow.begin(), paged.SortedWindow.end());
}

PagedTerrain* FindPagedTerrain(std::vector<PagedTerrain>& terrains, uint32 index, uint32 generation)
{
    for (PagedTerrain& paged : terrains)
        if (paged.Tiled.Index == index && paged.Tiled.Generation == generation)
            return &paged;
    return nullptr;
}

// Keeps a collider whose page is still in its terrain's window and whose samples come from the
// stream it reads now: it leaves the window list (not provisioned again) and takes the edits to its
// page as a region of its body. Any other collider is an orphan.
void SweepPageCollider(TerrainService& service, std::vector<PagedTerrain>& terrains, PageColliderOrphans& orphans,
                       ECS::EntityHandle entity, const Components::TerrainPageCollider& tag)
{
    PagedTerrain* paged = FindPagedTerrain(terrains, tag.TiledIndex, tag.TiledGeneration);
    const uint64 key = PageKey(tag.PageX, tag.PageZ);
    const TerrainService::PageColliderData* data =
        service.ResolvePageCollider(tag.PhysicsHandleIndex, tag.PhysicsHandleGeneration);
    const bool current = paged && data && data->ReadAt.Epoch == paged->Query.PageStamp().Epoch;
    if (current && std::binary_search(paged->SortedWindow.begin(), paged->SortedWindow.end(), key))
    {
        std::erase(paged->SortedWindow, key);
        service.RefreshPagePhysicsHandle(paged->Query, tag.PageX, tag.PageZ,
                                         {tag.PhysicsHandleIndex, tag.PhysicsHandleGeneration});
        return;
    }
    orphans.Entities.push_back(entity);
    orphans.Grounds.push_back({});
    orphans.Slots.push_back({tag.PhysicsHandleIndex, tag.PhysicsHandleGeneration});
}

// The active planet: the enabled spherical terrain with the smallest DrawOrderKey. Only one planet gets
// page bodies, as only one terrain draws. `planetCount` counts every enabled spherical terrain.
ECS::EntityHandle FindActivePlanet(ECS::World& world, uint32& planetCount)
{
    ECS::EntityHandle activePlanet{};
    uint64 activePlanetKey = 0;
    planetCount = 0;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain, const Components::WorldTransform&) {
            if (terrain.Domain != Components::TerrainDomain::Spherical)
                return;
            ++planetCount;
            const uint64 key = DrawOrderKey(terrain, entity);
            if (!activePlanet.IsValid() || key < activePlanetKey)
            {
                activePlanet = entity;
                activePlanetKey = key;
            }
        });
    return activePlanet;
}

// A terrain switched from Planar to Spherical keeps the planar heightfield collider the single-terrain
// branch put on the entity itself; shed it and its body, so a planet carries no stray planar body.
void ShedPlanarCollider(ECS::World& world, ECS::EntityHandle planet)
{
    if (!world.HasComponent<Components::HeightFieldColliderShape>(planet))
        return;
    ReleaseColliderBody(world, planet);
    world.RemoveComponent<Components::HeightFieldColliderShape>(planet);
    world.RemoveComponent<Components::PhysicsBody>(planet);
    world.RemoveComponent<Components::PhysicsCollider>(planet);
}

bool RecordKeyLess(const PlanetBodyRecord& a, const PlanetBodyRecord& b)
{
    return a.Key < b.Key;
}

bool RecordKeyEqual(const PlanetBodyRecord& a, const PlanetBodyRecord& b)
{
    return a.Key == b.Key;
}

bool RecordKeyBelow(const PlanetBodyRecord& record, uint64 key)
{
    return record.Key < key;
}

const PlanetBodyRecord* FindBody(const std::vector<PlanetBodyRecord>& sortedBodies, uint64 key)
{
    const auto it = std::lower_bound(sortedBodies.begin(), sortedBodies.end(), key, RecordKeyBelow);
    return it != sortedBodies.end() && it->Key == key ? &*it : nullptr;
}

bool HasBuiltBody(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* body = world.GetComponent<Components::PhysicsBody>(entity);
    return body && body->body.IsValid();
}

// Adds `record`'s body to the teardown. `ground` is the built body that takes over its ground, if any.
void AddOrphan(PageColliderOrphans& orphans, const PlanetBodyRecord& record, ECS::EntityHandle ground = {})
{
    orphans.Entities.push_back(record.Entity);
    orphans.Grounds.push_back(ground);
    orphans.Slots.push_back({record.Tag.PhysicsHandleIndex, record.Tag.PhysicsHandleGeneration});
}

// Sorts a body into the sweep. A body is an orphan when its planet is not the active planet with a CPU
// view, its slot is stale, or its page left every window. A body from an older stream is held for the
// hand-over (PlanetBodySweep::Previous).
void SweepPlanetBody(ECS::World& world, const TerrainService& service, ECS::EntityHandle planet,
                     const PlanetCpuView* view, const std::vector<uint64>& windowKeys, PlanetBodySweep& sweep,
                     ECS::EntityHandle entity, const Components::TerrainPlanetPageCollider& tag)
{
    const PlanetBodyRecord record{PlanetPageKey(tag), entity, tag};
    const bool owned = view && tag.TerrainEntityIndex == planet.index && tag.TerrainEntityVersion == planet.version;
    const bool slot = service.ResolvePageCollider(tag.PhysicsHandleIndex, tag.PhysicsHandleGeneration) != nullptr;
    if (!owned || !slot || !ContainsKey(windowKeys, record.Key))
    {
        AddOrphan(sweep.Orphans, record);
        return;
    }
    if (tag.Epoch != view->Epoch)
    {
        sweep.Previous.push_back(record);
        return;
    }
    sweep.Current.push_back(record);
    if (HasBuiltBody(world, entity))
        sweep.BuiltCurrent.push_back(record);
}

// The hand-over: an older stream's body goes once the current stream's body for its page is built (no
// frame without ground), and the bodies over it settle on the new surface. A second current body for one
// page goes too.
void HandOverPlanetBodies(PlanetBodySweep& sweep)
{
    std::sort(sweep.BuiltCurrent.begin(), sweep.BuiltCurrent.end(), RecordKeyLess);
    for (const PlanetBodyRecord& record : sweep.Previous)
        if (const PlanetBodyRecord* ground = FindBody(sweep.BuiltCurrent, record.Key))
            AddOrphan(sweep.Orphans, record, ground->Entity);
    std::sort(sweep.Current.begin(), sweep.Current.end(), RecordKeyLess);
    const auto firstDuplicate = std::unique(sweep.Current.begin(), sweep.Current.end(), RecordKeyEqual);
    for (auto it = firstDuplicate; it != sweep.Current.end(); ++it)
        AddOrphan(sweep.Orphans, *it);
    sweep.Current.erase(firstDuplicate, sweep.Current.end());
}

// Frees the orphans' bodies now and releases their slots; the entities are destroyed at the next flush.
void TearDownOrphans(ECS::World& world, TerrainService& service, const PageColliderOrphans& orphans)
{
    for (size_t i = 0; i < orphans.Entities.size(); ++i)
    {
        ReleaseColliderBody(world, orphans.Entities[i], orphans.Grounds[i]);
        world.DestroyEntity(orphans.Entities[i]);
    }
    for (const TerrainService::PagePhysicsHandle& slot : orphans.Slots)
        service.ReleasePagePhysicsHandle(slot.Index, slot.Generation);
}

// One world axis of a body's center: the planet center plus R Up plus the rectangle's center on the
// chart's tangents, in float64.
float64 BodyCenterAxis(float32 planetCenter, float64 radius, float64 rectX, float64 rectZ, float32 up,
                       float32 tangentX, float32 tangentZ)
{
    return static_cast<float64>(planetCenter) + radius * up + rectX * tangentX + rectZ * tangentZ;
}

// Creates the body entity of `fill`'s page on `slot`: a heightfield in the page's chart frame, centered
// on its plane rectangle. The samples are meters along the chart's Up, so the height scale is 1.
ECS::EntityHandle CreatePlanetPageBody(ECS::World& world, const Mathematics::Vector3& planetCenter,
                                       const PlanetCpuView& view, const PlanetPageFill& fill,
                                       TerrainService::PagePhysicsHandle slot)
{
    const PageStreaming::PageAddress& page = fill.Page;
    const PlanetChart chart = MakePlanetChartOfPage(view.Layout, view.ReferenceRadius, page.Face, page.X, page.Z);
    const PlanetPageBodyRect rect = PlanetPageBodyRectOf(view.Layout, view.ReferenceRadius, chart, page.X, page.Z);
    const float64 rectX = rect.MinX + 0.5 * rect.SizeX;
    const float64 rectZ = rect.MinZ + 0.5 * rect.SizeZ;
    const float64 radius = view.ReferenceRadius;
    const Mathematics::WorldPosition center{
        BodyCenterAxis(planetCenter.x, radius, rectX, rectZ, chart.Up.x, chart.TangentX.x, chart.TangentZ.x),
        BodyCenterAxis(planetCenter.y, radius, rectX, rectZ, chart.Up.y, chart.TangentX.y, chart.TangentZ.y),
        BodyCenterAxis(planetCenter.z, radius, rectX, rectZ, chart.Up.z, chart.TangentX.z, chart.TangentZ.z)};
    Components::Transform transform{};
    transform.Basis = ChartBasis(chart);
    transform.Position = center;
    Components::WorldTransform worldTransform{};
    worldTransform.Basis = transform.Basis;
    worldTransform.Position = transform.Position;
    const ECS::EntityHandle entity =
        CreateTerrainColliderEntity(world, transform, worldTransform, slot.Index, slot.Generation,
                                    static_cast<float32>(rect.SizeX), static_cast<float32>(rect.SizeZ), 1.0f);
    Components::TerrainPlanetPageCollider tag{};
    tag.TerrainEntityIndex = fill.Planet.index;
    tag.TerrainEntityVersion = fill.Planet.version;
    tag.Face = page.Face;
    tag.PageX = page.X;
    tag.PageZ = page.Z;
    tag.PhysicsHandleIndex = slot.Index;
    tag.PhysicsHandleGeneration = slot.Generation;
    tag.Epoch = fill.Epoch;
    tag.LayerVersion = fill.LayerVersion;
    world.AddComponent(entity, tag);
    return entity;
}

// Lands a done fill: its samples refill its body, or make a new body for its page. A fill whose planet,
// stream or window moved on since it started, or that found a page missing, is dropped. Returns the new
// body's record; its Entity is invalid when the fill made no body.
PlanetBodyRecord LandPlanetFill(ECS::World& world, TerrainService& service, ECS::EntityHandle planet,
                                const Mathematics::Vector3& planetCenter, const PlanetCpuView& view,
                                const std::vector<uint64>& windowKeys, PlanetPageFill& fill)
{
    const uint64 key = PlanetPageKey(fill.Page);
    if (!fill.Filled || fill.Planet != planet || fill.Epoch != view.Epoch || !ContainsKey(windowKeys, key))
        return {};
    if (fill.Body.IsValid())
    {
        auto* tag = world.GetComponentForWrite<Components::TerrainPlanetPageCollider>(fill.Body);
        if (!tag || tag->PhysicsHandleIndex != fill.Slot.Index || tag->PhysicsHandleGeneration != fill.Slot.Generation)
            return {};
        service.RefillPlanetPagePhysicsHandle(fill.Slot, fill.Samples);
        tag->LayerVersion = fill.LayerVersion;
        return {};
    }
    const TerrainService::PagePhysicsHandle slot = service.AcquirePlanetPagePhysicsHandle(std::move(fill.Samples));
    if (slot.Index == 0)
        return {};
    PlanetBodyRecord record{key, {}, {}};
    record.Entity = CreatePlanetPageBody(world, planetCenter, view, fill, slot);
    record.Tag.PhysicsHandleIndex = slot.Index;
    record.Tag.PhysicsHandleGeneration = slot.Generation;
    record.Tag.LayerVersion = fill.LayerVersion;
    return record;
}

// Starts the fill of `page`'s body: copies the pages it reads (with the planet's page layer) and submits
// it on the job pool, or runs it here without a pool. `body` is the body it refills (null: a new body).
// Null when a page it reads is not on the CPU yet.
std::shared_ptr<PlanetPageFill> StartPlanetFill(ECS::World& world, const TerrainPages& pages, ECS::EntityHandle planet,
                                                const PlanetCpuView& view, const PageStreaming::PageAddress& page,
                                                const PlanetBodyRecord* body,
                                                std::vector<PageStreaming::PageAddress>& reads)
{
    reads.clear();
    AppendPlanetPageBodyReads(view.Layout, view.ReferenceRadius, page.Face, page.X, page.Z, reads);
    auto fill = std::make_shared<PlanetPageFill>();
    fill->Input.Pages.resize(reads.size());
    for (size_t i = 0; i < reads.size(); ++i)
    {
        fill->Input.Pages[i].Address = reads[i];
        if (!pages.PlanetPageHeights(planet.id, reads[i], fill->Input.Pages[i].Heights))
            return nullptr;
    }
    fill->Planet = planet;
    fill->Epoch = view.Epoch;
    fill->Page = page;
    fill->LayerVersion = view.LayerVersion;
    if (body)
    {
        fill->Body = body->Entity;
        fill->Slot = {body->Tag.PhysicsHandleIndex, body->Tag.PhysicsHandleGeneration};
    }
    fill->Input.Layout = view.Layout;
    fill->Input.ReferenceRadius = view.ReferenceRadius;
    fill->Input.Page = page;
    fill->Samples.resize(static_cast<size_t>(kFinestPageGridSamples) * kFinestPageGridSamples);
    if (JobSystem::WorkStealingThreadPool* pool = world.GetJobSystem())
        pool->Submit([fill]() { fill->Run(); });
    else
        fill->Run();
    return fill;
}

} // namespace

TerrainPhysicsSystem::TerrainPhysicsSystem() : m_PlanetScratch(std::make_unique<PlanetColliderScratch>()) {}

TerrainPhysicsSystem::~TerrainPhysicsSystem() = default;

bool ColliderMatchesTerrain(const Components::HeightFieldColliderShape& shape, const Components::Terrain& terrain)
{
    return shape.dataHandle == terrain.TerrainDataHandle && shape.dataGeneration == terrain.TerrainDataGeneration &&
           shape.sizeX == terrain.SizeX && shape.sizeZ == terrain.SizeZ && shape.heightScale == terrain.HeightScale;
}

void TerrainPhysicsSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    // Only provision components when the physics world exists (play mode).
    if (!PhysicsECS::PhysicsWorldService::IsInitialized())
        return;

    auto* terrainService = TerrainService::TryGet();
    if (!terrainService)
        return;

    if (m_TrackedWorld != world.GetWorldId() || m_TrackedReset != world.GetLifecycleResetGeneration())
    {
        m_PlanarColliders.clear();
        m_PlanetFills.clear();
        m_TrackedWorld = world.GetWorldId();
        m_TrackedReset = world.GetLifecycleResetGeneration();
    }
    std::vector<ECS::EntityHandle> staleColliders;

    // ---- Single (non-tiled) terrain provisioning ----
    // One HeightFieldColliderShape on the terrain entity itself; PhysicsInitSystem
    // builds the Jolt body via its heightfield gather block, and tears it down when
    // the body's entity is switched off.
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity,
                  const Components::Terrain& terrain,
                  const Components::WorldTransform& /*wt*/)
        {
            // A spherical (planet) terrain gets page bodies (below), not a planar heightfield
            // collider off the SizeX/SizeZ heightfield extraction still builds.
            if (terrain.Domain == Components::TerrainDomain::Spherical)
                return;

            // Tiled terrains are provisioned per page below (extraction clears
            // their single TerrainDataHandle, so this block finds no data).
            if (terrain.TiledTerrainHandle != 0 || terrain.TiledTerrainGeneration != 0)
                return;

            TerrainHandle handle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
            auto* data = terrainService->GetTerrainData(handle);
            const bool dataReady = data && !data->Heightfield.IsEmpty() && data->HeightfieldVersion != 0;

            // A structural terrain change gives it a new data handle, and an edit can
            // change its extent or height scale; the collider follows once the new
            // data exists.
            if (const auto* shape = world.GetComponent<Components::HeightFieldColliderShape>(entity))
            {
                if (std::find(m_PlanarColliders.begin(), m_PlanarColliders.end(), entity) == m_PlanarColliders.end())
                    m_PlanarColliders.push_back(entity);
                if (dataReady && !ColliderMatchesTerrain(*shape, terrain))
                    staleColliders.push_back(entity);
                return;
            }
            if (!dataReady)
                return;

            // Add physics components (deferred to avoid archetype mutation during query).
            Components::PhysicsBody body{};
            body.motionType = Physics::MotionType::Static;
            body.mass = 0.0f;
            body.gravityScale = 0.0f;
            body.allowSleep = true;
            world.AddComponent(entity, body);

            Components::PhysicsCollider collider{};
            collider.layer = Physics::Layers::Static;
            world.AddComponent(entity, collider);

            Components::HeightFieldColliderShape shape{};
            shape.dataHandle = terrain.TerrainDataHandle;
            shape.dataGeneration = terrain.TerrainDataGeneration;
            shape.sizeX = terrain.SizeX;
            shape.sizeZ = terrain.SizeZ;
            shape.heightScale = terrain.HeightScale;
            world.AddComponent(entity, shape);
            m_PlanarColliders.push_back(entity);
        });

    for (ECS::EntityHandle entity : staleColliders)
        ResyncTerrainCollider(world, entity);
    RemoveOrphanedPlanarColliders(world);

    // ---- Paged terrain provisioning (design page-streaming D3): one collider entity per window page ----
    UpdatePageColliders(world, *terrainService);

    // ---- Streamed planet page bodies ----
    UpdatePlanetColliders(world, *terrainService);
}

void TerrainPhysicsSystem::UpdatePageColliders(ECS::World& world, TerrainService& service)
{
    std::vector<PagedTerrain> terrains;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [&terrains, &service](const Components::Terrain& terrain, const Components::WorldTransform& transform) {
            AddPagedTerrain(terrains, service, terrain, transform);
        });

    // Every page collider this system owns, whatever its enable state: one switched off still claims
    // its page. A collider whose page left every window of its terrain, whose terrain is no longer
    // paged, or whose samples were read from a stream since restarted (a new source) is torn down and
    // its page slot released here (its OnRemove hook then finds the slot gone).
    PageColliderOrphans orphans;
    world.Query<ECS::Read<Components::TerrainPageCollider>>().IncludeDisabled().Each(
        [&service, &terrains, &orphans](ECS::EntityHandle entity, const Components::TerrainPageCollider& tag) {
            SweepPageCollider(service, terrains, orphans, entity, tag);
        });
    // The body is freed now, in the window PhysicsInit mutates bodies in and before the step; the
    // entity's destruction is deferred to the next flush (ReleaseColliderBody).
    TearDownOrphans(world, service, orphans);

    // A window page without a collider gets one once it is on the CPU at level 0; the residency's
    // window fetch brings it (design D3), so a page still on its way is provisioned a later frame.
    // Its body is built on the job pool (PhysicsInitSystem) from the samples read here.
    constexpr float32 kOwned = static_cast<float32>(PageStreaming::kPageOwnedSamples);
    for (const PagedTerrain& paged : terrains)
        for (const PageStreaming::PageAddress& page : paged.Window)
        {
            if (!std::binary_search(paged.SortedWindow.begin(), paged.SortedWindow.end(), PageKey(page.X, page.Z)))
                continue;
            const TerrainService::PagePhysicsHandle handle = service.AcquirePagePhysicsHandle(paged.Query, page.X, page.Z);
            if (handle.Index == 0)
                continue;
            const SurfaceHeightQuery& query = paged.Query;
            const float32 sizeX = kOwned * query.LatticeSpacingX();
            const float32 sizeZ = kOwned * query.LatticeSpacingZ();
            const Mathematics::WorldPosition center{
                query.FootprintOriginX() + (static_cast<float64>(page.X) + 0.5) * sizeX, query.OriginY(),
                query.FootprintOriginZ() + (static_cast<float64>(page.Z) + 0.5) * sizeZ};
            const ECS::EntityHandle entity =
                CreateTerrainColliderEntity(world, MakeTranslation(center), MakeWorldTranslation(center), handle.Index,
                                            handle.Generation, sizeX, sizeZ, query.HeightScale());
            Components::TerrainPageCollider tag{};
            tag.TiledIndex = paged.Tiled.Index;
            tag.TiledGeneration = paged.Tiled.Generation;
            tag.PageX = page.X;
            tag.PageZ = page.Z;
            tag.PhysicsHandleIndex = handle.Index;
            tag.PhysicsHandleGeneration = handle.Generation;
            world.AddComponent(entity, tag);
        }
}

void TerrainPhysicsSystem::RemoveOrphanedPlanarColliders(ECS::World& world)
{
    std::erase_if(m_PlanarColliders, [&](ECS::EntityHandle entity)
    {
        if (!world.IsValid(entity))
            return true;
        const auto* terrain = world.GetComponent<Components::Terrain>(entity);
        const bool planarData = terrain && terrain->Domain == Components::TerrainDomain::Planar &&
                                terrain->TiledTerrainHandle == 0 && terrain->TiledTerrainGeneration == 0 &&
                                (terrain->TerrainDataHandle != 0 || terrain->TerrainDataGeneration != 0);
        if (planarData)
            return false; // Still tracked while its provisioned components are in flight.
        if (!world.HasComponent<Components::HeightFieldColliderShape>(entity))
            return true;
        ReleaseColliderBody(world, entity);
        world.RemoveComponent<Components::HeightFieldColliderShape>(entity);
        world.RemoveComponent<Components::PhysicsCollider>(entity);
        world.RemoveComponent<Components::PhysicsBody>(entity);
        Logger::Log::Info("TerrainPhysicsSystem: entity {} no longer has planar terrain data; removed its collider",
                          entity.id);
        return true;
    });
}

void TerrainPhysicsSystem::UpdatePlanetColliders(ECS::World& world, TerrainService& service)
{
    GE_CPU_PROFILE_SCOPE("Terrain.PlanetColliders");
    uint32 planetCount = 0;
    const ECS::EntityHandle planet = FindActivePlanet(world, planetCount);
    if (planetCount > 1 && !m_WarnedDuplicatePlanetClaim)
    {
        Logger::Log::Warning("TerrainPhysicsSystem: multiple spherical terrains; only the one that draws first "
                             "gets planet colliders");
        m_WarnedDuplicatePlanetClaim = true;
    }
    if (planet.IsValid())
        ShedPlanarCollider(world, planet);

    // The planet's CPU windows as its stream left them. With none, every planet page body is an orphan.
    const TerrainPages* pages = service.FindTerrainPages();
    PlanetCpuView view;
    const bool hasView = planet.IsValid() && pages && pages->FindPlanetCpuView(planet.id, view);
    std::vector<uint64>& windowKeys = m_PlanetScratch->WindowKeys;
    windowKeys.clear();
    if (hasView)
    {
        for (const PageStreaming::PageAddress& page : view.BodyPages)
            windowKeys.push_back(PlanetPageKey(page));
        std::sort(windowKeys.begin(), windowKeys.end());
    }

    // Every planet page body, whatever its enable state: one switched off still claims its page.
    PlanetBodySweep& sweep = m_PlanetScratch->Sweep;
    sweep.Clear();
    const PlanetCpuView* viewOrNull = hasView ? &view : nullptr;
    world.Query<ECS::Read<Components::TerrainPlanetPageCollider>>().IncludeDisabled().Each(
        [&](ECS::EntityHandle entity, const Components::TerrainPlanetPageCollider& tag) {
            SweepPlanetBody(world, service, planet, viewOrNull, windowKeys, sweep, entity, tag);
        });
    HandOverPlanetBodies(sweep);
    TearDownOrphans(world, service, sweep.Orphans);
    if (!hasView)
    {
        m_PlanetFills.clear();
        return;
    }

    // Done fills land on the main thread: a refill writes its body's slot, a new fill makes a body.
    const Mathematics::Vector3 planetCenter =
        world.GetComponent<Components::WorldTransform>(planet)->GetPosition() - Mathematics::WorldPosition{};
    size_t kept = 0;
    for (std::shared_ptr<PlanetPageFill>& fill : m_PlanetFills)
    {
        if (!fill->Done.load(std::memory_order_acquire))
        {
            m_PlanetFills[kept++] = std::move(fill);
            continue;
        }
        const PlanetBodyRecord landed = LandPlanetFill(world, service, planet, planetCenter, view, windowKeys, *fill);
        if (landed.Entity.IsValid())
            sweep.Current.push_back(landed);
    }
    m_PlanetFills.resize(kept);
    std::sort(sweep.Current.begin(), sweep.Current.end(), RecordKeyLess);

    // A window page with no body on the current stream gets a fill, and so does a body older than the
    // page layer. Window order puts the pages under the cameras first.
    std::vector<PageStreaming::PageAddress>& reads = m_PlanetScratch->Reads;
    size_t started = 0;
    for (const PageStreaming::PageAddress& page : view.BodyPages)
    {
        if (started == kPlanetFillsStartedPerFrame || m_PlanetFills.size() == kPlanetFillsInFlight)
            break;
        const uint64 key = PlanetPageKey(page);
        if (std::any_of(m_PlanetFills.begin(), m_PlanetFills.end(),
                        [key](const std::shared_ptr<PlanetPageFill>& fill) { return PlanetPageKey(fill->Page) == key; }))
            continue;
        const PlanetBodyRecord* body = FindBody(sweep.Current, key);
        if (body && body->Tag.LayerVersion == view.LayerVersion)
            continue;
        std::shared_ptr<PlanetPageFill> fill = StartPlanetFill(world, *pages, planet, view, page, body, reads);
        if (!fill)
            continue;
        m_PlanetFills.push_back(std::move(fill));
        ++started;
    }
}

} // namespace GameEngine::TerrainECS
