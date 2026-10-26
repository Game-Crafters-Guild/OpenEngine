// The terrain draw's prefiltered height chain (cbt_height_chain.glsl; #2651). The draw moves a far
// vertex to the chain level its distance asks for, so far facets show the surface at the detail the
// screen can show instead of aliasing the level-0 relief.
//
// The shared block of cbt_height_chain.glsl is extracted at build time (Tests/ExtractShaderBlock.cmake)
// and compiled here through GlslShim.h, so these tests run the shader's own level rule, addressing and
// blend. The real-device arms build the chain with the shipped kernels and read it back.
//
//   NearVerticesKeepTheirCachedCornerBits — below lambda 0.75 a vertex draws its cached level-0 height,
//     the same bits, so the near field draws what it drew without the chain.
//   ChainMatchesTheTentOracleAtThreeLevels — levels 1 to 3 of the GPU chain over a non-square lattice
//     equal the page store's filter ([1 2 1] / 4 around 2j, edges clamped, PagePyramidBuilder's order)
//     restated on the CPU.
//   EditRectRebuildsOnlyTheSamplesItsTentsReach — an edit announced by its rect rewrites the samples
//     whose tents read the edited texels, at every level, and leaves the rest as they were: a change
//     outside the rect that was not announced stays out of the chain.
//   FacetDrawnIntoTheFrustumIsKept — a facet whose cached corners lie outside the frustum but whose drawn
//     triangle reaches into it stays in the VISIBLE stream; facets that are out either way stay culled.
//   NormalChainMatchesTheChainSurfaceAtThreeMips — mips 0 to 2 of the far-field normal chain hold the
//     normal of chain levels 1 to 3 at each texel's center (the central difference across one level
//     sample each way, at world scale), restated on the CPU from the GPU's own height chain.
//   FragmentsTakeTheNormalAtTheirOwnLevel — the surface's normal weight: the level-0 map alone up to
//     lambda 1.75 (the near field unchanged, and level 1's ring), the chain alone from lambda 2 at mip
//     lambda - 1.
//   GrazingFootprintEndsARidgeRowWithoutAStep — along screen rows over a ridge field at a grazing
//     angle, the shading moves from the ridge faces to the averaged far surface over a distance band:
//     no two neighboring pixels differ by more than the critics' edge step (40 luma levels), while the
//     near field keeps the level-0 map and the far field reads the whole footprint.
//   SharedVertexDrawsOneHeight — every copy of a vertex in a converged tree, across facets of different
//     depths, draws one height, including vertices in the blend at lambda 1.4 to 1.8. The positive
//     control: a height chosen per facet (its centroid's level) cracks the same tree.
//   AFieldTheChainDrawsFlatDoesNotSplitPastItsDrawnRelief — the planar split's flat veto reads the range
//     of the surface the draw shows: on the stripe field, which every chain level draws flat, a converged
//     tree keeps no diamond of four leaves whose two parents both lie where the draw shows chain level 1
//     or coarser, although the level-0 relief there projects tens of pixels. (A diamond whose twin's child
//     a finer neighbor splits stays: conformity holds it.)

#include <gtest/gtest.h>

#include "GlslShim.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// The tap the block reads the chain through, and the block itself, inside the shim namespace so
// unqualified builtins resolve to the shim. `precise` has no meaning on the host: C++ does not contract.
namespace GameEngine::GlslShim::Shader
{
const std::vector<uint32_t>* g_ChainWords = nullptr;
float CBT_ChainLoad(uint word)
{
    return std::bit_cast<float>((*g_ChainWords)[word]);
}
#define precise
#include "CBTHeightChainExtracted.h"
#undef precise
} // namespace GameEngine::GlslShim::Shader

#include "CBTTerrain/CBTDemandTuning.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTTerrain/CBTResources.h"
#include "CBTTerrain/CBTTreeAudit.h"
#include "CBTDiamond.h"
#include "CBTReliefHeightSource.h"
#include "CBTTestHarness.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Rect.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;
namespace Shader = GameEngine::GlslShim::Shader;

namespace
{
constexpr float kScreenW = 1600.0f;
constexpr float kScreenH = 900.0f;
constexpr float kFovY = 1.05f;
constexpr float kTargetPx = 8.0f * kScreenH / 1080.0f;
constexpr float kTerrainM = 576.0f;
constexpr uint32_t kMaxDepth = 21u;
constexpr uint32_t kConvergeFrames = 90u;

// The draw's level scale for a lattice of `samples` over kTerrainM: t / (f x s0).
float LevelScale(uint32_t samples)
{
    const float focal = 0.5f * kScreenH / std::tan(0.5f * kFovY);
    return kTargetPx / (focal * (kTerrainM / static_cast<float>(samples - 1u)));
}

// ---- The CPU oracle: the page store's tent, PagePyramidBuilder's order of operations ----

float At(const std::vector<float>& level, uint32_t width, uint32_t height, int64_t x, int64_t z)
{
    const auto cx = static_cast<uint32_t>(std::clamp<int64_t>(x, 0, static_cast<int64_t>(width) - 1));
    const auto cz = static_cast<uint32_t>(std::clamp<int64_t>(z, 0, static_cast<int64_t>(height) - 1));
    return level[static_cast<size_t>(cz) * width + cx];
}

float TentAcross(const std::vector<float>& level, uint32_t width, uint32_t height, uint32_t i, int64_t z)
{
    const int64_t c = 2 * static_cast<int64_t>(i);
    return 0.25f * At(level, width, height, c - 1, z) + 0.5f * At(level, width, height, c, z) +
           0.25f * At(level, width, height, c + 1, z);
}

// Levels 0..top of `level0` (samplesX x samplesZ).
std::vector<std::vector<float>> OracleChain(const std::vector<float>& level0, uint32_t samplesX, uint32_t samplesZ)
{
    const uint32_t top = CBTHeightChainTopLevel(samplesX, samplesZ);
    std::vector<std::vector<float>> levels{level0};
    for (uint32_t level = 1; level <= top; ++level)
    {
        const uint32_t fx = CBTHeightChainSamples(samplesX, level - 1u);
        const uint32_t fz = CBTHeightChainSamples(samplesZ, level - 1u);
        const uint32_t sx = CBTHeightChainSamples(samplesX, level);
        const uint32_t sz = CBTHeightChainSamples(samplesZ, level);
        std::vector<float> coarse(static_cast<size_t>(sx) * sz);
        for (uint32_t z = 0; z < sz; ++z)
            for (uint32_t x = 0; x < sx; ++x)
            {
                const int64_t c = 2 * static_cast<int64_t>(z);
                coarse[static_cast<size_t>(z) * sx + x] = 0.25f * TentAcross(levels.back(), fx, fz, x, c - 1) +
                                                         0.5f * TentAcross(levels.back(), fx, fz, x, c) +
                                                         0.25f * TentAcross(levels.back(), fx, fz, x, c + 1);
            }
        levels.push_back(std::move(coarse));
    }
    return levels;
}

// The largest |GPU - oracle| over level `level`'s samples.
float LevelError(const std::vector<uint32_t>& words, const std::vector<std::vector<float>>& oracle, uint32_t samplesX,
                 uint32_t samplesZ, uint32_t level)
{
    const uint32_t base = CBTHeightChainLevelWord(samplesX, samplesZ, level);
    float worst = 0.0f;
    for (size_t i = 0; i < oracle[level].size(); ++i)
        worst = std::max(worst, std::fabs(std::bit_cast<float>(words[base + i]) - oracle[level][i]));
    return worst;
}

std::vector<float> RandomField(uint32_t samplesX, uint32_t samplesZ, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    std::vector<float> d(static_cast<size_t>(samplesX) * samplesZ);
    for (float& v : d)
        v = dist(rng);
    return d;
}

// Ridges at the lattice frequency along X: 1 on even columns, 0 on odd. Every chain level reads 0.5
// away from the edges, so a far facet with corners on even columns draws half its cached height.
std::vector<float> StripeField(uint32_t samples)
{
    std::vector<float> d(static_cast<size_t>(samples) * samples);
    for (uint32_t z = 0; z < samples; ++z)
        for (uint32_t x = 0; x < samples; ++x)
            d[static_cast<size_t>(z) * samples + x] = (x % 2u == 0u) ? 1.0f : 0.0f;
    return d;
}

// The six clip planes of a column-major view-projection (cbt_layout.glsl CBT_TriangleInFrustum).
std::array<std::array<float, 4>, 6> FrustumPlanes(const float* vp)
{
    std::array<std::array<float, 4>, 4> row{};
    for (uint32_t r = 0; r < 4u; ++r)
        for (uint32_t c = 0; c < 4u; ++c)
            row[r][c] = vp[c * 4u + r];
    std::array<std::array<float, 4>, 6> planes{};
    for (uint32_t i = 0; i < 4u; ++i)
    {
        planes[0][i] = row[3][i] + row[0][i];
        planes[1][i] = row[3][i] - row[0][i];
        planes[2][i] = row[3][i] + row[1][i];
        planes[3][i] = row[3][i] - row[1][i];
        planes[4][i] = row[3][i] + row[2][i];
        planes[5][i] = row[3][i] - row[2][i];
    }
    return planes;
}

// The triangle against the frustum, by CBT_TriangleInFrustum's rule (out when all three corners lie
// outside one plane). `margin` (meters along each plane's normal) makes "in" strict and "out" lax, so
// a rounding at a plane cannot decide either way.
bool TriangleIn(const std::array<std::array<float, 4>, 6>& planes, const std::array<std::array<float, 3>, 3>& p,
                float margin)
{
    for (const auto& pl : planes)
    {
        const float len = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
        bool allOut = true;
        for (const auto& v : p)
            allOut = allOut && (pl[0] * v[0] + pl[1] * v[1] + pl[2] * v[2] + pl[3]) < margin * len;
        if (allOut)
            return false;
    }
    return true;
}

uint32_t HeapDepth(uint64_t h)
{
    uint32_t d = 0;
    while (h > 1u)
    {
        h >>= 1u;
        ++d;
    }
    return d;
}

std::array<float, 3> Corner(const CBTVertexData& vd, uint32_t k)
{
    const float* c = k == 0u ? vd.Corner0 : (k == 1u ? vd.Corner1 : vd.Corner2);
    return {c[0], c[1], c[2]};
}

std::array<float, 2> CornerUv(const CBTVertexData& vd, uint32_t k)
{
    const float* c = k == 0u ? vd.Corner0 : (k == 1u ? vd.Corner1 : vd.Corner2);
    return {c[3], vd.Meta[k]};
}

float Distance(const std::array<float, 3>& a, const float* eye)
{
    const float dx = a[0] - eye[0];
    const float dy = a[1] - eye[1];
    const float dz = a[2] - eye[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// A converged tree, its chain and the frame it converged at.
struct StripeScene
{
    CBTFrameParams Params;
    CBTTreeSnapshot Snapshot;
    std::vector<uint32_t> Chain;
};

class CBTHeightChain : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = MakeHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "no headless Vulkan device";
        if (!m_Device->GetCapabilities().supportsShaderInt64)
            GTEST_SKIP() << "device lacks shaderInt64 (the narrow arm builds no chain)";
        if (!m_KernelSet.Initialize(*m_Device, ShaderOutputDir()))
            GTEST_SKIP() << "cbt_kernels.comp.spv missing (glslc unavailable at build)";
    }
    void TearDown() override
    {
        m_KernelSet.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    TextureHandle MakeTexture(const std::vector<float>& data, uint32_t width, uint32_t height)
    {
        return MakeR32Texture(*m_Device, width, height, data, "CBT.Test.ChainHeights");
    }

    // An instance on `tex` with the chain provisioned for the lattice.
    bool Bring(CBTInstance& inst, TextureHandle tex, uint32_t samplesX, uint32_t samplesZ)
    {
        if (!inst.Initialize(*m_Device, m_KernelSet) || !inst.InitializeRoots(kDomainPlanar))
            return false;
        for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
            inst.SetHeightSource(slot, tex);
        return inst.ProvisionHeightChainWords(CBTHeightChainWords(samplesX, samplesZ)) &&
               inst.ProvisionNormalChain(samplesX, samplesZ);
    }

    // The camera at terrain-local `eye`, looking along +Z pitched down `pitch` radians, with the
    // draw's chain on for a lattice of `samples` per axis and `heightScale` meters of relief.
    CBTFrameParams CameraParams(const GameEngine::Mathematics::Vector3& eye, float pitch, uint32_t samples,
                                float heightScale)
    {
        using namespace GameEngine::Mathematics;
        CBTFrameParams p{};
        p.Screen[0] = kScreenW;
        p.Screen[1] = kScreenH;
        p.Screen[2] = kTargetPx;
        p.Screen[3] = kTargetPx * 0.5f;
        p.TerrainSize[0] = kTerrainM;
        p.TerrainSize[1] = kTerrainM;
        p.TerrainSize[2] = heightScale;
        p.TerrainOrigin[2] = static_cast<float>(kMaxDepth);
        const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(kFovY, kScreenW / kScreenH, 0.5f, 20000.0f);
        const Vector3 forward(0.0f, -std::sin(pitch), std::cos(pitch));
        const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + forward, Vector3(0, 1, 0));
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x;
        p.CameraPos[1] = eye.y;
        p.CameraPos[2] = eye.z;
        p.CameraPos[3] = 1.0f;
        const CBTNearBiasRadii nb = ComputeNearBiasRadii(eye.y);
        p.NearBias[0] = 1.0f;
        p.NearBias[1] = nb.NearRadius;
        p.NearBias[2] = nb.FarRadius;
        p.NearBias[3] = kNearBiasMaxCoarsen;
        p.DemandTuning[0] = kNearFieldFacetTargetM;
        p.DemandTuning[1] = kOffFrustumKeepOcc;
        p.WaterPlane[3] = 1.0f;
        p.HeightChain[0] = LevelScale(samples);
        p.HeightChain[1] = static_cast<float>(CBTHeightChainTopLevel(samples, samples));
        return p;
    }

    // `frames` updates at `p`: the first forced (a newly bound texture), the rest gated. When
    // `edit` is set the first is instead gated and announces that UV rect as edited.
    void Run(CBTInstance& inst, const CBTFrameParams& p, uint32_t frames, uint32_t firstFrame,
             const GameEngine::Mathematics::Rect* edit = nullptr)
    {
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = kMaxDepth;
        c.NearFieldGate = 1u;
        for (uint32_t f = 0; f < frames; ++f)
        {
            c.GateVertexEval = (f == 0u && edit == nullptr) ? 0u : 1u;
            const bool announce = edit != nullptr && f == 0u;
            c.DirtyMinU = announce ? edit->X : 0.0f;
            c.DirtyMinV = announce ? edit->Y : 0.0f;
            c.DirtyMaxU = announce ? edit->Right() : 0.0f;
            c.DirtyMaxV = announce ? edit->Bottom() : 0.0f;
            auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            cl->Begin();
            inst.RecordUpdate(*cl, c, p, firstFrame + f);
            cl->End();
            std::vector<CommandList*> lists{cl.get()};
            m_Device->ExecuteCommandLists(lists);
            m_Device->WaitForIdle();
        }
    }

    std::vector<uint32_t> ReadChain(CBTInstance& inst)
    {
        CBTResources& res = inst.GetResources();
        return ReadWords(res.GetHeightChainBuffer(), res.GetHeightChainWords());
    }

    // The first `words` words of the storage buffer `src`, which rests in UnorderedAccess.
    std::vector<uint32_t> ReadWords(BufferHandle src, uint32_t words)
    {
        const size_t bytes = static_cast<size_t>(words) * sizeof(uint32_t);
        std::vector<uint32_t> out(words, 0u);
        BufferHandle readback = m_Device->CreateReadbackBuffer(bytes, "CBT.Test.WordsReadback");
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateBufferBarrier(src, ResourceState::UnorderedAccess, ResourceState::CopySource));
        cl->CopyBuffer(src, readback, bytes, 0, 0);
        cl->Barrier(ResourceBarrier::CreateBufferBarrier(src, ResourceState::CopySource, ResourceState::UnorderedAccess));
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
        if (const void* mapped = m_Device->MapBuffer(readback))
        {
            std::memcpy(out.data(), mapped, bytes);
            m_Device->UnmapBuffer(readback);
        }
        m_Device->DestroyBuffer(readback);
        return out;
    }

    // The height-range buffer a whole build makes from `heights`, `samples` per axis, at `p`.
    std::vector<uint32_t> WholePyramid(const std::vector<float>& heights, uint32_t samples, const CBTFrameParams& p)
    {
        const TextureHandle tex = MakeTexture(heights, samples, samples);
        CBTInstance inst;
        std::vector<uint32_t> words;
        if (tex.IsValid() && Bring(inst, tex, samples, samples))
        {
            Run(inst, p, 1u, 0u);
            words = ReadWords(inst.GetResources().GetHeightRangeBuffer(), kCBTHeightRangeBufferWords);
        }
        inst.Shutdown();
        m_Device->DestroyTexture(tex);
        return words;
    }

    // Converges a tree on the 1025^2 stripe field (4 m ridges at the lattice frequency) with the camera
    // at `eye` pitched `pitch` down along +Z, and reads it back with its corners and its chain.
    StripeScene ConvergeStripes(const GameEngine::Mathematics::Vector3& eye, float pitch)
    {
        constexpr uint32_t kN = 1025u;
        StripeScene scene;
        const TextureHandle tex = MakeTexture(StripeField(kN), kN, kN);
        CBTInstance inst;
        if (!tex.IsValid() || !Bring(inst, tex, kN, kN))
            return scene;
        scene.Params = CameraParams(eye, pitch, kN, 4.0f);
        Run(inst, scene.Params, kConvergeFrames, 0u);
        scene.Snapshot = ReadTreeSnapshot(inst, true);
        scene.Chain = ReadChain(inst);
        inst.Shutdown();
        m_Device->DestroyTexture(tex);
        return scene;
    }

    // Mip `mip` of the instance's normal chain as (x, z) pairs, row-major.
    std::vector<std::array<float, 2>> ReadNormalMip(CBTInstance& inst, uint32_t mip, uint32_t width, uint32_t height)
    {
        const TextureHandle tex = inst.GetResources().GetNormalChainTexture();
        const size_t bytes = static_cast<size_t>(width) * height * 4u; // RG16F
        std::vector<std::array<float, 2>> out(static_cast<size_t>(width) * height);
        BufferHandle readback = m_Device->CreateReadbackBuffer(bytes, "CBT.Test.NormalReadback");
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::ShaderResource, ResourceState::CopySource, mip, 1u));
        cl->CopyTextureSubresourceToBuffer(tex, mip, 0, readback, width, height, 0, 0, 0, static_cast<size_t>(width) * 4u);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopySource, ResourceState::ShaderResource, mip, 1u));
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
        if (const auto* mapped = static_cast<const uint16_t*>(m_Device->MapBuffer(readback)))
        {
            for (size_t i = 0; i < out.size(); ++i)
                out[i] = {HalfToFloat(mapped[2 * i]), HalfToFloat(mapped[2 * i + 1])};
            m_Device->UnmapBuffer(readback);
        }
        m_Device->DestroyBuffer(readback);
        return out;
    }

    static float HalfToFloat(uint16_t h)
    {
        const uint32_t sign = static_cast<uint32_t>(h >> 15) << 31;
        const uint32_t exponent = (h >> 10) & 0x1Fu;
        const uint32_t mantissa = h & 0x3FFu;
        if (exponent == 0u)
            return std::bit_cast<float>(sign) + (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
        return std::bit_cast<float>(sign | ((exponent + 112u) << 23) | (mantissa << 13));
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

} // namespace

TEST(CBTHeightChainShim, NearVerticesKeepTheirCachedCornerBits)
{
    const std::vector<uint32_t> words(64u, std::bit_cast<uint32_t>(0.5f));
    Shader::g_ChainWords = &words;
    std::mt19937 rng(7u);
    std::uniform_real_distribution<float> height(-300.0f, 3000.0f);
    for (const float lambda : {-40.0f, -1.0f, 0.0f, 0.25f, 0.5f, 0.74f, 0.75f})
        for (uint32_t i = 0; i < 64u; ++i)
        {
            const float cached = height(rng);
            const float drawn = Shader::CBT_ChainDrawnHeight(0.3f, 0.7f, cached, lambda, 5u, 5u, 2u, 10.0f, 0.0f);
            EXPECT_EQ(std::bit_cast<uint32_t>(drawn), std::bit_cast<uint32_t>(cached)) << "lambda " << lambda;
        }
    // Past 0.75 the chain takes over: the vertex leaves its cached height for the level-1 surface.
    EXPECT_NE(Shader::CBT_ChainDrawnHeight(0.3f, 0.7f, 123.0f, 0.9f, 5u, 5u, 2u, 10.0f, 0.0f), 123.0f);
    Shader::g_ChainWords = nullptr;
}

TEST_F(CBTHeightChain, ChainMatchesTheTentOracleAtThreeLevels)
{
    // 385 x 257: two odd axes that halve to an even 4 and reach the top two levels apart.
    constexpr uint32_t kX = 385u;
    constexpr uint32_t kZ = 257u;
    const std::vector<float> data = RandomField(kX, kZ, 11u);
    const TextureHandle tex = MakeTexture(data, kX, kZ);
    ASSERT_TRUE(tex.IsValid());
    CBTInstance inst;
    ASSERT_TRUE(Bring(inst, tex, kX, kZ));
    Run(inst, CameraParams({192.0f, 3.0f, 10.0f}, 0.2f, kX, 40.0f), 1u, 0u);
    const std::vector<uint32_t> words = ReadChain(inst);
    ASSERT_GE(words.size(), CBTHeightChainWords(kX, kZ));
    EXPECT_EQ(words[0], kX | (kZ << 16u)) << "the header names the lattice the chain was built from";
    EXPECT_EQ(words[1], CBTHeightChainTopLevel(kX, kZ));
    const std::vector<std::vector<float>> oracle = OracleChain(data, kX, kZ);
    for (uint32_t level = 1; level <= 3u; ++level)
    {
        const float err = LevelError(words, oracle, kX, kZ, level);
        std::printf("level %u: %u x %u samples, worst |GPU - oracle| %.3g\n", level, CBTHeightChainSamples(kX, level),
                    CBTHeightChainSamples(kZ, level), err);
        EXPECT_LE(err, 1e-6f) << "level " << level;
    }
    inst.Shutdown();
    m_Device->DestroyTexture(tex);
}

TEST(CBTHeightChainShim, FragmentsTakeTheNormalAtTheirOwnLevel)
{
    // A fragment and a vertex at one position take one level (the same rule on the same inputs).
    const float scale = 0.0171f;
    EXPECT_EQ(Shader::CBT_ChainLevel(150.0f, scale), Shader::CBT_ChainLevel(150.0f, scale));
    for (const float near : {-3.0f, 0.0f, 0.5f, 0.75f, 1.0f, 1.5f, 1.75f})
        EXPECT_EQ(Shader::CBT_ChainNormalWeight(near), 0.0f) << "the level-0 map alone at lambda " << near;
    EXPECT_FLOAT_EQ(Shader::CBT_ChainNormalWeight(1.875f), 0.5f);
    for (const float far : {2.0f, 2.6f, 4.25f, 12.0f})
        EXPECT_EQ(Shader::CBT_ChainNormalWeight(far), 1.0f) << "the chain alone at lambda " << far;
}

TEST(CBTHeightChainShim, GrazingFootprintEndsARidgeRowWithoutAStep)
{
    // The R3 evidence pose B: the eye 6 m over the mean plane, pitched 30 degrees down, a 60-degree
    // vertical field of view on a 3067 x 900 view; the level rule of a 0.5 m lattice at target error
    // 8 px over 1080 reference rows. Every pixel of a row sees the ground at its own distance and
    // grazing angle. A row over a ridge face shades the face (luma 60, the dark stripes the captures
    // show) where the normal is the level-0 map, the averaged far surface (luma 150) where it is the
    // chain, and the mix between. An edge is a step above 40 luma levels between horizontal neighbors
    // (the critics' vertical-edge instrument).
    constexpr float kEyeHeight = 6.0f;
    constexpr float kPitchDeg = 30.0f;
    constexpr int kWidth = 3067;
    constexpr int kHeight = 900;
    constexpr float kVerticalFovDeg = 60.0f;
    constexpr float kFaceLuma = 60.0f;
    constexpr float kMeanLuma = 150.0f;
    constexpr float kEdgeStep = 40.0f;
    constexpr float kMinGrazingCos = 1.0f / 64.0f; // cbt_surface.glsl kCBTMinGrazingCos
    const float scale = 8.0f / ((0.5f * 1080.0f / std::tan(0.5f * kVerticalFovDeg * 3.14159265f / 180.0f)) * 0.5f);
    const float focal = 0.5f * static_cast<float>(kHeight) / std::tan(0.5f * kVerticalFovDeg * 3.14159265f / 180.0f);
    const float pitch = kPitchDeg * 3.14159265f / 180.0f;

    int edges = 0;
    int rowsCrossingTheBand = 0;
    float nearestFull = 1.0e30f;
    for (int y = 0; y < kHeight / 2; ++y)
    {
        float previous = -1.0f;
        float rowMin = 1.0f;
        float rowMax = 0.0f;
        for (int x = 0; x < kWidth; ++x)
        {
            // The view ray in the eye's frame (x right, y up, z forward), then pitched down.
            const float rx = (static_cast<float>(x) + 0.5f - 0.5f * kWidth) / focal;
            const float ry = (0.5f * kHeight - static_cast<float>(y) - 0.5f) / focal;
            const float down = std::sin(pitch) * 1.0f - std::cos(pitch) * ry;
            if (down <= 0.0f)
            {
                previous = -1.0f;
                continue; // above the horizon
            }
            const float t = kEyeHeight / down;
            const float length = std::sqrt(rx * rx + ry * ry + 1.0f);
            const float distance = t * length;
            const float cosView = kEyeHeight / distance;
            const float spread = 1.0f / std::max(cosView, kMinGrazingCos);
            const float lambda = Shader::CBT_ChainLevel(distance, scale);
            const float chain = Shader::CBT_ChainNormalWeight(Shader::CBT_GrazingLevel(lambda, spread));
            const float luma = kFaceLuma + (kMeanLuma - kFaceLuma) * chain;
            if (previous >= 0.0f && std::fabs(luma - previous) > kEdgeStep)
                ++edges;
            previous = luma;
            rowMin = std::min(rowMin, chain);
            rowMax = std::max(rowMax, chain);
            if (chain >= 1.0f)
                nearestFull = std::min(nearestFull, distance);
        }
        if (rowMin == 0.0f && rowMax == 1.0f)
            ++rowsCrossingTheBand;
    }
    std::printf("rows that cross from the map to the chain: %d, step edges: %d, nearest full chain read %.1f m\n",
                rowsCrossingTheBand, edges, nearestFull);
    EXPECT_GT(rowsCrossingTheBand, 50) << "the instrument must see rows that cross the band";
    EXPECT_EQ(edges, 0) << "the shading changes over a distance band, not at a ring";

    // Nearer than lambda 0 the fragment keeps its head-on footprint and the level-0 map; from
    // CBT_GRAZING_FULL_LEVEL on it reads the whole grazing footprint.
    for (const float lambda : {-4.0f, -1.0f, 0.0f})
    {
        EXPECT_EQ(Shader::CBT_GrazingLevel(lambda, 16.0f), lambda) << lambda;
        EXPECT_EQ(Shader::CBT_GrazingShare(lambda), 0.0f) << lambda;
    }
    for (const float lambda : {1.0f, 2.5f, 9.0f})
    {
        EXPECT_FLOAT_EQ(Shader::CBT_GrazingLevel(lambda, 16.0f), lambda + 4.0f) << lambda;
        EXPECT_EQ(Shader::CBT_GrazingShare(lambda), 1.0f) << lambda;
    }
    EXPECT_FLOAT_EQ(Shader::CBT_GrazingShare(0.5f), 0.5f);
}

// The terrain surface takes its far normal's level from the level rule the test above pins, so the
// fade it proves is the one drawn, and hands the terrain shadow its head-on footprint, which the
// shadow widens by the same rule (TerrainShadowMap.FootprintWidensByTheDrawsLevelRule). A surface
// that read the grazing level as lambda + log2(spread) or widened the footprint by the bare spread
// would switch the footprint on at lambda 0, the ring a ridge field cuts into a staircase.
TEST(CBTHeightChainShim, SurfaceTakesTheGrazingFootprintFromTheLevelRule)
{
    const std::string path = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_surface.glsl";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();
    EXPECT_NE(src.find("float grazingLambda = CBT_GrazingLevel(lambda, spread);"), std::string::npos)
        << "the far normal's level comes from the level rule";
    EXPECT_NE(src.find("ge_ShadowReceiverFootprint = eyeDistance * pixelAngle;"), std::string::npos)
        << "the terrain shadow takes the head-on footprint and widens it by the shared rule";
    EXPECT_NE(src.find("ge_ShadowReceiverDrawLevel = lambda;"), std::string::npos)
        << "the shared rule widens by the draw level the surface's own fade uses";
    EXPECT_EQ(src.find("lambda + log2(spread)"), std::string::npos) << "the grazing level skips the fade";
    EXPECT_EQ(src.find("pixelAngle * spread;"), std::string::npos) << "the shadow's footprint skips the fade";
}

TEST_F(CBTHeightChain, NormalChainMatchesTheChainSurfaceAtThreeMips)
{
    constexpr uint32_t kX = 385u;
    constexpr uint32_t kZ = 257u;
    constexpr float kHeightScale = 40.0f;
    const TextureHandle tex = MakeTexture(RandomField(kX, kZ, 31u), kX, kZ);
    ASSERT_TRUE(tex.IsValid());
    CBTInstance inst;
    ASSERT_TRUE(Bring(inst, tex, kX, kZ));
    Run(inst, CameraParams({192.0f, 3.0f, 10.0f}, 0.2f, kX, kHeightScale), 1u, 0u);
    const std::vector<uint32_t> words = ReadChain(inst);
    Shader::g_ChainWords = &words;
    const float cellX = kTerrainM / static_cast<float>(kX - 1u);
    const float cellZ = kTerrainM / static_cast<float>(kZ - 1u);
    for (uint32_t mip = 0; mip < 3u; ++mip)
    {
        const uint32_t level = mip + 1u;
        const uint32_t tx = CBTNormalChainTexels(kX, mip);
        const uint32_t tz = CBTNormalChainTexels(kZ, mip);
        const std::vector<std::array<float, 2>> gpu = ReadNormalMip(inst, mip, tx, tz);
        float worst = 0.0f;
        for (uint32_t z = 0; z < tz; ++z)
            for (uint32_t x = 0; x < tx; ++x)
            {
                // The oracle: the drawn surface of this level one sample each side of the texel center.
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(tx);
                const float v = (static_cast<float>(z) + 0.5f) / static_cast<float>(tz);
                const float du = static_cast<float>(1u << level) / static_cast<float>(kX - 1u);
                const float dv = static_cast<float>(1u << level) / static_cast<float>(kZ - 1u);
                const auto height = [&](float a, float b) {
                    return Shader::CBT_ChainSampleLevel(a, b, level, kX, kZ) * kHeightScale;
                };
                const float x0 = std::max(u - du, 0.0f), x1 = std::min(u + du, 1.0f);
                const float z0 = std::max(v - dv, 0.0f), z1 = std::min(v + dv, 1.0f);
                const double sx = (height(u + du, v) - height(u - du, v)) / ((x1 - x0) * (kX - 1u) * cellX);
                const double sz = (height(u, v + dv) - height(u, v - dv)) / ((z1 - z0) * (kZ - 1u) * cellZ);
                const double len = std::sqrt(sx * sx + 1.0 + sz * sz);
                const std::array<float, 2>& n = gpu[static_cast<size_t>(z) * tx + x];
                worst = std::max({worst, static_cast<float>(std::fabs(n[0] + sx / len)),
                                  static_cast<float>(std::fabs(n[1] + sz / len))});
            }
        std::printf("mip %u (level %u): %u x %u texels, worst |GPU - oracle| %.3g\n", mip, level, tx, tz, worst);
        EXPECT_LE(worst, 2e-3f) << "mip " << mip; // RG16F holds about three decimal digits
    }
    Shader::g_ChainWords = nullptr;
    inst.Shutdown();
    m_Device->DestroyTexture(tex);
}

TEST_F(CBTHeightChain, EditRectRebuildsOnlyTheSamplesItsTentsReach)
{
    constexpr uint32_t kN = 513u;
    const std::vector<float> before = RandomField(kN, kN, 21u);
    const TextureHandle tex = MakeTexture(before, kN, kN);
    ASSERT_TRUE(tex.IsValid());
    CBTInstance inst;
    ASSERT_TRUE(Bring(inst, tex, kN, kN));
    const CBTFrameParams p = CameraParams({256.0f, 3.0f, 10.0f}, 0.2f, kN, 40.0f);
    Run(inst, p, 1u, 0u);

    // The edit: texels [200, 231] x [300, 311] raised. A second change far from it is NOT announced,
    // so a rebuild that reaches past the edit's own footprint shows up as that change in the chain.
    std::vector<float> edited = before;
    std::vector<float> uploaded = before;
    for (uint32_t z = 300u; z < 312u; ++z)
        for (uint32_t x = 200u; x < 232u; ++x)
        {
            edited[static_cast<size_t>(z) * kN + x] += 0.25f;
            uploaded[static_cast<size_t>(z) * kN + x] += 0.25f;
        }
    for (uint32_t z = 40u; z < 60u; ++z)
        for (uint32_t x = 400u; x < 430u; ++x)
            uploaded[static_cast<size_t>(z) * kN + x] -= 0.5f;
    UploadR32Texture(*m_Device, tex, kN, kN, uploaded, ResourceState::ShaderResource);
    // The rect the terrain announces for that texel rect (TerrainRenderFeature::AccumulateUnifiedHeightDirty).
    const GameEngine::Mathematics::Rect rect = GameEngine::Mathematics::Rect::FromEdges(
        (200.0f - 1.0f) / kN, (300.0f - 1.0f) / kN, (232.0f + 1.0f) / kN, (312.0f + 1.0f) / kN);
    Run(inst, p, 1u, 1u, &rect);

    const std::vector<uint32_t> words = ReadChain(inst);
    const std::vector<std::vector<float>> expected = OracleChain(edited, kN, kN);
    const std::vector<std::vector<float>> whole = OracleChain(uploaded, kN, kN);
    const std::vector<std::vector<float>> old = OracleChain(before, kN, kN);
    for (uint32_t level = 1; level <= CBTHeightChainTopLevel(kN, kN); ++level)
    {
        const uint32_t base = CBTHeightChainLevelWord(kN, kN, level);
        uint32_t rewritten = 0;
        float worst = 0.0f;
        for (size_t i = 0; i < expected[level].size(); ++i)
        {
            const float gpu = std::bit_cast<float>(words[base + i]);
            worst = std::max(worst, std::fabs(gpu - expected[level][i]));
            rewritten += expected[level][i] != old[level][i] ? 1u : 0u;
        }
        std::printf("level %u: %u samples rewritten by the edit, worst |GPU - announced edit| %.3g, unannounced change "
                    "in the chain: %s\n",
                    level, rewritten, worst, LevelError(words, whole, kN, kN, level) <= 1e-6f ? "yes" : "no");
        EXPECT_LE(worst, 1e-6f) << "level " << level;
        EXPECT_GT(rewritten, 0u) << "level " << level << ": the edit reaches every level";
    }
    inst.Shutdown();
    m_Device->DestroyTexture(tex);
}

namespace
{
// The words of the height-range buffer a build over a `samples` x `samples` lattice writes, with the
// chain's ranges for `chainLevels` chain levels (cbt_kernels.comp Kernel_HeightRangeBuild): the header
// words it sets, the ranges of every pyramid, the first stored level's differences and the level-0
// pyramid's planes.
std::vector<uint32_t> PyramidWords(uint32_t samples, uint32_t chainLevels)
{
    const uint32_t cells = samples - 1u;
    const uint32_t first = CBTHeightRangeFirstLevel(cells, cells);
    const uint32_t top = std::max(CBTHeightRangeTopLevel(cells, cells), first);
    const uint32_t levelZero = CBTHeightRangeOffset(cells, cells, first, top + 1u);
    uint32_t chainEntries = 0u;
    for (uint32_t j = 1u; j <= chainLevels; ++j)
        chainEntries += levelZero - CBTHeightRangeOffset(cells, cells, first, CBTHeightChainRangeBase(first, top, j));
    std::vector<uint32_t> words{0u, 1u, 18u, 19u};
    for (uint32_t k = first; k <= top; ++k)
        words.push_back(2u + k);
    for (uint32_t j = 1u; j <= chainLevels; ++j)
        words.push_back(20u + j);
    for (uint32_t w = 0; w < 2u * levelZero; ++w)
        words.push_back(kCBTHeightRangeHeaderWords + w);
    for (uint32_t w = 0; w < 2u * chainEntries; ++w) // the chain's pyramids start at entry kCBTHeightRangeEntries
        words.push_back(kCBTHeightRangeHeaderWords + 2u * kCBTHeightRangeEntries + w);
    const uint32_t firstBlocks = CBTHeightRangeDim(cells, first) * CBTHeightRangeDim(cells, first);
    for (uint32_t w = 0; w < kCBTHeightDifferencesWordsPerEntry * firstBlocks; ++w)
        words.push_back(kCBTHeightDifferencesFirstWord + w);
    for (uint32_t w = 0; w < kCBTHeightPlaneWordsPerEntry * levelZero; ++w)
        words.push_back(kCBTHeightPlaneFirstWord + w);
    return words;
}

// Height-range pyramid rebuilds, each compared word for word against a whole build.
class CBTHeightPyramid : public CBTHeightChain
{
  protected:
    static constexpr uint32_t kN = 513u;

    // A random field with the texels of `edit` (inclusive) raised far above the [0, 1] field and announced,
    // so every chain sample its tents reach is the highest of its blocks, and a block over it left unbuilt
    // shows. A second change far from it is not announced, so a build that reaches past the blocks over the
    // edit shows it. With `chainOn` false the draw's chain is off, the paged arm's configuration, and the
    // level-0 cover is the pyramid's only cover.
    void ExpectEditRectRebuildEqualsAWholeBuild(const CBTHeightBuildRegion& edit, bool chainOn)
    {
        const uint32_t chainLevels = chainOn ? CBTHeightChainRangeLevels(kN, kN) : 0u;
        ASSERT_TRUE(!chainOn || chainLevels > 0u) << "the lattice must carry the chain's ranges";
        const std::vector<uint32_t> written = PyramidWords(kN, chainLevels);
        const std::vector<float> before = RandomField(kN, kN, 23u);
        CBTFrameParams p = CameraParams({256.0f, 3.0f, 10.0f}, 0.2f, kN, 40.0f);
        if (!chainOn)
            p.HeightChain[0] = 0.0f;

        std::vector<float> edited = before;
        for (uint32_t z = edit.MinZ; z <= edit.MaxZ; ++z)
            for (uint32_t x = edit.MinX; x <= edit.MaxX; ++x)
                edited[static_cast<size_t>(z) * kN + x] += 64.0f;
        std::vector<float> uploaded = edited;
        for (uint32_t z = 40u; z < 60u; ++z)
            for (uint32_t x = 400u; x < 430u; ++x)
                uploaded[static_cast<size_t>(z) * kN + x] -= 0.5f;

        const TextureHandle tex = MakeTexture(before, kN, kN);
        ASSERT_TRUE(tex.IsValid());
        CBTInstance inst;
        ASSERT_TRUE(Bring(inst, tex, kN, kN));
        Run(inst, p, 1u, 0u);
        const BufferHandle range = inst.GetResources().GetHeightRangeBuffer();
        const std::vector<uint32_t> old = ReadWords(range, kCBTHeightRangeBufferWords);
        UploadR32Texture(*m_Device, tex, kN, kN, uploaded, ResourceState::ShaderResource);
        // The rect the terrain announces for that texel rect (CBTUpdateSystem::ConsumeDirtyRegion): one
        // texel of padding each side, the upper edge exclusive.
        const GameEngine::Mathematics::Rect rect = GameEngine::Mathematics::Rect::FromEdges(
            (static_cast<float>(edit.MinX) - 1.0f) / kN, (static_cast<float>(edit.MinZ) - 1.0f) / kN,
            (static_cast<float>(edit.MaxX) + 2.0f) / kN, (static_cast<float>(edit.MaxZ) + 2.0f) / kN);
        Run(inst, p, 1u, 1u, &rect);
        const std::vector<uint32_t> rebuilt = ReadWords(range, kCBTHeightRangeBufferWords);
        inst.Shutdown();
        m_Device->DestroyTexture(tex);

        const std::vector<uint32_t> expected = WholePyramid(edited, kN, p);
        const std::vector<uint32_t> unannounced = WholePyramid(uploaded, kN, p);
        ASSERT_EQ(expected.size(), rebuilt.size());
        ASSERT_EQ(unannounced.size(), rebuilt.size());
        uint32_t mismatched = 0;
        uint32_t edits = 0;
        uint32_t unannouncedWords = 0;
        for (uint32_t w : written)
        {
            mismatched += rebuilt[w] != expected[w] ? 1u : 0u;
            edits += expected[w] != old[w] ? 1u : 0u;
            unannouncedWords += unannounced[w] != expected[w] ? 1u : 0u;
        }
        std::printf("chain %s, pyramid words %zu: %u rewritten by the edit, %u the unannounced change moves in a "
                    "whole build, %u differ from a whole build of the announced edit\n",
                    chainOn ? "on" : "off", written.size(), edits, unannouncedWords, mismatched);
        EXPECT_GT(edits, 0u) << "the edit must reach the pyramid";
        EXPECT_GT(unannouncedWords, 0u) << "the unannounced change must show in a build that reaches it";
        EXPECT_EQ(mismatched, 0u) << "a rect rebuild must equal a whole build of the edit and leave the rest as it was";
    }
};
} // namespace

TEST_F(CBTHeightPyramid, EditRectRebuildsOnlyThePyramidBlocksOverIt)
{
    ExpectEditRectRebuildEqualsAWholeBuild({200u, 300u, 231u, 311u}, true);
}

// A 3 x 3 texel edit on the lattice's center straddles a block boundary at every pyramid level, so each
// coarser level's cover must take the parents of both sides.
TEST_F(CBTHeightPyramid, EditRectWithTheChainOffRebuildsOnlyThePyramidBlocksOverIt)
{
    ExpectEditRectRebuildEqualsAWholeBuild({255u, 255u, 257u, 257u}, false);
}

// A newly bound height source rebuilds every block, even on a frame that announces no edit.
TEST_F(CBTHeightPyramid, ReboundSourceRebuildsEveryBlock)
{
    const std::vector<uint32_t> written = PyramidWords(kN, 0u);
    const std::vector<float> before = RandomField(kN, kN, 23u);
    std::vector<float> other = before;
    for (uint32_t z = 100u; z < 300u; ++z)
        for (uint32_t x = 100u; x < 300u; ++x)
            other[static_cast<size_t>(z) * kN + x] += 64.0f;
    CBTFrameParams p = CameraParams({256.0f, 3.0f, 10.0f}, 0.2f, kN, 40.0f);
    p.HeightChain[0] = 0.0f;
    const TextureHandle first = MakeTexture(before, kN, kN);
    const TextureHandle second = MakeTexture(other, kN, kN);
    ASSERT_TRUE(first.IsValid() && second.IsValid());
    CBTInstance inst;
    ASSERT_TRUE(Bring(inst, first, kN, kN));
    Run(inst, p, 1u, 0u);
    const BufferHandle range = inst.GetResources().GetHeightRangeBuffer();
    const std::vector<uint32_t> old = ReadWords(range, kCBTHeightRangeBufferWords);
    for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
        inst.SetHeightSource(slot, second);
    const GameEngine::Mathematics::Rect none = GameEngine::Mathematics::Rect::FromEdges(0.0f, 0.0f, 0.0f, 0.0f);
    Run(inst, p, 1u, 1u, &none); // a gated frame that announces no edit
    const std::vector<uint32_t> rebuilt = ReadWords(range, kCBTHeightRangeBufferWords);
    inst.Shutdown();
    m_Device->DestroyTexture(first);
    m_Device->DestroyTexture(second);

    const std::vector<uint32_t> expected = WholePyramid(other, kN, p);
    ASSERT_EQ(expected.size(), rebuilt.size());
    uint32_t mismatched = 0;
    uint32_t changed = 0;
    for (uint32_t w : written)
    {
        mismatched += rebuilt[w] != expected[w] ? 1u : 0u;
        changed += expected[w] != old[w] ? 1u : 0u;
    }
    std::printf("rebind: %u pyramid words differ between the sources, %u differ from a whole build of the new one\n",
                changed, mismatched);
    EXPECT_GT(changed, 0u) << "the two sources must build different pyramids";
    EXPECT_EQ(mismatched, 0u) << "a new source must rebuild the whole pyramid";
}

namespace
{
// The height corner `k` of a facet draws at in `scene`, by the shipped block: the level of the corner's
// own distance, or (`perFacet`) the level of the facet's centroid, the rule that cracks.
float DrawnHeight(const StripeScene& scene, const CBTVertexData& vd, uint32_t k, bool perFacet)
{
    const std::array<float, 3> pos = Corner(vd, k);
    const std::array<float, 2> uv = CornerUv(vd, k);
    float distance = Distance(pos, scene.Params.CameraPos);
    if (perFacet)
    {
        std::array<float, 3> centroid{};
        for (uint32_t c = 0; c < 3u; ++c)
            for (uint32_t a = 0; a < 3u; ++a)
                centroid[a] += Corner(vd, c)[a] / 3.0f;
        distance = Distance(centroid, scene.Params.CameraPos);
    }
    const float lambda = Shader::CBT_ChainLevel(distance, scene.Params.HeightChain[0]);
    return Shader::CBT_ChainDrawnHeight(uv[0], uv[1], pos[1], lambda, 1025u, 1025u,
                                        static_cast<uint32_t>(scene.Params.HeightChain[1]), scene.Params.TerrainSize[2],
                                        scene.Params.TerrainSize[3]);
}

std::vector<uint8_t> VisibleSlots(const CBTTreeSnapshot& s)
{
    std::vector<uint8_t> visible(s.PoolSize, 0u);
    for (uint32_t v : s.IndicesVisible)
        if (v < s.PoolSize)
            visible[v] = 1u;
    return visible;
}

// One copy of a vertex: what one facet that has it as a corner draws there.
struct VertexCopy
{
    uint32_t Drawn = 0;    // the drawn height's bits, the level of the vertex's own distance
    uint32_t PerFacet = 0; // the same at the level of the facet's centroid
    uint32_t Cached = 0;   // the cached corner height's bits
    uint32_t Depth = 0;
    float Lambda = 0.0f;
};
} // namespace

TEST_F(CBTHeightChain, FacetDrawnIntoTheFrustumIsKept)
{
    // 10 m above the ridges' feet, pitched 32 degrees down: the top plane crosses the drawn 2 m plane
    // about 230 m out and the 4 m ridge crests about 170 m out, so far facets whose corners sit on
    // crests are above the frustum while the surface they draw is inside it.
    const StripeScene scene = ConvergeStripes({288.0f, 10.0f, 20.0f}, 0.56f);
    ASSERT_FALSE(scene.Snapshot.Vertices.empty());
    ASSERT_FALSE(scene.Chain.empty());
    Shader::g_ChainWords = &scene.Chain;
    const auto planes = FrustumPlanes(scene.Params.ViewProjRel);
    const std::vector<uint8_t> visible = VisibleSlots(scene.Snapshot);
    uint32_t widened = 0;
    uint32_t dropped = 0;
    uint32_t culled = 0;
    uint32_t live = 0;
    for (uint32_t slot = 0; slot < scene.Snapshot.PoolSize; ++slot)
    {
        if (scene.Snapshot.HeapIds[slot] == 0u)
            continue;
        ++live;
        const CBTVertexData& vd = scene.Snapshot.Vertices[slot];
        std::array<std::array<float, 3>, 3> cached{};
        std::array<std::array<float, 3>, 3> drawn{};
        for (uint32_t k = 0; k < 3u; ++k)
        {
            cached[k] = Corner(vd, k);
            drawn[k] = cached[k];
            drawn[k][1] = DrawnHeight(scene, vd, k, false);
        }
        const bool kept = visible[slot] != 0u;
        culled += kept ? 0u : 1u;
        if (!TriangleIn(planes, drawn, 0.01f))
            continue;
        widened += TriangleIn(planes, cached, -0.01f) ? 0u : 1u;
        dropped += kept ? 0u : 1u;
    }
    std::printf("live %u, visible %u, culled %u; drawn in view with cached corners out of it: %u; of the drawn "
                "in view, culled: %u\n",
                live, scene.Snapshot.VisibleCount, culled, widened, dropped);
    EXPECT_GT(widened, 0u) << "the pose must hold facets the cached corners alone would cull";
    EXPECT_EQ(dropped, 0u) << "a facet whose drawn triangle is in the frustum was culled";
    EXPECT_GT(culled, live / 4u) << "the widened cull still culls what is out of view";
    Shader::g_ChainWords = nullptr;
}

namespace
{
// The chain level-1 cover the planar split reads for a node's UV box [lo, hi] under the reach of a
// facet at continuous level `lambdaFar` (cbt_kernels.comp CBT_DrawnReachUV, CBT_RangeCells,
// CBT_ChainRangeOverUV): the lattice cells, along X, of the blocks of the coarsest level that holds the
// widened box to two blocks per axis, never finer than the level the chain-1 pyramid starts at.
std::array<int64_t, 2> ChainOneCoverCellsX(std::array<float, 2> lo, std::array<float, 2> hi, float lambdaFar,
                                           uint32_t cells, uint32_t top)
{
    const uint32_t reachLevel = std::min(static_cast<uint32_t>(std::min(lambdaFar, static_cast<float>(top))) + 1u, top);
    const float reach = static_cast<float>(2u << reachLevel) / static_cast<float>(cells);
    const float cf = static_cast<float>(cells);
    std::array<uint32_t, 2> c0{};
    std::array<uint32_t, 2> c1{};
    for (uint32_t a = 0; a < 2u; ++a)
    {
        c0[a] = static_cast<uint32_t>(std::clamp(std::floor(GameEngine::Math::Clamp01(lo[a] - reach) * cf), 0.0f, cf - 1.0f));
        c1[a] = static_cast<uint32_t>(std::clamp(std::ceil(GameEngine::Math::Clamp01(hi[a] + reach) * cf) - 1.0f,
                                                 static_cast<float>(c0[a]), cf - 1.0f));
    }
    const uint32_t span = std::max(c1[0] - c0[0], c1[1] - c0[1]);
    uint32_t level = span <= 1u ? 0u : static_cast<uint32_t>(std::bit_width(span - 1u));
    const uint32_t first = CBTHeightRangeFirstLevel(cells, cells);
    const uint32_t pyramidTop = CBTHeightRangeTopLevel(cells, cells);
    level = std::min(std::max(level, std::min(first + 1u, pyramidTop)), pyramidTop); // CBTHeightChainRangeBase
    return {static_cast<int64_t>((c0[0] >> level) << level), static_cast<int64_t>(((c1[0] >> level) + 1u) << level)};
}

std::array<float, 2> BoxLo(std::array<float, 2> a, std::array<float, 2> b, std::array<float, 2> c)
{
    return {std::min({a[0], b[0], c[0]}), std::min({a[1], b[1], c[1]})};
}

std::array<float, 2> BoxHi(std::array<float, 2> a, std::array<float, 2> b, std::array<float, 2> c)
{
    return {std::max({a[0], b[0], c[0]}), std::max({a[1], b[1], c[1]})};
}

// Whether the planar split vetoes a node of the stripe scene: the box sits wholly where the draw shows
// chain level 1 or coarser (lambda >= 1 at its nearest point), and the chain-1 cover the split reads
// there holds no sample at the lattice's first or last column, the only ones the tent's clamped edge
// lifts off 0.5. The box's Y spans the whole relief, so its distance is never above the shader's and
// its farthest corner never nearer: the reach is never narrower.
bool FlatForTheSplit(const StripeScene& scene, std::array<float, 2> lo, std::array<float, 2> hi, uint32_t samples)
{
    const float size = scene.Params.TerrainSize[0];
    const float* eye = scene.Params.CameraPos;
    const float boxLo[3] = {lo[0] * size, 0.0f, lo[1] * size};
    const float boxHi[3] = {hi[0] * size, scene.Params.TerrainSize[2], hi[1] * size};
    float nearSq = 0.0f;
    float farSq = 0.0f;
    for (uint32_t a = 0; a < 3u; ++a)
    {
        const float d = std::max(std::max(boxLo[a] - eye[a], eye[a] - boxHi[a]), 0.0f);
        const float f = std::max(std::abs(boxLo[a] - eye[a]), std::abs(boxHi[a] - eye[a]));
        nearSq += d * d;
        farSq += f * f;
    }
    if (Shader::CBT_ChainLevel(std::sqrt(nearSq), scene.Params.HeightChain[0]) < 1.0f)
        return false;
    const uint32_t cells = samples - 1u;
    const std::array<int64_t, 2> cover =
        ChainOneCoverCellsX(lo, hi, Shader::CBT_ChainLevel(std::sqrt(farSq), scene.Params.HeightChain[0]), cells,
                            static_cast<uint32_t>(scene.Params.HeightChain[1]));
    return cover[0] > 0 && cover[1] < static_cast<int64_t>(cells);
}
} // namespace

TEST_F(CBTHeightChain, AFieldTheChainDrawsFlatDoesNotSplitPastItsDrawnRelief)
{
    // 2 m above the ridges' feet looking 3 degrees down along +Z: lambda reaches 1 about 130 m out, and
    // the 4 m level-0 relief there still projects about 24 pixels. Every chain level draws the stripes as
    // the plane at 2 m, so the veto, on the drawn range, holds every node there. Neither parent of a
    // diamond the veto holds wants a split, so a converged tree has merged it, unless a finer neighbor
    // splits a child of the twin and conformity keeps the diamond.
    using GameEngine::CBTTerrain::Test::Diamond;
    using GameEngine::CBTTerrain::Test::DiamondOf;
    constexpr uint32_t kN = 1025u;
    const StripeScene scene = ConvergeStripes({288.0f, 2.0f, 20.0f}, 0.05f);
    ASSERT_FALSE(scene.Snapshot.Vertices.empty());
    const auto leaves = GameEngine::CBTTerrain::Test::LeafKeys(scene.Snapshot);
    uint32_t inFlatDiamond = 0;
    uint32_t mergeable = 0;
    for (uint32_t slot = 0; slot < scene.Snapshot.PoolSize; ++slot)
    {
        const uint64_t h = scene.Snapshot.HeapIds[slot];
        if (h == 0u || HeapDepth(h) <= 1u)
            continue;
        const Diamond d = DiamondOf(GameEngine::CBTTerrain::Test::LeafCorners(scene.Snapshot.Vertices[slot]), (h & 1u) != 0u);
        const auto& p = d.Parent;
        if (!FlatForTheSplit(scene, BoxLo(p[0], p[1], p[2]), BoxHi(p[0], p[1], p[2]), kN) ||
            (d.HasTwin && !FlatForTheSplit(scene, BoxLo(p[0], d.TwinApex, p[2]), BoxHi(p[0], d.TwinApex, p[2]), kN)))
            continue;
        ++inFlatDiamond;
        mergeable += GameEngine::CBTTerrain::Test::DiamondChildrenAreLeaves(leaves, d) ? 1u : 0u;
    }
    std::printf("live %zu; leaves of a diamond the veto holds: %u, of them in a diamond of four leaves: %u\n",
                leaves.size(), inFlatDiamond, mergeable);
    EXPECT_GT(inFlatDiamond, 0u) << "the pose must hold diamonds where the draw shows the chain";
    EXPECT_EQ(mergeable, 0u) << "a diamond the drawn surface shows flat stayed split";
}

TEST_F(CBTHeightChain, SharedVertexDrawsOneHeight)
{
    // At 6 m looking 8 degrees down, the far field runs through lambda 1.4 to 1.8 between about 170 m
    // and 230 m: facets of different depths meet there, inside the blend toward level 2.
    const StripeScene scene = ConvergeStripes({288.0f, 6.0f, 20.0f}, 0.14f);
    ASSERT_FALSE(scene.Snapshot.Vertices.empty());
    ASSERT_FALSE(scene.Chain.empty());
    Shader::g_ChainWords = &scene.Chain;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<VertexCopy>> copies;
    for (uint32_t slot = 0; slot < scene.Snapshot.PoolSize; ++slot)
    {
        if (scene.Snapshot.HeapIds[slot] == 0u)
            continue;
        const CBTVertexData& vd = scene.Snapshot.Vertices[slot];
        for (uint32_t k = 0; k < 3u; ++k)
        {
            const std::array<float, 2> uv = CornerUv(vd, k);
            VertexCopy c;
            c.Drawn = std::bit_cast<uint32_t>(DrawnHeight(scene, vd, k, false));
            c.PerFacet = std::bit_cast<uint32_t>(DrawnHeight(scene, vd, k, true));
            c.Cached = std::bit_cast<uint32_t>(Corner(vd, k)[1]);
            c.Depth = HeapDepth(scene.Snapshot.HeapIds[slot]);
            c.Lambda =
                Shader::CBT_ChainLevel(Distance(Corner(vd, k), scene.Params.CameraPos), scene.Params.HeightChain[0]);
            copies[{std::bit_cast<uint32_t>(uv[0]), std::bit_cast<uint32_t>(uv[1])}].push_back(c);
        }
    }
    uint32_t mixedDepthInBlend = 0;
    uint32_t cracked = 0;
    uint32_t crackedPerFacet = 0;
    uint32_t cachedDiffer = 0;
    for (const auto& [uv, list] : copies)
    {
        bool mixedDepth = false;
        bool split = false;
        bool splitPerFacet = false;
        bool cachedSplit = false;
        for (const VertexCopy& c : list)
        {
            mixedDepth = mixedDepth || c.Depth != list.front().Depth;
            split = split || c.Drawn != list.front().Drawn;
            splitPerFacet = splitPerFacet || c.PerFacet != list.front().PerFacet;
            cachedSplit = cachedSplit || c.Cached != list.front().Cached;
        }
        const float lambda = list.front().Lambda;
        mixedDepthInBlend += (mixedDepth && lambda >= 1.4f && lambda <= 1.8f) ? 1u : 0u;
        cracked += split ? 1u : 0u;
        crackedPerFacet += splitPerFacet ? 1u : 0u;
        cachedDiffer += cachedSplit ? 1u : 0u;
    }
    std::printf("%zu vertices; shared by facets of different depths at lambda 1.4 to 1.8: %u; cracked: %u per "
                "vertex, %u per facet\n",
                copies.size(), mixedDepthInBlend, cracked, crackedPerFacet);
    ASSERT_EQ(cachedDiffer, 0u) << "the cached corners of a shared vertex differ: the tree itself cracks";
    EXPECT_GT(mixedDepthInBlend, 0u) << "the pose must share blend-band vertices between facets of different depths";
    EXPECT_GT(crackedPerFacet, 0u) << "positive control: a level chosen per facet must crack this tree";
    EXPECT_EQ(cracked, 0u) << "a vertex draws at two heights";
    Shader::g_ChainWords = nullptr;
}
