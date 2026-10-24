// The tree a still camera settles to is a function of the pose, not of the path the camera took.
//
// Merges keep a hysteresis band while the camera moves: a pair merges only once its parents' demand is
// well under the split threshold, so a moving boundary does not churn. Every leaf inside that band is
// a valid rest state, so a camera that arrives from close by keeps finer leaves than one that arrives
// from far away. While the inputs hold still the update applies the rest rule
// (CBTClassifyDesc::Settle, CBT_PlanarClassify): the split and the merge use one threshold, and the
// planar demand is monotone in depth (a child never demands more than its parent), so the only tree
// that holds still is the one the pose asks for. These tests drive the kernels the way CBTRenderFeature
// does: the first update at a new pose has Settle 0, every later one at that pose Settle 1.
//
// Instrument: the live heap ids read back from the GPU after the tree is quiet, compared as a sorted
// set. Two arrivals settle to one tree when the sets are equal, not merely the counts.
//
// The pool-pressure controllers (the off-frustum keep step and the near-bias contention) carry their
// own state across a path when they engage; the poses here converge below the occupancy where either
// engages (keep step 0, contention 0 under 50% occupancy), which the tests assert, so the rest rule
// alone is under test.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CBTTerrain/CBTDemandTuning.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTTerrain/CBTTreeAudit.h"
#include "CBTDiamond.h"
#include "CBTReliefHeightSource.h"
#include "CBTTestHarness.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr float kScreenW = 1600.0f;
constexpr float kScreenH = 900.0f;
constexpr float kTerrainSizeM = 512.0f;
constexpr float kHeightScale = 60.0f; // the shipped ComposedIsland relief
constexpr uint32_t kReliefDim = 512u;
constexpr uint32_t kMaxDepth = 21u;
// A split threshold that keeps both rest poses under the contention ramp.
constexpr float kSplitPx = 20.0f;
// A split threshold that fills the pool at the walk pose.
constexpr float kFullPoolSplitPx = 6.0f;
// A split threshold that keeps the walk pose's rest tree on the near-bias contention ramp, between its
// foot (0.50) and its top (0.90), where the contention moves with the live count.
constexpr float kContendedSplitPx = 12.0f;
// Updates per stage. A stage converges in well under this many (the parent-metric suite converges in
// 140); the rest stage then has to stay quiet for kQuietUpdates more.
constexpr uint32_t kStageUpdates = 160u;
constexpr uint32_t kQuietUpdates = 8u;
// The contention ramp's lower bound (cbt_layout.glsl CBT_NEAR_BIAS_CONTENTION_LO): below it the
// near-bias is exactly 1.
constexpr float kContentionFreeOccupancy = 0.50f;

struct Pose
{
    float EyeX, EyeY, EyeZ;
    float YawDeg, PitchDeg;
};

// Two rest poses with an approach from close by (the tree arrives finer than the pose needs) and one
// from far away (it arrives coarser). The overview looks across the relief from above it, so the domain
// metric owns most of the tree; the walk stands 2 m above the ground looking along it, so the near-field
// bound owns the ground under the eye.
struct Arrival
{
    const char* Label;
    Pose Rest;
    Pose FromNear;
    Pose FromFar;
};
// The terrain spans [0, 512] m in X and Z; the eye heights are meters above the relief under the eye.
float GroundY(float x, float z)
{
    return ReliefHeightAt(x / kTerrainSizeM, z / kTerrainSizeM) * kHeightScale;
}

Pose Above(float x, float aboveM, float z, float yawDeg, float pitchDeg)
{
    return {x, GroundY(x, z) + aboveM, z, yawDeg, pitchDeg};
}

const Arrival kArrivals[] = {
    {"overview", Above(96.0f, 60.0f, 96.0f, 45.0f, -20.0f), Above(176.0f, 15.0f, 176.0f, 45.0f, -35.0f),
     Above(96.0f, 360.0f, 96.0f, 45.0f, -20.0f)},
    {"walk", Above(256.0f, 2.0f, 64.0f, 0.0f, -4.0f), Above(256.0f, 2.0f, 100.0f, 0.0f, -10.0f),
     Above(256.0f, 302.0f, 64.0f, 0.0f, -4.0f)},
};

// The demand shaping a test runs with. The default is the shipped set at kSplitPx; the pool-pressure tests
// turn single controllers off so the one under test is the only reader of the pool's occupancy.
struct Shaping
{
    float SplitPx = kSplitPx;
    bool NearBias = true;               // the near-bias contention ramp
    float KeepOcc = kOffFrustumKeepOcc; // 0 leaves the planar off-frustum field ungated (no keep step)
    uint32_t PoolPressure = 1u;         // 0 holds the pool-pressure scale at 1x
};

void BuildParams(CBTFrameParams& p, const Pose& pose, const Shaping& shaping = {})
{
    using namespace GameEngine::Mathematics;
    p = CBTFrameParams{};
    p.Screen[0] = kScreenW;
    p.Screen[1] = kScreenH;
    p.Screen[2] = shaping.SplitPx;
    p.Screen[3] = 0.5f * shaping.SplitPx; // CBTRenderFeature ships merge = split / 2
    p.TerrainSize[0] = kTerrainSizeM;
    p.TerrainSize[1] = kTerrainSizeM;
    p.TerrainSize[2] = kHeightScale;
    p.TerrainSize[3] = 0.0f;
    p.TerrainOrigin[2] = static_cast<float>(kMaxDepth);

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    const float yaw = pose.YawDeg * kDegToRad, pitch = pose.PitchDeg * kDegToRad;
    const Vector3 eye(pose.EyeX, pose.EyeY, pose.EyeZ);
    const Vector3 dir(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch));
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.05f, kScreenW / kScreenH, 0.5f, 20000.0f);
    const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + dir, Vector3(0, 1, 0));
    std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
    p.CameraPos[0] = eye.x;
    p.CameraPos[1] = eye.y;
    p.CameraPos[2] = eye.z;
    p.CameraPos[3] = 1.0f;

    // The shipped demand shaping: near-bias, near-field target, off-frustum keep, edge rescue.
    const CBTNearBiasRadii nb = ComputeNearBiasRadii(pose.EyeY);
    p.NearBias[0] = shaping.NearBias ? 1.0f : 0.0f;
    p.NearBias[1] = nb.NearRadius;
    p.NearBias[2] = nb.FarRadius;
    p.NearBias[3] = kNearBiasMaxCoarsen;
    p.DemandTuning[0] = kNearFieldFacetTargetM;
    p.DemandTuning[1] = shaping.KeepOcc;
    p.DemandTuning[2] = kEdgeRescueTpeMul * shaping.SplitPx;
    p.DemandTuning[3] = kEdgeRescueOcc;
}

// A motion stage then a rest stage at one pose: the live count the motion converged to, the lowest live
// count any rest update left, and whether the last kQuietUpdates rest updates split and merged nothing.
struct RestTrace
{
    uint32_t MotionLive = 0;
    uint32_t RestLiveMin = 0;
    uint32_t RestLiveEnd = 0;
    bool Quiet = false;
};

struct Settled
{
    std::vector<uint64_t> HeapIds; // sorted live heap ids
    std::vector<uint32_t> States;  // the classify state of each, in the same order
    CBTTessellationStats Stats{};
    bool Quiet = false; // the last kQuietUpdates rest updates split and merged nothing
};
} // namespace

class CBTRestSettle : public ::testing::Test
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

    void RunUpdate(CBTInstance& inst, const CBTClassifyDesc& desc, const CBTFrameParams& params)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        inst.RecordUpdate(*cl, desc, params, m_Frame++);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    // Holds the camera at `pose` for `updates` updates as the render feature would: the first update
    // after the move records with Settle 0 (its inputs changed), every later one with Settle 1.
    // Returns whether the last kQuietUpdates split and merged nothing.
    static CBTClassifyDesc ScreenClassify(const Shaping& shaping)
    {
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = kMaxDepth;
        c.NearFieldGate = 1u;
        c.PoolPressure = shaping.PoolPressure;
        return c;
    }

    bool Hold(CBTInstance& inst, const Pose& pose, uint32_t updates)
    {
        CBTClassifyDesc c = ScreenClassify(Shaping{});
        CBTFrameParams p;
        BuildParams(p, pose);
        uint32_t quiet = 0;
        for (uint32_t i = 0; i < updates; ++i)
        {
            c.Settle = i == 0u ? 0u : 1u;
            RunUpdate(inst, c, p);
            const CBTTessellationStats s = inst.ReadTessellationStats();
            quiet = (s.SplitServed == 0 && s.MergeServed == 0) ? quiet + 1u : 0u;
        }
        return quiet >= kQuietUpdates;
    }

    // `corners`, when given, receives the settled tree with its corners.
    Settled Arrive(const Pose& start, const Pose& rest, CBTTreeSnapshot* corners = nullptr)
    {
        CBTInstance inst;
        EXPECT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(inst.InitializeRoots(kDomainPlanar));
        ReliefHeightSource relief(*m_Device, inst, kHeightScale, kReliefDim);
        EXPECT_TRUE(relief.IsArmed());
        Hold(inst, start, kStageUpdates);
        Settled out;
        out.Quiet = Hold(inst, rest, kStageUpdates);
        out.Stats = inst.ReadTessellationStats();
        if (corners != nullptr)
            *corners = ReadTreeSnapshot(inst, true);
        const std::vector<uint32_t> words = inst.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        constexpr uint32_t kBisectorWords = sizeof(CBTBisectorData) / 4u;
        constexpr uint32_t kStateWord = 1u; // CBTBisectorData::BisectorState
        const std::vector<uint32_t> bisectors =
            inst.DebugReadWords(CBTBinding::BisectorData, kDefaultBisectorPoolSize * kBisectorWords);
        std::vector<std::pair<uint64_t, uint32_t>> live;
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            const uint64_t h = static_cast<uint64_t>(words[s * 2u]) | (static_cast<uint64_t>(words[s * 2u + 1u]) << 32);
            if (h != 0u)
                live.emplace_back(h, bisectors[s * kBisectorWords + kStateWord]);
        }
        std::sort(live.begin(), live.end());
        for (const auto& [h, state] : live)
        {
            out.HeapIds.push_back(h);
            out.States.push_back(state);
        }
        EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << "the settled tree must be conforming";
        inst.Shutdown();
        return out;
    }

    // Converges the motion tree at `pose` (every update with moving inputs), then holds the camera still
    // there for kStageUpdates rest updates.
    RestTrace MotionThenRest(const Pose& pose, const Shaping& shaping)
    {
        CBTInstance inst;
        EXPECT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(inst.InitializeRoots(kDomainPlanar));
        ReliefHeightSource relief(*m_Device, inst, kHeightScale, kReliefDim);
        CBTClassifyDesc c = ScreenClassify(shaping);
        CBTFrameParams p;
        BuildParams(p, pose, shaping);
        c.Settle = 0u;
        for (uint32_t i = 0; i < kStageUpdates; ++i)
            RunUpdate(inst, c, p);
        RestTrace t;
        t.MotionLive = inst.ReadTessellationStats().LiveCount;
        t.RestLiveMin = t.MotionLive;
        c.Settle = 1u;
        uint32_t quiet = 0;
        for (uint32_t i = 0; i < kStageUpdates; ++i)
        {
            RunUpdate(inst, c, p);
            const CBTTessellationStats s = inst.ReadTessellationStats();
            t.RestLiveMin = std::min(t.RestLiveMin, s.LiveCount);
            t.RestLiveEnd = s.LiveCount;
            quiet = (s.SplitServed == 0 && s.MergeServed == 0) ? quiet + 1u : 0u;
        }
        t.Quiet = quiet >= kQuietUpdates;
        EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << "the rest tree must be conforming";
        inst.Shutdown();
        return t;
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
    uint32_t m_Frame = 0;
};

// #2702: one pose, two arrivals, one tree.
TEST_F(CBTRestSettle, ArrivalsFromNearAndFarSettleToOneTree)
{
    for (const Arrival& a : kArrivals)
    {
        SCOPED_TRACE(a.Label);
        const Settled fromNear = Arrive(a.FromNear, a.Rest);
        const Settled fromFar = Arrive(a.FromFar, a.Rest);
        std::printf("[rest:%s] live from near %u, from far %u; keep step %d / %d; merge demand %d / %d\n",
                    a.Label, fromNear.Stats.LiveCount, fromFar.Stats.LiveCount,
                    fromNear.Stats.OffFrustumKeepStep, fromFar.Stats.OffFrustumKeepStep,
                    fromNear.Stats.MergeDemand, fromFar.Stats.MergeDemand);

        // The controllers stay out of the comparison: no keep step and no contention at rest.
        for (const Settled* s : {&fromNear, &fromFar})
        {
            EXPECT_EQ(s->Stats.OffFrustumKeepStep, 0);
            EXPECT_EQ(s->Stats.PressureStep, 0);
            EXPECT_LT(static_cast<float>(s->Stats.LiveCount) / static_cast<float>(kDefaultBisectorPoolSize),
                      kContentionFreeOccupancy);
            EXPECT_TRUE(s->Quiet) << "the tree must stop changing at rest";
        }

        ASSERT_GT(fromNear.HeapIds.size(), 10000u) << "the rest pose barely refined; the test would prove nothing";
        EXPECT_EQ(fromNear.HeapIds.size(), fromFar.HeapIds.size());
        size_t onlyNear = 0, onlyFar = 0;
        for (size_t i = 0, j = 0; i < fromNear.HeapIds.size() || j < fromFar.HeapIds.size();)
        {
            const bool nearLeft = i < fromNear.HeapIds.size();
            if (j == fromFar.HeapIds.size() || (nearLeft && fromNear.HeapIds[i] < fromFar.HeapIds[j]))
                ++onlyNear, ++i;
            else if (i == fromNear.HeapIds.size() || fromFar.HeapIds[j] < fromNear.HeapIds[i])
                ++onlyFar, ++j;
            else
                ++i, ++j;
        }
        EXPECT_EQ(onlyNear, 0u) << "leaves only the arrival from near kept";
        EXPECT_EQ(onlyFar, 0u) << "leaves only the arrival from far kept";
    }
}

// #2600: merge demand at rest that is never served. The two children of a pair must agree on the merge
// at rest, or PrepareSimplify can never serve it. Both children ask the same question of the same
// parent box and the same twin box, so they reach one answer on any relief.
TEST_F(CBTRestSettle, PairsAgreeOnTheMergeAtRest)
{
    for (const Arrival& a : kArrivals)
    {
        SCOPED_TRACE(a.Label);
        const Settled rest = Arrive(a.FromNear, a.Rest);
        EXPECT_TRUE(rest.Quiet);
        uint64_t pairs = 0, disagree = 0, bothMerge = 0;
        for (size_t i = 0; i + 1 < rest.HeapIds.size(); ++i)
        {
            if ((rest.HeapIds[i] >> 1) != (rest.HeapIds[i + 1] >> 1))
                continue;
            ++pairs;
            const bool m0 = rest.States[i] == kStateSimplify, m1 = rest.States[i + 1] == kStateSimplify;
            disagree += m0 != m1 ? 1u : 0u;
            bothMerge += (m0 && m1) ? 1u : 0u;
        }
        std::printf("[agree:%s] sibling pairs %llu, disagreeing %llu, both asking to merge %llu, merge demand %d\n",
                    a.Label, static_cast<unsigned long long>(pairs), static_cast<unsigned long long>(disagree),
                    static_cast<unsigned long long>(bothMerge), rest.Stats.MergeDemand);
        ASSERT_GT(pairs, 1000u);
        EXPECT_EQ(disagree, 0u) << "a pair whose children disagree can never merge";
    }
}

// #2600, the half diamond: a merge flag that stands at rest although its diamond could merge. The four
// children of a diamond ask the same two parents (the facet's parent and its twin) the same question,
// so once the tree is quiet every flag that stands belongs to a diamond conformity holds: a finer
// neighbor splits a child of the twin, and PrepareSimplify waits for it.
TEST_F(CBTRestSettle, NoMergeFlagStandsOnADiamondThatCanMerge)
{
    for (const Arrival& a : kArrivals)
    {
        SCOPED_TRACE(a.Label);
        CBTTreeSnapshot snapshot;
        const Settled rest = Arrive(a.FromNear, a.Rest, &snapshot);
        ASSERT_TRUE(rest.Quiet);
        const auto leaves = LeafKeys(snapshot);
        uint32_t flags = 0;
        uint32_t mergeable = 0;
        for (uint32_t slot = 0; slot < snapshot.PoolSize; ++slot)
        {
            const uint64_t h = snapshot.HeapIds[slot];
            if (h == 0u || snapshot.Bisectors[slot].BisectorState != kStateSimplify)
                continue;
            ++flags;
            mergeable += DiamondChildrenAreLeaves(leaves, DiamondOf(LeafCorners(snapshot.Vertices[slot]), (h & 1u) != 0u))
                             ? 1u
                             : 0u;
        }
        std::printf("[standing:%s] live %zu, merge flags at rest %u, on a diamond of four leaves %u\n", a.Label,
                    leaves.size(), flags, mergeable);
        EXPECT_EQ(mergeable, 0u) << "a merge flag stands on a diamond that could merge";
    }
}

// The rest rule has no hysteresis band, so if a still camera's thresholds followed the live count the
// rest itself changes, a facet on the near-bias contention's threshold would split and merge on
// alternate updates forever. Kernel_Reset latches the live count on every update with moving inputs; at
// rest the latch follows only a move larger than 1/kRestLatchDeadbandDivisor of the pool, which a camera
// jump's rebuild makes and a rest's own flips do not. The depth-target classify sets the live count
// exactly: 2^depth facets.
TEST_F(CBTRestSettle, TheRestLatchFollowsOnlyLargeMovesAtRest)
{
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
    const CBTFrameParams p = CBTIdentityFrameParams();
    const uint32_t deadband = kDefaultBisectorPoolSize / kRestLatchDeadbandDivisor;
    auto run = [&](uint32_t targetDepth, uint32_t settle) {
        CBTClassifyDesc c{kClassifyDepthTarget, kFocusRootAll, targetDepth};
        c.Settle = settle;
        for (uint32_t i = 0; i < 24u; ++i)
            RunUpdate(inst, c, p);
        return inst.ReadTessellationStats().LiveCount;
    };
    auto latched = [&] { return static_cast<uint32_t>(inst.ReadWorkQueueCounter(kWQRestLiveCount)); };

    const uint32_t moving = run(8u, 0u);
    ASSERT_EQ(moving, 256u);
    EXPECT_EQ(latched(), moving) << "an update with moving inputs latches the live count";

    const uint32_t small = run(9u, 1u);
    ASSERT_EQ(small, 512u);
    ASSERT_LT(small - moving, deadband);
    EXPECT_EQ(latched(), moving) << "at rest a move inside the deadband leaves the latch";

    const uint32_t large = run(16u, 1u);
    ASSERT_EQ(large, 65536u);
    EXPECT_NE(latched(), moving) << "at rest a move past the deadband moves the latch";
    EXPECT_LE(large - latched(), deadband) << "the latch ends within the deadband of the tree it follows";
    inst.Shutdown();
}

// A pose whose tree fills the pool: at rest the splits the pool cannot serve roll back, and nothing may
// take the tree apart to make room, or the live count falls, the tree grows back and the rest never
// ends. Nothing reads occupancy here: the near-bias, the keep step and the pool-pressure scale are off.
TEST_F(CBTRestSettle, AFullPoolKeepsItsTreeAtRest)
{
    Shaping gateOnly;
    gateOnly.SplitPx = kFullPoolSplitPx;
    gateOnly.NearBias = false;
    gateOnly.KeepOcc = 0.0f;
    gateOnly.PoolPressure = 0u;
    const RestTrace t = MotionThenRest(kArrivals[1].Rest, gateOnly);
    std::printf("[fullpool] motion live %u (occupancy %.3f), rest minimum %u, rest end %u, quiet %d\n", t.MotionLive,
                static_cast<float>(t.MotionLive) / static_cast<float>(kDefaultBisectorPoolSize), t.RestLiveMin,
                t.RestLiveEnd, t.Quiet ? 1 : 0);
    ASSERT_GE(static_cast<float>(t.RestLiveEnd) / static_cast<float>(kDefaultBisectorPoolSize), 0.90f)
        << "the rest tree must fill the pool for the test to say anything";
    EXPECT_TRUE(t.Quiet) << "the tree must stop changing at rest";
    // A tree taken apart would lose far more than 5% of its facets.
    EXPECT_GE(static_cast<float>(t.RestLiveMin), 0.95f * static_cast<float>(t.MotionLive))
        << "the rest must not take the tree apart";
}

// A pool on the near-bias contention ramp, where the contention moves with the live count: the walk
// pose's rest tree sits past the ramp's foot and under its top. At rest the planar classify reads the
// live count latched at the stop
// (CBT_WQ_REST_LIVE_COUNT), so the contention's thresholds hold still and the tree goes quiet; read live,
// a facet on a threshold the tree's own size sets splits and merges on alternate updates forever.
TEST_F(CBTRestSettle, AContendedPoolGoesQuietAtRest)
{
    Shaping contended;
    contended.SplitPx = kContendedSplitPx;
    contended.KeepOcc = 0.0f;
    contended.PoolPressure = 0u;
    const RestTrace t = MotionThenRest(kArrivals[1].Rest, contended);
    const float occupancy = static_cast<float>(t.RestLiveEnd) / static_cast<float>(kDefaultBisectorPoolSize);
    std::printf("[contended] motion live %u, rest minimum %u, rest end %u (occupancy %.3f), quiet %d\n",
                t.MotionLive, t.RestLiveMin, t.RestLiveEnd, occupancy, t.Quiet ? 1 : 0);
    ASSERT_GT(occupancy, kContentionFreeOccupancy) << "the rest tree must sit on the contention ramp";
    ASSERT_LT(occupancy, 0.90f) << "the rest tree must sit under the contention ramp's top";
    EXPECT_TRUE(t.Quiet) << "the tree must stop changing at rest";
}
