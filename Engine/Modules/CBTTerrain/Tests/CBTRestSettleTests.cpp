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
// The pool-pressure controllers (the off-frustum keep step, the near-bias contention) keep state across a path.
// A test turns them off or stays below where they engage, so the rest rule alone is under test, or it puts the
// pool under the one controller it tests (AContendedPoolGoesQuietAtRest, APoolUnderTheKeepStepGoesQuietAtRest).
//
// The projected-area metric, which decides the planar terrains without a height pyramid, keeps its motion
// band at rest; the TheAreaMetric tests bind the relief through the atlas source and assert that it goes
// quiet and that the band holds under a small move.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "CBTTerrain/CBTDemandTuning.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTTerrain/CBTPoolCensus.h"
#include "CBTTerrain/CBTTreeAudit.h"
#include "CBTTerrain/CBTUpdateRestGate.h"
#include "CBTDiamond.h"
#include "CBTReliefHeightSource.h"
#include "CBTTestHarness.h"
#include "Logger/Logger.h"
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
// The fraction of the largest live count the smallest must reach over the motion stage's second half,
// where the camera holds still and the tree has converged.
constexpr float kStillLiveBand = 0.98f;
// The updates a held stage under the keep step may take to stop changing, out of kStageUpdates.
constexpr uint32_t kKeepSettleUpdates = 120u;
// The updates the turned and back stages of both keep-path poses may take together to stop changing.
constexpr uint32_t kStopSettleUpdates = 150u;
// The pressure step the gate test's arrival climbs to in motion: one notch survives the update that drains
// the pool under the keep ceiling, so the rest starts above pressure step 0.
constexpr int32_t kArrivalPressureStep = 2;
// The contention ramp's lower bound (cbt_layout.glsl CBT_NEAR_BIAS_CONTENTION_LO): below it the
// near-bias is exactly 1.
constexpr float kContentionFreeOccupancy = 0.50f;
// The rest updates the area metric may take to go quiet after a converged motion tree. At rest the area
// metric keeps the motion thresholds, so a converged motion tree is already a rest tree; the bound catches
// a rest rule that keeps changing the tree.
constexpr uint32_t kAreaMetricRestUpdates = 24u;
// A camera that moves 1 cm and back on alternate updates: far inside the motion band of every facet.
constexpr float kJiggleM = 0.01f;
constexpr uint32_t kJiggleUpdates = 48u;
constexpr uint32_t kJiggleCounted = 32u;
// The water plane the render feature sends for an authored opaque surface: the margin under the surface where
// the seabed still refines (CBTRenderFeature.cpp kOpaqueWaterMarginM) and the coarsen factor of a facet under it
// (kSubmergedCoarsen).
constexpr float kWaterMarginM = 2.0f;
constexpr float kSubmergedCoarsen = 8.0f;
// Water surfaces (world Y, m) under the shore poses' eye, 72.8 m. The ground on their eye planes spans 4.2 m to
// 49.9 m, so each surface puts shorelines on the eye plane beside the eye.
constexpr float kShoreWaterY[] = {26.0f, 30.0f, 34.0f, 38.0f};
constexpr float kShoreYawsDeg[] = {0.0f, 45.0f, 90.0f, 135.0f, 180.0f, 225.0f, 270.0f, 315.0f};

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

// A view along the ground from 30 m above the middle of the terrain, turned to `yawDeg`.
Pose ShorePose(float yawDeg)
{
    return Above(256.0f, 30.0f, 256.0f, yawDeg, -4.0f);
}

// The demand shaping a test runs with. The default is the shipped set at kSplitPx; the pool-pressure tests
// turn single controllers off so the one under test is the only reader of the pool's occupancy.
struct Shaping
{
    float SplitPx = kSplitPx;
    bool NearBias = true;               // the near-bias contention ramp
    float KeepOcc = kOffFrustumKeepOcc; // 0 leaves the planar off-frustum field ungated (no keep step)
    uint32_t PoolPressure = 1u;         // 0 holds the pool-pressure scale at 1x
    bool AreaMetric = false;            // the relief through the atlas source, where the area metric decides
    std::optional<float> WaterY;        // the water surface's world Y; empty sends no water plane
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
    if (shaping.WaterY)
    {
        p.WaterPlane[0] = *shaping.WaterY;
        p.WaterPlane[1] = kWaterMarginM;
        p.WaterPlane[2] = kSubmergedCoarsen;
    }
}

// The relief a test runs on: the unified height texture, where the planar criterion decides, or with
// Shaping::AreaMetric the same relief through the atlas source, where the projected-area metric decides.
class ShapedRelief
{
  public:
    ShapedRelief(IDevice& device, CBTInstance& instance, const Shaping& shaping)
    {
        if (shaping.AreaMetric)
            m_Atlas.emplace(device, instance, kReliefDim + 1u);
        else
            m_Unified.emplace(device, instance, kHeightScale, kReliefDim);
    }

    // Whether the relief's height source is bound. Unbound, the lattice is flat.
    bool IsArmed() const { return m_Atlas ? m_Atlas->IsArmed() : m_Unified && m_Unified->IsArmed(); }

    // Writes the atlas geometry into `p` when the relief comes through the atlas.
    void Apply(CBTFrameParams& p) const
    {
        if (m_Atlas)
            m_Atlas->Apply(p);
    }

  private:
    std::optional<ReliefHeightSource> m_Unified;
    std::optional<ReliefAtlasSource> m_Atlas;
};

// A motion stage then a rest stage at one pose: the live count the motion converged to, the lowest live
// count any rest update left, and whether the last kQuietUpdates rest updates split and merged nothing.
struct RestTrace
{
    uint32_t MotionLive = 0;
    uint32_t RestLiveMin = 0;
    uint32_t RestLiveEnd = 0;
    uint32_t StillLiveMin = std::numeric_limits<uint32_t>::max(); // the live count over the motion stage's second half
    uint32_t StillLiveMax = 0;
    uint32_t RestStillMin = std::numeric_limits<uint32_t>::max(); // the live count over the rest stage's second half
    uint32_t RestStillMax = 0;
    int32_t KeepStepMax = 0;            // the highest off-frustum keep step over both stages
    uint32_t RestUpdatesChanging = 0; // the rest updates up to the last one that split or merged anything
    bool Quiet = false;
};

// One stage of a held camera: where the tree rests and how long it took.
struct StageRest
{
    uint32_t Live = 0;
    int32_t KeepStep = 0;
    uint32_t UpdatesChanging = 0; // the stage's updates up to the last one that split or merged anything
    uint32_t SplitRuns = 0;       // the runs of consecutive updates that split anything
    bool Quiet = false;           // the last kQuietUpdates updates split and merged nothing
    int32_t ArrivalPressureStep = 0;       // the pressure step after the stage's moving update
    uint32_t RestPressureClimbs = 0;       // the rest updates that raised the pressure step
    uint32_t DownStepsAbovePressureZero = 0; // the rest updates that lowered the keep step after an
                                             // update that ran above pressure step 0
    int32_t ArrivalMergeDemand = 0;      // the merges the stage's moving update asked for
    int32_t ArrivalMergeCandidates = 0;  // the merge candidates the moving update counted
    int32_t RestMergeCandidatesMax = 0;  // the most merge candidates one rest update counted
};

// A keep-path walk: rest at a pose, turn the camera 180 degrees, turn back.
struct PathStages
{
    StageRest Rest;
    StageRest Turned;
    StageRest Back;
};

// The keep-path poses, under kFullPoolSplitPx: each comes under the keep step at rest, and its turned
// view rests at another keep step.
const Pose kKeepPathPoses[] = {Above(256.0f, 6.0f, 256.0f, 0.0f, -30.0f), Above(256.0f, 6.0f, 300.0f, 45.0f, -30.0f)};

Pose Turned(const Pose& pose)
{
    Pose turned = pose;
    turned.YawDeg += 180.0f;
    return turned;
}

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
            c.Settle = i == 0u ? kSettleMoved : kSettleLasted;
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
        const ShapedRelief relief(*m_Device, inst, shaping);
        EXPECT_TRUE(relief.IsArmed()) << "the relief bound no height source, so the lattice is flat";
        CBTClassifyDesc c = ScreenClassify(shaping);
        CBTFrameParams p;
        BuildParams(p, pose, shaping);
        relief.Apply(p);
        c.Settle = 0u;
        RestTrace t;
        for (uint32_t i = 0; i < kStageUpdates; ++i)
        {
            RunUpdate(inst, c, p);
            t.KeepStepMax = std::max(t.KeepStepMax, inst.ReadWorkQueueCounter(kWQOffFrustumKeepStep));
            if (i < kStageUpdates / 2u)
                continue;
            const uint32_t live = inst.ReadTessellationStats().LiveCount;
            t.StillLiveMin = std::min(t.StillLiveMin, live);
            t.StillLiveMax = std::max(t.StillLiveMax, live);
        }
        t.MotionLive = inst.ReadTessellationStats().LiveCount;
        t.RestLiveMin = t.MotionLive;
        c.Settle = kSettleLasted;
        uint32_t quiet = 0;
        for (uint32_t i = 0; i < kStageUpdates; ++i)
        {
            RunUpdate(inst, c, p);
            const CBTTessellationStats s = inst.ReadTessellationStats();
            t.RestLiveMin = std::min(t.RestLiveMin, s.LiveCount);
            t.RestLiveEnd = s.LiveCount;
            t.KeepStepMax = std::max(t.KeepStepMax, s.OffFrustumKeepStep);
            if (i >= kStageUpdates / 2u)
            {
                t.RestStillMin = std::min(t.RestStillMin, s.LiveCount);
                t.RestStillMax = std::max(t.RestStillMax, s.LiveCount);
            }
            const bool changed = s.SplitServed != 0 || s.MergeServed != 0;
            quiet = changed ? 0u : quiet + 1u;
            t.RestUpdatesChanging = changed ? i + 1u : t.RestUpdatesChanging;
        }
        t.Quiet = quiet >= kQuietUpdates;
        EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << "the rest tree must be conforming";
        inst.Shutdown();
        return t;
    }

    // Holds the camera at `pose` for kStageUpdates updates under `shaping`, the first with Settle 0.
    StageRest HoldStage(CBTInstance& inst, const Pose& pose, const Shaping& shaping)
    {
        CBTClassifyDesc c = ScreenClassify(shaping);
        CBTFrameParams p;
        BuildParams(p, pose, shaping);
        StageRest out;
        uint32_t quietUpdates = 0;
        bool splitting = false;
        uint32_t splitRuns = 0;
        int32_t pressureStep = 0;
        int32_t keepStep = 0;
        for (uint32_t i = 0; i < kStageUpdates; ++i)
        {
            const bool moving = i == 0u;
            c.Settle = moving ? kSettleMoved : kSettleLasted;
            RunUpdate(inst, c, p);
            const CBTTessellationStats s = inst.ReadTessellationStats();
            splitRuns += (s.SplitServed != 0 && !splitting) ? 1u : 0u;
            splitting = s.SplitServed != 0;
            out.ArrivalPressureStep = moving ? s.PressureStep : out.ArrivalPressureStep;
            out.ArrivalMergeDemand = moving ? s.MergeDemand : out.ArrivalMergeDemand;
            out.ArrivalMergeCandidates = moving ? s.MergeCandidates : out.ArrivalMergeCandidates;
            out.RestMergeCandidatesMax = moving ? 0 : std::max(out.RestMergeCandidatesMax, s.MergeCandidates);
            out.RestPressureClimbs += (!moving && s.PressureStep > pressureStep) ? 1u : 0u;
            out.DownStepsAbovePressureZero += (!moving && s.OffFrustumKeepStep < keepStep && pressureStep > 0) ? 1u : 0u;
            pressureStep = s.PressureStep;
            keepStep = s.OffFrustumKeepStep;
            const bool changed = s.SplitServed != 0 || s.MergeServed != 0;
            quietUpdates = changed ? 0u : quietUpdates + 1u;
            out.UpdatesChanging = changed ? i + 1u : out.UpdatesChanging;
        }
        const CBTTessellationStats s = inst.ReadTessellationStats();
        out.Live = s.LiveCount;
        out.KeepStep = s.OffFrustumKeepStep;
        out.SplitRuns = splitRuns;
        out.Quiet = quietUpdates >= kQuietUpdates;
        return out;
    }

    // Rests at `pose`, turns the camera 180 degrees and turns back, each stage held for kStageUpdates.
    PathStages WalkPath(const Pose& pose, const Shaping& shaping)
    {
        CBTInstance inst;
        EXPECT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(inst.InitializeRoots(kDomainPlanar));
        const ShapedRelief relief(*m_Device, inst, shaping);
        EXPECT_TRUE(relief.IsArmed()) << "the relief bound no height source, so the lattice is flat";
        PathStages out;
        out.Rest = HoldStage(inst, pose, shaping);
        out.Turned = HoldStage(inst, Turned(pose), shaping);
        out.Back = HoldStage(inst, pose, shaping);
        EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << "the tree must be conforming";
        inst.Shutdown();
        return out;
    }

    // Converges the motion tree at `pose`, then moves the eye kJiggleM to one side and back on alternate
    // updates, every update with moving inputs (Settle 0). Returns the splits and merges served over the
    // last kJiggleCounted updates.
    uint64_t JiggleServed(const Pose& pose, const Shaping& shaping)
    {
        CBTInstance inst;
        EXPECT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(inst.InitializeRoots(kDomainPlanar));
        const ShapedRelief relief(*m_Device, inst, shaping);
        EXPECT_TRUE(relief.IsArmed()) << "the relief bound no height source, so the lattice is flat";
        CBTClassifyDesc c = ScreenClassify(shaping);
        CBTFrameParams here;
        CBTFrameParams there;
        Pose moved = pose;
        moved.EyeX += kJiggleM;
        BuildParams(here, pose, shaping);
        BuildParams(there, moved, shaping);
        relief.Apply(here);
        relief.Apply(there);
        c.Settle = 0u;
        for (uint32_t i = 0; i < kStageUpdates; ++i)
            RunUpdate(inst, c, here);
        uint64_t served = 0;
        for (uint32_t i = 0; i < kJiggleUpdates; ++i)
        {
            RunUpdate(inst, c, (i & 1u) == 0u ? there : here);
            const CBTTessellationStats s = inst.ReadTessellationStats();
            if (i + kJiggleCounted >= kJiggleUpdates)
                served += static_cast<uint64_t>(s.SplitServed) + static_cast<uint64_t>(s.MergeServed);
        }
        EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << "the tree must be conforming";
        inst.Shutdown();
        return served;
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

// #5355: with half the field behind the eye, the keep step holds a full pool still in both stages. If that
// field leaves in one step, occupancy falls under the hold band and the field refills the pool on every cycle.
// The first pose cycles under the motion threshold on a shared excess, the second under the rest threshold.
TEST_F(CBTRestSettle, APoolUnderTheKeepStepGoesQuietAtRest)
{
    Shaping pressured;
    pressured.SplitPx = kFullPoolSplitPx;
    for (const Pose& pose : {Above(256.0f, 2.0f, 256.0f, 0.0f, -4.0f), Above(256.0f, 2.0f, 300.0f, 45.0f, -4.0f)})
    {
        SCOPED_TRACE(pose.YawDeg);
        const RestTrace t = MotionThenRest(pose, pressured);
        std::printf("[keep-step] live over the stages' second halves %u to %u and %u to %u, keep step max %d, quiet %d\n",
                    t.StillLiveMin, t.StillLiveMax, t.RestStillMin, t.RestStillMax, t.KeepStepMax, t.Quiet ? 1 : 0);
        ASSERT_GT(t.KeepStepMax, 0) << "the pool must come under the keep step for the test to say anything";
        EXPECT_TRUE(t.Quiet) << "the tree must stop changing at rest";
        EXPECT_GE(static_cast<float>(t.StillLiveMin), kStillLiveBand * static_cast<float>(t.StillLiveMax))
            << "the live count must hold still under a still camera, not refill and drain";
        EXPECT_GE(static_cast<float>(t.RestStillMin), kStillLiveBand * static_cast<float>(t.RestStillMax))
            << "the live count must hold still at rest, not refill and drain";
    }
}

// #5355: under the keep step the tree at rest is a function of the pose. The camera rests, turns around and back;
// the turned rest matches a fresh arrival there and the return matches the first rest, in keep step and live count.
// Several keep steps fit the hold band, so a step that rests wherever the path left it fails here.
TEST_F(CBTRestSettle, AKeepStepTreeDependsOnThePoseNotThePath)
{
    Shaping pressured;
    pressured.SplitPx = kFullPoolSplitPx;
    for (const Pose& pose : kKeepPathPoses)
    {
        SCOPED_TRACE(pose.YawDeg);
        const PathStages path = WalkPath(pose, pressured);
        const StageRest& rest = path.Rest;
        const StageRest& turn = path.Turned;
        const StageRest& back = path.Back;
        CBTInstance fresh;
        ASSERT_TRUE(fresh.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(fresh.InitializeRoots(kDomainPlanar));
        const ShapedRelief freshRelief(*m_Device, fresh, pressured);
        ASSERT_TRUE(freshRelief.IsArmed());
        const StageRest direct = HoldStage(fresh, Turned(pose), pressured);
        fresh.Shutdown();
        std::printf("[keep-path] rest %u (keep step %d, %u updates), turned %u (%d, %u), back %u (%d, %u), "
                    "turned pose direct %u (%d, %u)\n",
                    rest.Live, rest.KeepStep, rest.UpdatesChanging, turn.Live, turn.KeepStep, turn.UpdatesChanging,
                    back.Live, back.KeepStep, back.UpdatesChanging, direct.Live, direct.KeepStep,
                    direct.UpdatesChanging);
        ASSERT_GT(rest.KeepStep, 0) << "the pool must come under the keep step for the test to say anything";
        for (const StageRest* stage : {&rest, &turn, &back, &direct})
        {
            EXPECT_TRUE(stage->Quiet) << "every stage must stop changing at rest";
            EXPECT_LE(stage->UpdatesChanging, kKeepSettleUpdates) << "every stage must settle within the bound";
        }
        EXPECT_EQ(back.KeepStep, rest.KeepStep) << "the return must rest at the keep step the first rest found";
        EXPECT_GE(static_cast<float>(std::min(rest.Live, back.Live)),
                  kStillLiveBand * static_cast<float>(std::max(rest.Live, back.Live)))
            << "the second rest at the pose must keep the tree the first rest kept, not what the turn left";
        EXPECT_EQ(turn.KeepStep, direct.KeepStep) << "the turned pose must rest at its own keep step";
        EXPECT_GE(static_cast<float>(std::min(turn.Live, direct.Live)),
                  kStillLiveBand * static_cast<float>(std::max(turn.Live, direct.Live)))
            << "the turned pose must keep the tree a fresh arrival there keeps";
    }
}

// #5421, #5416: a stop under the keep step costs one settle. The rest starts one keep step down, so the probe
// of that step refines while the old view merges, in the same split run.
TEST_F(CBTRestSettle, AStopSettlesInOneSplitRun)
{
    Shaping pressured;
    pressured.SplitPx = kFullPoolSplitPx;
    uint32_t stopUpdates = 0;
    for (uint32_t i = 0; i < 2u; ++i)
    {
        SCOPED_TRACE(i);
        const PathStages path = WalkPath(kKeepPathPoses[i], pressured);
        std::printf("[stop] pose %u: turned %u updates, %u split runs; back %u updates, %u split runs\n", i + 1u,
                    path.Turned.UpdatesChanging, path.Turned.SplitRuns, path.Back.UpdatesChanging, path.Back.SplitRuns);
        ASSERT_GT(path.Rest.KeepStep, 0) << "the pool must come under the keep step for the test to say anything";
        EXPECT_TRUE(path.Turned.Quiet && path.Back.Quiet) << "every stage must stop changing at rest";
        stopUpdates += path.Turned.UpdatesChanging + path.Back.UpdatesChanging;
        if (i != 0u)
            continue;
        EXPECT_EQ(path.Turned.SplitRuns, 1u) << "the probe must refine in the stop's own split run";
        EXPECT_EQ(path.Back.SplitRuns, 1u) << "the probe must refine in the stop's own split run";
    }
    EXPECT_LE(stopUpdates, kStopSettleUpdates) << "the stops must settle within the bound";
}

// #5563: at rest under keep step 24 the pressure step does not climb. The rest tree settles under the keep
// ceiling, where the pressure step recovers to 0, so a climb during the keep search only coarsens the view
// and hides the merge-free update the keep step's up rule reads.
TEST_F(CBTRestSettle, ThePressureStepYieldsToTheKeepStepAtRest)
{
    Shaping pressured;
    pressured.SplitPx = kFullPoolSplitPx;
    for (const Pose& pose : kKeepPathPoses)
    {
        SCOPED_TRACE(pose.YawDeg);
        const PathStages path = WalkPath(pose, pressured);
        for (const StageRest* stage : {&path.Rest, &path.Turned, &path.Back})
        {
            ASSERT_GT(stage->KeepStep, 0) << "the pool must come under the keep step for the test to say anything";
            ASSERT_LT(stage->KeepStep, int32_t(kOffFrustumKeepSteps)) << "the keep step must be able to rise";
            EXPECT_EQ(stage->RestPressureClimbs, 0u) << "the pressure step climbed during the keep search";
            EXPECT_TRUE(stage->Quiet) << "every stage must stop changing at rest";
        }
    }
}

// The early up proof reads the merge candidates of a rest update. A moving update counts none, so the camera in
// motion pays no atomic per merging leaf.
TEST_F(CBTRestSettle, MergeCandidatesAreCountedOnlyAtRest)
{
    Shaping pressured;
    pressured.SplitPx = kFullPoolSplitPx;
    const PathStages path = WalkPath(kKeepPathPoses[0], pressured);
    const StageRest& turned = path.Turned;
    ASSERT_GT(turned.ArrivalMergeDemand, 0) << "the turn must leave leaves to merge for the test to say anything";
    EXPECT_EQ(turned.ArrivalMergeCandidates, 0) << "a moving update counted merge candidates";
    EXPECT_GT(turned.RestMergeCandidatesMax, 0) << "a rest update with merges to make counted no merge candidates";
}

// A rest steps the keep step down only at pressure step 0: a tree measured above it is smaller than the settled tree.
// The arrival fills the pool in motion until the pressure step climbs, then a coarser target drains the pool under
// the keep ceiling in motion, so the rest starts under the ceiling with the pressure step raised.
TEST_F(CBTRestSettle, AKeepStepStepsDownOnlyAtPressureStepZero)
{
    Shaping pressured;
    pressured.SplitPx = kFullPoolSplitPx;
    Shaping coarser;
    coarser.SplitPx = kContendedSplitPx;
    const Pose& pose = kKeepPathPoses[0];
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
    const ShapedRelief relief(*m_Device, inst, pressured);
    ASSERT_TRUE(relief.IsArmed());

    CBTClassifyDesc filling = ScreenClassify(pressured);
    filling.Settle = 0u;
    CBTFrameParams fillingParams;
    BuildParams(fillingParams, pose, pressured);
    CBTTessellationStats stats = inst.ReadTessellationStats();
    for (uint32_t i = 0; i < kStageUpdates && stats.PressureStep < kArrivalPressureStep; ++i)
    {
        RunUpdate(inst, filling, fillingParams);
        stats = inst.ReadTessellationStats();
    }
    ASSERT_GE(stats.PressureStep, kArrivalPressureStep) << "the arrival must climb the pressure step in motion";

    CBTClassifyDesc draining = ScreenClassify(coarser);
    draining.Settle = 0u;
    CBTFrameParams drainingParams;
    BuildParams(drainingParams, pose, coarser);
    for (uint32_t i = 0; i < kStageUpdates && stats.LiveCount >= uint32_t(kOffFrustumKeepOcc * float(stats.PoolSize));
         ++i)
    {
        RunUpdate(inst, draining, drainingParams);
        stats = inst.ReadTessellationStats();
    }
    ASSERT_GT(stats.OffFrustumKeepStep, 0) << "the keep step must have a step to take down";

    const StageRest rest = HoldStage(inst, pose, coarser);
    EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << "the tree must be conforming";
    inst.Shutdown();
    ASSERT_GT(rest.ArrivalPressureStep, 0) << "the rest must start above pressure step 0 for the test to say anything";
    EXPECT_EQ(rest.DownStepsAbovePressureZero, 0u) << "the keep step stepped down above pressure step 0";
}

// #4823: the projected-area metric (here the atlas source) comes to rest at a still camera within a bounded
// number of updates. Its near-field merge measures the child's own edge, so one threshold for the split and
// the merge would flip the facets that straddle it.
TEST_F(CBTRestSettle, TheAreaMetricSettlesAtRest)
{
    Shaping areaMetric;
    areaMetric.AreaMetric = true;
    for (const Arrival& a : kArrivals)
    {
        SCOPED_TRACE(a.Label);
        const RestTrace t = MotionThenRest(a.Rest, areaMetric);
        Logger::Log::Info("[area-rest:{}] motion live {}, rest minimum {}, rest end {}, rest updates changing {}, quiet {}",
                          a.Label, t.MotionLive, t.RestLiveMin, t.RestLiveEnd, t.RestUpdatesChanging, t.Quiet ? 1 : 0);
        ASSERT_GT(t.MotionLive, 10000u) << "the pose barely refined; the test would prove nothing";
        EXPECT_TRUE(t.Quiet) << "the tree must stop changing at rest";
        EXPECT_LE(t.RestUpdatesChanging, kAreaMetricRestUpdates) << "the rest must settle within a bound";
    }
}

// A shoreline across the eye plane. The water plane coarsens a facet whose corners all lie more than the margin
// under it, and a near-field pair merges into its parent. A pair under the water can have a parent with one corner
// above it, so the area metric must still come to rest when the parent does not take the pair's coarse threshold.
TEST_F(CBTRestSettle, TheAreaMetricSettlesAtRestOnAShore)
{
    for (float yawDeg : kShoreYawsDeg)
    {
        for (float waterY : kShoreWaterY)
        {
            // No off-frustum keep band: the off-view children of a diamond merge, as they do once the band has
            // narrowed at a busy pool, so the shore's near-field pairs alone decide whether the diamond merges.
            Shaping shore;
            shore.AreaMetric = true;
            shore.KeepOcc = 0.0f;
            shore.WaterY = waterY;
            SCOPED_TRACE(::testing::Message() << "yaw " << yawDeg << ", water at " << waterY << " m");
            const RestTrace t = MotionThenRest(ShorePose(yawDeg), shore);
            Logger::Log::Info("[area-shore:yaw {}, water at {} m] motion live {}, rest end {}, rest updates changing {}, "
                              "quiet {}",
                              yawDeg, waterY, t.MotionLive, t.RestLiveEnd, t.RestUpdatesChanging, t.Quiet ? 1 : 0);
            ASSERT_GT(t.MotionLive, 10000u) << "the pose barely refined; the test would prove nothing";
            EXPECT_TRUE(t.Quiet) << "the tree must stop changing at rest on a shore";
            EXPECT_LE(t.RestUpdatesChanging, kAreaMetricRestUpdates) << "the rest must settle within a bound";
        }
    }
}

// The area metric's motion band: while the camera moves, a facet splits well above the threshold its
// children merge below, so a camera that moves a centimeter and back does not split and merge the same
// facets on every update. The walk pose holds the near field under the eye, where the split and the merge
// measure different edges. (At the overview a centimeter moves a few facets across the eye plane, from the
// near-field rule to the area metric and back, which splits and merges them whatever the band.)
TEST_F(CBTRestSettle, TheAreaMetricDoesNotChurnUnderASmallMove)
{
    Shaping areaMetric;
    areaMetric.AreaMetric = true;
    const Arrival& walk = kArrivals[1];
    const uint64_t served = JiggleServed(walk.Rest, areaMetric);
    Logger::Log::Info("[area-jiggle:{}] splits and merges served over the last {} updates: {}", walk.Label,
                      kJiggleCounted, served);
    EXPECT_EQ(served, 0u) << "a centimeter of camera motion must not churn the mesh";
}

// A census update at rest counts each live leaf once by level, in view or off view, and each off-view leaf in one
// class. It serves nothing, so it counts the rest tree, and a second census update gives the same counts.
TEST_F(CBTRestSettle, TheCensusCountsTheRestTreeAndLeavesItAsItIs)
{
    Shaping pressured;
    pressured.SplitPx = kFullPoolSplitPx;
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
    const ShapedRelief relief(*m_Device, inst, pressured);
    ASSERT_TRUE(relief.IsArmed());
    CBTClassifyDesc c = ScreenClassify(pressured);
    CBTFrameParams p;
    BuildParams(p, Above(256.0f, 2.0f, 256.0f, 0.0f, -4.0f), pressured);
    relief.Apply(p);
    for (uint32_t i = 0; i < 2u * kStageUpdates; ++i)
    {
        c.Settle = i < kStageUpdates ? kSettleMoved : kSettleLasted;
        RunUpdate(inst, c, p);
    }
    const uint64_t restSignature = TreeSignature(ReadTreeSnapshot(inst, false));

    c.Census = 1u;
    RunUpdate(inst, c, p);
    const CBTTessellationStats stats = inst.ReadTessellationStats();
    const CBTPoolCensus census = inst.ReadPoolCensus();
    const CBTTreeSnapshot snapshot = ReadTreeSnapshot(inst, false);
    ASSERT_GT(stats.OffFrustumKeepStep, 0) << "the pool must come under the keep step for the test to say anything";
    EXPECT_EQ(stats.SplitServed, 0);
    EXPECT_EQ(stats.MergeServed, 0);
    EXPECT_EQ(TreeSignature(snapshot), restSignature) << "the census update must leave the rest tree as it is";

    CBTPoolCensus expected;
    uint32_t mergeWanted = 0;
    for (uint32_t s = 0; s < snapshot.PoolSize; ++s)
    {
        const uint64_t h = snapshot.HeapIds[s];
        if (h == 0u)
            continue;
        const uint32_t level = static_cast<uint32_t>(std::bit_width(h)) - 1u;
        const bool inView = (snapshot.Bisectors[s].Flags & kFlagVisible) != 0u;
        ++(inView ? expected.InViewByLevel : expected.OffViewByLevel)[level];
        mergeWanted += !inView && snapshot.Bisectors[s].BisectorState == kStateSimplify ? 1u : 0u;
    }
    EXPECT_EQ(census.InViewByLevel, expected.InViewByLevel);
    EXPECT_EQ(census.OffViewByLevel, expected.OffViewByLevel);
    EXPECT_EQ(census.OffViewByClass[static_cast<uint32_t>(CBTCensusClass::MergeWanted)], mergeWanted);

    uint32_t offView = 0;
    for (uint32_t count : census.OffViewByLevel)
        offView += count;
    uint32_t classified = 0;
    for (uint32_t count : census.OffViewByClass)
        classified += count;
    std::printf("[census] live %u, off view %u: merge %u, behind %u, beside %u, across %u, margin %u, horizon %u\n",
                stats.LiveCount, offView, census.OffViewByClass[0], census.OffViewByClass[1], census.OffViewByClass[2],
                census.OffViewByClass[3], census.OffViewByClass[4], census.OffViewByClass[5]);
    EXPECT_GT(offView, stats.LiveCount / 4u) << "the ground behind the eye must be live and off view";
    EXPECT_EQ(classified, offView) << "every off-view leaf takes exactly one class";
    EXPECT_GT(census.OffViewByClass[static_cast<uint32_t>(CBTCensusClass::BehindEye)], 0u);
    EXPECT_EQ(census.OffViewByClass[static_cast<uint32_t>(CBTCensusClass::BehindHorizon)], 0u)
        << "a planar terrain has no horizon class";

    RunUpdate(inst, c, p);
    const CBTPoolCensus again = inst.ReadPoolCensus();
    EXPECT_EQ(again.InViewByLevel, census.InViewByLevel) << "each census update must count from zero";
    EXPECT_EQ(again.OffViewByLevel, census.OffViewByLevel);
    EXPECT_EQ(again.OffViewByClass, census.OffViewByClass);
    inst.Shutdown();
}

namespace
{

// The rest tree of one behind-eye release arm (CBTClassifyDesc::BehindEye).
struct ReleaseArm
{
    CBTTessellationStats Stats;
    CBTPoolCensus Census;
    CBTPoolCensus SecondCensus;
    std::vector<uint64_t> InView; // the heap ids of the leaves in view, sorted
};

// A split threshold under which the area metric's rest tree at kReleasePose goes over the keep ceiling.
constexpr float kReleaseSplitPx = 1.75f;

// The area metric at a pose that fills the pool, with the pool-pressure scale held at 1x so that both arms ask
// the view for the same tree.
Shaping ReleaseShaping()
{
    Shaping s;
    s.SplitPx = kReleaseSplitPx;
    s.AreaMetric = true;
    s.PoolPressure = 0u;
    return s;
}

const Pose kReleasePose = Above(256.0f, 2.0f, 256.0f, 0.0f, -4.0f);

uint32_t CensusClass(const CBTPoolCensus& census, CBTCensusClass c)
{
    return census.OffViewByClass[static_cast<uint32_t>(c)];
}

// The leaves of `finer` (sorted heap ids) that have a strict ancestor among the leaves of `coarser`.
size_t CountCoarsened(const std::vector<uint64_t>& finer, const std::vector<uint64_t>& coarser)
{
    size_t coarsened = 0;
    for (uint64_t id : finer)
        for (uint64_t parent = id >> 1u; parent != 0u; parent >>= 1u)
            if (std::binary_search(coarser.begin(), coarser.end(), parent))
            {
                ++coarsened;
                break;
            }
    return coarsened;
}

// The legs of `series` longer than `amplitude`: a leg runs from one turning point to the next.
uint32_t CountSwings(const std::vector<float>& series, float amplitude)
{
    uint32_t swings = 0;
    int direction = 0; // +1 on a rise, -1 on a fall, 0 before the first leg
    float low = series.empty() ? 0.0f : series.front();
    float high = low;
    for (float value : series)
    {
        low = direction > 0 ? low : std::min(low, value);
        high = direction < 0 ? high : std::max(high, value);
        if (direction <= 0 && value - low > amplitude)
        {
            ++swings;
            direction = 1;
            high = value;
        }
        else if (direction >= 0 && high - value > amplitude)
        {
            ++swings;
            direction = -1;
            low = value;
        }
    }
    return swings;
}

} // namespace

class CBTBehindEyeRelease : public CBTRestSettle
{
  protected:
    // Moves to `pose` and holds it: kStageUpdates in motion, then kStageUpdates at rest.
    void HoldShaped(CBTInstance& inst, const ShapedRelief& relief, CBTClassifyDesc& c, const Pose& pose)
    {
        CBTFrameParams p;
        BuildParams(p, pose, ReleaseShaping());
        relief.Apply(p);
        for (uint32_t i = 0; i < 2u * kStageUpdates; ++i)
        {
            c.Settle = i < kStageUpdates ? kSettleMoved : kSettleLasted;
            RunUpdate(inst, c, p);
        }
    }

    // A camera that moves in kStrokes strokes along +Z from `eyeZ`, `aboveM` over the ground, with `restUpdates` at
    // rest after each. The rest gate sets each update's Settle from a clock at 60 updates per second. Returns how
    // often the keep step left `restStep` and came back to it.
    uint32_t StopAndGo(CBTInstance& inst, const ShapedRelief& relief, CBTClassifyDesc& c, CBTUpdateRestGate& gate,
                       float& eyeZ, float aboveM, int32_t restStep, uint32_t restUpdates)
    {
        constexpr float kWalkStepM = 0.125f; // per moving update
        uint32_t returns = 0;
        bool offRest = false;
        for (uint32_t stroke = 0; stroke < kStrokes; ++stroke)
            for (uint32_t i = 0; i < kStrokeUpdates + restUpdates; ++i)
            {
                eyeZ += i < kStrokeUpdates ? kWalkStepM : 0.0f;
                const bool atRestStep = RunGated(inst, relief, c, gate, Above(kReleasePose.EyeX, aboveM, eyeZ, 0.0f, -4.0f),
                                                 ReleaseShaping())
                                            .OffFrustumKeepStep == restStep;
                returns += offRest && atRestStep ? 1u : 0u;
                offRest = !atRestStep;
            }
        return returns;
    }

    // One update at `pose` with the Settle the rest gate gives one 60 Hz tick later.
    CBTTessellationStats RunGated(CBTInstance& inst, const ShapedRelief& relief, CBTClassifyDesc& c,
                                  CBTUpdateRestGate& gate, const Pose& pose, const Shaping& shaping)
    {
        m_Clock += kUpdatePeriod;
        gate.CanSkip(std::span(reinterpret_cast<const uint8_t*>(&pose), sizeof(pose)), m_Clock);
        c.Settle = gate.Settle();
        CBTFrameParams p;
        BuildParams(p, pose, shaping);
        relief.Apply(p);
        RunUpdate(inst, c, p);
        return inst.ReadTessellationStats();
    }

    static constexpr uint32_t kStrokes = 12u;
    static constexpr uint32_t kStrokeUpdates = 8u;
    static constexpr std::chrono::microseconds kUpdatePeriod{16667};
    std::chrono::steady_clock::time_point m_Clock{};

    // The rest tree at kReleasePose, read with two census updates.
    ReleaseArm Settle(BehindEyeRelease release)
    {
        ReleaseArm arm;
        CBTInstance inst;
        EXPECT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(inst.InitializeRoots(kDomainPlanar));
        const ShapedRelief relief(*m_Device, inst, ReleaseShaping());
        EXPECT_TRUE(relief.IsArmed());
        CBTClassifyDesc c = ScreenClassify(ReleaseShaping());
        c.BehindEye = release;
        HoldShaped(inst, relief, c, kReleasePose);
        const CBTTreeSnapshot snapshot = ReadTreeSnapshot(inst, false);
        for (uint32_t s = 0; s < snapshot.PoolSize; ++s)
            if (snapshot.HeapIds[s] != 0u && (snapshot.Bisectors[s].Flags & kFlagVisible) != 0u)
                arm.InView.push_back(snapshot.HeapIds[s]);
        std::sort(arm.InView.begin(), arm.InView.end());

        CBTFrameParams p;
        BuildParams(p, kReleasePose, ReleaseShaping());
        relief.Apply(p);
        c.Census = 1u;
        RunUpdate(inst, c, p);
        arm.Stats = inst.ReadTessellationStats();
        arm.Census = inst.ReadPoolCensus();
        RunUpdate(inst, c, p);
        arm.SecondCensus = inst.ReadPoolCensus();
        std::printf("[release:%u] live %u, keep step %d, pressure %d, in view %zu: merge %u, behind %u, beside %u, "
                    "across %u, margin %u\n",
                    static_cast<uint32_t>(release), arm.Stats.LiveCount, arm.Stats.OffFrustumKeepStep, arm.Stats.PressureStep,
                    arm.InView.size(), CensusClass(arm.Census, CBTCensusClass::MergeWanted),
                    CensusClass(arm.Census, CBTCensusClass::BehindEye),
                    CensusClass(arm.Census, CBTCensusClass::BesideFrustum),
                    CensusClass(arm.Census, CBTCensusClass::AcrossEyePlane),
                    CensusClass(arm.Census, CBTCensusClass::InMarginFrustum));
        inst.Shutdown();
        return arm;
    }
};

// At rest the keep band releases the area metric's ground wholly behind the eye from the farthest octave in, only as
// far as the pool needs: the tree rests under the keep ceiling with fewer leaves, and no leaf in view goes coarser.
// Always Release lets the ground behind the eye merge down to the leaves whose parent reaches across the eye plane.
TEST_F(CBTBehindEyeRelease, TheGroundBehindTheEyeIsReleasedUnderTheKeepCeiling)
{
    const ReleaseArm keep = Settle(BehindEyeRelease::AlwaysKeep);
    const ReleaseArm band = Settle(BehindEyeRelease::ReleaseWhenNeeded);
    const ReleaseArm always = Settle(BehindEyeRelease::AlwaysRelease);
    ASSERT_EQ(keep.Stats.OffFrustumKeepStep, int32_t(kOffFrustumKeepSteps))
        << "the pose must rest at the last keep step for the test to say anything";
    ASSERT_GT(CensusClass(keep.Census, CBTCensusClass::BehindEye), 0u);
    EXPECT_LT(static_cast<double>(band.Stats.LiveCount) / kDefaultBisectorPoolSize, double(kOffFrustumKeepOcc));
    EXPECT_LT(band.Stats.LiveCount, keep.Stats.LiveCount);
    // A leaf wholly behind the eye keeps its level only where its parent reaches across the eye plane.
    EXPECT_LT(100u * CensusClass(always.Census, CBTCensusClass::BehindEye),
              CensusClass(keep.Census, CBTCensusClass::BehindEye))
        << "Always Release must let the ground behind the eye merge";
    EXPECT_LE(always.Stats.LiveCount, band.Stats.LiveCount);
    for (const ReleaseArm* arm : {&band, &always})
    {
        EXPECT_EQ(arm->Stats.SplitServed, 0);
        EXPECT_EQ(arm->Stats.MergeServed, 0);
        // The freed pool can let the view refine; no leaf in view may go coarser.
        EXPECT_EQ(CountCoarsened(keep.InView, arm->InView), 0u) << "the release must not merge a leaf in view";
        EXPECT_GE(arm->InView.size(), keep.InView.size());
    }
    for (const ReleaseArm* arm : {&keep, &band, &always})
    {
        EXPECT_EQ(arm->SecondCensus.InViewByLevel, arm->Census.InViewByLevel) << "a census at rest must repeat";
        EXPECT_EQ(arm->SecondCensus.OffViewByLevel, arm->Census.OffViewByLevel);
        EXPECT_EQ(arm->SecondCensus.OffViewByClass, arm->Census.OffViewByClass);
    }
}

// A walk at the pose that fills the pool. One keep step moves the release by one octave of distance, less than the
// keep step's hold band, so the step settles inside the band and the occupancy does not swing. A release that frees
// the whole class at one step moves more than the band and cycles: the pool fills, empties and refills.
TEST_F(CBTBehindEyeRelease, AWalkDoesNotCycleThePool)
{
    constexpr float kWalkStepM = 0.125f;       // per update, along the view (+Z)
    constexpr float kSwingOccupancy = 0.20f;   // the registered rule's swing
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
    const ShapedRelief relief(*m_Device, inst, ReleaseShaping());
    ASSERT_TRUE(relief.IsArmed());
    CBTClassifyDesc c = ScreenClassify(ReleaseShaping());
    c.BehindEye = BehindEyeRelease::ReleaseWhenNeeded;
    c.Settle = 0u;
    std::vector<float> occupancy;
    int32_t keepStepMax = 0;
    for (uint32_t i = 0; i < 3u * kStageUpdates; ++i)
    {
        CBTFrameParams p;
        BuildParams(p, Above(kReleasePose.EyeX, 2.0f, 200.0f + kWalkStepM * float(i), 0.0f, -4.0f),
                    ReleaseShaping());
        relief.Apply(p);
        RunUpdate(inst, c, p);
        const CBTTessellationStats s = inst.ReadTessellationStats();
        keepStepMax = std::max(keepStepMax, s.OffFrustumKeepStep);
        if (i >= kStageUpdates)
            occupancy.push_back(static_cast<float>(s.LiveCount) / static_cast<float>(kDefaultBisectorPoolSize));
    }
    const auto [low, high] = std::minmax_element(occupancy.begin(), occupancy.end());
    std::printf("[walk] keep step max %d, occupancy %.3f to %.3f, swings %u\n", keepStepMax, *low, *high,
                CountSwings(occupancy, kSwingOccupancy));
    ASSERT_GT(keepStepMax, 0) << "the walk must reach the keep ceiling for the test to say anything";
    EXPECT_EQ(CountSwings(occupancy, kSwingOccupancy), 0u) << "the release must not cycle the pool";
    inst.Shutdown();
}

// A camera that moves in strokes with short rests between them. A rest shorter than the probe's settle must leave
// the keep step at its rest value; a probe that the next stroke undoes is work that the move throws away.
TEST_F(CBTBehindEyeRelease, AShortRestDoesNotProbeTheKeepStep)
{
    constexpr uint32_t kRestUpdates[] = {2u, 6u, 11u};
    for (uint32_t restUpdates : kRestUpdates)
    {
        CBTInstance inst;
        ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
        const ShapedRelief relief(*m_Device, inst, ReleaseShaping());
        ASSERT_TRUE(relief.IsArmed());
        CBTClassifyDesc c = ScreenClassify(ReleaseShaping());
        c.BehindEye = BehindEyeRelease::ReleaseWhenNeeded;
        float eyeZ = kReleasePose.EyeZ;
        HoldShaped(inst, relief, c, Above(kReleasePose.EyeX, 2.0f, eyeZ, 0.0f, -4.0f));
        const int32_t restStep = inst.ReadTessellationStats().OffFrustumKeepStep;
        ASSERT_GT(restStep, 0) << "the walk must rest above keep step 0 for a probe to exist";
        ASSERT_LT(restStep, int32_t(kOffFrustumKeepSteps)) << "the walk must rest under the keep ceiling";
        ASSERT_LT(kUpdatePeriod * (restUpdates - 1u), CBTUpdateRestGate::kKeepProbeSettle);
        CBTUpdateRestGate gate;
        const uint32_t returns = StopAndGo(inst, relief, c, gate, eyeZ, 2.0f, restStep, restUpdates);
        std::printf("[stop-and-go] rest %u updates: rest step %d, returns to it %u\n", restUpdates, restStep, returns);
        EXPECT_EQ(returns, 0u) << "a rest of " << restUpdates << " updates must not probe the keep step";
        inst.Shutdown();
    }
}

// The same walk with rests longer than the settle, over like ground. Each rest searches; a probe of the step under the
// rest step that went over the ceiling is not repeated while the headroom is what it was, so the step stays put.
TEST_F(CBTBehindEyeRelease, ALongRestDoesNotRepeatAFailedProbe)
{
    constexpr uint32_t kMaxReturns = 2u; // the registered rule's limit
    constexpr uint32_t kRestUpdates[] = {30u, 60u};
    for (uint32_t restUpdates : kRestUpdates)
    {
        CBTInstance inst;
        ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
        const ShapedRelief relief(*m_Device, inst, ReleaseShaping());
        ASSERT_TRUE(relief.IsArmed());
        CBTClassifyDesc c = ScreenClassify(ReleaseShaping());
        c.BehindEye = BehindEyeRelease::ReleaseWhenNeeded;
        float eyeZ = kReleasePose.EyeZ;
        HoldShaped(inst, relief, c, Above(kReleasePose.EyeX, 2.0f, eyeZ, 0.0f, -4.0f));
        const int32_t restStep = inst.ReadTessellationStats().OffFrustumKeepStep;
        ASSERT_GT(restStep, 0) << "the walk must rest above keep step 0 for a probe to exist";
        ASSERT_LT(restStep, int32_t(kOffFrustumKeepSteps)) << "the walk must rest under the keep ceiling";
        ASSERT_GE(kUpdatePeriod * (restUpdates - 1u), CBTUpdateRestGate::kKeepProbeSettle);
        CBTUpdateRestGate gate;
        const uint32_t returns = StopAndGo(inst, relief, c, gate, eyeZ, 2.0f, restStep, restUpdates);
        std::printf("[stop-and-go] rest %u updates: rest step %d, returns to it %u\n", restUpdates, restStep, returns);
        EXPECT_LE(returns, kMaxReturns) << "a rest of " << restUpdates << " updates must not repeat a failed probe";
        inst.Shutdown();
    }
}

// The walk over like ground, then on toward the far edge, where the view holds less ground and a lower keep step fits.
// The search must probe again and rest on the step a fresh tree rests on there: a failed probe must not hold it above.
TEST_F(CBTBehindEyeRelease, AFailedProbeIsTriedAgainWhereTheLowerStepFits)
{
    constexpr uint32_t kRestUpdates = 60u;
    constexpr float kSparseZ = 400.0f;
    constexpr uint32_t kApproachUpdates = 8u;
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
    const ShapedRelief relief(*m_Device, inst, ReleaseShaping());
    ASSERT_TRUE(relief.IsArmed());
    CBTClassifyDesc c = ScreenClassify(ReleaseShaping());
    c.BehindEye = BehindEyeRelease::ReleaseWhenNeeded;
    float eyeZ = kReleasePose.EyeZ;
    HoldShaped(inst, relief, c, Above(kReleasePose.EyeX, 2.0f, eyeZ, 0.0f, -4.0f));
    const int32_t restStep = inst.ReadTessellationStats().OffFrustumKeepStep;
    CBTUpdateRestGate gate;
    StopAndGo(inst, relief, c, gate, eyeZ, 2.0f, restStep, kRestUpdates);
    ASSERT_NE(inst.DebugReadWords(CBTBinding::WorkQueue, 1u, kWQKeepFailedStep).at(0), 0u)
        << "the walk over like ground must leave a failed probe for the test to say anything";
    const float walkedZ = eyeZ;
    for (uint32_t i = 1; i <= kApproachUpdates + 2u * kStageUpdates; ++i)
    {
        const float z = walkedZ + (kSparseZ - walkedZ) * float(std::min(i, kApproachUpdates)) / float(kApproachUpdates);
        RunGated(inst, relief, c, gate, Above(kReleasePose.EyeX, 2.0f, z, 0.0f, -4.0f), ReleaseShaping());
    }
    EXPECT_EQ(inst.DebugReadWords(CBTBinding::WorkQueue, 1u, kWQKeepFailedStep).at(0), 0u)
        << "a quiet tree under the ceiling at the failed step must clear the failed probe";
    const CBTTessellationStats walked = inst.ReadTessellationStats();
    inst.Shutdown();

    CBTInstance fresh;
    ASSERT_TRUE(fresh.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(fresh.InitializeRoots(kDomainPlanar));
    const ShapedRelief freshRelief(*m_Device, fresh, ReleaseShaping());
    ASSERT_TRUE(freshRelief.IsArmed());
    CBTClassifyDesc freshClassify = ScreenClassify(ReleaseShaping());
    freshClassify.BehindEye = BehindEyeRelease::ReleaseWhenNeeded;
    HoldShaped(fresh, freshRelief, freshClassify, Above(kReleasePose.EyeX, 2.0f, kSparseZ, 0.0f, -4.0f));
    const CBTTessellationStats rested = fresh.ReadTessellationStats();
    fresh.Shutdown();
    std::printf("[sparse] like ground step %d; at z %.0f m: walked step %d (occ %.3f), fresh step %d (occ %.3f)\n",
                restStep, kSparseZ, walked.OffFrustumKeepStep,
                double(walked.LiveCount) / kDefaultBisectorPoolSize, rested.OffFrustumKeepStep,
                double(rested.LiveCount) / kDefaultBisectorPoolSize);
    ASSERT_LT(rested.OffFrustumKeepStep, restStep) << "the sparse pose must fit a lower step for the test to say anything";
    EXPECT_EQ(walked.OffFrustumKeepStep, rested.OffFrustumKeepStep) << "a failed probe must not hold the step above";
}
