// cbt_layout.glsl — std430 GPU mirror of Include/CBTTerrain/CBTLayout.h.
// Edited in lockstep with the C++ header; the host-side static_asserts guard
// struct sizes/offsets, the constants below MUST match the kX values there.
//
// Include contract:
//   * Compute kernels include this with CBT_DESCRIPTOR_SET undefined (-> 0) and
//     CBT_GRAPHICS_INCLUDE undefined: all SSBOs are writable (restrict) and the
//     GPU-write helpers + push-constant block are defined.
//   * The graphics vertex modifier (C3) will define CBT_GRAPHICS_INCLUDE and
//     CBT_DESCRIPTOR_SET=2: SSBOs become `readonly restrict` and every
//     buffer-write helper is compiled out. This is the POC's hardest-won trap —
//     an unconditionally-defined atomic helper made the vertex stage fail to
//     compile silently, producing a null pipeline variant (plan §9).
#ifndef CBT_LAYOUT_GLSL
#define CBT_LAYOUT_GLSL

#ifndef CBT_GRAPHICS_INCLUDE
// Heap width. The default (desktop/Vulkan) arm carries the u64 heap; GE_CBT_HEAP32
// selects a u32 arm for targets with no 64-bit integers at all — WGSL/WebGPU is the
// one that forced it. Both arms are the same algorithm: every heap op here is a
// load/store, >>1, ^1, ==0 or a small-constant multiply, and the barycentric walk is
// add/halve. Nothing multiplies two wide values, so narrowing costs RANGE, not
// correctness — the u32 arm simply caps the reachable subdivision
// (CBT_MAX_NUM_SUBDIV_EFFECTIVE below), and the crack-free argument is untouched:
// both sides of a shared edge still compute a bit-identical integer, so the single
// float cast still rounds identically.
#if defined(GE_CBT_HEAP32)
#define CBT_DECODE_INT32 1
#define CBT_HEAPID_T uint
#define CBT_BINT int
#define CBT_BVEC2 ivec2
#define CBT_BVEC3 ivec3
#else
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
// Marks the translation units that have int64 (the compute kernels, which include this file).
// cbt_domain.glsl's int64 barycentric decode functions are guarded on this — the surface fragment
// includes cbt_domain.glsl but NOT cbt_layout.glsl, so it never sees int64 and skips them (the
// glslang GL_EXT_...int64 macro is predefined even when the extension is not required, so it cannot
// be the discriminator).
#define CBT_DECODE_INT64 1
#define CBT_HEAPID_T uint64_t
#define CBT_BINT int64_t
#define CBT_BVEC2 i64vec2
#define CBT_BVEC3 i64vec3
// NOTE: deliberately NOT GL_EXT_shader_atomic_int64. HeapID is a plain u64
// load/store; every atomic in the pipeline is a core-GLSL u32 op because the
// occupancy bitfield is u32-worded (plan §6). Keeping int64 atomics out is what
// makes CBT portable to Metal/MoltenVK. Gated out of the graphics include: the
// vertex modifier never touches HeapID, and pulling GL_EXT_...int64 into the
// vertex stage (a mid-file #extension, after the adapter's declarations) is both
// unnecessary and a portability risk.
#endif
#endif

#ifndef CBT_DESCRIPTOR_SET
#define CBT_DESCRIPTOR_SET 0
#endif

// SSBO qualifier: compute may write, the graphics include is read-only (the
// vertex stage cannot write SSBOs without vertexPipelineStoresAndAtomics).
#ifdef CBT_GRAPHICS_INCLUDE
#define CBT_SSBO_QUAL restrict readonly
#else
#define CBT_SSBO_QUAL restrict
#endif

// See the SumTree declaration: `coherent` is meaningful to SPIR-V and unrepresentable
// (and unnecessary) in WGSL.
#if defined(CBT_DECODE_INT32)
#define CBT_SSBO_COHERENT
#else
#define CBT_SSBO_COHERENT coherent
#endif

// ---------------------------------------------------------------------------
// Constants (mirror CBTLayout.h)
// ---------------------------------------------------------------------------
const uint CBT_POOL_SIZE = 1048576u;      // MUST equal kDefaultBisectorPoolSize/kGlslCbtPoolSize (CBTLayout.h); bounds the draw clamp (kMaxIndexCount = 3*CBT_POOL_SIZE). 1M pool bump: 2^20.

const uint CBT_WG_SIZE = 64u;            // kComputeWorkgroupSize
const uint CBT_INVALID_POINTER = 0xFFFFFFFFu;
// Max LEB subdivision (numSubdiv = depth - baseDepth) the VertexEval decode may reach.
// MUST equal kMaxDecodeSubdiv (CBTLayout.h); locked by CBTLayout.GlslMaxDecodeSubdivMatchesCpp.
// The int64 exact-N walk is crack-free to ~52; 40 keeps ~2x margin above the fp32 storage ULP.
const uint CBT_MAX_NUM_SUBDIV = 40u;

// The cap the decode actually applies, which is heap-width dependent. On the u32 arm a
// heapID must stay under 2^32, so depth = baseDepth + numSubdiv <= 31; the deepest base
// mesh is the cube-sphere's 5, and the barycentric scale (1 << numSubdiv) must also stay
// inside a signed 32-bit walk. 25 satisfies both with margin. MUST equal
// kHeap32DecodeSubdiv (CBTLayout.h) — locked by CBTLayout.GlslMaxDecodeSubdivMatchesCpp
// alongside the u64 cap, so neither arm can drift from the host clamp that feeds it.
#if defined(CBT_DECODE_INT32)
const uint CBT_MAX_NUM_SUBDIV_EFFECTIVE = 25u;
#else
const uint CBT_MAX_NUM_SUBDIV_EFFECTIVE = CBT_MAX_NUM_SUBDIV;
#endif

// Subdivision-pattern bits.
const uint CBT_NO_SPLIT = 0x0u;
const uint CBT_CENTER_SPLIT = 0x1u;
const uint CBT_RIGHT_SPLIT = 0x2u;
const uint CBT_RIGHT_DOUBLE_SPLIT = 0x3u;
const uint CBT_LEFT_SPLIT = 0x4u;
const uint CBT_LEFT_DOUBLE_SPLIT = 0x5u;
const uint CBT_TRIPLE_SPLIT = 0x7u;

// Bisector state enum (NOT bit positions).
const uint CBT_STATE_UNCHANGED = 0u;
const uint CBT_STATE_BISECT = 1u;
const uint CBT_STATE_SIMPLIFY = 2u;
const uint CBT_STATE_MERGED = 3u;

// Draw-stream binning flags.
const uint CBT_FLAG_VISIBLE = 0x1u;
const uint CBT_FLAG_MODIFIED = 0x2u;

// Classify metric selector (CBTPushConstants.classifyMode). DEPTH_TARGET is the
// deterministic C2/C3 depth-band metric (no camera dependence) kept for the unit
// tests and bring-up; SCREEN_SPACE is the C4 production metric (projected edge
// length vs a target pixel error, camera-driven). Plan §8 C4.
const uint CBT_CLASSIFY_DEPTH_TARGET = 0u;
const uint CBT_CLASSIFY_SCREEN_SPACE = 1u;

// View-priority contention ramp (arc slice S2, Option C). The near-bias only redistributes
// slots when the pool is actually contended, so an UNSATURATED frame classifies exactly as
// today (the neutrality oracle): Classify reads last frame's live occupancy (sum-tree root /
// pool) and ramps the bias in over [LO, HI]. Below LO the bias is a no-op even when enabled;
// at/above HI it is at full strength. The band is deliberately WIDE (low feedback gain): the
// bias frees far-field slots which lowers occupancy which weakens the bias — a negative
// feedback loop whose equilibrium must be a stable point, not an oscillation. LO sits above a
// clearly-unsaturated pose's occupancy (a ~4 km R=2000 descent settles ~33%) so neutrality
// holds; the width damps the loop. These are GPU-side policy, not part of the std430 mirror.
const float CBT_NEAR_BIAS_CONTENTION_LO = 0.50;
const float CBT_NEAR_BIAS_CONTENTION_HI = 0.90;

// Per-frame params UBO is quad-buffered so the CPU can write frame N's element while the
// GPU still reads earlier frames'. One deeper than the device's frames in flight, which is
// what buys the write-after-fence margin.
//
// pc.frameIndex is an UNWRAPPED, monotonically increasing frame counter (C++ side:
// CBTResources::AdvanceFrameCounter), NOT the device's wrapped frame slot — the shader is
// the second half of the reduction, so it must be handed a number whose domain can reach
// every element. Every ring read below applies `pc.frameIndex % CBT_FRAME_RING`, which is
// the GLSL twin of CBTFrameRingSlot in CBTLayout.h; CBTLayout.GlslFrameRingMirrorsCpp locks
// the depth and the selector so the two halves cannot drift apart.
const uint CBT_FRAME_RING = 4u;

// Work-queue counter slots.
const uint CBT_WQ_ALLOC_CURSOR = 0u;
const uint CBT_WQ_FREE_COUNT = 1u;
const uint CBT_WQ_SPLIT_COUNTER = 2u;
const uint CBT_WQ_SIMPLIFY_CLASS_COUNTER = 3u;
const uint CBT_WQ_ALLOCATE_COUNTER = 4u;
const uint CBT_WQ_PROPAGATE_BISECT_COUNTER = 5u;
const uint CBT_WQ_PROPAGATE_SIMPLIFY_COUNTER = 6u;
const uint CBT_WQ_SIMPLIFY_COUNTER = 7u;
const uint CBT_WQ_OVERFLOW_COUNTER = 8u;
const uint CBT_WQ_RIGHT_DOUBLE_COUNTER = 9u;
const uint CBT_WQ_LEFT_DOUBLE_COUNTER = 10u;
const uint CBT_WQ_TRIPLE_COUNTER = 11u;
// Incremental vertices VertexEval evaluated this frame (only counted when GateVertexEval is
// on — plan §planet-shading perf). Reset zeroes it; the perf oracle reads it to assert a
// quiescent frame evaluates ~0 vertices, not the whole live pool.
const uint CBT_WQ_VERTEX_EVAL_COUNTER = 12u;
// Since-init telemetry (Reset does NOT clear it, same as the overflow/fan-out slots):
// bumped by CBT_BoundedDispatchGroups when a dispatch-arg writer clamped a GPU-written
// element count to the pool bound. Mirrors kWQDispatchClampCounter (CBTLayout.h).
const uint CBT_WQ_DISPATCH_CLAMP_COUNTER = 13u;
// Planar pool-pressure scale: the current threshold-scale step (scale = 2^(step /
// CBT_PRESSURE_STEPS_PER_OCTAVE)). Stepped by Kernel_Reset thread 0 from the previous tail's live
// occupancy, once per Reset DISPATCH — a region re-seed dispatches Reset a second time inside the
// frame, so a far-field drain takes two steps that frame. Never cleared per frame (the re-seed
// zero-fill clears it). CBTLayout.h mirror.
const uint CBT_WQ_PRESSURE_STEP = 14u;
const uint CBT_PRESSURE_STEPS_PER_OCTAVE = 16u;
const uint CBT_PRESSURE_MAX_STEP = 48u;      // 8x threshold scale
const float CBT_PRESSURE_FULL_OCC = 0.98;    // step up while live occupancy is at/above this (the
                                             // pool-health "effectively full" mark)
const float CBT_PRESSURE_RECOVER_OCC = 0.90; // step back toward 1x while occupancy is below this (the
                                             // keep ceiling: below it the pool is not under pressure)
// Planar off-frustum keep step: how far the planar off-frustum keep band has collapsed (see
// CBT_PlanarKeepBand). Step 0 leaves the planar off-frustum field ungated. Stepped by Kernel_Reset
// thread 0 from the previous tail's live occupancy, once per Reset dispatch like
// CBT_WQ_PRESSURE_STEP. Never cleared per frame (the re-seed zero-fill clears it). CBTLayout.h
// mirrors the slot; CBTDemandTuning.h mirrors the schedule constants and states why they are these.
const uint CBT_WQ_OFF_FRUSTUM_KEEP_STEP = 15u;
const uint CBT_OFF_FRUSTUM_KEEP_STEPS = 24u;             // the last step: band 0
const float CBT_OFF_FRUSTUM_KEEP_WIDEST_NDC = 1024.0;    // step 1's band
const uint CBT_OFF_FRUSTUM_KEEP_STEPS_PER_OCTAVE = 2u;   // the band halves every two steps
const uint CBT_OFF_FRUSTUM_KEEP_SATURATED_STRIDE = 2u;    // steps per update while occupancy is at
                                                         // or above CBT_PRESSURE_FULL_OCC
// pc.behindEye (mirror CBTTerrain::BehindEyeRelease): the planar area metric's facets wholly behind the eye keep
// their mirror image's depth, take the keep band at their distance excess, or take band 0 at every step.
const uint CBT_BEHIND_EYE_ALWAYS_KEEP = 0u;
const uint CBT_BEHIND_EYE_RELEASE_WHEN_NEEDED = 1u;
const uint CBT_BEHIND_EYE_ALWAYS_RELEASE = 2u;
// The live count the planar classify reads its occupancy from while the camera holds still
// (pc.settle). Kernel_Reset copies the live count here on every update whose inputs moved; at rest it
// follows only a move larger than the deadband (1/CBT_REST_LATCH_DEADBAND_DIVISOR of the pool), so the
// near-bias contention and the near-field occupancy gate hold still under the few splits and merges
// the rest itself makes, and still follow the rebuild after a camera jump. Without it a facet on the
// threshold the tree's own size sets splits and merges on alternate updates forever. The deadband
// bounds how far two arrivals' latched counts can differ while the near-bias is engaged.
// CBTLayout.h mirrors both.
const uint CBT_WQ_REST_LIVE_COUNT = 16u;
const uint CBT_REST_LATCH_DEADBAND_DIVISOR = 1024u;
// The page samples of this update that found no resident page (CBTLayout.h kWQUnresolvedPageSamples).
const uint CBT_WQ_UNRESOLVED_PAGE_SAMPLES = 17u;
// The rest's keep floor: one past the highest keep step proven not to fit under the keep ceiling, or a phase below 0:
// CBT_KEEP_FLOOR_ARRIVING after an update that moved, CBT_KEEP_FLOOR_PROBING while the first step down has no
// measured tree. CBTLayout.h mirrors all three.
const uint CBT_WQ_OFF_FRUSTUM_KEEP_FLOOR = 18u;
const int CBT_KEEP_FLOOR_ARRIVING = -1;
const int CBT_KEEP_FLOOR_PROBING = -2;
// pc.settle when every input has matched for the probe's settle time (CBTLayout.h kSettleLasted).
const uint CBT_SETTLE_LASTED = 2u;
// The leaves the planar criterion wants to merge, counted only at rest. A leaf that does not want to merge at rest never
// merges in that rest, so live minus this count is a lower bound on the quiet tree (Kernel_Reset's early up proof).
// CBTLayout.h mirrors it.
const uint CBT_WQ_MERGE_CANDIDATE_COUNTER = 19u;
// The rest search's memory of a failed probe. PROBE_BASE: the live count when the rest last stepped the keep step down,
// 0 when no step down is open. FAILED_STEP: one past the step whose probe went over the keep ceiling, 0 for none (so the
// zero-fill clears it). FAILED_ADDED: the leaves that probe added before it went over. CBTLayout.h mirrors the three.
const uint CBT_WQ_KEEP_PROBE_BASE = 20u;
const uint CBT_WQ_KEEP_FAILED_STEP = 21u;
const uint CBT_WQ_KEEP_FAILED_ADDED = 22u;
// A rest probes the failed step again only with the headroom it added plus 1/this of the pool.
const uint CBT_KEEP_RETRY_MARGIN_DIVISOR = 64u;
const uint CBT_WQ_COUNTER_SLOTS = 23u;

// Work-queue payload lanes — 3 ALIASED physical lanes over the 6 logical queues (slot-diet r2;
// mirror of CBTLayout.h WQ*Offset). The six queues' live spans over the per-frame kernel sequence
// are pairwise disjoint within each lane (a proven interval-graph 3-colouring — full lane-lifetime
// table + aliasing proof at the CBTLayout.h definition), and RecordDataBarrier fences every stage,
// so a lane's next occupant fills from index 0 over drained-and-barriered old data. Each queue keeps
// its own counter slot; the aliasing is purely at the payload offset. WQElementCount = the counters + 3*P + the census.
//   LANE 0: Split -> PropagateBisect -> Simplify   LANE 1: SimplifyClass -> PropagateSimplify   LANE 2: Allocate
const uint CBT_WQ_SPLIT_QUEUE = CBT_WQ_COUNTER_SLOTS + 0u * CBT_POOL_SIZE;              // lane 0
const uint CBT_WQ_PROPAGATE_BISECT_QUEUE = CBT_WQ_COUNTER_SLOTS + 0u * CBT_POOL_SIZE;   // lane 0
const uint CBT_WQ_SIMPLIFY_QUEUE = CBT_WQ_COUNTER_SLOTS + 0u * CBT_POOL_SIZE;           // lane 0
const uint CBT_WQ_SIMPLIFY_CLASS_QUEUE = CBT_WQ_COUNTER_SLOTS + 1u * CBT_POOL_SIZE;     // lane 1
const uint CBT_WQ_PROPAGATE_SIMPLIFY_QUEUE = CBT_WQ_COUNTER_SLOTS + 1u * CBT_POOL_SIZE; // lane 1
const uint CBT_WQ_ALLOCATE_QUEUE = CBT_WQ_COUNTER_SLOTS + 2u * CBT_POOL_SIZE;           // lane 2

// The pool census after the lanes (CBTLayout.h kCensus*, CBTPoolCensus.h): per level the leaves in view at
// BASE + 2 * level and off view at + 1, then the off-view leaves at CLASSES + CBT_CENSUS_CLASS_*.
// Only an update with pc.census set clears and counts it.
const uint CBT_CENSUS_LEVELS = 64u;
const uint CBT_WQ_CENSUS_BASE = CBT_WQ_COUNTER_SLOTS + 3u * CBT_POOL_SIZE;
const uint CBT_WQ_CENSUS_CLASSES = CBT_WQ_CENSUS_BASE + 2u * CBT_CENSUS_LEVELS;
const uint CBT_CENSUS_CLASS_MERGE_WANTED = 0u;
const uint CBT_CENSUS_CLASS_BEHIND_EYE = 1u;
const uint CBT_CENSUS_CLASS_BESIDE_FRUSTUM = 2u;
const uint CBT_CENSUS_CLASS_ACROSS_EYE_PLANE = 3u;
const uint CBT_CENSUS_CLASS_IN_MARGIN_FRUSTUM = 4u;
const uint CBT_CENSUS_CLASS_BEHIND_HORIZON = 5u;
const uint CBT_CENSUS_CLASS_COUNT = 6u;

// Far-field region reseed scratch (round-9 far-field-drain). The K_REGION_FREE_SEED kernel
// records each freed root's newly-decoded physical slot here so K_REGION_FREE_LINK can wire the
// base neighbor links across roots (rootSlot[r] at CBT_WQ_REGION_ROOT_SLOT + r). Reuses the split
// work-queue PAYLOAD region: the reseed runs on its OWN command list (never interleaved with the
// per-frame update), so it does not race Classify/Split's use of that queue. 24-root sphere fits.
const uint CBT_WQ_REGION_ROOT_SLOT = CBT_WQ_COUNTER_SLOTS + 0u * CBT_POOL_SIZE; // == CBT_WQ_SPLIT_QUEUE

// Cube-sphere base-mesh root topology (mirror BuildSphereRoots in CBTSphereRoots.h; locked by
// CBTLayoutTests.GlslRootTwinTableMatchesCpp). 24 roots = 6 faces * 4 pie slices; root r sits on
// face r/4, slice r&3. Within-face legs are simple wraps; the cross-face TWIN (the pie slice on
// the adjacent face sharing this root's cube-edge split edge) is a fixed permutation baked here so
// the region-reseed link kernel can re-derive base adjacency without a per-planet upload.
const uint CBT_SPHERE_ROOT_COUNT_LOCAL = 24u;
const uint CBT_ROOT_TWIN[24] = uint[24](22u, 10u, 17u, 13u, 15u, 19u, 8u, 20u, 6u, 18u, 1u, 21u,
                                        23u, 3u, 16u, 4u, 14u, 2u, 9u, 5u, 7u, 11u, 0u, 12u);
uint CBT_RootNeighbor0(uint r) { return (r & ~3u) + ((r + 1u) & 3u); } // across the (v1,v2) leg
uint CBT_RootNeighbor1(uint r) { return (r & ~3u) + ((r + 3u) & 3u); } // across the (v0,v1) leg

// OCBT bitfield + sum tree (u32 re-derivation — see CBTLayout.h derivation block).
const uint CBT_BITFIELD_WORDS = CBT_POOL_SIZE / 32u;    // 32768
const uint CBT_LEAF_NODE_COUNT = CBT_POOL_SIZE / 32u;   // 32768
const uint CBT_LEAF_PACKED_WORDS = (CBT_POOL_SIZE / 32u) / 4u; // 8192
const uint CBT_SUMTREE_NODE_COUNT = 2u * (CBT_POOL_SIZE / 32u) - 1u; // 65535
const uint CBT_SUMTREE_LEAF_OFFSET = (CBT_POOL_SIZE / 32u) - 1u;     // 32767 (heap-relative)
const uint CBT_SUMTREE_DEPTH = 15u;                                  // log2(32768)
const uint CBT_SUMTREE_HEAP_BASE = CBT_LEAF_PACKED_WORDS;            // heap starts after packed leaves

// Indirect dispatch slots (uvec3 x3) and draw records (5-word x3).
const uint CBT_DRAW_STREAM_STRIDE = 5u;
const uint CBT_DRAW_STREAM_ALL = 0u;
const uint CBT_DRAW_STREAM_VISIBLE = 1u;
const uint CBT_DRAW_STREAM_MODIFIED = 2u;
const uint CBT_DRAW_INDEX_COUNT_FIELD = 0u;
const uint CBT_DRAW_INSTANCE_COUNT_FIELD = 1u;

const uint CBT_VALIDATION_ERROR_COUNTER = 0u;  // neighbor link non-reciprocity
const uint CBT_VALIDATION_BUDGET_COUNTER = 1u; // FreeCount went negative
const uint CBT_VALIDATION_ZOMBIE_COUNTER = 2u; // bitfield bit set XOR HeapID != 0
const uint CBT_VALIDATION_COMPACT_COUNTER = 3u; // IndicesAll[0..liveCount) != the live set
const uint CBT_FOCUS_ROOT_ALL = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// CBTVertexData — ONE definition, shared by the compute writer and the graphics
// reader (declared before the #ifdef so the two include branches CANNOT diverge;
// the layout is identical std430 either way — a divergence was suspected as the C4
// seam/dark-triangle bug and is ruled out structurally here). 96B (mirror of
// CBTLayout.h CBTVertexData — S2a): each corner = (position xyz, uv.x); meta =
// (uv0.y, uv1.y, uv2.y, terrain normalmap bindless index as float); then the
// Earth-scale (sector, local) tail. deepTag.x selects the corner meaning PER SLOT:
//   0 (planar; the sphere on the narrow arm): corner*.xyz = fp32 WORLD position, tail is zero.
//   1 (the sphere on the wide arm): corner*.xyz = fp32 SECTOR-LOCAL offset; sector<k>
//     packs corner k's ivec3 world sector (1024 m grid) as 3 x 16-bit lanes.
//     Consumers reconstruct rel = vec3(sector - originSector) * CBT_SECTOR_SIZE +
//     local — the GE_ClipFromSectorLocal math (camera_relative.glsl) verbatim.
// Height is baked into the corner (y / radial) by VertexEval. The graphics surface
// samples the normalmap per-pixel for a smooth normal (CDLOD A/B parity), so no
// per-vertex normal is stored.
struct CBTVertexData
{
    vec4 corner0; // xyz = world position (m) OR sector-local offset (deepTag.x), w = uv.x
    vec4 corner1;
    vec4 corner2;
    vec4 meta;    // (uv0.y, uv1.y, uv2.y, normalmapBindlessIndex)
    uvec2 sector0; // x = (sx & 0xFFFF) | (sy << 16), y = (sz & 0xFFFF) — 16-bit two's complement
    uvec2 sector1;
    uvec2 sector2;
    uvec2 deepTag; // x: 0 = world corners, 1 = (sector, local); y reserved 0
};

// Sector lane packing (mirror PackSectorPair / UnpackSector* in CBTLayout.h; range
// +/-32767 sectors ~ +/-3.36e7 m — 5x Earth radius, locked by
// CBTLayoutTests.DeepSectorPackingContract). Shared by the compute writer and the
// graphics reader, so it lives before the #ifdef split.
uvec2 CBT_PackCornerSector(ivec3 s)
{
    return uvec2((uint(s.x) & 0xFFFFu) | (uint(s.y) << 16u), uint(s.z) & 0xFFFFu);
}
ivec3 CBT_UnpackCornerSector(uvec2 p)
{
    return ivec3(int(p.x << 16u) >> 16, int(p.x) >> 16, int(p.y << 16u) >> 16);
}

// ---------------------------------------------------------------------------
// SSBO declarations (compute set 0; graphics set 2 via CBT_DESCRIPTOR_SET)
// ---------------------------------------------------------------------------
#ifdef CBT_GRAPHICS_INCLUDE
// -------- Graphics (vertex-modifier) minimal view --------
// The camera forward draw rides the VISIBLE index stream (binding 10) — Classify
// frustum-culls bisectors into it, so the modifier indexes gIdxVis to map a run of
// three vertex ids to a visible bisector's corners (binding 12). Declaring just these
// two — readonly, int64-free, and with none of the compute-only write/atomic helpers —
// is what keeps the vertex-stage SPIR-V clean: the POC's silent null-variant was a
// readonly-SSBO l-value error inside an atomic helper the graphics include dragged in
// (plan §9). Instance names (gIdxVis / gVertex) match the compute side, because
// MaterialBinder resolves DrawBindings entries by the reflected INSTANCE name (see
// CBTResources DrawBindings). CBTVertexData is defined once above (shared with the
// compute branch); the graphics side reads corner positions + uv + the normalmap index
// and samples that normalmap per-pixel in the surface.
layout(std430, set = CBT_DESCRIPTOR_SET, binding = 10) CBT_SSBO_QUAL buffer CBTIndicesVisibleBuffer
{
    uint IndicesVisible[];
} gIdxVis;

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 12) CBT_SSBO_QUAL buffer CBTVertexBuffer
{
    CBTVertexData Vertex[];
} gVertex;

#if !defined(GE_COMPAT_PROFILE)
// The draw's level rule (mirror CBTLayout.h CBTDrawLevelParams), offset-bound to this frame's ring
// slot, and the prefiltered height chain the compute update builds (binding 24 there). The compat
// profile (WebGPU class) declares neither: its vertex stage has no storage-buffer budget to spare,
// and its kernels build no chain, so it draws the cached corners.
#define CBT_DRAW_HEIGHT_CHAIN 1
#include "cbt_draw_level.glsl"

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 13) CBT_SSBO_QUAL buffer CBTHeightChainDrawBuffer
{
    uint Words[];
} gCbtChain;

#include "cbt_height_chain.glsl"
float CBT_ChainLoad(uint word)
{
    return uintBitsToFloat(gCbtChain.Words[word]);
}
#endif

#else // !CBT_GRAPHICS_INCLUDE — full compute layout
// The heap is 8 bytes per slot in BOTH arms: the u32 arm declares uvec2 so the stride
// (and therefore the C++ allocation, which is sizeof(uint64_t) either way) is untouched;
// only the high word goes unused. Read/write through CBT_LoadHeapID / CBT_StoreHeapID
// below rather than indexing gHeap directly — that pair is what makes the two arms one
// piece of code.
layout(std430, set = CBT_DESCRIPTOR_SET, binding = 0) CBT_SSBO_QUAL buffer CBTHeapIDBuffer
{
#if defined(CBT_DECODE_INT32)
    uvec2 HeapID[];
#else
    uint64_t HeapID[];
#endif
} gHeap;

// WGSL treats a vector as one memory location: concurrent stores to distinct
// components can overwrite each other. Array elements are independent locations.
// Keep the same 16-byte neighbor record while giving the narrow/WebGPU arm four
// scalar locations for the propagation kernels' independently owned edge writes.
#if defined(CBT_DECODE_INT32)
struct CBTNeighborStorage
{
    uint links[4];
};
#define CBT_NEIGHBOR_COMPONENT(n, c) ((n).links[c])
uvec4 CBT_LoadNeighbors(CBTNeighborStorage n)
{
    return uvec4(n.links[0], n.links[1], n.links[2], n.links[3]);
}
CBTNeighborStorage CBT_StoreNeighbors(uvec4 n)
{
    return CBTNeighborStorage(uint[4](n.x, n.y, n.z, n.w));
}
#else
#define CBTNeighborStorage uvec4
#define CBT_NEIGHBOR_COMPONENT(n, c) ((n)[c])
// Macros, not functions: the record already IS a uvec4 here, so these are the
// identity, and a function would put an OpFunction in the SPIR-V that the
// direct access does not have.
#define CBT_LoadNeighbors(n) (n)
#define CBT_StoreNeighbors(n) (n)
#endif

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 1) CBT_SSBO_QUAL buffer CBTNeighborsABuffer
{
    CBTNeighborStorage NeighborsA[];
} gNbrA;

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 2) CBT_SSBO_QUAL buffer CBTNeighborsBBuffer
{
    CBTNeighborStorage NeighborsB[];
} gNbrB;

struct CBTBisectorData
{
    uint subdivisionPattern;
    uint bisectorState;
    uint flags;
    uint problematicNeighbor;
    uint propagationID;
    uint indices0;
    uint indices1;
    uint indices2;
};

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 3) CBT_SSBO_QUAL buffer CBTBisectorDataBuffer
{
    CBTBisectorData Bisector[];
} gBisector;

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 4) CBT_SSBO_QUAL buffer CBTBitfieldBuffer
{
    uint Bitfield[];
} gBitfield;

// No kernel reads, through SumTree, a value another invocation wrote in the same dispatch:
// ReduceSecondPass reduces its shared levels in workgroup memory, and every other read is of
// values an earlier dispatch wrote. The `coherent` qualifier is therefore not relied on; its
// removal is #2596. WGSL has no such qualifier (naga's @coherent output is an extension tint
// rejects), hence CBT_SSBO_COHERENT.
layout(std430, set = CBT_DESCRIPTOR_SET, binding = 5) CBT_SSBO_COHERENT CBT_SSBO_QUAL buffer CBTSumTreeBuffer
{
    uint SumTree[];
} gSumTree;

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 6) CBT_SSBO_QUAL buffer CBTWorkQueueBuffer
{
    int WorkQueue[];
} gQueue;

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 7) CBT_SSBO_QUAL buffer CBTIndirectDispatchBuffer
{
    uint IndirectDispatch[];
} gDispatch;

// Indirect-dispatch slots, 3 u32 (x,y,z) each — mirror of kDispatchSlot* in CBTLayout.h.
// STAGE (0) carries the CURRENT stage's width and is rewritten several times per frame; LIVE (1)
// carries the frame's live-set width, seeded once by Reset and left alone, so the neighbor copy
// can dispatch over the live set after the split path has already rewritten slot 0.
const uint CBT_DISPATCH_SLOT_STAGE = 0u;
const uint CBT_DISPATCH_SLOT_LIVE = 1u;
const uint CBT_DISPATCH_SLOT_WORDS = 3u;

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 8) CBT_SSBO_QUAL buffer CBTIndirectDrawBuffer
{
    uint IndirectDraw[];
} gDraw;

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 9) CBT_SSBO_QUAL buffer CBTIndicesAllBuffer
{
    uint IndicesAll[];
} gIdxAll;

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 10) CBT_SSBO_QUAL buffer CBTIndicesVisibleBuffer
{
    uint IndicesVisible[];
} gIdxVis;

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 11) CBT_SSBO_QUAL buffer CBTIndicesModifiedBuffer
{
    uint IndicesModified[];
} gIdxMod;

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 12) CBT_SSBO_QUAL buffer CBTVertexBuffer
{
    CBTVertexData Vertex[];
} gVertex;

layout(std430, set = CBT_DESCRIPTOR_SET, binding = 13) CBT_SSBO_QUAL buffer CBTValidationBuffer
{
    uint Validation[];
} gValidation;

// -------- C4 per-frame params UBO + terrain height source (compute only) --------
// Camera + terrain params consumed by Classify (screen-space error) and VertexEval
// (world-space displacement). NOT declared in the graphics include: the vertex
// modifier reads evaluated positions and needs neither (plan §8 C4). std140 mirror
// of CBTFrameParams (CBTLayout.h) — mat4 + 12 vec4 + the page vec4 + the height-chain vec4 = 288 B;
// ring-buffered so the CPU can write frame N while the GPU reads frame N-1.
struct CBTFrameParams
{
    mat4 viewProjRel;   // REBASED world -> clip (reverse-Z LH): consumes render-origin-relative corners (slice 1b); == the full-world viewProj when the origin is inactive
    vec4 cameraPos;     // xyz = camera world position (PLANET-CENTER space; the horizon cull needs true radii, NOT rebased)
    vec4 screen;        // x=width px, y=height px, z=split threshold px, w=merge threshold px
    vec4 terrainSize;   // x=sizeX, y=sizeZ, z=heightScale, w=originY
    vec4 terrainOrigin; // x=originX, y=originZ, z=maxDepth (float), w=normalmap bindless index
    vec4 planetParams;  // C7 spherical domain: x=radius, y=reliefAmplitude, z=reliefFrequency, w=unused
    // Phase E resident-window atlas geometry (mirror CBTLayout.h). z of atlasParams1 selects
    // the planar height source: 0 = sample the unified height texture (binding 15) unchanged; 1 =
    // resolve terrain UV -> tile -> slot through the indirection SSBO + atlas texture; 2 = the
    // paged resolve through the page table (binding 22) + page cache (binding 23), wide arm only.
    vec4 atlasParams0;  // x=AtlasDim, y=SlotStride, z=SlotsPerRow, w=TileRes
    vec4 atlasParams1;  // x=TilesPerAxisX, y=TilesPerAxisZ, z=source(0/1/2), w=coarseDim (coarse field texels/axis)
    vec4 renderOriginSector; // xyz = render origin sector (float, exact integer); w = sector size (informational; CBT_RenderOriginWorld uses the constant)
    // View-prioritised split metric (arc slice S2, Option C — mirror CBTLayout.h NearBias).
    // x=enabled(0/1), y=near radius (m), z=far radius (m), w=max coarsen factor. Consumed by
    // Kernel_Classify only; 0 => byte-identical to the pre-slice metric.
    vec4 nearBias;
    // Walking-headroom demand shaping (mirror CBTLayout.h DemandTuning). Consumed by
    // Kernel_Classify only; every field 0 => byte-identical to the pre-slice metric.
    //   x = near-field world facet target (m): the behind-eye/straddle force-split stops once the
    //       split edge is this short instead of driving to the decode cap (a facet at the eye plane
    //       does not need cap-depth detail — that over-refinement saturates the walking pose). 0 = off.
    //   y = off-frustum keep-depth occupancy ceiling [0,1]: an off-frustum (view-dependent) bisector
    //       keeps its depth while live occupancy is below this (no yaw re-refine churn) and coarsens
    //       to base only under pool pressure. 0 = off (always coarsen-to-base, the pre-slice gate).
    //   z = flat-disc edge-rescue projected-edge threshold (px): a near facet the AREA metric
    //       under-refines (grazing/flattened, projected area collapsed) still splits while its
    //       projected LEB edge exceeds this, and a rescued parent's children hold their merge while
    //       the parent's projected split edge still exceeds it (the rescue's merge counterpart: a
    //       diamond never both re-rescues and merges). Self-limiting (splitting shortens the edge).
    //       0 = off.
    //   w = edge-rescue occupancy ceiling [0,1]: the rescue and its merge hold only fire while
    //       occupancy is below this, so they never fight the visible near field for a saturated
    //       pool. 0 = off.
    vec4 demandTuning;
    // Screen-area priority ordering (round-8c look-back — mirror CBTLayout.h PriorityParams). Orders
    // the split budget by ON-SCREEN facet area under saturation so the largest facets refine first.
    // Classify raises a projected-area floor on split demand along a wide occupancy contention ramp.
    //   x = enabled (0/1); y = contention ramp start occupancy [0,1]; z = max area floor (px^2 at full
    //       saturation); w = keep-large off-frustum band (NDC) — a large just-off-frustum facet keeps
    //       depth even under saturation within this annulus (a glance away does not demote it).
    vec4 priorityParams;
    // Water plane (mirror CBTLayout.h WaterPlane), Kernel_Classify only, planar only. Seen from a camera above x, a
    // bisector whose corners all lie more than y below x is hidden: its split threshold and its children's merge
    // threshold scale by z. z <= 1 (every field 0) => byte-identical to the metric.
    //   x = water surface height (world Y); y = submerged margin (m); z = coarsen factor;
    //   w = content-aware split threshold (px), planar Classify only; 0 = off.
    vec4 waterPlane;
    // The paged height resolve (mirror CBTLayout.h PageParams): x = the first word of this frame's
    // page-table ring slot (CBT_PAGE_RING_BASE_LANE, wide arm), an integer as float; yzw = 0.
    vec4 pageParams;
    // The draw's prefiltered height chain, for Classify's frustum cull (mirror CBTLayout.h HeightChain).
    // x = the level scale of the draw's rule lambda = log2(distance x scale), 0 = off; y = the top level.
    vec4 heightChain;
};

// One ring slot: the frame params and the face dirty rects. The rects are a sibling of the frame
// struct, not a member: CBT_Frame() copies the params by value per thread, so a large member adds
// per-thread local memory. Rect lookups read the UBO directly.
struct CBTFrameSlot
{
    CBTFrameParams frame;
    // Each cube face's dirty rect in face-local UV (minU, minV, maxU, maxV); empty when z <= x
    // (mirror CBTLayout.h SphereDirty).
    vec4 sphereDirty[6];
    // xy = the terrain's authored size in terrain uv (mirror CBTLayout.h SurfaceExtent): planar Classify
    // drops a triangle wholly past it from the VISIBLE stream.
    vec4 surfaceExtent;
};

layout(std140, set = CBT_DESCRIPTOR_SET, binding = 14) uniform CBTFrameParamsBuffer
{
    CBTFrameSlot Params[CBT_FRAME_RING];
} gParams;

// The SAME bindless GPU height texture the CDLOD vertex path samples (R32_FLOAT,
// normalized [0,1]), bound with a linear-clamp sampler (identical to CDLOD's
// GE_TS_CLAMP). Compute has no implicit derivatives, so VertexEval samples with
// textureLod(..., 0.0). Ring of CBT_FRAME_RING combined-image-samplers indexed by the
// frame slot: the CPU rewrites only slot (frameIndex % ring) each frame, so a mid-
// session heightmap handle change (async streaming, hot-reload, terrain identity)
// never rewrites a descriptor element an in-flight frame is still reading (the frame
// fence retires slot N before frame N+ring reuses it — same discipline as the UBO).
// Ring width. Targets with no descriptor arrays at all (WebGPU/WGSL has no binding
// arrays in core) collapse the three texture rings to a single element and rebind it
// every frame — legal there precisely because that backend rebuilds its bind group on
// every descriptor write, so the in-flight-element hazard the ring exists to avoid
// cannot occur. CBT_RING() is the one place the two shapes differ; the params UBO stays
// a real array in both arms (UBO arrays are portable).
#if defined(CBT_DECODE_INT32)
#define CBT_TEX_RING_DECL
#define CBT_RING(tex, slot) tex
#else
#define CBT_TEX_RING_DECL [CBT_FRAME_RING]
#define CBT_RING(tex, slot) tex[slot]
#endif

layout(set = CBT_DESCRIPTOR_SET, binding = 15) uniform sampler2D gHeight CBT_TEX_RING_DECL;

#if !defined(CBT_DECODE_INT32)
// Height-range pyramid (binding 21, compute only, wide arm only — mirror CBTLayout.h
// kCBTHeightRangeEntries, kCBTHeightChainRangeEntries). Kernel_HeightRangeBuild writes it from the
// bound height texture and the height chain whenever either may have changed; Classify reads it for
// the planar split's node boxes and drawn ranges. Each entry is a square block of height-lattice cells
// at one level: the lowest and highest normalized height over the block as two raw floats, after a
// header (CBT_HEIGHT_RANGE_HEADER words). The chain levels' pyramids follow the level-0 entries.
#define CBT_HEIGHT_RANGE_ENTRIES (1u << 20)
#define CBT_HEIGHT_RANGE_MAX_FIRST_LEVEL 4u
// [0] lattice cells, [1] first | top << 8, [2 + k] level first entries, [18] the chain ranges' level-0
// samples (0: none), [19] chain levels with a range, [20 + j] chain level j's first entry
#define CBT_HEIGHT_RANGE_HEADER 64u
// The first stored level's differences per block (steepest step, largest second difference along X and
// along Z, largest mixed difference), four words per block, after the chain ranges (CBTLayout.h
// kCBTHeightDifferencesFirstWord): header + 2 x (2^20 + 2^19) words.
#define CBT_HEIGHT_DIFFERENCES_FIRST_WORD 3145792u
// Each level-0 block's plane (x slope, z slope, residual low, residual width), four words per entry,
// after the differences (CBTLayout.h kCBTHeightPlaneFirstWord): CBT_HEIGHT_DIFFERENCES_FIRST_WORD +
// 4 x 2^20 words.
#define CBT_HEIGHT_PLANE_FIRST_WORD 7340096u
layout(std430, set = CBT_DESCRIPTOR_SET, binding = 21) CBT_SSBO_QUAL buffer CBTHeightRangeBuffer
{
    uint Range[];
} gHeightRange;

// The prefiltered height chain (binding 24, wide arm only — mirror CBTLayout.h kCBTHeightChainBinding).
// Kernel_HeightChainBuild writes it beside the pyramid; the terrain draw reads it (gCbtChain on the
// graphics set 2). Layout in cbt_height_chain.glsl.
layout(std430, set = CBT_DESCRIPTOR_SET, binding = 24) CBT_SSBO_QUAL buffer CBTHeightChainBuffer
{
    uint Words[];
} gHeightChain;

// The far-field normal chain's mips as storage images (binding 25, wide arm only — mirror CBTLayout.h
// kCBTNormalChainBinding / kCBTNormalChainMaxMips); Kernel_NormalChainBuild writes mip pc.passIndex.
#define CBT_NORMAL_CHAIN_MAX_MIPS 14
layout(set = CBT_DESCRIPTOR_SET, binding = 25, rg16f) uniform writeonly image2D gNormalChain[CBT_NORMAL_CHAIN_MAX_MIPS];

#include "cbt_height_chain.glsl"
float CBT_ChainLoad(uint word)
{
    return uintBitsToFloat(gHeightChain.Words[word]);
}
#endif

// Phase E resident-window atlas (design §3). The pure-math resolve (CBTTileAtlasSlot /
// CBTAtlasParams / CBT_AtlasResolve) lives in cbt_atlas.glsl; pull it in here so the
// SSBO row type is single-sourced and CBT_SampleHeight below can call the resolve. No
// bindings inside it, so this is safe to include before the taps.
#include "cbt_atlas.glsl" // defines CBT_ATLAS_WINDOW_ROWS (single-sourced, shared with the surface)

// Binding 17: the per-tile indirection table, host-visible ring. The CPU writes ring
// slot (frameIndex % ring) each frame from the residency controller's table; the shader
// reads Rows[frameSlot * CBT_ATLAS_WINDOW_ROWS + CBT_AtlasRowIndex(tile)]. A CBT_ATLAS_NO_SLOT
// row, or a row another tile owns -> the coarse fallback (never a hole).
//
// STALE-ROW PROOF (why CBTTileAtlasSlot.Generation stays RESERVED — no GPU stale-row check is
// needed, referenced by the three RESERVED notes: cbt_atlas.glsl, TerrainAtlas.h, design §3.2):
// a slot the shader samples this frame can never hold a different tile's texels than its row
// names, because three mechanisms compose to guarantee it —
//   1. Each ring slot is rewritten from the CURRENT table before it is next sampled: extraction
//      hands the current indirection rows to the render thread EVERY frame, and the render side
//      re-uploads ring slot (frameIndex % CBT_FRAME_RING) whenever that slot's last-written table
//      version differs from the current one (CBTRenderFeature's per-slot version gate). A slot thus
//      never samples a row written for a residency older than the frame sampling it — and because
//      the rows are always available, a slot flagged dirty always recovers (no parked-copy hole).
//   2. Slot quarantine >= device frames-in-flight (AtlasSlotPool): an evicted atlas slot is not
//      reassigned to a new tile until framesInFlight frames elapse, so no frame still in flight
//      (still sampling the old texels) can have its row repointed to a new tile mid-flight.
//   3. Single graphics-queue serialization: the slot-texture patch, the row upload, and the
//      VertexEval that samples them are recorded on ONE queue in submit order, so row and texels
//      are always mutually consistent for the sampling frame.
// A per-row generation compare would only re-detect what (1)-(3) already prevent, so it is
// omitted; the field is kept only so the SSBO layout is final if a future multi-queue path ever
// needs it.
layout(std430, set = CBT_DESCRIPTOR_SET, binding = 17) CBT_SSBO_QUAL buffer CBTAtlasRowsBuffer
{
    CBTTileAtlasSlot Rows[];
} gAtlasRows;

// Binding 18: the atlas height texture ring (R32_FLOAT, AtlasDim^2), linear-clamp — the
// 1-texel apron absorbs the bilinear straddle at a slot's interior edge. Rebound per frame
// slot like binding 15, so a mid-session atlas rebind never touches an in-flight element.
layout(set = CBT_DESCRIPTOR_SET, binding = 18) uniform sampler2D gAtlasHeight CBT_TEX_RING_DECL;

// Binding 19: the per-terrain COARSE height field ring (R32_FLOAT, coarseDim^2), a small
// always-resident low-res downsample of the whole terrain. An out-of-window tile (no slot)
// resolves here instead of a flat plane, so the fallback is HEIGHT-CONTINUOUS with the
// resident relief at the streaming-window edge (design §8 Risk 3, "degrade to the coarse
// source, never garbage"). Sampled endpoint-exact to match the CPU mirror SampleGridBilinearTexel.
layout(set = CBT_DESCRIPTOR_SET, binding = 19) uniform sampler2D gAtlasCoarse CBT_TEX_RING_DECL;

// Endpoint-exact bilinear read of the coarse field, a lattice like every terrain texture
// (CBT_LatticeTextureUV; the CPU mirror is SampleGridBilinearTexel at u*(Dim-1)).
float CBT_AtlasCoarseSample(vec2 uv, float coarseDim, uint frameSlot)
{
    if (coarseDim < 2.0)
        return 0.0;
    return textureLod(CBT_RING(gAtlasCoarse, frameSlot), CBT_LatticeTextureUV(uv, vec2(coarseDim)), 0.0).r;
}

#if !defined(CBT_DECODE_INT32)
// The paged height resolve (cbt_page.glsl), wide arm only (mirror CBTLayout.h kCBTPageTableBinding):
// the narrow arm keeps the unified and atlas heights.
// Binding 22: TerrainPageFeature's page-table ring, one slot per frame in flight; this frame's first
// word rides its params (pageParams lane CBT_PAGE_RING_BASE_LANE), never a word of another slot.
// Layout (mirror CBTLayout.h): [0, 4) CBTPageField's shape, [4] the cache's slot count, [5, 8) the
// floor level, the tier count and the floor directory's offset (all 0 for a dense table),
// [8, 8 + 4 * CBT_PAGE_MAX_LEVELS) one CBTPageLevel per level, then one arrival fade per cache slot
// (float bits), then the table's words (TerrainECS PageTable). The word offsets are in cbt_page.glsl.
#define CBT_PAGE_RING_BASE_LANE 0
layout(std430, set = CBT_DESCRIPTOR_SET, binding = 22) CBT_SSBO_QUAL buffer CBTPageTableBuffer
{
    uint Words[];
} gPageTable;

// Binding 23: the height page cache texture ring (R32_FLOAT, square-packed 130^2 slots), linear-
// clamp; each page's apron keeps a bilinear tap inside its slot. Rebound per frame slot like 18.
layout(set = CBT_DESCRIPTOR_SET, binding = 23) uniform sampler2D gPageCache CBT_TEX_RING_DECL;

#include "cbt_page.glsl"
#endif

// ---------------------------------------------------------------------------
// Push constants (compute only; the graphics adapter owns the sole PC block —
// plan §9). 84 bytes, mirror of CBTPushConstants.
// ---------------------------------------------------------------------------
#ifndef CBT_GRAPHICS_INCLUDE
layout(push_constant) uniform CBTPushConstants
{
    uint passIndex;
    uint baseDepth;
    uint poolSize;
    uint totalElements;
    uint neighborsReadIsA;
    uint focusRoot;
    uint targetDepth;
    uint frameIndex;
    uint classifyMode; // CBT_CLASSIFY_* — DEPTH_TARGET (tests) or SCREEN_SPACE (C4)
    uint domainMode;   // CBT_DOMAIN_* — planar (default) or spherical cube-sphere (C7)
    // C5 region-dirty reclassification (plan §8 C5): the edited terrain footprint
    // in UV [0,1]. Empty (dirtyMaxU <= dirtyMinU) => no edit => Classify's dirty
    // branch is skipped (quiescence). Only Kernel_Classify reads these.
    float dirtyMinU;
    float dirtyMinV;
    float dirtyMaxU;
    float dirtyMaxV;
    // The planar dirty rect above; the sphere's face rects are CBTFrameSlot.sphereDirty.
    // Quiescence gate for VertexEval (plan §planet-shading perf). 0 = evaluate the whole live
    // pool (the pre-slice behavior; used for the force-eval frame after init / a param change).
    // 1 = evaluate ONLY bisectors flagged CBT_FLAG_MODIFIED this frame (created / promoted /
    // edit-dirty) — unchanged bisectors keep their prior camera-independent corners, so a
    // quiescent frame re-evaluates ~0 vertices instead of the whole pool (the 35ms -> ~0 fix).
    uint gateVertexEval;
    // Near-field force-split occupancy gate A/B (round-8e): 1 = under pool pressure, stop the invisible
    // near-plane/behind-eye force-split treadmill that pins the pool at 100%; 0 = the pre-slice
    // unconditional force-split. Only Classify reads it, on the sphere with the screen-space metric.
    uint nearFieldGate;
    // Far-field region reseed (round-9 far-field-drain): a bitmask of ROOT indices (bit r == free
    // root r's subtree back to base). Only the K_REGION_FREE_* kernels read it. The set is chosen
    // CPU-side (FarFieldReseedDetector) as the INTERIOR of a wholly-horizon-invisible root region,
    // so every freed root borders only other invisible roots (freed or the retained ring) — the
    // one-root retained ring keeps the base/refined seam out of the visible boundary (design §3
    // candidate 3). 0 = no reseed this dispatch.
    uint regionFreeMask;
    uint poolPressure; // 1 = planar pool-pressure scale on; 0 = the A/B arm (tests), scale held at 1x
    // The height build kernels' region at level passIndex (mirror CBTPushConstants::BuildRegionMin): first
    // element (x | z << 16), extent (width | height << 16); samples for Kernel_HeightChainBuild, texels for
    // Kernel_NormalChainBuild, blocks for Kernel_HeightRangeBuild. Only they read them.
    uint buildRegionMin;
    uint buildRegionExtent;
    // The rest rule (mirror CBTPushConstants::Settle). Above 0 = every input matches the previous update's:
    // Kernel_Classify's planar split and merge use one threshold (CBT_PlanarClassify). Classify and
    // Kernel_Reset's live-count latch read it. CBT_SETTLE_LASTED lets the keep step's rest search step down.
    uint settle;
    // 1 = this update counts the pool census (mirror CBTPushConstants::Census). Kernel_Reset and Kernel_Classify
    // read it.
    uint census;
    // What Kernel_Classify's planar area metric keeps of a facet wholly behind the eye (CBT_BEHIND_EYE_*; mirror
    // CBTPushConstants::BehindEye).
    uint behindEye;
} pc;
#endif

// ---------------------------------------------------------------------------
// HeapID helpers (read-only, safe in both include modes)
// ---------------------------------------------------------------------------
// The heap's only storage accessors. Every read/write of a heap ID goes through this pair,
// so the u32 arm's packing (low word live, high word always 0) stays in one place. Macros
// rather than functions: the default arm then expands to exactly the direct subscript it
// always was, which keeps its SPIR-V byte-identical instead of merely equivalent.
#if defined(CBT_DECODE_INT32)
#define CBT_LoadHeapID(slot) gHeap.HeapID[slot].x
#define CBT_StoreHeapID(slot, h) gHeap.HeapID[slot] = uvec2((h), 0u)
#else
#define CBT_LoadHeapID(slot) gHeap.HeapID[slot]
#define CBT_StoreHeapID(slot, h) gHeap.HeapID[slot] = (h)
#endif

// findMSB(uint64_t) is invalid SPIR-V (GLSL.std.450 FindUMsb is u32-only, plan
// §9 / port-notes iter-4). Split hi/lo and take findMSB of the 32-bit halves.
uint CBT_HeapIDDepth(CBT_HEAPID_T h)
{
#if defined(CBT_DECODE_INT32)
    return uint(findMSB(h));
#else
    uint hi = uint(h >> 32);
    uint lo = uint(h);
    return (hi != 0u) ? (32u + uint(findMSB(hi))) : uint(findMSB(lo));
#endif
}

bool CBT_IsFreeSlot(CBT_HEAPID_T h) { return h == CBT_HEAPID_T(0); }
CBT_HEAPID_T CBT_ParentHeapID(CBT_HEAPID_T h) { return h >> 1; }
CBT_HEAPID_T CBT_SiblingHeapID(CBT_HEAPID_T h) { return h ^ CBT_HEAPID_T(1); }

uint CBT_BitWord(uint slot) { return slot >> 5u; }
uint CBT_BitMask(uint slot) { return 1u << (slot & 31u); }
uint CBT_GetBit(uint slot) { return (gBitfield.Bitfield[CBT_BitWord(slot)] >> (slot & 31u)) & 1u; }

// Live-bisector occupancy = the sum-tree root (popcount of all set bits). Valid
// once a reduce has run (InitializeRoots seeds it; the per-frame sequence
// rebuilds it at the tail). Mirrors the reference bit_count_buffer().
uint CBT_LiveCount() { return gSumTree.SumTree[CBT_SUMTREE_HEAP_BASE]; }

// ---------------------------------------------------------------------------
// GPU-write helpers — gated out of the graphics (readonly) include.
// ---------------------------------------------------------------------------
#ifndef CBT_GRAPHICS_INCLUDE

// Occupancy bit set/clear via core-GLSL u32 atomics (no int64 atomics — plan §6).
void CBT_SetBitAtomic(uint slot) { atomicOr(gBitfield.Bitfield[CBT_BitWord(slot)], CBT_BitMask(slot)); }
void CBT_ClearBitAtomic(uint slot) { atomicAnd(gBitfield.Bitfield[CBT_BitWord(slot)], ~CBT_BitMask(slot)); }

// Push `value` onto the work queue whose counter lives at `counterSlot` and
// whose payload region starts at `queueOffset`. Returns the claimed slot index
// (may be >= pool on overflow; callers ignore out-of-range slots).
int CBT_QueuePush(uint counterSlot, uint queueOffset, uint value)
{
    int slot = atomicAdd(gQueue.WorkQueue[counterSlot], 1);
    if (uint(slot) < CBT_POOL_SIZE)
        gQueue.WorkQueue[queueOffset + uint(slot)] = int(value);
    return slot;
}

// ---------------------------------------------------------------------------
// Neighbor ping-pong (port-notes §8 item 8 / mesh_updater). The two neighbor
// SSBOs form a double buffer: CURRENT (live) is read, NEXT is written. Bisect
// reads CURRENT and writes NEXT (no in-place read/write race); the host copies
// CURRENT->NEXT before Bisect so unchanged bisectors survive, then
// PropagateBisect / Simplify / PropagateSimplify operate on NEXT in place. The
// host flips pc.neighborsReadIsA every frame so NEXT becomes next frame CURRENT.
// ---------------------------------------------------------------------------
bool CBT_CurIsA() { return pc.neighborsReadIsA != 0u; }
uvec4 CBT_ReadCur(uint id) { return CBT_CurIsA() ? CBT_LoadNeighbors(gNbrA.NeighborsA[id]) : CBT_LoadNeighbors(gNbrB.NeighborsB[id]); }
uvec4 CBT_ReadNext(uint id) { return CBT_CurIsA() ? CBT_LoadNeighbors(gNbrB.NeighborsB[id]) : CBT_LoadNeighbors(gNbrA.NeighborsA[id]); }
void CBT_WriteNext(uint id, uvec4 n)
{
    if (CBT_CurIsA())
        gNbrB.NeighborsB[id] = CBT_StoreNeighbors(n);
    else
        gNbrA.NeighborsA[id] = CBT_StoreNeighbors(n);
}

// Per-component store into NEXT. The propagate kernels patch a single neighbor
// link and rely on uniquely-owned slots (no atomics), so a whole-uvec4 store
// would clobber the other components another thread may own — write just the
// one scalar (matches the reference's `_NeighborsBuffer[id][i] = value`).
void CBT_WriteNextComp(uint id, uint comp, uint value)
{
    if (CBT_CurIsA())
        CBT_NEIGHBOR_COMPONENT(gNbrB.NeighborsB[id], comp) = value;
    else
        CBT_NEIGHBOR_COMPONENT(gNbrA.NeighborsA[id], comp) = value;
}

// Bit-complement free-slot decode (reference ocbt_generic::decode_bit_complement,
// re-derived for our u32 sum tree — the reference's is bound to its u64 hybrid
// heap). Returns the physical slot of the k-th UNSET (free) bit by binary
// search: a subtree covering `slots` slots with `set` set bits has `slots - set`
// free bits. The sum tree is the prior frame's snapshot (rebuilt at each frame
// tail), so distinct alloc-cursor offsets k decode to distinct free slots. This
// is what makes merged slots reusable — Simplify clears their bits, the tail
// reduce folds that in, and the next frame's decode hands them back out.
uint CBT_DecodeFreeSlot(uint k)
{
    uint node = 0u;             // 0-based heap position, root
    uint slots = CBT_POOL_SIZE; // slots covered by the root
    for (uint d = 0u; d < CBT_SUMTREE_DEPTH; ++d)
    {
        uint leftChild = 2u * node + 1u;
        uint leftSlots = slots >> 1u;
        uint leftSet = gSumTree.SumTree[CBT_SUMTREE_HEAP_BASE + leftChild];
        uint leftFree = leftSlots - leftSet;
        if (k < leftFree)
        {
            node = leftChild;
        }
        else
        {
            k -= leftFree;
            node = leftChild + 1u; // right child
        }
        slots = leftSlots;
    }
    uint wordIndex = node - CBT_SUMTREE_LEAF_OFFSET; // leaf node -> bitfield word
    uint word = gBitfield.Bitfield[wordIndex];
    for (uint b = 0u; b < 32u; ++b)
    {
        if (((word >> b) & 1u) == 0u)
        {
            if (k == 0u)
                return wordIndex * 32u + b;
            --k;
        }
    }
    return CBT_INVALID_POINTER; // pool exhausted (the Split FreeCount gate prevents this)
}

// ---------------------------------------------------------------------------
// C4 terrain helpers (compute only): frame params, world displacement, height
// sampling, and screen-space projection for the Classify metric (plan §8 C4).
// ---------------------------------------------------------------------------
// The CPU writes the params slot (frameIndex % CBT_FRAME_RING); the shader reads
// the same slot so an in-flight frame never reads a half-written struct.
CBTFrameParams CBT_Frame() { return gParams.Params[pc.frameIndex % CBT_FRAME_RING].frame; }

// Meters per sector, equal to Mathematics::kWorldSectorSize and GE_SECTOR_SIZE. A constant,
// not renderOriginSector.w: a params fill can leave .w zero.
const float CBT_SECTOR_SIZE = 1024.0;

// World-space position of the render origin (sector * sector size). Zero when the origin is
// inactive (sector 0), so subtracting it from a world corner is an IEEE-exact no-op then and
// Classify's metric stays byte-identical to the pre-slice build. renderOriginSector.xyz holds
// the sector as float(int), so this equals camera_relative.glsl's GE_RenderOriginWorld
// (vec3(ivec3) * GE_SECTOR_SIZE) bit-for-bit — the CBT compute + the mesh/terrain draw share
// the origin the CameraData chokepoint derived, so terrain and meshes agree on where the
// camera is (Risk 2 cross-pass agreement). Exact while |sector| * CBT_SECTOR_SIZE < 2^24
// (~16,777 km — Earth's 6,371 km has vast margin); the integer sector carries the large scale.
vec3 CBT_RenderOriginWorld(CBTFrameParams fp) { return fp.renderOriginSector.xyz * CBT_SECTOR_SIZE; }

// One bisector's three corners in the frames every compute consumer needs: planet-center WORLD
// (horizon cull; fp32 at planet magnitude) and render-origin-RELATIVE
// (frustum cull, screen metric). deepTag.x selects the store of the slot:
//   0 (world corners): world = the stored corner, rel = world - originW.
//   1 (sector, local): rel = vec3(sector - originSector) * CBT_SECTOR_SIZE + local (an exact
//     integer delta, fp32-precise at any radius); world = sector * CBT_SECTOR_SIZE + local.
struct CBTCornersWS
{
    vec3 W0; vec3 W1; vec3 W2; // planet-center world
    vec3 R0; vec3 R1; vec3 R2; // render-origin-relative
};

CBTCornersWS CBT_FetchCorners(uint slot, CBTFrameParams fp)
{
    CBTCornersWS c;
    vec3 l0 = gVertex.Vertex[slot].corner0.xyz;
    vec3 l1 = gVertex.Vertex[slot].corner1.xyz;
    vec3 l2 = gVertex.Vertex[slot].corner2.xyz;
    if (gVertex.Vertex[slot].deepTag.x != 0u)
    {
        ivec3 originSector = ivec3(fp.renderOriginSector.xyz); // float holds the exact integer
        ivec3 s0 = CBT_UnpackCornerSector(gVertex.Vertex[slot].sector0);
        ivec3 s1 = CBT_UnpackCornerSector(gVertex.Vertex[slot].sector1);
        ivec3 s2 = CBT_UnpackCornerSector(gVertex.Vertex[slot].sector2);
        c.R0 = vec3(s0 - originSector) * CBT_SECTOR_SIZE + l0;
        c.R1 = vec3(s1 - originSector) * CBT_SECTOR_SIZE + l1;
        c.R2 = vec3(s2 - originSector) * CBT_SECTOR_SIZE + l2;
        c.W0 = vec3(s0) * CBT_SECTOR_SIZE + l0;
        c.W1 = vec3(s1) * CBT_SECTOR_SIZE + l1;
        c.W2 = vec3(s2) * CBT_SECTOR_SIZE + l2;
    }
    else
    {
        vec3 originW = CBT_RenderOriginWorld(fp);
        c.W0 = l0;
        c.W1 = l1;
        c.W2 = l2;
        c.R0 = l0 - originW;
        c.R1 = l1 - originW;
        c.R2 = l2 - originW;
    }
    return c;
}

// One render-origin-relative corner of `slot` (index 0, 1 or 2), equal to the matching R of
// CBT_FetchCorners: it reads one 16-byte corner, the deep tag and, for a deep slot, that corner's sector.
vec3 CBT_FetchCorner(uint slot, uint index, CBTFrameParams fp)
{
    vec3 local = index == 0u ? gVertex.Vertex[slot].corner0.xyz
               : (index == 1u ? gVertex.Vertex[slot].corner1.xyz : gVertex.Vertex[slot].corner2.xyz);
    if (gVertex.Vertex[slot].deepTag.x != 0u)
    {
        uvec2 packedSector = index == 0u ? gVertex.Vertex[slot].sector0
                           : (index == 1u ? gVertex.Vertex[slot].sector1 : gVertex.Vertex[slot].sector2);
        ivec3 originSector = ivec3(fp.renderOriginSector.xyz); // float holds the exact integer
        return vec3(CBT_UnpackCornerSector(packedSector) - originSector) * CBT_SECTOR_SIZE + local;
    }
    return local - CBT_RenderOriginWorld(fp);
}

// Terrain UV (== the LEB unit-square coord) -> world XZ, matching the CDLOD vertex
// path exactly: worldXZ = origin + uv * size (origin = terrain corner, not centre).
vec2 CBT_TerrainToWorldXZ(vec2 uv, CBTFrameParams fp)
{
    return fp.terrainOrigin.xy + uv * fp.terrainSize.xy;
}

// Sample the SAME height source the CDLOD path samples: normalized [0,1] texel
// scaled by HeightScale, offset by the terrain base height (WorldOriginY). Compute
// has no implicit LOD so we use textureLod(..., 0.0) — identical to CDLOD's
// linear-clamp mip-0 sample. uv is clamped to stay on the heightmap, and every source reads it
// at its lattice position (CBT_LatticeTextureUV, the atlas slot UV, the coarse field's UV), so the
// surface is the CPU height field's.
#if !defined(CBT_DECODE_INT32)
// cbt_page.glsl's taps over this frame's page-table ring slot (binding 22) and page cache (23).
uint CBT_PageRingBase()
{
    uint slot = pc.frameIndex % CBT_FRAME_RING;
    return uint(gParams.Params[slot].frame.pageParams[CBT_PAGE_RING_BASE_LANE]);
}

uint CBT_PageTableWord(uint index)
{
    uint base = CBT_PageRingBase();
    return gPageTable.Words[base + CBT_PAGE_FADE_OFFSET + gPageTable.Words[base + 4u] + index];
}

void CBT_PageUnresolved()
{
    atomicAdd(gQueue.WorkQueue[CBT_WQ_UNRESOLVED_PAGE_SAMPLES], 1);
}

float CBT_PageSlotFade(uint slot)
{
    return uintBitsToFloat(gPageTable.Words[CBT_PageRingBase() + CBT_PAGE_FADE_OFFSET + slot]);
}

CBTPageLevel CBT_PageLevelShape(uint level)
{
    uint at = CBT_PageRingBase() + CBT_PAGE_HEADER_WORDS + 4u * level;
    CBTPageLevel shape;
    shape.FirstEntry = gPageTable.Words[at];
    shape.PagesX = gPageTable.Words[at + 1u];
    shape.PagesZ = gPageTable.Words[at + 2u];
    shape.Pad = 0u;
    return shape;
}

float CBT_PageCacheBilinear(float texelX, float texelZ)
{
    uint frameSlot = pc.frameIndex % CBT_FRAME_RING;
    vec2 cacheDim = vec2(textureSize(CBT_RING(gPageCache, frameSlot), 0));
    return textureLod(CBT_RING(gPageCache, frameSlot), CBT_PageCacheUV(texelX, texelZ, cacheDim), 0.0).r;
}

CBTPageField CBT_PageFieldOfFrame()
{
    uint base = CBT_PageRingBase();
    CBTPageField field;
    field.Level0SamplesX = gPageTable.Words[base];
    field.Level0SamplesZ = gPageTable.Words[base + 1u];
    field.LevelCount = gPageTable.Words[base + 2u];
    field.SlotsPerRow = gPageTable.Words[base + 3u];
    field.FloorLevel = gPageTable.Words[base + 5u];
    field.TierCount = gPageTable.Words[base + 6u];
    field.DirectoryOffset = gPageTable.Words[base + 7u];
    field.Pad = 0u;
    return field;
}
#endif

float CBT_SampleHeight(vec2 uv, CBTFrameParams fp)
{
    uint frameSlot = pc.frameIndex % CBT_FRAME_RING;
    vec2 cuv = clamp(uv, 0.0, 1.0);
    float texel;
#if !defined(CBT_DECODE_INT32)
    // The paged resolve at the finest resident level (lambda 0): camera-independent, so a
    // bisector's stored corners stay valid until the residency changes under them. The camera
    // picks the levels through the residency's requests; every assign, release and arrival-fade
    // step publishes its footprint into the dirty rect, which re-evaluates the bisectors over it.
    if (fp.atlasParams1.z == 2.0)
        return CBT_PageSample(cuv.x, cuv.y, 0.0, CBT_PageFieldOfFrame()) * fp.terrainSize.z + fp.terrainSize.w;
#endif
    if (fp.atlasParams1.z != 0.0)
    {
        // Resident-window atlas (design §3.3): terrain UV -> tile -> slot -> atlas texel.
        // The row is read from THIS frame's ring slot (written before this frame's GPU
        // work), so no stale-row hazard — the CPU-side quarantine + this ring together make
        // the per-row generation check redundant (see cbt_atlas.glsl / AtlasSlotPool).
        CBTAtlasParams ap;
        ap.TileRes = uint(fp.atlasParams0.w);
        ap.SlotStride = uint(fp.atlasParams0.y);
        ap.SlotsPerRow = uint(fp.atlasParams0.z);
        ap.AtlasDim = uint(fp.atlasParams0.x);
        ap.TilesPerAxisX = uint(fp.atlasParams1.x);
        ap.TilesPerAxisZ = uint(fp.atlasParams1.y);

        ivec2 tile;
        vec2 localUV;
        CBT_AtlasTile(cuv, ap, tile, localUV);
        // The window index is below CBT_ATLAS_WINDOW_ROWS by construction.
        CBTTileAtlasSlot row = gAtlasRows.Rows[frameSlot * CBT_ATLAS_WINDOW_ROWS + CBT_AtlasRowIndex(tile, ap)];

        AtlasResolve r = CBT_AtlasResolve(cuv, ap, row);
        // Out-of-window: the coarse field (binding 19), height-continuous with the resident
        // relief at the window edge — never a flat cliff / sky-gap. coarseDim rides atlasParams1.w.
        texel = r.Resident ? textureLod(CBT_RING(gAtlasHeight, frameSlot), r.SlotUV, 0.0).r
                           : CBT_AtlasCoarseSample(cuv, fp.atlasParams1.w, frameSlot);
    }
    else
    {
        vec2 dim = vec2(textureSize(CBT_RING(gHeight, frameSlot), 0));
        texel = textureLod(CBT_RING(gHeight, frameSlot), CBT_LatticeTextureUV(cuv, dim), 0.0).r;
    }
    precise float height = texel * fp.terrainSize.z + fp.terrainSize.w; // see CBT_TerrainToWorldXZ
    return height;
}

// Project a render-origin-relative point to viewport pixel coordinates through the rebased
// view-proj (slice 1b — the caller subtracts CBT_RenderOriginWorld before calling; at origin
// 0 relPos == the world point and viewProjRel == the world viewProj, so this is byte-identical
// to the pre-slice screen metric). Returns false when the point is at/behind the near plane
// (clip.w <= 0); the caller treats that as "very close" and force-splits up to the depth cap.
// Edge LENGTH is invariant to the negative-viewport-Y flip, so no Y-flip is applied.
bool CBT_ProjectPixels(vec3 relPos, CBTFrameParams fp, out vec2 pixels)
{
    vec4 clip = fp.viewProjRel * vec4(relPos, 1.0);
    if (clip.w <= 1e-5)
    {
        pixels = vec2(0.0);
        return false;
    }
    vec2 ndc = clip.xy / clip.w;               // [-1,1]
    pixels = (ndc * 0.5 + 0.5) * fp.screen.xy; // to pixels
    return true;
}

// Conservative camera-frustum test for a bisector triangle (Classify's visible-stream
// bin, plan §8 C6). Extracts the 6 Gribb-Hartmann planes from the column-major world->
// clip matrix (row_i = (vp[0][i], vp[1][i], vp[2][i], vp[3][i]); the standard
// Gribb-Hartmann extraction) and culls the triangle ONLY when all three corners fall
// strictly outside one plane. Never culls a triangle that touches the frustum, so the
// visible stream is a superset of the truly-visible set (correct for rendering; a few
// just-outside triangles near the frustum corners may survive). Reverse-Z LH ZO safe:
// the near/far planes use the engine's row3±row2 convention, and an over-inclusive
// depth bound only keeps extra triangles. Loop is fixed at 6 iterations (bounded).
bool CBT_TriangleInFrustum(mat4 vp, vec3 a, vec3 b, vec3 c)
{
    vec4 r0 = vec4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    vec4 r1 = vec4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    vec4 r2 = vec4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    vec4 r3 = vec4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    vec4 planes[6];
    planes[0] = r3 + r0; // left
    planes[1] = r3 - r0; // right
    planes[2] = r3 + r1; // bottom
    planes[3] = r3 - r1; // top
    planes[4] = r3 + r2; // near
    planes[5] = r3 - r2; // far
    for (uint i = 0u; i < 6u; ++i)
    {
        vec4 pl = planes[i];
        float da = dot(pl.xyz, a) + pl.w;
        float db = dot(pl.xyz, b) + pl.w;
        float dc = dot(pl.xyz, c) + pl.w;
        if (da < 0.0 && db < 0.0 && dc < 0.0)
            return false; // all three corners outside this plane
    }
    return true;
}

// Exact conservative sphere occlusion (plan §planet-shading horizon cull). Is point P (a
// displaced sphere corner, |P| >= r) occluded by the floor-sphere of radius r as seen from
// camera C? A horizon-PLANE test (dot(P,C) < r|P|) is NOT conservative — it culls terrain
// BEYOND the geometric horizon that is still visible up to angular distance thetaC+thetaP
// (a raised limb peak). The exact test: P is occluded iff the angular distance between P and
// C exceeds thetaC + thetaP, where cos(thetaC)=r/|C| (camera horizon cap) and cos(thetaP)=r/|P|
// (the point's elevation reach) -> occluded iff cos(angle(P,C)) < cosC*cosP - sinC*sinP.
// r is the conservative surface FLOOR (radius minus the max relief drop) so no visible point
// is ever culled. Mirror of SphereCornerOccluded (CBTPlanetShading.h). camLen/cosC/sinC are
// per-frame constants the caller precomputes once.
bool CBT_CornerOccluded(vec3 P, vec3 camP, float camLen, float r, float cosC, float sinC)
{
    float pLen = length(P);
    if (pLen <= 0.0)
        return false;
    float cosTheta = dot(P, camP) / (pLen * camLen);
    float cosP = clamp(r / pLen, -1.0, 1.0);
    float sinP = sqrt(max(1.0 - cosP * cosP, 0.0));
    return cosTheta < cosC * cosP - sinC * sinP;
}

#endif // !CBT_GRAPHICS_INCLUDE (write/decode helpers)

#endif // CBT_GRAPHICS_INCLUDE (compute layout)

#endif // CBT_LAYOUT_GLSL
