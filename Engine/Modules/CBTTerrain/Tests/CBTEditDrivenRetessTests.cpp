// Edit-driven retessellation on a planet: the real GPU update kernels (RecordUpdate, screen-space
// Classify) over a planet whose sculpt reaches VertexEval through its height pages (the paged resolve,
// bindings 22 and 23). The test builds the page table and the page cache itself: one level of pages,
// a shared zero page, and a page for each block of the sculpt, as the height pages hold a layer's
// deltas over a zero base.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <vector>

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTTerrain/CBTSphereFaceMap.h"
#include "CBTReliefHeightSource.h"
#include "CBTTestHarness.h"
#include "Mathematics/CubeFaces.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;
using GameEngine::Mathematics::Vector3;

namespace
{
constexpr double kScreenW = 1600.0;
constexpr double kScreenH = 900.0;
constexpr double kSplitPx = 8.0; // TargetPixelError analog (matches CBTDemandAuditTests)
constexpr double kMergePx = 4.0;

// A field of steep bumps inside a spherical cap: kBumpRiseM tall, one bump every 2 pi / kBumpFrequency
// radians along each tangent axis (about 42 m at R = 2000).
constexpr float kBumpCapAngle = 0.05f; // rad
constexpr float kBumpRiseM = 40.0f;
constexpr float kBumpFrequency = 300.0f; // rad^-1
constexpr float kBumpSharpness = 4.0f;   // the exponent on the cosine lobe

struct BumpField
{
    Vector3 Center;
    Vector3 AxisU;
    Vector3 AxisV;
    float CapCos = 1.0f;
};

float BumpFieldHeight(const BumpField& field, const Vector3& point)
{
    const float length = std::sqrt(Vector3::Dot(point, point));
    if (length <= 0.0f || Vector3::Dot(point, field.Center) / length < field.CapCos)
        return 0.0f;
    const float alongU = Vector3::Dot(point, field.AxisU) / length;
    const float alongV = Vector3::Dot(point, field.AxisV) / length;
    const float lobe = std::max(0.0f, std::cos(kBumpFrequency * alongU) * std::cos(kBumpFrequency * alongV));
    return kBumpRiseM * std::pow(lobe, kBumpSharpness);
}

// The planet's height pages (cbt_page.glsl): kPlanetFaceSamples level-0 samples a face side, one level,
// pages of kPageOwned samples and a stride of kPageStride with the one-sample apron.
constexpr uint32_t kPlanetFaceSamples = 3169u; // PlanetFaceSamples of a 2000 m planet at one sample a meter
constexpr uint32_t kPageOwned = 128u;          // mirror PageStreaming::kPageOwnedSamples
constexpr uint32_t kPageStride = 130u;         // mirror PageStreaming::kPageStrideSamples
constexpr uint32_t kPagesPerAxis = (kPlanetFaceSamples - 1u + kPageOwned - 1u) / kPageOwned;
constexpr uint32_t kPageSampleCount = kPageStride * kPageStride;

// The cube coordinate of a level-0 sample along a face axis: the sample sits at the equal-angle
// coordinate (2 sample - (samples - 1)) / (samples - 1). Apron samples (-1, samples) are valid input.
double PlanetSampleCubeCoordinate(int64_t sample)
{
    const double last = static_cast<double>(kPlanetFaceSamples - 1u);
    return std::tan((2.0 * static_cast<double>(sample) - last) / last * 0.25 * std::numbers::pi);
}

// The unit direction of level-0 sample (x, z) of `face`, each face axis along +1 on its world axis.
Vector3 PlanetSampleDirection(uint32_t face, int64_t x, int64_t z)
{
    const GameEngine::Mathematics::CubeFaceAxes& axes = GameEngine::Mathematics::kCubeFaceAxes[face];
    Vector3 cube;
    cube[axes.NormalAxis] = static_cast<float>(axes.NormalSign);
    cube[axes.XAxis] = static_cast<float>(PlanetSampleCubeCoordinate(x));
    cube[axes.ZAxis] = static_cast<float>(PlanetSampleCubeCoordinate(z));
    return cube.Normalize();
}

// One page table (CBTLayout.h kCBTPageHeaderWords) and its cache: slot 0 holds zeros and every page the
// bumps do not reach names it; each page the bumps reach has its own slot. Every slot is settled.
struct PlanetPageFeed
{
    std::vector<uint32_t> Words;
    std::vector<float> Cache;
    uint32_t CacheDim = 0;
    uint32_t BumpPages = 0;
};

PlanetPageFeed BuildPlanetPageFeed(const BumpField& bumps)
{
    std::vector<std::vector<float>> slots(1u, std::vector<float>(kPageSampleCount, 0.0f));
    std::vector<uint32_t> entries(GameEngine::Mathematics::kCubeFaceCount * kPagesPerAxis * kPagesPerAxis, 0u);
    // A page more than this far from the cap center holds no bump: the cap plus a page's half diagonal.
    const float reachCos = std::cos(kBumpCapAngle + 0.1f);
    for (uint32_t face = 0; face < GameEngine::Mathematics::kCubeFaceCount; ++face)
        for (uint32_t pageZ = 0; pageZ < kPagesPerAxis; ++pageZ)
            for (uint32_t pageX = 0; pageX < kPagesPerAxis; ++pageX)
            {
                const int64_t firstX = static_cast<int64_t>(pageX * kPageOwned) - 1;
                const int64_t firstZ = static_cast<int64_t>(pageZ * kPageOwned) - 1;
                const Vector3 middle = PlanetSampleDirection(face, firstX + kPageStride / 2u, firstZ + kPageStride / 2u);
                if (Vector3::Dot(middle, bumps.Center) < reachCos)
                    continue;
                std::vector<float> page(kPageSampleCount, 0.0f);
                bool any = false;
                for (uint32_t z = 0; z < kPageStride; ++z)
                    for (uint32_t x = 0; x < kPageStride; ++x)
                    {
                        const float height = BumpFieldHeight(bumps, PlanetSampleDirection(face, firstX + x, firstZ + z));
                        page[z * kPageStride + x] = height;
                        any = any || height != 0.0f;
                    }
                if (!any)
                    continue;
                entries[(face * kPagesPerAxis + pageZ) * kPagesPerAxis + pageX] = static_cast<uint32_t>(slots.size());
                slots.push_back(std::move(page));
            }

    PlanetPageFeed feed;
    feed.BumpPages = static_cast<uint32_t>(slots.size()) - 1u;
    const uint32_t slotCount = static_cast<uint32_t>(slots.size());
    const uint32_t slotsPerRow = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(slotCount))));
    feed.CacheDim = slotsPerRow * kPageStride;
    feed.Cache.assign(static_cast<size_t>(feed.CacheDim) * feed.CacheDim, 0.0f);
    for (uint32_t slot = 0; slot < slotCount; ++slot)
    {
        const uint32_t originX = (slot % slotsPerRow) * kPageStride;
        const uint32_t originZ = (slot / slotsPerRow) * kPageStride;
        for (uint32_t z = 0; z < kPageStride; ++z)
            std::copy_n(slots[slot].begin() + z * kPageStride, kPageStride,
                        feed.Cache.begin() + static_cast<size_t>(originZ + z) * feed.CacheDim + originX);
    }

    feed.Words.assign(kCBTPageFadeOffsetWords + slotCount + entries.size(), 0u);
    feed.Words[0] = kPlanetFaceSamples;
    feed.Words[1] = kPlanetFaceSamples;
    feed.Words[2] = 1u; // level count
    feed.Words[3] = slotsPerRow;
    feed.Words[4] = slotCount;
    feed.Words[kCBTPageHeaderWords + 0u] = 0u; // level 0's first entry
    feed.Words[kCBTPageHeaderWords + 1u] = kPagesPerAxis;
    feed.Words[kCBTPageHeaderWords + 2u] = kPagesPerAxis;
    std::fill_n(feed.Words.begin() + kCBTPageFadeOffsetWords, slotCount, std::bit_cast<uint32_t>(1.0f));
    std::copy(entries.begin(), entries.end(), feed.Words.begin() + kCBTPageFadeOffsetWords + slotCount);
    return feed;
}

// Screen-space frame parameters for a sphere with no closed-form relief, seen from eye toward target,
// with the editor's demand shaping (the near bias and the demand tuning of CBTRenderFeature BuildFrameParams).
CBTFrameParams MakeSphereEditFrameParams(float radius, uint32_t cap, const Vector3& eye, const Vector3& target,
                                         const Vector3& up, float nearZ, float farZ)
{
    using namespace GameEngine::Mathematics;
    CBTFrameParams params{};
    params.PlanetParams[0] = radius;
    params.PlanetParams[1] = 0.0f; // amplitude 0: the corner height is the page height alone
    params.PlanetParams[2] = 6.0f;
    params.PlanetParams[3] = 1.0f;
    params.Screen[0] = static_cast<float>(kScreenW);
    params.Screen[1] = static_cast<float>(kScreenH);
    params.Screen[2] = static_cast<float>(kSplitPx);
    params.Screen[3] = static_cast<float>(kMergePx);
    params.TerrainOrigin[2] = static_cast<float>(cap);

    const Matrix4x4 projection = MakePerspectiveLH_ZO_ReverseZ(1.1f, 16.0f / 9.0f, nearZ, farZ);
    const Matrix4x4 viewProj = projection * MakeLookAtLH(eye, target, up);
    std::memcpy(params.ViewProjRel, &viewProj.GetGLM()[0][0], sizeof(params.ViewProjRel));
    params.CameraPos[0] = eye.x;
    params.CameraPos[1] = eye.y;
    params.CameraPos[2] = eye.z;
    params.CameraPos[3] = 1.0f;

    const float cameraDistance = std::sqrt(Vector3::Dot(eye, eye));
    const CBTNearBiasRadii nearBias = ComputeNearBiasRadii(std::max(cameraDistance - radius, 1.0f));
    params.NearBias[0] = 1.0f;
    params.NearBias[1] = nearBias.NearRadius;
    params.NearBias[2] = nearBias.FarRadius;
    params.NearBias[3] = kNearBiasMaxCoarsen;
    params.DemandTuning[0] = 0.5f;
    params.DemandTuning[1] = 0.9f;
    params.DemandTuning[2] = 3.0f * static_cast<float>(kSplitPx);
    params.DemandTuning[3] = 0.9f;
    return params;
}
} // namespace

class CBTEditDrivenRetess : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = MakeHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "no headless Vulkan device";
        if (!m_Device->GetCapabilities().supportsShaderInt64)
            GTEST_SKIP() << "device lacks shaderInt64";
        if (!m_KernelSet.Initialize(*m_Device, ShaderOutputDir()))
            GTEST_SKIP() << "cbt_kernels.comp.spv missing (glslc unavailable at build)";
    }
    void TearDown() override
    {
        m_KernelSet.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    void RunFrame(CBTInstance& inst, const CBTClassifyDesc& desc, const CBTFrameParams& params, uint32_t frame)
    {
        auto commandList = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commandList->Begin();
        inst.RecordUpdate(*commandList, desc, params, frame);
        commandList->End();
        std::vector<CommandList*> lists{commandList.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    std::vector<uint64_t> ReadHeap(CBTInstance& inst)
    {
        const auto words = inst.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        std::vector<uint64_t> heap(kDefaultBisectorPoolSize);
        for (uint32_t index = 0; index < kDefaultBisectorPoolSize; ++index)
            heap[index] = static_cast<uint64_t>(words[index * 2u]) | (static_cast<uint64_t>(words[index * 2u + 1u]) << 32);
        return heap;
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// A still camera over a sculpted bump field settles to one tree. A split whose new midpoint lands on a
// bump flank can put both children under the area merge bound while the parent stays over the split
// bound; the merge reads the parent, so the tree holds.
TEST_F(CBTEditDrivenRetess, StationaryEditSettlesToOneTree)
{
    using namespace GameEngine::Mathematics;

    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));

    const float radius = 2000.0f;
    const uint32_t cap = inst.GetBaseDepth() + kMaxDecodeSubdiv;

    // A low oblique look across the bump field, so the bump flanks are seen near edge-on.
    const Vector3 dir = Vector3(0.03f, 0.0f, 1.0f).Normalize();
    const Vector3 surface = dir * radius;
    const Vector3 tangent = Vector3::Cross(dir, Vector3(0, 1, 0)).Normalize();
    const float altitude = 40.0f;
    const float standoff = 300.0f;
    const Vector3 eye = surface + dir * altitude - tangent * standoff;
    CBTFrameParams params = MakeSphereEditFrameParams(radius, cap, eye, surface, dir, 0.05f, 200000.0f);

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = cap;
    for (uint32_t frame = 0; frame < 90u; ++frame)
    {
        classify.GateVertexEval = (frame == 0u) ? 0u : 1u;
        RunFrame(inst, classify, params, frame);
    }

    // The edit lands: the planet's pages carry the bumps, every corner re-evaluates once, and Classify
    // takes the edit's face rects for this one update.
    const BumpField bumps{dir, tangent, Vector3::Cross(dir, tangent).Normalize(), std::cos(kBumpCapAngle)};
    const PlanetPageFeed feed = BuildPlanetPageFeed(bumps);
    ASSERT_GT(feed.BumpPages, 0u);
    const TextureHandle cache = MakeR32Texture(*m_Device, feed.CacheDim, feed.CacheDim, feed.Cache, "CBT.Test.PlanetPages");
    ASSERT_TRUE(cache.IsValid());
    BufferDesc tableDesc{};
    tableDesc.size = feed.Words.size() * sizeof(uint32_t);
    tableDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    tableDesc.memoryUsage = BufferMemoryUsage::Upload;
    tableDesc.debugName = "CBT.Test.PlanetPageTable";
    const BufferHandle table = m_Device->CreateBuffer(tableDesc);
    ASSERT_TRUE(table.IsValid());
    m_Device->UpdateBuffer(table, 0u, tableDesc.size, feed.Words.data());
    for (uint32_t ringSlot = 0; ringSlot < kCBTFrameParamsRing; ++ringSlot)
        inst.SetPageCacheSource(ringSlot, cache);
    inst.SetPageTable(table, 0u);
    params.AtlasParams1[2] = 2.0f; // the paged resolve (CBTRenderFeature::BuildFrameParams)

    SphereEditRegions regions = ClassifySphereCapEdit(dir.x, dir.y, dir.z, kBumpCapAngle);
    ASSERT_GT(regions.Count, 0u);
    classify.GateVertexEval = 0u;
    classify.SphereDirty = regions;
    RunFrame(inst, classify, params, 90u);
    EXPECT_EQ(inst.ReadWorkQueueCounter(kWQUnresolvedPageSamples), 0) << "every corner resolves on a page";

    // The edit converges well inside kSettleFrames still updates; the held updates after it serve nothing.
    constexpr uint32_t kSettleFrames = 60u;
    constexpr uint32_t kHeldFrames = 8u;
    classify.GateVertexEval = 1u;
    classify.SphereDirty = {};
    for (uint32_t frame = 0; frame < kSettleFrames; ++frame)
        RunFrame(inst, classify, params, 91u + frame);

    std::vector<uint64_t> settled = ReadHeap(inst);
    std::sort(settled.begin(), settled.end());
    uint32_t churnFrames = 0;
    for (uint32_t frame = 0; frame < kHeldFrames; ++frame)
    {
        RunFrame(inst, classify, params, 91u + kSettleFrames + frame);
        const int32_t splits = inst.ReadWorkQueueCounter(kWQAllocateCounter);
        const int32_t merges = inst.ReadWorkQueueCounter(kWQSimplifyCounter);
        std::vector<uint64_t> heap = ReadHeap(inst);
        std::sort(heap.begin(), heap.end());
        const bool sameTree = heap == settled;
        std::printf("[edit-settle] held update %u: splitServed=%d mergeServed=%d sameTree=%d\n", frame, splits,
                    merges, sameTree ? 1 : 0);
        if (splits != 0 || merges != 0 || !sameTree)
            ++churnFrames;
    }
    EXPECT_EQ(churnFrames, 0u) << "a still camera over a converged edit must hold one tree";
    EXPECT_EQ(inst.ReadValidationErrorCount(), 0u);

    inst.SetPageTable({}, 0u);
    for (uint32_t ringSlot = 0; ringSlot < kCBTFrameParamsRing; ++ringSlot)
        inst.SetPageCacheSource(ringSlot, {});
    m_Device->DestroyBuffer(table);
    m_Device->DestroyTexture(cache);
}
