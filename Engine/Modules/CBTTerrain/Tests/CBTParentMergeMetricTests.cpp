// The planar merge is decided on the parent, not on the facet's own edge.
//
// A facet's split edge is one LEG of its parent and its sibling's is the other. Under a grazing
// perspective camera the two legs project an order of magnitude apart, so a per-child screen-space
// merge test could only ever be answered YES by one of the two, and PrepareSimplify needs BOTH to
// carry CBT_STATE_SIMPLIFY before a diamond can collapse. The planar criterion (cbt_kernels.comp
// CBT_PlanarClassify) asks the parent and the parent's twin instead, each from the UV box both
// children rebuild bit for bit, so the two children ask the same question and reach the same answer,
// on flat ground and on relief alike.
//
// The instrument here is the GPU's own CBTBisectorData::BisectorState, read back after the tree has
// converged: with nothing left to serve, that word is exactly the decision Kernel_Classify reached.
// The CPU mirror below (ClassifyPairs) restates the screen-space edge rule the arms without a height
// pyramid keep; here it only sorts the pairs into the population the census counts.
//
// EVERY test runs BOTH height regimes: flat ground and the shipped ComposedIsland relief (512 m,
// heightScale 60).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "CBTTerrain/CBTDemandTuning.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTReliefHeightSource.h"
#include "CBTTestHarness.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;
using GameEngine::Mathematics::Vector3;

namespace
{
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u;
constexpr uint32_t kBisectorWordsPerSlot = sizeof(CBTBisectorData) / 4u;
constexpr uint32_t kBisectorStateWord = 1u; // CBTBisectorData::BisectorState

// Height regimes. TerrainSize[2] is heightScale (CBTLayout.h). Flat (0) is a control. Relief binds the
// unified height texture, where the planar criterion decides; atlas binds the same relief through the atlas
// source, where the screen-space edge rule decides.
struct Regime
{
    const char* Label;
    float HeightScale;
    bool AtlasSource = false;
};
constexpr Regime kFlat{"flat", 0.0f};
constexpr Regime kRelief{"relief", 60.0f}; // the shipped ComposedIsland heightScale
constexpr Regime kReliefAtlas{"relief-atlas", 60.0f, true};
constexpr Regime kRegimes[] = {kFlat, kRelief, kReliefAtlas};
constexpr uint32_t kReliefDim = 512u;

constexpr float kScreenW = 1600.0f;
constexpr float kScreenH = 900.0f;

// The registered ComposedIsland mid pose — eye [-173.333, 106.667, -173.333], yaw 45, pitch -12 —
// over the shipped 512 m island, so the population these tests measure is the one the runtime
// look judges. Elevated and oblique: the near edge is close and the far edge is seen nearly
// edge-on, which is the projection regime that drives the two legs of a parent an order of
// magnitude apart. Looking DOWN also keeps the behind-eye force-split population small, so the
// domain metric (not the near-field world rule) governs most of the tree.
constexpr float kTerrainSizeM = 512.0f;
constexpr float kEyeX = -173.333f, kEyeY = 106.667f, kEyeZ = -173.333f;
constexpr float kYawDeg = 45.0f, kPitchDeg = -12.0f;
constexpr uint32_t kMaxDepth = 21u;
constexpr float kSplitPx = 8.0f;
constexpr float kMergePx = 4.0f;

// Demand tuning runs the shipped quartet (CBTDemandTuning.h); the near-bias coarsen factor is
// this probe's own (the shipped one is kNearBiasMaxCoarsen).
constexpr float kNearBiasMaxCoarsenLocal = 4.0f;

// Mirrors of in-shader constants (cbt_kernels.comp / cbt_layout.glsl); value-locked by
// CBTLayout.GlslPlanarMetricConstantsMirrorCpp.
constexpr float kNearBiasContentionLo = 0.50f;
constexpr float kNearBiasContentionHi = 0.90f;
constexpr float kLebParentEdgeScale = 1.41421356f; // CBT_LEB_PARENT_EDGE_SCALE
constexpr float kSplitNdcMargin = 1.1f;            // CBT_SPLIT_NDC_MARGIN

// The CPU mirror re-derives Kernel_Classify's predicate in host arithmetic, so near the merge bound its
// side can differ from the shader's by rounding: measured never beyond 8.8e-6 relative over 19.8M child
// comparisons. This band sits an order of magnitude above that.
constexpr float kMirrorBoundaryRelTol = 1e-4f;

constexpr uint32_t kConvergeFrames = 140u;
constexpr uint32_t kDrainFrames = 90u;

// The two poses the runtime look uses. The mid pose is the registered one; the walk pose is the
// near-field arm — eye at walking height looking along the ground, which puts most of the terrain
// behind the eye plane (the near-field world rule owns those, so they are excluded by branch) and
// leaves a long grazing visible wedge.
struct Pose
{
    const char* Label;
    float EyeX, EyeY, EyeZ;
    float YawDeg, PitchDeg;
};
constexpr Pose kMidPose{"mid", kEyeX, kEyeY, kEyeZ, kYawDeg, kPitchDeg};
constexpr Pose kWalkPose{"walk", kTerrainSizeM * 0.5f, 2.0f, 0.0f, 0.0f, 0.0f};
constexpr Pose kPoses[] = {kMidPose, kWalkPose};

// The three poses the coordinator's runtime A/B uses on ComposedIsland, at the shipped
// TargetPixelError. They exist because the liveCount delta CHANGES SIGN across them and the first
// two poses in this file did not span that: a claim measured only where the sign is positive is the
// pose-class version of measuring only on flat ground.
constexpr Pose kAbMidPose{"ab-mid", -173.333f, 106.667f, -173.333f, 45.0f, -12.0f};
constexpr Pose kAbWalkPose{"ab-walk", -40.0f, 8.0f, -60.0f, 120.0f, -4.0f};
constexpr Pose kAbFarPose{"ab-far", -400.0f, 300.0f, -400.0f, 45.0f, -20.0f};
constexpr Pose kAbPoses[] = {kAbMidPose, kAbWalkPose, kAbFarPose};
// ComposedIsland ships TargetPixelError 4; CBTRenderFeature sets splitPx = TPE x renderHeight / 1080
// (3.33 px on this file's 900-row screen), mergePx = splitPx * 0.5. The sweep keeps the 1080-row 4 / 2.
constexpr float kShippedSplitPx = 4.0f;
constexpr float kShippedMergePx = 2.0f;
constexpr uint32_t kSweepMaxDepth = 23u;

float SmoothStep(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Proj
{
    float Px = 0.0f, Py = 0.0f;
    bool InFront = false;
};

// CPU mirror of CBT_ProjectPixels (cbt_layout.glsl): column-major clip = M * (x,y,z,1); InFront is
// false at/behind the near plane (clip.w <= 1e-5).
Proj ProjectPix(const float* matrix, const Vector3& point)
{
    Proj r;
    const float w = matrix[3] * point[0] + matrix[7] * point[1] + matrix[11] * point[2] + matrix[15];
    if (w <= 1e-5f)
        return r;
    const float cx = matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12];
    const float cy = matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13];
    r.Px = (cx / w * 0.5f + 0.5f) * kScreenW;
    r.Py = (cy / w * 0.5f + 0.5f) * kScreenH;
    r.InFront = true;
    return r;
}

// CPU mirror of CBT_NearBiasCoarsen (cbt_kernels.comp).
float NearBiasCoarsen(const CBTFrameParams& p, float contention, const Vector3& point)
{
    if (contention <= 0.0f)
        return 1.0f;
    const float dx = point[0] - p.CameraPos[0], dy = point[1] - p.CameraPos[1],
                dz = point[2] - p.CameraPos[2];
    const float farT =
        SmoothStep(p.NearBias[1], p.NearBias[2], std::sqrt(dx * dx + dy * dy + dz * dz));
    return 1.0f + contention * farT * std::max(p.NearBias[3] - 1.0f, 0.0f);
}

// CPU mirror of CBT_SplitGate's planar half (cbt_kernels.comp): true when the facet is off-frustum
// and the gate — not the domain metric — owns its merge direction. The spherical horizon half is
// unreachable in the planar domain.
bool PlanarFrustumGated(const CBTFrameParams& p, const Proj& p0, const Proj& p1, const Proj& p2)
{
    if (!p0.InFront && !p1.InFront && !p2.InFront)
        return true; // fully behind the eye — the gate reports CBT_OFF_FRUSTUM_BEHIND_NDC excess
    if (!p0.InFront || !p1.InFront || !p2.InFront)
        return false; // straddling the near plane: genuinely close, gate says RELEVANT
    const float m = kSplitNdcMargin;
    auto ndc = [&](const Proj& q) {
        return std::array<float, 2>{q.Px / p.Screen[0] * 2.0f - 1.0f, q.Py / p.Screen[1] * 2.0f - 1.0f};
    };
    const auto n0 = ndc(p0), n1 = ndc(p1), n2 = ndc(p2);
    const float exL = std::min(std::min(-m - n0[0], -m - n1[0]), -m - n2[0]);
    const float exR = std::min(std::min(n0[0] - m, n1[0] - m), n2[0] - m);
    const float exB = std::min(std::min(-m - n0[1], -m - n1[1]), -m - n2[1]);
    const float exT = std::min(std::min(n0[1] - m, n1[1] - m), n2[1] - m);
    return std::max(std::max(exL, exR), std::max(exB, exT)) > 0.0f;
}

// Is the planar off-frustum gate live? Planar retains totally at keep step 0 (Kernel_Reset), so there
// the domain metric is the only thing deciding merges.
bool PlanarGateActive(CBTInstance& inst)
{
    return inst.ReadWorkQueueCounter(kWQOffFrustumKeepStep) > 0;
}

struct Facet
{
    uint64_t HeapId = 0;
    uint32_t State = 0;
    Vector3 C0, C1, C2;
};

// Whether the parent's split edge sits so close to the merge bound that this CPU mirror cannot say which
// side the shader chose (kMirrorBoundaryRelTol). The tests count these pairs apart and bound their number.
bool NearMergeBound(float parentLenPx, float boundPx)
{
    return std::abs(boundPx - parentLenPx) <= boundPx * kMirrorBoundaryRelTol;
}

// How the two children of one parent were classified, and by which rule.
struct PairVerdict
{
    bool OnDomainBranch = false; // both children had both split-edge endpoints in front
    bool GateOwned = false; // the off-frustum gate is live AND claims at least one child, so the
                            // domain metric does not own this pair's merge direction
    bool EvenSimplify = false, OddSimplify = false;
    float EvenChildRatio = 0.0f, OddChildRatio = 0.0f; // own edgePx / mergePx
    float ParentRatio = 0.0f;                          // parentEdgePx / mergePx at the parent mid
    bool NearBound = false; // the parent's edge is within kMirrorBoundaryRelTol of the merge bound
};
} // namespace

class CBTParentMergeMetric : public ::testing::Test
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

    void RunFrame(CBTInstance& inst, const CBTClassifyDesc& desc, const CBTFrameParams& params,
                  uint32_t frame)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        inst.RecordUpdate(*cl, desc, params, frame);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    // All four validation counters (CBTLayout.h kValidation*Counter), not just the aggregate.
    // Kernel_Validate is armed by default on CBTInstance and DISABLED by the shipping render
    // feature (CBTRenderFeature SetValidateEachUpdate(false)), so these counters exist ONLY on the
    // headless path -- which makes this suite the only place a conformance claim can be made at all.
    //
    // The link-reciprocity counter is the one this branch specifically needs: a merge rewrites
    // neighbour links, and this branch makes diamonds form that never formed before (bothMerge
    // 0 -> 7,151), so it exercises that rewrite on pairs the previous rule never collapsed.
    struct ValidationCounters
    {
        uint32_t LinkReciprocity = 0, Budget = 0, Zombie = 0, Compact = 0;
        uint32_t Total() const { return LinkReciprocity + Budget + Zombie + Compact; }
    };

    // All four counters share one region, so one read brings the whole set back rather than
    // paying a submit + idle per slot.
    ValidationCounters ReadAllValidationCounters(CBTInstance& inst)
    {
        const std::vector<uint32_t> w = inst.DebugReadWords(CBTBinding::Validation, kValidationWords);
        auto slot = [&](uint32_t i) { return i < w.size() ? w[i] : 0u; };
        ValidationCounters v;
        v.LinkReciprocity = slot(kValidationErrorCounter);
        v.Budget = slot(kValidationBudgetCounter);
        v.Zombie = slot(kValidationZombieCounter);
        v.Compact = slot(kValidationCompactCounter);
        return v;
    }

    uint32_t ValidationErrors(CBTInstance& inst) { return inst.ReadValidationErrorCount(); }

    // Every live facet with its GPU-assigned classification state and its corners, sorted by heapID
    // so siblings (which differ only in the low bit) land adjacent.
    std::vector<Facet> ReadLiveFacets(CBTInstance& inst)
    {
        const auto heapWords =
            inst.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        const auto bisWords = inst.DebugReadWords(
            CBTBinding::BisectorData, kDefaultBisectorPoolSize * kBisectorWordsPerSlot);
        const auto vertWords = inst.DebugReadWords(
            CBTBinding::CurrentVertex, kDefaultBisectorPoolSize * kVertexWordsPerSlot);
        auto asFloat = [](uint32_t bits) {
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        };
        auto corner = [&](uint32_t slot, uint32_t k) {
            const uint32_t base = slot * kVertexWordsPerSlot + k * 4u;
            return Vector3(asFloat(vertWords[base + 0u]), asFloat(vertWords[base + 1u]), asFloat(vertWords[base + 2u]));
        };

        std::vector<Facet> live;
        live.reserve(kDefaultBisectorPoolSize / 2u);
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            const uint64_t h = static_cast<uint64_t>(heapWords[s * 2u]) |
                               (static_cast<uint64_t>(heapWords[s * 2u + 1u]) << 32);
            if (h == 0u)
                continue;
            Facet f;
            f.HeapId = h;
            f.State = bisWords[s * kBisectorWordsPerSlot + kBisectorStateWord];
            f.C0 = corner(s, 0);
            f.C1 = corner(s, 1);
            f.C2 = corner(s, 2);
            live.push_back(f);
        }
        std::sort(live.begin(), live.end(),
                  [](const Facet& a, const Facet& b) { return a.HeapId < b.HeapId; });
        return live;
    }

    // Classify every live sibling pair: which branch it took and what the GPU decided.
    std::vector<PairVerdict> ClassifyPairs(const std::vector<Facet>& live, const CBTFrameParams& p,
                                           float occupancy, bool gateActive)
    {
        const float contention =
            p.NearBias[0] != 0.0f
                ? SmoothStep(kNearBiasContentionLo, kNearBiasContentionHi, occupancy)
                : 0.0f;

        std::vector<PairVerdict> out;
        for (size_t i = 0; i + 1 < live.size(); ++i)
        {
            if ((live[i].HeapId >> 1) != (live[i + 1].HeapId >> 1))
                continue;
            const bool aIsEven = (live[i].HeapId & 1ull) == 0ull;
            const Facet& even = aIsEven ? live[i] : live[i + 1];
            const Facet& odd = aIsEven ? live[i + 1] : live[i];

            PairVerdict v;
            v.EvenSimplify = even.State == kStateSimplify;
            v.OddSimplify = odd.State == kStateSimplify;

            const Proj e0 = ProjectPix(p.ViewProjRel, even.C0), e1 = ProjectPix(p.ViewProjRel, even.C1),
                       e2 = ProjectPix(p.ViewProjRel, even.C2);
            const Proj o0 = ProjectPix(p.ViewProjRel, odd.C0), o1 = ProjectPix(p.ViewProjRel, odd.C1),
                       o2 = ProjectPix(p.ViewProjRel, odd.C2);
            v.OnDomainBranch = e0.InFront && e2.InFront && o0.InFront && o2.InFront;
            if (!v.OnDomainBranch)
            {
                out.push_back(v);
                continue;
            }
            // UNEXERCISED WHILE THE GATE IS DORMANT, and the count is printed so that stays
            // visible. Planar only gates off-frustum merges once its keep step leaves 0, which
            // takes occupancy reaching the keep ceiling; all four arms here converge below it, so
            // gate-owned reads 0 and this exclusion is inert. It is kept because the gate decides per child on
            // its own ndcExcess and is NOT diamond-consistent — at occupancy 0.60 that was measured
            // at 361 disagreeing gate-owned pairs while the domain-owned population stayed clean.
            // Without the exclusion this suite would fail for a defect it does not own the moment a
            // pose converged heavier.
            v.GateOwned = gateActive &&
                          (PlanarFrustumGated(p, e0, e1, e2) || PlanarFrustumGated(p, o0, o1, o2));

            const Vector3 evenMid = 0.5f * (even.C0 + even.C2);
            const Vector3 oddMid = 0.5f * (odd.C0 + odd.C2);
            const float evenMergePx = p.Screen[3] * NearBiasCoarsen(p, contention, evenMid);
            const float oddMergePx = p.Screen[3] * NearBiasCoarsen(p, contention, oddMid);
            v.EvenChildRatio =
                evenMergePx > 0.0f ? std::hypot(e0.Px - e2.Px, e0.Py - e2.Py) / evenMergePx : 0.0f;
            v.OddChildRatio =
                oddMergePx > 0.0f ? std::hypot(o0.Px - o2.Px, o0.Py - o2.Py) / oddMergePx : 0.0f;

            // The parent's split edge runs from the odd child's corner2 to the even child's corner0, and the
            // shader reads both from the stored corners (CBT_ParentSplitEdgeFar), so both children measure
            // it alike. The threshold is taken at the edge's midpoint, as CBT_PlanarParentMergeWish takes it.
            const Vector3 parentMid = 0.5f * (odd.C2 + even.C0);
            const float parentMergePx = p.Screen[3] * NearBiasCoarsen(p, contention, parentMid);
            const float parentLenPx = std::hypot(o2.Px - e0.Px, o2.Py - e0.Py);
            v.ParentRatio = parentMergePx > 0.0f ? parentLenPx / parentMergePx : 0.0f;
            v.NearBound = NearMergeBound(parentLenPx, parentMergePx * kLebParentEdgeScale);
            out.push_back(v);
        }
        return out;
    }

    ReliefHeightSource ArmRelief(CBTInstance& inst, const Regime& regime)
    {
        ReliefHeightSource r(*m_Device, inst, regime.HeightScale, kReliefDim);
        EXPECT_EQ(r.IsArmed(), regime.HeightScale != 0.0f)
            << regime.Label << ": height source arming did not match the regime";
        return r;
    }

    // Converge a grazing planar walking pose and leave the tree settled.
    uint32_t Converge(CBTInstance& inst, const CBTClassifyDesc& desc, const CBTFrameParams& p,
                      uint32_t frames)
    {
        uint32_t frame = 0;
        for (; frame < frames; ++frame)
            RunFrame(inst, desc, p, frame);
        return frame;
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

namespace
{
void BuildGrazingPlanarParams(CBTFrameParams& p, const Pose& pose, const Regime& regime,
                              float splitPx, float mergePx)
{
    using namespace GameEngine::Mathematics;
    p = CBTFrameParams{};
    p.Screen[0] = kScreenW;
    p.Screen[1] = kScreenH;
    p.Screen[2] = splitPx;
    p.Screen[3] = mergePx;
    p.TerrainSize[0] = kTerrainSizeM;
    p.TerrainSize[1] = kTerrainSizeM;
    p.TerrainSize[2] = regime.HeightScale; // heightScale (CBTLayout.h) — 0 makes corners coplanar
    p.TerrainSize[3] = 0.0f;
    p.TerrainOrigin[2] = static_cast<float>(kMaxDepth);

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    const float yaw = pose.YawDeg * kDegToRad, pitch = pose.PitchDeg * kDegToRad;
    const Vector3 eye(pose.EyeX, pose.EyeY, pose.EyeZ);
    const Vector3 dir(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                      std::cos(yaw) * std::cos(pitch));
    const Matrix4x4 proj =
        MakePerspectiveLH_ZO_ReverseZ(1.05f, kScreenW / kScreenH, 0.5f, 20000.0f);
    const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + dir, Vector3(0, 1, 0));
    std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
    p.CameraPos[0] = eye.x;
    p.CameraPos[1] = eye.y;
    p.CameraPos[2] = eye.z;
    p.CameraPos[3] = 1.0f;

    const CBTNearBiasRadii nb = ComputeNearBiasRadii(pose.EyeY);
    p.NearBias[0] = 1.0f;
    p.NearBias[1] = nb.NearRadius;
    p.NearBias[2] = nb.FarRadius;
    p.NearBias[3] = kNearBiasMaxCoarsenLocal;
    p.DemandTuning[0] = kNearFieldFacetTargetM;
    p.DemandTuning[1] = kOffFrustumKeepOcc;
    p.DemandTuning[2] = kEdgeRescueTpeMul * splitPx;
    p.DemandTuning[3] = kEdgeRescueOcc;
}
} // namespace

// Two children of one parent reach the same merge decision: on the relief arm by the parent's box, on the
// atlas arm by the parent's split edge from the stored corners (CBT_ParentSplitEdgeFar). Red when the merge
// reads the facet's own box (relief) or reflects the held corner through the midpoint (atlas).
TEST_F(CBTParentMergeMetric, SiblingsAgreeOnTheMergeDecision)
{
    for (const Regime& regime : kRegimes)
    for (const Pose& pose : kPoses)
    {
    SCOPED_TRACE(testing::Message() << "arm " << regime.Label << "/" << pose.Label);
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = kMaxDepth;
    c.NearFieldGate = 1u;
    CBTFrameParams p;
    BuildGrazingPlanarParams(p, pose, regime, kSplitPx, kMergePx);
    std::optional<ReliefHeightSource> relief;
    std::optional<ReliefAtlasSource> atlas;
    if (regime.AtlasSource)
    {
        atlas.emplace(*m_Device, inst, kReliefDim + 1u);
        EXPECT_TRUE(atlas->IsArmed()) << "the atlas bound no height source, so the lattice is flat";
        atlas->Apply(p);
    }
    else
    {
        relief.emplace(ArmRelief(inst, regime));
    }

    Converge(inst, c, p, kConvergeFrames);
    const CBTTessellationStats stats = inst.ReadTessellationStats();
    EXPECT_EQ(ValidationErrors(inst), 0u) << "the census must read a conforming tree";

    const float occ = static_cast<float>(stats.LiveCount) / static_cast<float>(kDefaultBisectorPoolSize);
    const std::vector<PairVerdict> pairs = ClassifyPairs(ReadLiveFacets(inst), p, occ, PlanarGateActive(inst));

    uint64_t domainPairs = 0, bothSimplify = 0, disagree = 0, offBranch = 0;
    uint64_t gateOwned = 0, gateOwnedDisagree = 0;
    for (const PairVerdict& v : pairs)
    {
        if (!v.OnDomainBranch)
        {
            ++offBranch;
            continue;
        }
        if (v.GateOwned)
        {
            ++gateOwned;
            if (v.EvenSimplify != v.OddSimplify)
                ++gateOwnedDisagree;
            continue;
        }
        ++domainPairs;
        if (v.EvenSimplify && v.OddSimplify)
            ++bothSimplify;
        else if (v.EvenSimplify != v.OddSimplify)
            ++disagree;
    }

    std::printf("[parent-merge:%s/%s] live=%u occ=%.4f mergeDemand=%d mergeServed=%d "
                "splitDemand=%d\n",
                regime.Label, pose.Label, stats.LiveCount, occ, stats.MergeDemand,
                stats.MergeServed, stats.SplitDemand);
    std::printf("[parent-merge:%s/%s] pairs=%llu domain-owned=%llu bothSimplify=%llu DISAGREE=%llu "
                "| off-branch=%llu | gate-owned=%llu (disagree %llu)\n",
                regime.Label, pose.Label, static_cast<unsigned long long>(pairs.size()),
                static_cast<unsigned long long>(domainPairs),
                static_cast<unsigned long long>(bothSimplify),
                static_cast<unsigned long long>(disagree),
                static_cast<unsigned long long>(offBranch),
                static_cast<unsigned long long>(gateOwned),
                static_cast<unsigned long long>(gateOwnedDisagree));

    // Positive control: a zero-size population would make the disagreement assert vacuous.
    // EXPECT + continue rather than ASSERT: the arms are an inner loop, so an ASSERT here would
    // return from the whole test and silently skip every later arm.
    EXPECT_GT(domainPairs, 1000u) << "too few domain-owned pairs to characterize";
    if (domainPairs <= 1000u)
    {
        inst.Shutdown();
        continue;
    }

    EXPECT_EQ(disagree, 0u) << pose.Label << ": " << disagree << " of " << domainPairs
                            << " sibling pairs disagree on the merge";
    // A merge test that never fires agrees on every pair, so the atlas arm must merge some pairs.
    if (regime.AtlasSource)
        EXPECT_GT(bothSimplify, 0u) << pose.Label << ": no sibling pair merges on the atlas arm";

    // And the consequence, read off the GPU's own counters rather than re-derived from geometry: a
    // converged tree must not stand at a large unservable merge demand. Every disagreeing pair
    // contributes a candidate PrepareSimplify rejects plus a SIMPLIFY flag that goes on bailing
    // neighbouring splits, so this demand is the defect's running total. Measured either side of
    // the parent metric: mid 19.3% -> 2.2% of live, walk 12.2% -> 1.7%. The bound sits between.
    constexpr double kMaxStandingDemandFraction = 0.05;
    const double demandFraction =
        static_cast<double>(stats.MergeDemand) / static_cast<double>(stats.LiveCount);
    EXPECT_LT(demandFraction, kMaxStandingDemandFraction)
        << pose.Label << ": " << stats.MergeDemand << " standing merge candidates against "
        << stats.LiveCount << " live facets (" << 100.0 * demandFraction
        << "%) — a converged tree is asking for merges that cannot be served";
    inst.Shutdown();
    }
}

// The parent-edge merge of the screen-space edge rule, on an arm without a height pyramid. The flat
// regime binds no height texture, so the planar criterion is off (CBT_PlanarCriterionOn) and Classify
// runs the edge rule the atlas source, the narrow heap and a lattice too large for the pyramid keep: a
// facet merges on its parent's split edge (CBT_ParentSplitEdgeFar), not on its own. The over-conservative
// case: a pair where at least one child REFUSES on its own edge while the parent they would collapse into
// is comfortably inside the merge band. A per-child metric pins exactly these; the parent edge must merge
// them.
//
// RED ARM: neuter the parent branch and this population is classified NOT-SIMPLIFY on the refusing
// child, so bothSimplify collapses to 0 while the population itself stays non-empty.
TEST_F(CBTParentMergeMetric, ChildRefusesButAdequateParentIsMerged)
{
    const Regime& regime = kFlat;
    for (const Pose& pose : kPoses)
    {
    SCOPED_TRACE(testing::Message() << "arm " << regime.Label << "/" << pose.Label);
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = kMaxDepth;
    c.NearFieldGate = 1u;
    CBTFrameParams p;
    BuildGrazingPlanarParams(p, pose, regime, kSplitPx, kMergePx);
    const ReliefHeightSource relief = ArmRelief(inst, regime);

    Converge(inst, c, p, kConvergeFrames);
    const CBTTessellationStats stats = inst.ReadTessellationStats();
    EXPECT_EQ(ValidationErrors(inst), 0u) << "the census must read a conforming tree";

    const float occ = static_cast<float>(stats.LiveCount) / static_cast<float>(kDefaultBisectorPoolSize);
    const std::vector<PairVerdict> pairs = ClassifyPairs(ReadLiveFacets(inst), p, occ, PlanarGateActive(inst));

    // The over-conservative population: one child's own edge is at/over its merge threshold (it
    // would have refused), and the parent is inside the parent band.
    uint64_t overConservative = 0, merged = 0, unexplainedRefusals = 0, boundaryRefusals = 0;
    // The control population: the parent is well OVER the band, so the pair must NOT be merged
    // however short one child's leg happens to project. Guards the comparison's direction.
    uint64_t parentTooCoarse = 0, wronglyMerged = 0;
    for (const PairVerdict& v : pairs)
    {
        if (!v.OnDomainBranch || v.GateOwned)
            continue;
        const bool aChildRefuses = v.EvenChildRatio >= 1.0f || v.OddChildRatio >= 1.0f;
        if (aChildRefuses && v.ParentRatio < kLebParentEdgeScale)
        {
            ++overConservative;
            if (v.EvenSimplify && v.OddSimplify)
                ++merged;
            else if (v.NearBound)
                ++boundaryRefusals; // the mirror cannot call this pair's side of the bound
            else
                ++unexplainedRefusals;
        }
        if (v.ParentRatio > 2.0f * kLebParentEdgeScale)
        {
            ++parentTooCoarse;
            if (v.EvenSimplify || v.OddSimplify)
                ++wronglyMerged;
        }
    }

    std::printf("[parent-merge:%s/%s] over-conservative pairs=%llu of which SIMPLIFY=%llu "
                "(mirror-boundary refusals=%llu) | control parent-too-coarse=%llu of which "
                "flagged=%llu\n",
                regime.Label, pose.Label, static_cast<unsigned long long>(overConservative),
                static_cast<unsigned long long>(merged),
                static_cast<unsigned long long>(boundaryRefusals),
                static_cast<unsigned long long>(parentTooCoarse),
                static_cast<unsigned long long>(wronglyMerged));

    // EXPECT + continue rather than ASSERT: an inner-loop ASSERT would skip every later arm.
    EXPECT_GT(overConservative, 0u)
        << "this pose no longer produces the over-conservative case, so the assertions below prove "
           "nothing -- pick a pose whose far field is seen edge-on";
    EXPECT_GT(parentTooCoarse, 0u) << "no control population, so the direction of the test is "
                                      "unguarded";
    if (overConservative == 0u || parentTooCoarse == 0u)
    {
        inst.Shutdown();
        continue;
    }

    // ATTRIBUTION, not a tuned threshold. An over-conservative pair either merges, or it sits so close
    // to the bound that this CPU mirror cannot say which side the shader chose (kMirrorBoundaryRelTol).
    EXPECT_EQ(unexplainedRefusals, 0u)
        << regime.Label << "/" << pose.Label << ": " << unexplainedRefusals << " of "
        << overConservative
        << " over-conservative pairs refused to merge away from the bound -- the parent rule is not "
           "reaching them";

    // Magnitude guard: the rule must still fire for the large majority, so a wholesale regression
    // that made every pair "explained" cannot pass quietly.
    constexpr double kMinMergedFraction = 0.75;
    const double mergedFraction =
        static_cast<double>(merged) / static_cast<double>(overConservative);
    EXPECT_GT(mergedFraction, kMinMergedFraction)
        << regime.Label << "/" << pose.Label << ": only " << merged << " of " << overConservative
        << " (" << 100.0 * mergedFraction << "%) over-conservative pairs merged";

    // The abstention band must stay a sliver here too, or widening the tolerance
    // could quietly reclassify a real regression as "the mirror cannot say".
    // Measured population at the shipped tolerance: 0-1 of hundreds.
    if (overConservative > 0)
    {
        EXPECT_LT(static_cast<double>(boundaryRefusals) / static_cast<double>(overConservative),
                  0.01)
            << regime.Label << "/" << pose.Label << ": " << boundaryRefusals << " of "
            << overConservative << " over-conservative pairs fell in the boundary band";
    }

    // The control sits at twice the band, so a hit here is an inverted comparison.
    EXPECT_EQ(wronglyMerged, 0u)
        << regime.Label << "/" << pose.Label << ": " << wronglyMerged
        << " pairs were flagged SIMPLIFY with a parent well past the band";
    inst.Shutdown();
    }
}

// Conformance across the largest topology change the merge side can drive: raise the pixel target
// 4x on a converged tree and let ~86% of the facets merge away, checking the LEB invariants every
// frame of the cascade. Bounded on both sides — it must drain, and it must not collapse to base, so
// a merge rule that fired regardless of the parent would fail the second bound.
//
// RELIEF ONLY (heightScale 60). The flat cascade is no longer exercised anywhere: this was the
// last flat cascade arm, and it was moved rather than duplicated because conformance through a
// large merge cascade is harder to hold with relief, not easier. A flat regression in the cascade
// path would therefore not be caught here.
//
// NOT part of the parent merge's red arm: bulk coarsening is served under either rule, because a 4x
// threshold move puts BOTH legs of a parent far below the band, and the legs' projected spread
// only decides pairs sitting near it. What the parent merge changes is the converged fringe,
// which the census above gates.
TEST_F(CBTParentMergeMetric, CoarseningCascadeStaysConforming)
{
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = kMaxDepth;
    c.NearFieldGate = 1u;

    CBTFrameParams fine;
    BuildGrazingPlanarParams(fine, kMidPose, kRelief, kSplitPx, kMergePx);
    const ReliefHeightSource relief = ArmRelief(inst, kRelief);
    uint32_t frame = Converge(inst, c, fine, kConvergeFrames);
    const CBTTessellationStats before = inst.ReadTessellationStats();
    ASSERT_EQ(ValidationErrors(inst), 0u) << "the converged tree must be conforming before draining";
    ASSERT_GT(before.LiveCount, 10000u) << "nothing was built, so there is nothing to drain";

    // Four LEB levels of coarsening asked for, camera unchanged.
    constexpr float kCoarsenFactor = 4.0f;
    CBTFrameParams coarse;
    BuildGrazingPlanarParams(coarse, kMidPose, kRelief, kSplitPx * kCoarsenFactor,
                             kMergePx * kCoarsenFactor);

    int32_t mergeServedMax = 0;
    for (uint32_t i = 0; i < kDrainFrames; ++i, ++frame)
    {
        RunFrame(inst, c, coarse, frame);
        mergeServedMax = std::max(mergeServedMax, inst.ReadTessellationStats().MergeServed);
        ASSERT_EQ(ValidationErrors(inst), 0u)
            << "tree stopped conforming at drain frame " << i << " — a merge broke the LEB invariant";
    }
    const CBTTessellationStats after = inst.ReadTessellationStats();

    std::printf("[parent-merge:relief/mid] drain: live %u -> %u (%.1f%%), mergeServedMax=%d, "
                "mergeDemand %d -> %d, splitDemand after=%d\n",
                before.LiveCount, after.LiveCount,
                100.0 * static_cast<double>(after.LiveCount) / static_cast<double>(before.LiveCount),
                mergeServedMax, before.MergeDemand, after.MergeDemand, after.SplitDemand);

    EXPECT_GT(mergeServedMax, 0) << "no merge was ever served: the demand is still unservable";
    EXPECT_LT(after.LiveCount, before.LiveCount / 2u)
        << "a 4x coarser pixel target must remove most of the tree";
    EXPECT_GT(after.LiveCount, 1000u)
        << "the tree collapsed toward base — the merge rule is firing regardless of the parent";
    inst.Shutdown();
}

// CHARACTERIZATION, not a gate: what decides the SIGN of the converged liveCount delta.
//
// The parent metric pushes liveCount two ways at once, and which one wins is a property of the
// POSE, not of the metric:
//
//   DOWN  every over-conservative pair it newly merges is a facet pair collapsing to one parent.
//         These concentrate in a grazing far field, where the two legs of a parent project
//         furthest apart.
//   UP    every phantom SIMPLIFY flag it removes un-bails a facing neighbour in Kernel_Split, so
//         splits the metric had always been asking for finally get served. This needs the pool to
//         have been under pressure for there to be starved splits to release.
//
// So a pose with a big grazing far field and a slack pool falls, and a pose with a contended pool
// rises. Neither is a defect: they are the same correction seen from two regimes. This test prints
// the two predictors beside the outcome so the sign is attributable rather than surprising, and it
// asserts only what must hold in BOTH directions -- conformance, and that the tree never collapses.
//
// Run it under both shaders (swap cbt_kernels.comp.spv) to reproduce the A/B; the printed
// overConservative and split-pressure columns are what the delta should be read against.
TEST_F(CBTParentMergeMetric, PoseSweepCharacterizesTheLiveCountDeltaSign)
{
    for (const Pose& pose : kAbPoses)
    {
        SCOPED_TRACE(testing::Message() << "sweep pose " << pose.Label);
        CBTInstance inst;
        ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = kSweepMaxDepth;
        c.NearFieldGate = 1u;
        CBTFrameParams p;
        BuildGrazingPlanarParams(p, pose, kRelief, kShippedSplitPx, kShippedMergePx);
        p.TerrainOrigin[2] = static_cast<float>(kSweepMaxDepth);
        const ReliefHeightSource relief = ArmRelief(inst, kRelief);

        Converge(inst, c, p, kConvergeFrames);
        const CBTTessellationStats stats = inst.ReadTessellationStats();
        const ValidationCounters vc = ReadAllValidationCounters(inst);
        std::printf("[sweep:%s] validation link=%u budget=%u zombie=%u compact=%u\n", pose.Label,
                    vc.LinkReciprocity, vc.Budget, vc.Zombie, vc.Compact);
        EXPECT_EQ(vc.Total(), 0u)
            << "conformance broke at this pose: link=" << vc.LinkReciprocity
            << " budget=" << vc.Budget << " zombie=" << vc.Zombie << " compact=" << vc.Compact;

        const float occ =
            static_cast<float>(stats.LiveCount) / static_cast<float>(kDefaultBisectorPoolSize);
        const std::vector<PairVerdict> pairs = ClassifyPairs(ReadLiveFacets(inst), p, occ, PlanarGateActive(inst));

        uint64_t domainPairs = 0, overConservative = 0;
        for (const PairVerdict& v : pairs)
        {
            if (!v.OnDomainBranch || v.GateOwned)
                continue;
            ++domainPairs;
            if ((v.EvenChildRatio >= 1.0f || v.OddChildRatio >= 1.0f) &&
                v.ParentRatio < kLebParentEdgeScale)
                ++overConservative;
        }

        // The two predictors. overConservativePerMille is the DOWN pressure (how much of the live
        // population the parent rule newly merges); splitShortfall is the UP pressure (splits the
        // frame asked for and did not get, which phantom SIMPLIFY flags were part of causing).
        const double overPerMille =
            domainPairs ? 1000.0 * static_cast<double>(overConservative) /
                              static_cast<double>(domainPairs)
                        : 0.0;
        const int32_t splitShortfall = stats.SplitDemand - stats.SplitServed;
        std::printf("[sweep:%s] live=%u occ=%.4f | overConservative=%llu of %llu domain pairs "
                    "(%.2f/1000) | splitDemand=%d splitServed=%d shortfall=%d | mergeDemand=%d "
                    "mergeServed=%d\n",
                    pose.Label, stats.LiveCount, occ,
                    static_cast<unsigned long long>(overConservative),
                    static_cast<unsigned long long>(domainPairs), overPerMille, stats.SplitDemand,
                    stats.SplitServed, splitShortfall, stats.MergeDemand, stats.MergeServed);

        EXPECT_GT(stats.LiveCount, 1000u) << "the tree collapsed toward base at this pose";
        inst.Shutdown();
    }
}
