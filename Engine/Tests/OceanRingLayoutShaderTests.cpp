#include <gtest/gtest.h>

#include "Mathematics/Vector2.h"
#include "Mathematics/Vector4.h"
#include "Ocean/OceanTypes.h"
#include "OceanRingMesh.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Ocean;

namespace
{
// Mirrors OceanRingProbeCase / the result vec4 in ocean_ring_layout_probe.comp.
struct ProbeCase
{
    Mathematics::Vector4 GridData;
    Mathematics::Vector4 Camera;
    Mathematics::Vector4 Rings;
};
static_assert(sizeof(ProbeCase) == 48);

struct ProbeResult
{
    float X = 0.0f;
    float Z = 0.0f;
    float Footprint = 0.0f;
    float GridWidth = 0.0f;
};
static_assert(sizeof(ProbeResult) == 16);

constexpr uint32_t kProbeGroupSize = 64;
constexpr float kSeaLevel = 0.0f;
constexpr float kCameraX = 3.3f;
constexpr float kCameraZ = -7.7f;
// The root tile at the default patch extent (patch extent * 5 / 2^8), and the
// smallest root tile the layout allows (GE_OCEAN_RING_MIN_ROOT_SCALE).
constexpr float kBaseRootScale = kDefaultPatchExtent * 5.0f / 256.0f;
constexpr float kMinRootScale = 20.0f;
// Index of the outermost LOD (GE_OCEAN_GEOMETRY_LOD_COUNT - 1); its outer edge is the skirt.
constexpr float kOutermostLod = 7.0f;
constexpr float kMorphTolerance = 1.0e-4f;
// Cameras at several snap phases, and heights on both sides of the 40 m and 80 m resizes.
const Mathematics::Vector2 kEdgeCamerasXZ[] = {{0.0f, -26.0f}, {0.0f, -7.7f}, {3.3f, -7.7f}, {11.9f, 5.6f}};
constexpr float kEdgeHeights[] = {1.7f, 9.0f, 19.9f, 39.9f, 40.0f, 40.3f, 79.9f, 80.0f};
constexpr uint32_t kEdgeCellCounts[] = {8u, 16u, 64u};

// Non-skirt vertices on each ring's outer border (LOD 0 to 6) or on its inner
// border around the hole (LOD 1 to 7). Ring distance is in ring tiles.
std::vector<Mathematics::Vector4> RingEdgeVertices(const OceanRingMesh& mesh, uint32_t cellsPerTile, bool outer)
{
    const float cell = 1.0f / static_cast<float>(cellsPerTile);
    std::vector<Mathematics::Vector4> edge;
    for (const Mathematics::Vector4& vertex : mesh.Vertices)
    {
        if (vertex.w > 1.0f)
            continue;
        const float ringDistance = std::max(std::abs(vertex.x), std::abs(vertex.y)) / std::exp2(vertex.z);
        const bool onEdge = outer ? vertex.z < kOutermostLod && ringDistance >= 2.0f - 1.5f * cell
                                  : vertex.z > 0.5f && ringDistance <= 1.0f + 0.5f * cell;
        if (onEdge)
            edge.push_back(vertex);
    }
    return edge;
}

// Footprint of the vertex under the camera: (1 + morph) * root / cells, with
// root = 80 m * 2^floor(level), morph = fract(level), level = log2(h / 80 m),
// and level held at log2(20 m / 80 m) or above.
float ExpectedCellUnderCamera(float height, float cellsPerTile)
{
    const float level = std::max(std::log2(height / kBaseRootScale),
                                 std::log2(kMinRootScale / kBaseRootScale));
    const float rootScale = kBaseRootScale * std::exp2(std::floor(level));
    return (1.0f + (level - std::floor(level))) * rootScale / cellsPerTile;
}

class OceanRingLayoutShaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        const auto bytes = Utils::ReadFile(
            (TestPaths::StagedRoot() / "Shaders" / "ocean_ring_layout_probe.comp.spv").string());
        ASSERT_FALSE(bytes.empty()) << "the shader target stages the probe; missing SPIR-V is a build error";

        DescriptorBinding binding{};
        binding.type = DescriptorType::StorageBuffer;
        binding.count = 1;
        binding.shaderStages = kShaderStageCompute;
        DescriptorSetLayoutDesc layout{};
        binding.binding = 0;
        layout.bindings.push_back(binding);
        binding.binding = 1;
        layout.bindings.push_back(binding);
        m_Layout = layout;
        ComputePipelineDesc pipelineDesc{};
        pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(bytes);
        pipelineDesc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipelineDesc));
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (const auto buffer : m_Buffers)
        {
            if (buffer.IsValid())
                m_Device->DestroyBuffer(buffer);
        }
        m_Device->Shutdown();
    }

    BufferHandle MakeBuffer(size_t size, const void* data)
    {
        BufferDesc desc{};
        desc.size = size;
        desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        const BufferHandle buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        void* mapped = buffer.IsValid() ? m_Device->MapBuffer(buffer) : nullptr;
        EXPECT_NE(mapped, nullptr);
        if (mapped)
        {
            std::memcpy(mapped, data, size);
            m_Device->UnmapBuffer(buffer);
        }
        return buffer;
    }

    // Places `vertices` once per edge camera and counts the vertices whose morph
    // (footprint / grid width - 1) is not `expectedMorph`; names the first few.
    uint32_t CountMorphMismatches(const std::vector<Mathematics::Vector4>& vertices,
                                  float cellsPerTile, float expectedMorph)
    {
        std::vector<ProbeCase> cases;
        for (const Mathematics::Vector2& cameraXZ : kEdgeCamerasXZ)
        {
            for (const float height : kEdgeHeights)
            {
                for (const Mathematics::Vector4& vertex : vertices)
                {
                    cases.push_back({vertex, {cameraXZ.x, kSeaLevel + height, cameraXZ.y, kSeaLevel},
                                     {kDefaultPatchExtent, cellsPerTile, 0.0f, 0.0f}});
                }
            }
        }
        const std::vector<ProbeResult> placed = Dispatch(cases);
        uint32_t mismatches = 0;
        for (size_t i = 0; i < placed.size(); ++i)
        {
            const float morph = placed[i].Footprint / placed[i].GridWidth - 1.0f;
            if (std::abs(morph - expectedMorph) <= kMorphTolerance)
                continue;
            if (mismatches++ < 3u)
            {
                const ProbeCase& probe = cases[i];
                ADD_FAILURE() << "camera (" << probe.Camera.x << ", " << probe.Camera.y << ", " << probe.Camera.z
                              << ") lod=" << probe.GridData.z << " local=(" << probe.GridData.x << ", "
                              << probe.GridData.y << ") morph=" << morph << ", expected " << expectedMorph;
            }
        }
        return mismatches;
    }

    // Places every ring mesh vertex with the shipped layout for one camera height.
    std::vector<ProbeResult> Place(const OceanRingMesh& mesh, float height, float cellsPerTile)
    {
        const size_t count = mesh.Vertices.size();
        std::vector<ProbeCase> cases(count);
        for (size_t i = 0; i < count; ++i)
        {
            cases[i].GridData = mesh.Vertices[i];
            cases[i].Camera = {kCameraX, kSeaLevel + height, kCameraZ, kSeaLevel};
            cases[i].Rings = {kDefaultPatchExtent, cellsPerTile, 0.0f, 0.0f};
        }
        return Dispatch(cases);
    }

    // Runs the shipped layout once per case on the GPU.
    std::vector<ProbeResult> Dispatch(const std::vector<ProbeCase>& cases)
    {
        const size_t count = cases.size();
        std::vector<ProbeResult> results(count);
        const BufferHandle input = MakeBuffer(cases.size() * sizeof(ProbeCase), cases.data());
        const BufferHandle output = MakeBuffer(results.size() * sizeof(ProbeResult), results.data());
        DescriptorSetDesc descriptor{};
        descriptor.layout = m_Layout;
        const auto set = m_Device->CreateDescriptorSet(descriptor);
        EXPECT_TRUE(set.IsValid());
        m_Device->UpdateStorageBufferBinding(set, 0, input, 0, cases.size() * sizeof(ProbeCase));
        m_Device->UpdateStorageBufferBinding(set, 1, output, 0, results.size() * sizeof(ProbeResult));
        auto command = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        command->Begin();
        command->SetPipeline(m_Pipeline);
        command->BindDescriptorSet(0, set, m_Pipeline);
        command->Dispatch(static_cast<uint32_t>((count + kProbeGroupSize - 1u) / kProbeGroupSize), 1, 1);
        command->End();
        m_Device->ExecuteCommandLists({command.get()});
        m_Device->WaitForIdle();
        const void* mapped = m_Device->MapBuffer(output);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
        {
            std::memcpy(results.data(), mapped, results.size() * sizeof(ProbeResult));
            m_Device->UnmapBuffer(output);
        }
        return results;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    std::vector<BufferHandle> m_Buffers;
};

int64_t CellIndex(float coordinate, float cellSize)
{
    return static_cast<int64_t>(std::floor(static_cast<double>(coordinate) / cellSize));
}

int64_t CellKey(int64_t cx, int64_t cz)
{
    return (cx << 32) ^ (cz & 0xffffffffll);
}

// Largest gap from a vertex in `from` (inside `radius` of the camera, horizon
// skirt excluded) to the nearest vertex of `to` with a footprint within 1 percent.
float LargestUnmatchedGap(const std::vector<ProbeResult>& from, const std::vector<ProbeResult>& to,
                          const OceanRingMesh& mesh, float radius, float tolerance)
{
    std::unordered_multimap<int64_t, size_t> grid;
    grid.reserve(to.size());
    for (size_t i = 0; i < to.size(); ++i)
        grid.emplace(CellKey(CellIndex(to[i].X, tolerance), CellIndex(to[i].Z, tolerance)), i);

    float largestGap = 0.0f;
    uint32_t reported = 0;
    for (size_t i = 0; i < from.size(); ++i)
    {
        const ProbeResult& vertex = from[i];
        const bool skirt = mesh.Vertices[i].w > 1.0f;
        if (skirt || std::max(std::abs(vertex.X - kCameraX), std::abs(vertex.Z - kCameraZ)) > radius)
            continue;
        const int64_t cx = CellIndex(vertex.X, tolerance);
        const int64_t cz = CellIndex(vertex.Z, tolerance);
        float nearest = 2.0f * tolerance;
        for (int64_t dz = -1; dz <= 1; ++dz)
        {
            for (int64_t dx = -1; dx <= 1; ++dx)
            {
                const auto range = grid.equal_range(CellKey(cx + dx, cz + dz));
                for (auto it = range.first; it != range.second; ++it)
                {
                    const ProbeResult& other = to[it->second];
                    if (std::abs(other.Footprint - vertex.Footprint) > 0.01f * vertex.Footprint)
                        continue;
                    nearest = std::min(nearest, std::hypot(other.X - vertex.X, other.Z - vertex.Z));
                }
            }
        }
        // Names the first few unmatched vertices so a failure says where the layout moved.
        if (nearest > tolerance && reported++ < 3u)
        {
            ADD_FAILURE() << "unmatched vertex " << i << " lod=" << mesh.Vertices[i].z
                          << " local=(" << mesh.Vertices[i].x << ", " << mesh.Vertices[i].y
                          << ") world=(" << vertex.X << ", " << vertex.Z << ") footprint=" << vertex.Footprint;
        }
        largestGap = std::max(largestGap, nearest);
    }
    return largestGap;
}
} // namespace

TEST_F(OceanRingLayoutShaderTest, CellUnderTheCameraIsFineNearTheSeaAndTracksHeightAbove)
{
    const float cells = static_cast<float>(kDefaultOceanRingCellsPerTile);
    const OceanRingMesh mesh = BuildOceanRingMesh(kDefaultOceanRingCellsPerTile);
    // Heights bracket the octave boundaries at 20, 40, 80 and 160 m.
    for (const float height : {1.7f, 10.0f, 19.9f, 20.1f, 39.9f, 40.1f, 79.9f, 80.1f, 159.9f, 160.1f, 300.0f})
    {
        SCOPED_TRACE(::testing::Message() << "height=" << height);
        const std::vector<ProbeResult> placed = Place(mesh, height, cells);
        ASSERT_FALSE(placed.empty());
        const auto nearest = std::min_element(placed.begin(), placed.end(),
            [](const ProbeResult& a, const ProbeResult& b) {
                return std::hypot(a.X - kCameraX, a.Z - kCameraZ) < std::hypot(b.X - kCameraX, b.Z - kCameraZ);
            });
        EXPECT_NEAR(nearest->Footprint, ExpectedCellUnderCamera(height, cells),
                    1.0e-3f * ExpectedCellUnderCamera(height, cells));
    }
    // The default density: 20 m / 24 cells under a camera up to 20 m above the sea.
    EXPECT_FLOAT_EQ(ExpectedCellUnderCamera(1.7f, cells), 20.0f / 24.0f);
}

TEST_F(OceanRingLayoutShaderTest, RaisingTheCameraAcrossAResizeMovesNoVertex)
{
    const float cells = static_cast<float>(kDefaultOceanRingCellsPerTile);
    const OceanRingMesh mesh = BuildOceanRingMesh(kDefaultOceanRingCellsPerTile);
    constexpr float kHeightStep = 1.0e-3f;
    constexpr float kMaxGap = 0.01f;
    for (const float height : {20.0f, 40.0f, 80.0f, 160.0f, 320.0f})
    {
        SCOPED_TRACE(::testing::Message() << "resize height=" << height);
        const std::vector<ProbeResult> below = Place(mesh, height - kHeightStep, cells);
        const std::vector<ProbeResult> above = Place(mesh, height + kHeightStep, cells);
        // Compare inside the outer ring; the horizon skirt moves out at each octave.
        const float radius = 200.0f * std::max(height * 0.5f, kMinRootScale);
        EXPECT_LE(LargestUnmatchedGap(below, above, mesh, radius, kMaxGap), kMaxGap);
        EXPECT_LE(LargestUnmatchedGap(above, below, mesh, radius, kMaxGap), kMaxGap);
    }
}

TEST_F(OceanRingLayoutShaderTest, HeightAtAnExactPowerOfTwoKeepsItsOwnRootTile)
{
    const float cells = static_cast<float>(kDefaultOceanRingCellsPerTile);
    // Patch extents whose root tile divides awkwardly, and heights at an exact
    // power of two of that root tile: the root tile there equals the height.
    std::vector<ProbeCase> cases;
    std::vector<float> expectedRoots;
    for (uint32_t extentIndex = 0; extentIndex < 64u; ++extentIndex)
    {
        const float patchExtent = 1000.0f + 97.3f * static_cast<float>(extentIndex);
        const float baseRootScale = patchExtent * 5.0f / 256.0f;
        for (int32_t octave = -3; octave <= 6; ++octave)
        {
            const float height = std::ldexp(baseRootScale, octave);
            if (height < kMinRootScale)
                continue;
            ProbeCase probe{};
            probe.GridData = {0.0f, 0.0f, 0.0f, 1.0f};
            probe.Camera = {kCameraX, kSeaLevel + height, kCameraZ, kSeaLevel};
            probe.Rings = {patchExtent, cells, 0.0f, 0.0f};
            cases.push_back(probe);
            expectedRoots.push_back(height);
        }
    }
    const std::vector<ProbeResult> placed = Dispatch(cases);
    ASSERT_EQ(placed.size(), cases.size());
    uint32_t halved = 0;
    for (size_t i = 0; i < placed.size(); ++i)
    {
        const float root = placed[i].GridWidth * cells;
        if (std::abs(root - expectedRoots[i]) > 1.0e-4f * expectedRoots[i] && halved++ < 3u)
        {
            ADD_FAILURE() << "patch extent " << cases[i].Rings.x << " height " << cases[i].Camera.y
                          << ": root tile " << root << ", expected " << expectedRoots[i];
        }
    }
    EXPECT_EQ(halved, 0u) << "of " << placed.size() << " exact power-of-two heights";
}

TEST_F(OceanRingLayoutShaderTest, RingOuterEdgeIsFullyMorphedOntoTheCoarserRing)
{
    // A fully morphed outer edge lies on the next ring's grid, so the rings share vertices.
    for (const uint32_t cellsPerTile : kEdgeCellCounts)
    {
        SCOPED_TRACE(::testing::Message() << "cells per tile=" << cellsPerTile);
        const std::vector<Mathematics::Vector4> edge =
            RingEdgeVertices(BuildOceanRingMesh(cellsPerTile), cellsPerTile, true);
        ASSERT_FALSE(edge.empty());
        EXPECT_EQ(CountMorphMismatches(edge, static_cast<float>(cellsPerTile), 1.0f), 0u);
    }
}

TEST_F(OceanRingLayoutShaderTest, RingInnerEdgeKeepsItsOwnGrid)
{
    // An unmorphed inner edge lies on the grid of the finer ring's fully morphed outer edge.
    for (const uint32_t cellsPerTile : kEdgeCellCounts)
    {
        SCOPED_TRACE(::testing::Message() << "cells per tile=" << cellsPerTile);
        const std::vector<Mathematics::Vector4> edge =
            RingEdgeVertices(BuildOceanRingMesh(cellsPerTile), cellsPerTile, false);
        ASSERT_FALSE(edge.empty());
        EXPECT_EQ(CountMorphMismatches(edge, static_cast<float>(cellsPerTile), 0.0f), 0u);
    }
}
