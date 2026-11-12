// HLOD runtime integration (design v0.2 §5, integration layer). End-to-end on a
// real GPUScene + device: bake a world's HLOD clusters to a .gehlod, reconcile it
// back (HlodRuntime), and drive the HLODSelectSystem's coverage switch. Verifies
// the integration glue the pure/logic tests (HlodBaker, HlodReconcile, HlodSelect,
// HlodEvictionStick) don't cover together: proxies spawn EVICTED while members
// stay resident, and a far/near camera flips residency between members and proxy.
//
// Device-gated (GTEST_SKIP without Vulkan): ReconcileScene registers proxy meshes
// through the real MeshGPURegistry, which needs a device.

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/HlodRuntime.h"
#include "ECSModules/Rendering/Systems/HLODSelectSystem.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Components/Hierarchy.h"

#include "Assets/HlodBakeDriver.h"
#include "Assets/HlodCache.h"
#include "Assets/ModelAsset.h"

#include "Components/Rendering/HLODProxy.h"
#include "Components/Rendering/HLODVolume.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"

#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include "Rendering/Camera/CameraTypes.h"
#include "TestCameraEye.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Components;
using namespace GameEngine::Engine::Renderer;

#include "TestDeviceHelper.h"

namespace {

Mesh MakeBox(float extent) {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    Vertex a{}, b{}, c{};
    a.Position[1] = extent;
    b.Position[0] = -extent; b.Position[1] = -extent;
    c.Position[0] = extent;  c.Position[1] = -extent;
    a.Normal[2] = b.Normal[2] = c.Normal[2] = 1.0f;
    mesh.Vertices = {a, b, c};
    mesh.Indices = {0, 1, 2};
    mesh.MinBounds[0] = mesh.MinBounds[1] = mesh.MinBounds[2] = -extent;
    mesh.MaxBounds[0] = mesh.MaxBounds[1] = mesh.MaxBounds[2] = extent;
    return mesh;
}

struct ModelBank {
    std::unordered_map<GUID, std::unique_ptr<ModelAsset>> Models;
    const ModelAsset* Add(const GUID& guid, uint64 contentHash) {
        auto a = std::make_unique<ModelAsset>(guid, std::filesystem::path{});
        a->SetMeshesForTest({MakeBox(1.0f)});
        a->SetSourceContentHashForTest(contentHash);
        const ModelAsset* raw = a.get();
        Models[guid] = std::move(a);
        return raw;
    }
    Hlod::ModelResolver Resolver() {
        return [this](const GUID& g) -> const ModelAsset* {
            auto it = Models.find(g);
            return it == Models.end() ? nullptr : it->second.get();
        };
    }
};

Components::WorldTransform WorldAt(float x, float y, float z) {
    Components::WorldTransform wt{};
    wt.Position = Mathematics::WorldPosition{x, y, z};
    return wt;
}

Components::SceneEntityTag Tag(const std::string& s) {
    Components::SceneEntityTag t{};
    std::memset(t.value, 0, sizeof(t.value));
    std::strncpy(t.value, s.c_str(), sizeof(t.value) - 1);
    return t;
}

Components::LocalBounds BoxBounds(float extent) {
    Components::LocalBounds lb{};
    lb.Box = Mathematics::BoundingBox::FromMinMax(
        Mathematics::Vector3{-extent, -extent, -extent},
        Mathematics::Vector3{extent, extent, extent});
    return lb;
}

// The near and culling range of the CameraAt() cameras.
constexpr Rendering::CameraRanges kCameraAtRanges{0.1f, 1000.0f};

// A camera at (x,y,z) with an identity projection (projScaleY = proj[1][1] = 1).
Rendering::CameraData CameraAt(float x, float y, float z) {
    Rendering::CameraData cam{};
    cam.view = Mathematics::Matrix4x4::Identity();
    cam.proj = Mathematics::Matrix4x4::Identity();
    cam.viewProj = Mathematics::Matrix4x4::Identity();
    cam.cameraPos.x = x;
    cam.cameraPos.y = y;
    cam.cameraPos.z = z;
    cam.cameraPos.w = 0.0f; // not a 2D ortho view -> LOD enabled
    return cam;
}

std::filesystem::path TempPath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

// One cell of `count` FAR statics, each a distinct model (r = 1 so the benefit
// gate admits), tagged for stable reconcile. Placed around x0 so the cluster sits
// far from the origin and a coverage test can drive a proxy switch.
void SpawnCluster(ECS::World& world, ModelBank& bank, int count, float x0, const GUID& matGuid) {
    for (int i = 0; i < count; ++i) {
        const GUID modelGuid = GUID::Derive(GUID::Null(), "reconcile/model/" + std::to_string(i));
        const ModelAsset* model = bank.Add(modelGuid, 0x9000u + static_cast<uint64>(i));
        (void)model;
        ECS::Entity e = world.Create();
        e.Set(WorldAt(x0 + static_cast<float>(i) * 2.0f, 0.0f, 0.0f));
        e.Set(BoxBounds(1.0f));
        e.Set(Tag("member_" + std::to_string(i)));
        MeshRenderer mr{};
        mr.materialAssetGuid.Set(matGuid);
        mr.modelAssetGuid.Set(modelGuid);
        e.Set(mr);
        // The GPUScene bridge extraction would add; the test drives the select
        // switch without extraction, so seed it so the residency flag has a home.
        e.Set(MeshGPUData{});
    }
}

void AddVolume(ECS::World& world) {
    ECS::Entity v = world.Create();
    HLODVolume vol{};
    vol.CellSize = 1000.0f;    // whole cluster in one cell
    vol.MaxInstancingRatio = 4.0f;
    vol.MinMembers = 8u;
    vol.VBBudgetMB = 256u;
    vol.SwitchCoverage = 0.08f;
    vol.SwitchHysteresis = 0.25f;
    v.Set(vol);
}

constexpr int kMemberCount = 10;

// Bake + reconcile the world as it is; returns the .gehlod path (caller removes it).
std::filesystem::path BakeAndReconcileWorld(RenderServices& rs, ECS::World& world, ModelBank& bank,
                                            Hlod::HlodRuntime& runtime, const char* fileName) {
    const std::filesystem::path out = TempPath(fileName);
    std::filesystem::remove(out);
    const Hlod::BakeDriverResult bake = Hlod::BakeHlodForWorld(world, bank.Resolver(), out);
    EXPECT_EQ(bake.Outcome, Hlod::BakeOutcome::Wrote);

    runtime.ReconcileScene(world, rs.GetMeshGPURegistry(), out, "test", bank.Resolver());
    return out;
}

// Bake + reconcile a fresh world; returns the .gehlod path (caller removes it).
std::filesystem::path BakeAndReconcile(RenderServices& rs, ECS::World& world, ModelBank& bank,
                                       Hlod::HlodRuntime& runtime, const GUID& matGuid,
                                       float clusterX, const char* fileName) {
    AddVolume(world);
    SpawnCluster(world, bank, kMemberCount, clusterX, matGuid);
    world.ProcessCommands();
    return BakeAndReconcileWorld(rs, world, bank, runtime, fileName);
}

ECS::EntityHandle FindTagged(ECS::World& world, const std::string& name) {
    ECS::EntityHandle found{};
    world.Query<ECS::Read<SceneEntityTag>>().Each([&](ECS::EntityHandle e, const SceneEntityTag& tag) {
        if (tag.View() == name)
            found = e;
    });
    return found;
}

// A still scene: the hierarchy and the select system run each frame and nothing moves.
// The destroyed member is alone in its archetype, so its chunk empties and gets no column stamp.
void ExpectDestroyedMemberMakesClusterStale(Rendering::IDevice* device, bool parented, const char* fileName) {
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();
    TransformHierarchySystem hierarchy;
    HLODSelectSystem select(&rs);
    constexpr float kDt = 1.0f / 60.0f;

    AddVolume(*world);
    SpawnCluster(*world, bank, kMemberCount, 0.0f, GUID::Derive(GUID::Null(), fileName));
    world->ProcessCommands();
    const ECS::EntityHandle lone = FindTagged(*world, "member_0");
    ASSERT_TRUE(lone.IsValid());
    world->AddComponent(lone, Transform::FromTRS(Mathematics::WorldPosition{0.0, 0.0, 0.0}, Mathematics::Quaternion{},
                                                 Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
    if (parented) {
        // An identity root, so the member is a hierarchy node and its world pose does not change.
        ECS::Entity anchor = world->Create();
        anchor.Set(Transform{});
        anchor.Set(WorldTransform{});
        world->AddComponent(lone, Parent{anchor.GetHandle()});
    }
    world->ProcessCommands();
    // The first update writes every pose. Do it before the reconcile records the member versions.
    hierarchy.Update(*world, kDt);
    const std::filesystem::path out = BakeAndReconcileWorld(rs, *world, bank, runtime, fileName);
    ASSERT_EQ(runtime.GetClusters().size(), 1u);

    Rendering::CameraId cam = rs.Views().AllocateCamera("HlodDestroyedMemberCam");
    rs.Views().SetViewpointCamera(cam);
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)), kCameraAtRanges);
    for (int frame = 0; frame < 3; ++frame) {
        hierarchy.Update(*world, kDt);
        select.Update(*world, kDt);
    }
    ASSERT_TRUE(runtime.GetClusters()[0].ProxyActive);
    ASSERT_FALSE(runtime.GetClusters()[0].Stale);

    world->DestroyEntity(lone);
    world->ProcessCommands();
    for (int frame = 0; frame < 2; ++frame) {
        hierarchy.Update(*world, kDt);
        select.Update(*world, kDt);
    }
    EXPECT_TRUE(runtime.GetClusters()[0].Stale) << "a destroyed member must mark the cluster stale";
    EXPECT_FALSE(runtime.GetClusters()[0].ProxyActive) << "the proxy must not stand in for a destroyed member";

    runtime.Retire(*world, rs.GetMeshGPURegistry());
    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

} // namespace

TEST(HlodRuntimeReconcile, ReconcileSpawnsEvictedProxiesAndTagsMembers) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();

    const GUID matGuid = GUID::Derive(GUID::Null(), "reconcile/material");
    const std::filesystem::path out =
        BakeAndReconcile(rs, *world, bank, runtime, matGuid, 0.0f, "hlod_reconcile_spawn.gehlod");

    ASSERT_EQ(runtime.GetClusters().size(), 1u);
    const Hlod::RuntimeCluster& cluster = runtime.GetClusters()[0];
    EXPECT_EQ(cluster.Members.size(), static_cast<size_t>(kMemberCount));
    EXPECT_FALSE(cluster.Stale);
    ASSERT_FALSE(cluster.Proxies.empty()) << "an admitted cluster must spawn a proxy";
    EXPECT_FALSE(cluster.ProxyActive) << "proxies start evicted; members are the resident set";

    // Every proxy spawned EVICTED (members are what renders until a far switch).
    for (ECS::EntityHandle proxy : cluster.Proxies) {
        EXPECT_TRUE(world->HasComponent<HLODProxy>(proxy));
        EXPECT_TRUE(world->HasComponent<RuntimeOnlyEntity>(proxy));
        const auto* gpu = world->GetComponent<MeshGPUData>(proxy);
        ASSERT_NE(gpu, nullptr);
        EXPECT_TRUE(gpu->hlodEvicted) << "proxy must spawn evicted";
    }

    // Every member stays resident (NOT evicted) after reconcile.
    for (ECS::EntityHandle member : cluster.Members) {
        const auto* gpu = world->GetComponent<MeshGPUData>(member);
        if (gpu)
        {
            EXPECT_FALSE(gpu->hlodEvicted);
        }
        // C6: eviction never disables the entity.
        ASSERT_NE(world->GetComponent<MeshRenderer>(member), nullptr);
        EXPECT_TRUE(ECS::Entity(world.get(), member).IsEnabled<MeshRenderer>());
    }

    // C7: retire must release every proxy mesh registration — it is the ONLY
    // release path. SceneDocumentManager mirrors this call before World::Clear
    // on File > New (Clear restarts entity versions, so surviving cluster
    // handles would alias freshly-seeded entities).
    const std::vector<GameEngine::Rendering::MeshGPUKey> proxyKeys = cluster.ProxyMeshKeys;
    ASSERT_FALSE(proxyKeys.empty());
    runtime.Retire(*world, rs.GetMeshGPURegistry());
    EXPECT_TRUE(runtime.GetClusters().empty());
    for (const GameEngine::Rendering::MeshGPUKey& key : proxyKeys)
        EXPECT_EQ(rs.GetMeshGPURegistry().FindByKey(key), nullptr)
            << "proxy mesh must be unregistered on retire";

    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

// Members in one cell on two layers bake into one cluster per layer set, and each proxy draws on its
// members' layers: no proxy draws a member in a view that does not draw that member.
TEST(HlodRuntimeReconcile, ProxyLayersEqualTheirMembersLayers) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();

    const GUID matGuid = GUID::Derive(GUID::Null(), "reconcile/material-layers");
    AddVolume(*world);
    for (int i = 0; i < 2 * kMemberCount; ++i) {
        const GUID modelGuid = GUID::Derive(GUID::Null(), "reconcile/layers/model/" + std::to_string(i));
        bank.Add(modelGuid, 0xA000u + static_cast<uint64>(i));
        ECS::Entity e = world->Create();
        e.Set(WorldAt(static_cast<float>(i) * 2.0f, 0.0f, 0.0f));
        e.Set(BoxBounds(1.0f));
        e.Set(Tag("layered_" + std::to_string(i)));
        MeshRenderer mr{};
        mr.materialAssetGuid.Set(matGuid);
        mr.modelAssetGuid.Set(modelGuid);
        mr.RenderLayers = (i % 2 == 0) ? Rendering::RenderLayerMask::Default() : Rendering::RenderLayerMask::FromLayer(3);
        e.Set(mr);
        e.Set(MeshGPUData{});
    }
    world->ProcessCommands();

    const std::filesystem::path out = TempPath("hlod_reconcile_layers.gehlod");
    std::filesystem::remove(out);
    ASSERT_EQ(Hlod::BakeHlodForWorld(*world, bank.Resolver(), out).Outcome, Hlod::BakeOutcome::Wrote);
    runtime.ReconcileScene(*world, rs.GetMeshGPURegistry(), out, "test", bank.Resolver());

    ASSERT_EQ(runtime.GetClusters().size(), 2u) << "one cluster per layer set in the cell";
    for (const Hlod::RuntimeCluster& cluster : runtime.GetClusters()) {
        ASSERT_FALSE(cluster.Proxies.empty());
        ASSERT_FALSE(cluster.Members.empty());
        for (ECS::EntityHandle proxy : cluster.Proxies) {
            const auto* proxyRenderer = world->GetComponent<MeshRenderer>(proxy);
            ASSERT_NE(proxyRenderer, nullptr);
            for (ECS::EntityHandle member : cluster.Members)
                EXPECT_EQ(proxyRenderer->RenderLayers, world->GetComponent<MeshRenderer>(member)->RenderLayers);
        }
    }

    runtime.Retire(*world, rs.GetMeshGPURegistry());
    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

TEST(HlodRuntimeReconcile, SelectSystemFlipsResidencyByCoverage) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();

    const GUID matGuid = GUID::Derive(GUID::Null(), "reconcile/material2");
    const std::filesystem::path out =
        BakeAndReconcile(rs, *world, bank, runtime, matGuid, 0.0f, "hlod_reconcile_flip.gehlod");
    ASSERT_EQ(runtime.GetClusters().size(), 1u);

    // A viewpoint camera the select system reads. Start it far so the cluster
    // (center ~ (9,0,0), radius ~10) subtends little screen -> proxy-active.
    Rendering::CameraId cam = rs.Views().AllocateCamera("HlodReconcileCam");
    rs.Views().SetViewpointCamera(cam);

    HLODSelectSystem select(&rs);
    constexpr float kDt = 1.0f / 60.0f;

    // FAR: coverage = radius * 1 / dist ~ 10 / 2000 = 0.005 < enter (0.08) -> proxy.
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)), kCameraAtRanges);
    select.Update(*world, kDt);

    ASSERT_TRUE(rs.ConsumeHlodResidencyExtractionPending())
        << "a residency flip must request the full extraction lane";
    EXPECT_TRUE(runtime.GetClusters()[0].ProxyActive);
    for (ECS::EntityHandle member : runtime.GetClusters()[0].Members) {
        const auto* gpu = world->GetComponent<MeshGPUData>(member);
        ASSERT_NE(gpu, nullptr);
        EXPECT_TRUE(gpu->hlodEvicted) << "far cluster: members evicted";
    }
    for (ECS::EntityHandle proxy : runtime.GetClusters()[0].Proxies) {
        const auto* gpu = world->GetComponent<MeshGPUData>(proxy);
        ASSERT_NE(gpu, nullptr);
        EXPECT_FALSE(gpu->hlodEvicted) << "far cluster: proxy resident";
    }

    // NEAR: camera close to the cluster -> coverage high -> members back.
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(9.0f, 0.0f, -12.0f)), kCameraAtRanges);
    select.Update(*world, kDt);

    EXPECT_TRUE(rs.ConsumeHlodResidencyExtractionPending());
    EXPECT_FALSE(runtime.GetClusters()[0].ProxyActive);
    for (ECS::EntityHandle member : runtime.GetClusters()[0].Members) {
        const auto* gpu = world->GetComponent<MeshGPUData>(member);
        ASSERT_NE(gpu, nullptr);
        EXPECT_FALSE(gpu->hlodEvicted) << "near cluster: members resident";
    }
    for (ECS::EntityHandle proxy : runtime.GetClusters()[0].Proxies) {
        const auto* gpu = world->GetComponent<MeshGPUData>(proxy);
        ASSERT_NE(gpu, nullptr);
        EXPECT_TRUE(gpu->hlodEvicted) << "near cluster: proxy evicted";
    }

    runtime.Retire(*world, rs.GetMeshGPURegistry());
    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

// S1 (stale HLOD proxies): the production reconcile recomputes the expected
// ConfigHash from the live HLODVolume and the CURRENT cook-semantics versions.
// A .gehlod keyed under different semantics (e.g. baked before a
// kLodGeneratorVersion bump) must be rejected fail-visibly — members-only, no
// proxies — never silently drawn beside freshly-cooked members.
TEST(HlodRuntimeReconcile, StaleConfigHashBakeIsRejectedAtReconcile) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();

    const GUID matGuid = GUID::Derive(GUID::Null(), "reconcile/material4");
    const std::filesystem::path out = BakeAndReconcile(
        rs, *world, bank, runtime, matGuid, 0.0f, "hlod_reconcile_staleconfig.gehlod");
    ASSERT_EQ(runtime.GetClusters().size(), 1u) << "fresh bake must reconcile";

    // Re-key the bake as if it were produced under different cook semantics.
    Hlod::HlodBakedScene baked;
    ASSERT_EQ(Hlod::ReadHlodCache(out, nullptr, baked), Hlod::HlodCacheStatus::Hit);
    baked.Key.ConfigHash ^= 0x1ull;
    ASSERT_TRUE(Hlod::WriteHlodCache(out, baked));

    const uint32 clusters =
        runtime.ReconcileScene(*world, rs.GetMeshGPURegistry(), out, "test", bank.Resolver());
    EXPECT_EQ(clusters, 0u) << "a config/version-stale bake reconciled clusters";
    EXPECT_TRUE(runtime.GetClusters().empty());

    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

TEST(HlodRuntimeReconcile, MovedMemberFallsBackToMembersOnlyAndStaysStale) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();

    const GUID matGuid = GUID::Derive(GUID::Null(), "reconcile/material3");
    const std::filesystem::path out =
        BakeAndReconcile(rs, *world, bank, runtime, matGuid, 0.0f, "hlod_reconcile_stale.gehlod");
    ASSERT_EQ(runtime.GetClusters().size(), 1u);

    Rendering::CameraId cam = rs.Views().AllocateCamera("HlodStaleCam");
    rs.Views().SetViewpointCamera(cam);
    HLODSelectSystem select(&rs);
    constexpr float kDt = 1.0f / 60.0f;

    // Far -> proxy-active.
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)), kCameraAtRanges);
    select.Update(*world, kDt);
    ASSERT_TRUE(runtime.GetClusters()[0].ProxyActive);
    (void)rs.ConsumeHlodResidencyExtractionPending();

    // Move one member (bump its WorldTransform.Version, as an editor edit would):
    // the cluster's frozen proxy no longer represents it (C3 / §3.4).
    const ECS::EntityHandle moved = runtime.GetClusters()[0].Members.front();
    auto* wt = world->GetComponentForWrite<WorldTransform>(moved);
    ASSERT_NE(wt, nullptr);
    wt->Position.X += 50.0;
    wt->Version += 1u;

    // Next select tick: the edit is detected -> cluster stale -> members-only.
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)), kCameraAtRanges);
    select.Update(*world, kDt);

    EXPECT_TRUE(runtime.GetClusters()[0].Stale) << "a moved member must mark the cluster stale";
    EXPECT_FALSE(runtime.GetClusters()[0].ProxyActive) << "stale cluster falls back to members-only";
    for (ECS::EntityHandle member : runtime.GetClusters()[0].Members) {
        const auto* gpu = world->GetComponent<MeshGPUData>(member);
        ASSERT_NE(gpu, nullptr);
        EXPECT_FALSE(gpu->hlodEvicted) << "stale cluster: members resident";
    }
    for (ECS::EntityHandle proxy : runtime.GetClusters()[0].Proxies) {
        const auto* gpu = world->GetComponent<MeshGPUData>(proxy);
        ASSERT_NE(gpu, nullptr);
        EXPECT_TRUE(gpu->hlodEvicted) << "stale cluster: proxy evicted";
    }

    // Staleness is persistent: even a far camera never re-engages the proxy.
    (void)rs.ConsumeHlodResidencyExtractionPending();
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -3000.0f)), kCameraAtRanges);
    select.Update(*world, kDt);
    EXPECT_TRUE(runtime.GetClusters()[0].Stale);
    EXPECT_FALSE(runtime.GetClusters()[0].ProxyActive) << "stale is sticky until rebake + reload";

    runtime.Retire(*world, rs.GetMeshGPURegistry());
    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

TEST(HlodRuntimeReconcile, DestroyedParentedMemberMakesClusterStaleInStillScene) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    ExpectDestroyedMemberMakesClusterStale(device.get(), true, "hlod_reconcile_destroyed_parented.gehlod");
}

TEST(HlodRuntimeReconcile, DestroyedRootMemberMakesClusterStaleInStillScene) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    ExpectDestroyedMemberMakesClusterStale(device.get(), false, "hlod_reconcile_destroyed_root.gehlod");
}
