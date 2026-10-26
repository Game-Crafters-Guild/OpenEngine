// Tests for PrimitiveGenerator: mesh generation correctness, well-known GUID
// stability, and MeshGPURegistry registration.

#include <gtest/gtest.h>

#include "Components/Rendering/LocalBounds.h"
#include "Engine/Rendering/MeshReprovisionSource.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/Device.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;
using GameEngine::Mathematics::Vector3;

#include "TestDeviceHelper.h"

// --- GUID stability ---

TEST(PrimitiveGeneratorTest, GUIDs_AreDeterministic)
{
    GUID cube1 = PrimitiveGenerator::CubeGuid();
    GUID cube2 = PrimitiveGenerator::CubeGuid();
    EXPECT_EQ(cube1, cube2);
    EXPECT_FALSE(cube1.IsNull());
}

TEST(PrimitiveGeneratorTest, GUIDs_AreUnique)
{
    GUID cube = PrimitiveGenerator::CubeGuid();
    GUID sphere = PrimitiveGenerator::SphereGuid();
    GUID capsule = PrimitiveGenerator::CapsuleGuid();
    GUID plane = PrimitiveGenerator::PlaneGuid();
    GUID planeSprite = PrimitiveGenerator::PlaneSpriteUvGuid();
    GUID ground = PrimitiveGenerator::GroundGuid();

    EXPECT_NE(cube, sphere);
    EXPECT_NE(cube, capsule);
    EXPECT_NE(cube, plane);
    EXPECT_NE(cube, planeSprite);
    EXPECT_NE(sphere, capsule);
    EXPECT_NE(sphere, plane);
    EXPECT_NE(sphere, planeSprite);
    EXPECT_NE(capsule, plane);
    EXPECT_NE(capsule, planeSprite);
    EXPECT_NE(plane, planeSprite);
    for (const GUID& other : {cube, sphere, capsule, plane, planeSprite})
        EXPECT_NE(ground, other);
}

// --- Mesh generation ---

TEST(PrimitiveGeneratorTest, Cube_HasCorrectTopology)
{
    Mesh cube = PrimitiveGenerator::GenerateCube();
    EXPECT_EQ(cube.Vertices.size(), 24u); // 6 faces * 4 verts
    EXPECT_EQ(cube.Indices.size(), 36u);  // 6 faces * 2 tris * 3 indices
    EXPECT_EQ(cube.Name, "Cube");
}

namespace
{
Vector3 PositionOf(const Vertex& vertex)
{
    return {vertex.Position[0], vertex.Position[1], vertex.Position[2]};
}

// The y component of (p1 - p0) x (p2 - p0) for the triangle whose first index is `first`.
float TriangleCrossY(const Mesh& mesh, size_t first)
{
    const Vector3 p0 = PositionOf(mesh.Vertices[mesh.Indices[first]]);
    const Vector3 p1 = PositionOf(mesh.Vertices[mesh.Indices[first + 1]]);
    const Vector3 p2 = PositionOf(mesh.Vertices[mesh.Indices[first + 2]]);
    return Vector3::Cross(p1 - p0, p2 - p0).y;
}

// The 1 m XZ square cut into segments x segments cells: the Plane's UV layout
// (u = 0.5 - x, v = z + 0.5), every triangle wound (-X-Z, +X+Z, +X-Z), no edge longer
// than a cell, and the triangles cover the square exactly once.
void ExpectSquareCutIntoCells(const Mesh& mesh, uint32_t segments)
{
    ASSERT_EQ(mesh.Vertices.size(), (segments + 1) * (segments + 1));
    ASSERT_EQ(mesh.Indices.size(), segments * segments * 6);

    for (const Vertex& v : mesh.Vertices)
    {
        EXPECT_FLOAT_EQ(v.Position[1], 0.0f);
        EXPECT_NEAR(v.TexCoords[0], 0.5f - v.Position[0], 1e-6f);
        EXPECT_NEAR(v.TexCoords[1], v.Position[2] + 0.5f, 1e-6f);
    }
    for (int axis : {0, 2})
    {
        EXPECT_FLOAT_EQ(mesh.MinBounds[axis], -0.5f);
        EXPECT_FLOAT_EQ(mesh.MaxBounds[axis], 0.5f);
    }

    // Coverage: the triangles' areas sum to the square's and every vertex is used, so no
    // cell is missing or doubled.
    const float cell = 1.0f / static_cast<float>(segments);
    double area = 0.0;
    std::vector<bool> used(mesh.Vertices.size(), false);
    for (size_t first = 0; first < mesh.Indices.size(); first += 3)
    {
        const float crossY = TriangleCrossY(mesh, first);
        EXPECT_GT(crossY, 0.0f) << "triangle " << first / 3;
        area += 0.5 * std::abs(static_cast<double>(crossY));
        for (size_t corner = 0; corner < 3; ++corner)
        {
            used[mesh.Indices[first + corner]] = true;
            const Vector3 edge = PositionOf(mesh.Vertices[mesh.Indices[first + (corner + 1) % 3]]) -
                                 PositionOf(mesh.Vertices[mesh.Indices[first + corner]]);
            EXPECT_LE(std::abs(edge.x), cell * 1.001f);
            EXPECT_LE(std::abs(edge.z), cell * 1.001f);
        }
    }
    EXPECT_NEAR(area, 1.0, 1e-5);
    EXPECT_EQ(std::count(used.begin(), used.end(), false), 0) << "unreferenced vertices";
}
} // namespace

// A Plane drawn as one quad jumps near the camera by tens of pixels for a frame while a
// far corner crosses the camera's eye plane: the GPU's interpolation across the triangle
// loses precision. The cells keep every triangle small.
TEST(PrimitiveGeneratorTest, Plane_IsTheSquareCutIntoCells)
{
    const Mesh plane = PrimitiveGenerator::GeneratePlane();
    EXPECT_EQ(plane.Name, "Plane");
    ExpectSquareCutIntoCells(plane, 32);
}

// Sprites stay one quad: particles draw it in the thousands.
TEST(PrimitiveGeneratorTest, PlaneSpriteUv_IsOneQuadWithSpriteUvOrientation)
{
    const Mesh sprite = PrimitiveGenerator::GeneratePlaneSpriteUv();
    EXPECT_EQ(sprite.Name, "PlaneSpriteUv");
    ASSERT_EQ(sprite.Vertices.size(), 4u);
    ASSERT_EQ(sprite.Indices.size(), 6u);
    for (const Vertex& v : sprite.Vertices)
    {
        EXPECT_FLOAT_EQ(std::abs(v.Position[0]), 0.5f);
        EXPECT_FLOAT_EQ(std::abs(v.Position[2]), 0.5f);
        // The Plane's UV flipped on both axes.
        EXPECT_NEAR(v.TexCoords[0], v.Position[0] + 0.5f, 1e-6f);
        EXPECT_NEAR(v.TexCoords[1], 0.5f - v.Position[2], 1e-6f);
    }
    EXPECT_GT(TriangleCrossY(sprite, 0), 0.0f);
    EXPECT_GT(TriangleCrossY(sprite, 3), 0.0f);
}

// The Ground is the Plane's square, UV layout and winding cut into 120 x 120 cells
// (GroundGuid's contract).
TEST(PrimitiveGeneratorTest, Ground_IsThePlaneCutIntoCells)
{
    const Mesh ground = PrimitiveGenerator::GenerateByGuid(PrimitiveGenerator::GroundGuid());
    EXPECT_EQ(ground.Name, "Ground");
    ExpectSquareCutIntoCells(ground, 120);
}

TEST(PrimitiveGeneratorTest, GuidFromName_AcceptsSerializedPlaneSpriteUv)
{
    EXPECT_EQ(PrimitiveGenerator::GuidFromName("PlaneSpriteUv"), PrimitiveGenerator::PlaneSpriteUvGuid());
    EXPECT_EQ(PrimitiveGenerator::GuidFromName("planespriteuv"), PrimitiveGenerator::PlaneSpriteUvGuid());
    EXPECT_EQ(PrimitiveGenerator::GuidFromName("plane_sprite_uv"), PrimitiveGenerator::PlaneSpriteUvGuid());
    EXPECT_EQ(PrimitiveGenerator::GuidFromName("plane_sprite"), PrimitiveGenerator::PlaneSpriteUvGuid());
}

TEST(PrimitiveGeneratorTest, SerializedPrimitiveNames_RoundTripThroughGuidLookup)
{
    const GUID primitives[] = {
        PrimitiveGenerator::CubeGuid(),
        PrimitiveGenerator::SphereGuid(),
        PrimitiveGenerator::CapsuleGuid(),
        PrimitiveGenerator::PlaneGuid(),
        PrimitiveGenerator::PlaneSpriteUvGuid(),
        PrimitiveGenerator::GroundGuid(),
    };

    for (const GUID& guid : primitives)
    {
        const std::string_view name = PrimitiveGenerator::NameFromGuid(guid);
        ASSERT_FALSE(name.empty());
        EXPECT_EQ(PrimitiveGenerator::GuidFromName(name), guid) << name;
    }
}

TEST(PrimitiveGeneratorTest, Sphere_HasCorrectVertexCount)
{
    Mesh sphere = PrimitiveGenerator::GenerateSphere(0.5f, 16, 8);
    // (rings+1) * (segments+1) = 9 * 17 = 153
    EXPECT_EQ(sphere.Vertices.size(), 153u);
    EXPECT_EQ(sphere.Name, "Sphere");
    EXPECT_GT(sphere.Indices.size(), 0u);
}

TEST(PrimitiveGeneratorTest, Primitives_HaveGeneratedTangents)
{
    auto hasNonZeroTangent = [](const Mesh& mesh) {
        for (const Vertex& v : mesh.Vertices)
        {
            const float len2 = v.Tangent[0] * v.Tangent[0] + v.Tangent[1] * v.Tangent[1] +
                               v.Tangent[2] * v.Tangent[2];
            if (len2 > 1e-10f)
                return true;
        }
        return false;
    };

    EXPECT_TRUE(hasNonZeroTangent(PrimitiveGenerator::GenerateSphere()));
    EXPECT_TRUE(hasNonZeroTangent(PrimitiveGenerator::GenerateCube()));
    EXPECT_TRUE(hasNonZeroTangent(PrimitiveGenerator::GeneratePlane()));
    EXPECT_TRUE(hasNonZeroTangent(PrimitiveGenerator::GenerateCapsule()));
}

TEST(PrimitiveGeneratorTest, Capsule_HasVertices)
{
    Mesh capsule = PrimitiveGenerator::GenerateCapsule(0.25f, 0.5f, 16, 4);
    EXPECT_GT(capsule.Vertices.size(), 0u);
    EXPECT_GT(capsule.Indices.size(), 0u);
    EXPECT_EQ(capsule.Name, "Capsule");
}

TEST(PrimitiveGeneratorTest, AllPrimitives_HaveNormals)
{
    Mesh cube = PrimitiveGenerator::GenerateCube();
    bool hasNonZeroNormal = false;
    for (const auto& v : cube.Vertices)
    {
        if (v.Normal[0] != 0.0f || v.Normal[1] != 0.0f || v.Normal[2] != 0.0f)
        {
            hasNonZeroNormal = true;
            break;
        }
    }
    EXPECT_TRUE(hasNonZeroNormal);
}

TEST(PrimitiveGeneratorTest, AllPrimitives_HaveBounds)
{
    Mesh cube = PrimitiveGenerator::GenerateCube();
    EXPECT_LT(cube.MinBounds[0], cube.MaxBounds[0]);
    EXPECT_LT(cube.MinBounds[1], cube.MaxBounds[1]);
}

// --- Registration ---

class PrimitiveRegistrationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_device = CreateVulkanDeviceFast();
        if (!m_device)
            GTEST_SKIP() << "No Vulkan device available";
        m_rs = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_rs->Initialize(m_device.get()));
    }

    void TearDown() override
    {
        if (m_rs) m_rs->Shutdown();
        if (m_device) m_device->Shutdown();
    }

    std::unique_ptr<IDevice> m_device;
    std::unique_ptr<RenderServices> m_rs;
};

TEST_F(PrimitiveRegistrationTest, RegisterAll_CreatesGPUEntries)
{
    PrimitiveGenerator::RegisterAll(*m_rs);

    auto& reg = m_rs->GetMeshGPURegistry();

    // All five primitives should be registered.
    EXPECT_NE(reg.FindByKey({PrimitiveGenerator::CubeGuid(), 0}), nullptr);
    EXPECT_NE(reg.FindByKey({PrimitiveGenerator::SphereGuid(), 0}), nullptr);
    EXPECT_NE(reg.FindByKey({PrimitiveGenerator::CapsuleGuid(), 0}), nullptr);
    EXPECT_NE(reg.FindByKey({PrimitiveGenerator::PlaneGuid(), 0}), nullptr);
    EXPECT_NE(reg.FindByKey({PrimitiveGenerator::PlaneSpriteUvGuid(), 0}), nullptr);
}

// A device rebuild re-uploads every registry entry from a CPU source. Built-in
// primitives have no ModelAsset and no retained picking mirror, so a source lookup
// that consulted only the AssetManager tombstoned all five — and nothing
// re-registers them after a rebuild, so a scene built from primitives rendered
// nothing but sky for the life of the process.
TEST_F(PrimitiveRegistrationTest, ReprovisionRegeneratesPrimitivesInsteadOfTombstoningThem)
{
    PrimitiveGenerator::RegisterAll(*m_rs);

    auto& reg = m_rs->GetMeshGPURegistry();
    const MeshGPUEntry* cubeEntry = reg.FindByKey({PrimitiveGenerator::CubeGuid(), 0});
    ASSERT_NE(cubeEntry, nullptr) << "the fixture must have registered the primitives";
    const uint32_t gpuRowBefore = cubeEntry->gpuMeshIndex;

    MeshReprovisionSource sourceLookup;
    const MeshGPUReprovisionReport report =
        reg.ReprovisionAfterDeviceRebuild(std::ref(sourceLookup));

    EXPECT_EQ(report.Tombstoned, 0u)
        << "no built-in primitive may tombstone: nothing re-registers it after a rebuild";
    EXPECT_EQ(report.RestoredFromGenerated, report.EntriesTotal)
        << "every entry here is a built-in, so all must come back by regeneration";

    // The GPUScene row index is the identity that can actually be lost here (F9).
    // FindHandle is NOT a check: reprovision never touches the key->handle map, so it
    // returns the same handle even when every entry tombstoned.
    EXPECT_EQ(report.GpuRowsPreserved, report.EntriesTotal)
        << "GPUInstance.meshIndex references stay valid only if every row survives in place";
    EXPECT_EQ(gpuRowBefore, reg.FindByKey({PrimitiveGenerator::CubeGuid(), 0})->gpuMeshIndex);

    for (const GUID& guid : {PrimitiveGenerator::CubeGuid(), PrimitiveGenerator::SphereGuid(),
                             PrimitiveGenerator::CapsuleGuid(), PrimitiveGenerator::PlaneGuid(),
                             PrimitiveGenerator::PlaneSpriteUvGuid(), PrimitiveGenerator::GroundGuid()})
    {
        const MeshGPUEntry* entry = reg.FindByKey({guid, 0});
        ASSERT_NE(entry, nullptr);
        EXPECT_GT(entry->indexCount, 0u) << "a restored primitive must have drawable geometry";
    }
}

// Regeneration restores a primitive GUID's DEFAULT geometry, which is not necessarily
// what was registered — RegisterSubmesh supports re-uploading different content under
// an existing key. Carrying the pre-rebuild content hash onto that default geometry
// would leave the entry unhealable: re-registering the real geometry would hash to the
// retained value and take the dedup fast path instead of uploading.
TEST_F(PrimitiveRegistrationTest, RegeneratedEntryStaysHealableWhenItsContentWasNotTheDefault)
{
    auto& reg = m_rs->GetMeshGPURegistry();

    // Sphere geometry registered under the cube's GUID: a stand-in for any caller that
    // registers non-default content (bench LOD chains, a custom-sized capsule). The
    // index counts differ, so the geometry actually in the entry is observable.
    const Mesh custom = PrimitiveGenerator::GenerateSphere();
    const Mesh defaultCube = PrimitiveGenerator::GenerateCube();
    ASSERT_NE(custom.Indices.size(), defaultCube.Indices.size());

    const MeshGPUKey key{PrimitiveGenerator::CubeGuid(), 0};
    ASSERT_TRUE(reg.RegisterSubmesh(key, custom).IsValid());
    ASSERT_EQ(reg.FindByKey(key)->indexCount, static_cast<uint32_t>(custom.Indices.size()));

    MeshReprovisionSource sourceLookup;
    reg.ReprovisionAfterDeviceRebuild(std::ref(sourceLookup));

    // Recovery legitimately restores the default geometry — it has no way to know what
    // the caller had put there.
    EXPECT_EQ(reg.FindByKey(key)->indexCount, static_cast<uint32_t>(defaultCube.Indices.size()));

    // The owner re-registering its real geometry must take effect.
    ASSERT_TRUE(reg.RegisterSubmesh(key, custom).IsValid());
    EXPECT_EQ(reg.FindByKey(key)->indexCount, static_cast<uint32_t>(custom.Indices.size()))
        << "a stale content hash swallowed the re-registration: the entry is unhealable";
}

// GenerateByGuid is the table RegisterAll and rebuild recovery share. A primitive
// missing from it would register at startup and then silently fail to come back
// after a device loss.
TEST(PrimitiveGeneratorTest, GenerateByGuid_CoversEveryRegisteredPrimitive)
{
    for (const GUID& guid : {PrimitiveGenerator::CubeGuid(), PrimitiveGenerator::SphereGuid(),
                             PrimitiveGenerator::CapsuleGuid(), PrimitiveGenerator::PlaneGuid(),
                             PrimitiveGenerator::PlaneSpriteUvGuid(), PrimitiveGenerator::GroundGuid()})
    {
        const Mesh mesh = PrimitiveGenerator::GenerateByGuid(guid);
        EXPECT_FALSE(mesh.Vertices.empty()) << PrimitiveGenerator::NameFromGuid(guid);
        EXPECT_FALSE(mesh.Indices.empty()) << PrimitiveGenerator::NameFromGuid(guid);
    }
}

TEST(PrimitiveGeneratorTest, GenerateByGuid_ReturnsEmptyForANonPrimitive)
{
    const Mesh mesh = PrimitiveGenerator::GenerateByGuid(GUID::Generate());
    EXPECT_TRUE(mesh.Vertices.empty());
    EXPECT_TRUE(mesh.Indices.empty());
}

TEST_F(PrimitiveRegistrationTest, RegisterAll_Deduplicates)
{
    PrimitiveGenerator::RegisterAll(*m_rs);
    size_t countAfterFirst = m_rs->GetMeshGPURegistry().GetEntryCount();

    PrimitiveGenerator::RegisterAll(*m_rs);
    size_t countAfterSecond = m_rs->GetMeshGPURegistry().GetEntryCount();

    EXPECT_EQ(countAfterFirst, countAfterSecond) << "Second registration should not create new entries";
}

TEST_F(PrimitiveRegistrationTest, CubeGPUEntry_HasValidBuffers)
{
    PrimitiveGenerator::RegisterAll(*m_rs);

    auto& reg = m_rs->GetMeshGPURegistry();
    const auto* entry = reg.FindByKey({PrimitiveGenerator::CubeGuid(), 0});
    ASSERT_NE(entry, nullptr);
    MeshGPUEntryBindings bindings{};
    ASSERT_TRUE(reg.TryGetDrawableBindings(*entry, bindings));

    EXPECT_TRUE(bindings.coreVB.IsValid());
    EXPECT_TRUE(bindings.indexBuffer.IsValid());
    EXPECT_EQ(entry->indexCount, 36u);
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasPosition));
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasNormal));
    EXPECT_TRUE(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasTangent));
}


// --- MakePrimitiveLocalBounds (Phase D follow-up) ---

TEST(PrimitiveGeneratorTest, LocalBounds_DefaultPathFallbackPerType)
{
    // Without a RenderServices the factory falls back to default-parameter
    // geometry bounds. Each primitive has a documented expected halfExtents.
    using LB = Components::LocalBounds;

    LB cube    = PrimitiveGenerator::MakePrimitiveLocalBounds(nullptr, PrimitiveGenerator::CubeGuid());
    LB sphere  = PrimitiveGenerator::MakePrimitiveLocalBounds(nullptr, PrimitiveGenerator::SphereGuid());
    LB capsule = PrimitiveGenerator::MakePrimitiveLocalBounds(nullptr, PrimitiveGenerator::CapsuleGuid());
    LB plane   = PrimitiveGenerator::MakePrimitiveLocalBounds(nullptr, PrimitiveGenerator::PlaneGuid());
    LB planeSp = PrimitiveGenerator::MakePrimitiveLocalBounds(nullptr, PrimitiveGenerator::PlaneSpriteUvGuid());
    LB ground  = PrimitiveGenerator::MakePrimitiveLocalBounds(nullptr, PrimitiveGenerator::GroundGuid());

    // Cube + Sphere: unit cube halfExtents (0.5, 0.5, 0.5).
    EXPECT_FLOAT_EQ(cube.Box.halfExtents.x, 0.5f);
    EXPECT_FLOAT_EQ(cube.Box.halfExtents.y, 0.5f);
    EXPECT_FLOAT_EQ(cube.Box.halfExtents.z, 0.5f);
    EXPECT_FLOAT_EQ(sphere.Box.halfExtents.x, 0.5f);
    EXPECT_FLOAT_EQ(sphere.Box.halfExtents.y, 0.5f);
    EXPECT_FLOAT_EQ(sphere.Box.halfExtents.z, 0.5f);

    // Capsule: 1 wide, 2 tall (radius 0.5 + cylinder length 1.0 = total height 2.0).
    EXPECT_FLOAT_EQ(capsule.Box.halfExtents.x, 0.5f);
    EXPECT_FLOAT_EQ(capsule.Box.halfExtents.y, 1.0f);
    EXPECT_FLOAT_EQ(capsule.Box.halfExtents.z, 0.5f);

    // Plane: flat in XZ, zero Y extent.
    EXPECT_FLOAT_EQ(plane.Box.halfExtents.x, 0.5f);
    EXPECT_FLOAT_EQ(plane.Box.halfExtents.y, 0.0f);
    EXPECT_FLOAT_EQ(plane.Box.halfExtents.z, 0.5f);
    EXPECT_FLOAT_EQ(planeSp.Box.halfExtents.x, plane.Box.halfExtents.x);
    EXPECT_FLOAT_EQ(planeSp.Box.halfExtents.y, plane.Box.halfExtents.y);
    EXPECT_FLOAT_EQ(planeSp.Box.halfExtents.z, plane.Box.halfExtents.z);
    EXPECT_FLOAT_EQ(ground.Box.halfExtents.x, plane.Box.halfExtents.x);
    EXPECT_FLOAT_EQ(ground.Box.halfExtents.y, plane.Box.halfExtents.y);
    EXPECT_FLOAT_EQ(ground.Box.halfExtents.z, plane.Box.halfExtents.z);
}

TEST(PrimitiveGeneratorTest, LocalBounds_UnknownGuidFallsBackToUnitCube)
{
    GUID arbitrary = GUID::Generate();
    Components::LocalBounds lb =
        PrimitiveGenerator::MakePrimitiveLocalBounds(nullptr, arbitrary);
    EXPECT_FLOAT_EQ(lb.Box.halfExtents.x, 0.5f);
    EXPECT_FLOAT_EQ(lb.Box.halfExtents.y, 0.5f);
    EXPECT_FLOAT_EQ(lb.Box.halfExtents.z, 0.5f);
}

TEST_F(PrimitiveRegistrationTest, LocalBounds_RegistryPathReadsActualMeshBounds)
{
    // After RegisterAll, the registry-aware factory path returns bounds
    // computed from the actual generated geometry (via MeshGPUEntry.bounds).
    // This decouples the factory from hard-coded parameter assumptions.
    PrimitiveGenerator::RegisterAll(*m_rs);

    Components::LocalBounds cube =
        PrimitiveGenerator::MakePrimitiveLocalBounds(m_rs.get(), PrimitiveGenerator::CubeGuid());
    // Unit cube: tight AABB matches the generator (-0.5..0.5 on each axis).
    EXPECT_NEAR(cube.Box.halfExtents.x, 0.5f, 1e-6f);
    EXPECT_NEAR(cube.Box.halfExtents.y, 0.5f, 1e-6f);
    EXPECT_NEAR(cube.Box.halfExtents.z, 0.5f, 1e-6f);
    EXPECT_NEAR(cube.Box.center.x, 0.0f, 1e-6f);
    EXPECT_NEAR(cube.Box.center.y, 0.0f, 1e-6f);
    EXPECT_NEAR(cube.Box.center.z, 0.0f, 1e-6f);

    Components::LocalBounds plane =
        PrimitiveGenerator::MakePrimitiveLocalBounds(m_rs.get(), PrimitiveGenerator::PlaneGuid());
    // Plane is flat in XZ.
    EXPECT_NEAR(plane.Box.halfExtents.x, 0.5f, 1e-6f);
    EXPECT_NEAR(plane.Box.halfExtents.y, 0.0f, 1e-6f);
    EXPECT_NEAR(plane.Box.halfExtents.z, 0.5f, 1e-6f);

    Components::LocalBounds planeSp =
        PrimitiveGenerator::MakePrimitiveLocalBounds(m_rs.get(), PrimitiveGenerator::PlaneSpriteUvGuid());
    EXPECT_NEAR(planeSp.Box.halfExtents.x, plane.Box.halfExtents.x, 1e-6f);
    EXPECT_NEAR(planeSp.Box.halfExtents.y, plane.Box.halfExtents.y, 1e-6f);
    EXPECT_NEAR(planeSp.Box.halfExtents.z, plane.Box.halfExtents.z, 1e-6f);

    // Sphere: radius 0.5 inscribes a unit cube — registry-derived
    // AABB should match the hard-coded fallback.
    Components::LocalBounds sphere =
        PrimitiveGenerator::MakePrimitiveLocalBounds(m_rs.get(), PrimitiveGenerator::SphereGuid());
    EXPECT_NEAR(sphere.Box.halfExtents.x, 0.5f, 1e-6f);
    EXPECT_NEAR(sphere.Box.halfExtents.y, 0.5f, 1e-6f);
    EXPECT_NEAR(sphere.Box.halfExtents.z, 0.5f, 1e-6f);

    // Capsule: total height 2 (cylinder length 1.0 + 2 hemispheres of
    // radius 0.5), width 1. Registry-derived bounds should agree with
    // the hard-coded fallback for the default-parameter capsule.
    Components::LocalBounds capsule =
        PrimitiveGenerator::MakePrimitiveLocalBounds(m_rs.get(), PrimitiveGenerator::CapsuleGuid());
    EXPECT_NEAR(capsule.Box.halfExtents.x, 0.5f, 1e-6f);
    EXPECT_NEAR(capsule.Box.halfExtents.y, 1.0f, 1e-6f);
    EXPECT_NEAR(capsule.Box.halfExtents.z, 0.5f, 1e-6f);
}

// ---- Built-in material names (inspector asset-field display) ----

// The default scene's Sphere carries DefaultMaterialGuid() and SphereGuid().
// Neither is a registry asset, so an inspector asset field's metadata lookup
// misses both by design and used to fall back to a truncated GUID — the row read
// "9aae4628-ba39..." with no way to tell what was assigned. These resolvers are
// what let the field name them instead.
TEST(PrimitiveGeneratorTest, BuiltInMaterialGuidsResolveToNames)
{
    EXPECT_EQ(PrimitiveGenerator::MaterialNameFromGuid(PrimitiveGenerator::DefaultMaterialGuid()),
              "Default Material");
    EXPECT_EQ(PrimitiveGenerator::MaterialNameFromGuid(
                  PrimitiveGenerator::ReflectionProbeTestMaterialGuid()),
              "Reflection Probe Test Material");
}

TEST(PrimitiveGeneratorTest, UnknownGuidHasNoBuiltInMaterialName)
{
    EXPECT_TRUE(PrimitiveGenerator::MaterialNameFromGuid(GUID::Generate()).empty());
    EXPECT_TRUE(PrimitiveGenerator::MaterialNameFromGuid(GUID::Null()).empty());
}

// Mutation guard: the material lookup must stay OUT of the primitive-mesh table.
// Folding it into NameFromGuid would make IsPrimitive() — which means "is this a
// primitive MESH" — answer true for a material GUID.
TEST(PrimitiveGeneratorTest, BuiltInMaterialsAreNotPrimitiveMeshes)
{
    EXPECT_FALSE(PrimitiveGenerator::IsPrimitive(PrimitiveGenerator::DefaultMaterialGuid()));
    EXPECT_FALSE(PrimitiveGenerator::IsPrimitive(
        PrimitiveGenerator::ReflectionProbeTestMaterialGuid()));
    EXPECT_TRUE(PrimitiveGenerator::NameFromGuid(PrimitiveGenerator::DefaultMaterialGuid()).empty());

    // And a primitive mesh has no built-in MATERIAL name.
    EXPECT_TRUE(PrimitiveGenerator::MaterialNameFromGuid(PrimitiveGenerator::SphereGuid()).empty());
    EXPECT_TRUE(PrimitiveGenerator::IsPrimitive(PrimitiveGenerator::SphereGuid()));
}
