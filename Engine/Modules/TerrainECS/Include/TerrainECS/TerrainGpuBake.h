#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <cstdlib>
#include <vector>

// The CPU side of the GPU terrain height bake: the kill switch, the rows the kernel
// (terrain_height_bake.comp) reads, and the report of why a bake stays on the CPU. A bake that holds
// one modifier or tile the kernel cannot express runs whole on the CPU.

namespace GameEngine::TerrainECS
{

// Kill switch: GE_TERRAIN_GPU_BAKE arms the planar GPU height bake, default ON (=0 disables).
// Defined in TerrainGpuBake.cpp (not header-inline) so the test override below is one instance across the
// Engine-dll / test-exe boundary — a DATA symbol does not export reliably under the auto-export.
bool IsGpuHeightBakeEnabled();

// Test seam for the GPU eval-skip oracles: force the gate on/off (state 0/1) or clear back to the
// env (state -1). The env gate caches on first read, so a test can't arm the path by setting the
// env after the first Update() already latched it off — this drives it deterministically instead.
void SetGpuHeightBakeEnabledForTests(int state);

// Why a tiled terrain's height bake runs on the CPU instead of the GPU kernel. The order of the
// enumerators is not the check order: the terrain gates (Disabled, TextureLayout, BaseSource) are
// checked first, then each modifier in priority order (PackModifiersForGpuBake).
enum class GpuHeightBakeRefusalReason : uint8
{
    None,
    // Terrain gates.
    Disabled,              // GE_TERRAIN_GPU_BAKE=0
    TextureLayout,         // the heights are not in an atlas that holds every tile (Single, Unified)
    BaseSource,            // the base is not the procedural noise the kernel composes
    // Modifier gates, one modifier and one effect at a time.
    SplineShape,
    GlobalVolume,
    InwardFeatherOrWeight, // an inward feather, or a master weight other than 1
    PooledBlend,           // an effect with the Average blend
    HeightFromVolume,      // a flatten that takes its height from the volume
    Erosion,               // a noise effect with erosion armed
    HeightOffset,
    Stamp,
    GroundClaim,
    OtherEffect,           // an effect kind the GPU bake has no code for (grass, color)
    SculptZone,
};

// The reason in plain words, for the debug readback and the log.
const char* GpuHeightBakeRefusalName(GpuHeightBakeRefusalReason reason);

// The first refusal and the volume or zone entity that holds it. Entity is invalid for a terrain gate.
struct GpuHeightBakeRefusal
{
    GpuHeightBakeRefusalReason Reason = GpuHeightBakeRefusalReason::None;
    ECS::EntityHandle Entity{};

    bool operator==(const GpuHeightBakeRefusal&) const = default;
};

enum class TerrainHeightBakePath : uint8
{
    NotBaked,
    Gpu,
    Cpu,
};

// The path in one word, for the debug readback.
const char* TerrainHeightBakePathName(TerrainHeightBakePath path);

// What the last height bake of a tiled terrain did (TerrainModifierSystem writes it; the debug
// readback and the volume inspector read it). The modifier refusal is found even when a terrain gate
// refuses first, so a reader sees both walls.
struct TerrainHeightBakeReport
{
    TerrainHeightBakePath Path = TerrainHeightBakePath::NotBaked;
    GpuHeightBakeRefusalReason TerrainRefusal = GpuHeightBakeRefusalReason::None;
    GpuHeightBakeRefusal ModifierRefusal;
    // The refusing effects after ModifierRefusal; a modifier that refuses as a whole counts once.
    uint32 OtherModifierRefusals = 0;
    // Increases each time the path or a refusal changes, so a reader can follow the report.
    uint32 Revision = 0;
    // Lattice samples the page overlay composed with the modifier stack itself in its last build.
    // Zero when the overlay read every sample from the baked tiles.
    uint64 PageOverlayComposedSamples = 0;

    // The refusal a reader acts on: the terrain gate when one refuses, else the modifier refusal.
    GpuHeightBakeRefusal FirstRefusal() const
    {
        if (TerrainRefusal != GpuHeightBakeRefusalReason::None)
            return GpuHeightBakeRefusal{TerrainRefusal, ECS::EntityHandle{}};
        return ModifierRefusal;
    }
};

// GPU-bakeable modifier kinds (slice 1). Mirror the branch ids in terrain_height_bake.comp.
enum class ModifierGpuType : uint32
{
    Flatten = 0u,
    Noise   = 1u,
};

// GPU-bakeable shape kinds (slice 1: circle + rectangle; spline stays CPU). Mirror the
// SHAPE_* ids in terrain_height_bake.comp (which match Components::TerrainModifierShape).
enum class ModifierGpuShape : uint32
{
    Circle    = 0u,
    Rectangle = 1u,
};

// std430 GPU twin of the slice-1 subset of ResolvedModifier (mirror ModifierGpu in
// terrain_height_bake.comp). Rows are pre-sorted by priority CPU-side; the kernel applies
// them in array order exactly as TerrainModifierSystem::ApplyHeightModifiers iterates the
// sorted vector. 80 bytes, std430-compatible (each vec4 group is naturally 16-aligned).
struct ModifierGpu
{
    uint32 Type;      // ModifierGpuType
    uint32 Shape;     // ModifierGpuShape
    uint32 Blend;     // Components::TerrainModifierBlend — read by BOTH flatten and noise
    uint32 Octaves;   // noise octaves (unused for flatten)

    // vec4 CenterYaw: x=world center X, y=world center Z, z=yaw radians, w=unused.
    float CenterX;
    float CenterZ;
    float Yaw;
    float CenterPad;

    // vec4 ExtentsFalloff: x=radius, y=rectHalfX, z=rectHalfZ, w=falloff.
    float Radius;
    float RectHalfX;
    float RectHalfZ;
    float Falloff;

    // vec4 NoiseParams: noise -> x=frequency, y=amplitude, z=lacunarity, w=persistence;
    // flatten -> x=target height, the AUTHORED world Y. The row stays terrain-independent (one
    // packed list serves the whole bake); the kernel measures TerrainOriginY off per dispatch.
    float NoiseFreqOrTarget;
    float NoiseAmp;
    float NoiseLacunarity;
    float NoisePersistence;

    uint32 Seed;            // noise seed (0 -> kernel substitutes 12345, mirroring the CPU)
    float BlendSmoothing;   // SmoothMin / SmoothMax blend radius, metres of height
    uint32 Pad0;
    uint32 Pad1;
};
static_assert(sizeof(ModifierGpu) == 80, "ModifierGpu must match terrain_height_bake.comp std430 layout");

// How a bake's modifier rows become the height pass's storage binding and its push count.
//
// Those are two different numbers, derived in two different files — the buffer in
// TerrainRenderFeature, the push in TerrainExtractionSystem — so both come from here. A storage
// binding may not be zero-sized, so a modifier-free bake (a base-fill-only re-bake) still binds
// ONE zero-filled row, while pc.ModifierCount stays 0 and the kernel's stack loop never runs.
// Sizing the push off the padded element instead would run the stack once over a fabricated row.
// Reading terrain_height_bake.comp, such a row evaluates to nothing today: it is a circle of
// radius 0 with falloff 0, for which ComputeWeight returns 0 at every world position (radius 0
// leaves distFromEdge <= 0 everywhere, and the falloff <= 0 branch there returns 0), so EvalHeight
// skips it at `w <= 0.0` before Type or Blend are read. Carrying the authored count keeps
// base-only bakes correct without depending on that weight staying zero across kernel edits.
struct ModifierBindingSizes
{
    size_t ModifierBytes = 0;          // never 0 — a zero-sized storage binding is invalid
    uint32 KernelModifierCount = 0;    // what the push carries: the AUTHORED count, padding excluded
    bool ModifiersArePadding = false;  // true when the buffer holds only the zero-fill element
};

inline ModifierBindingSizes ComputeModifierBindingSizes(size_t modifierCount)
{
    ModifierBindingSizes s{};
    s.ModifiersArePadding = (modifierCount == 0);
    s.ModifierBytes = (modifierCount == 0 ? size_t{1} : modifierCount) * sizeof(ModifierGpu);
    s.KernelModifierCount = static_cast<uint32>(modifierCount);
    return s;
}

// GPU-expressible volume scopes for a surface rule row. Circle/Rectangle share the
// ModifierGpuShape ids; Global has no TerrainModifierShape member (it is a volume shape,
// not a modifier shape) and so gets its own id. Mirror the GE_RULE_SHAPE_* constants in
// terrain_surface_rules.glsl.
enum class SurfaceRuleGpuShape : uint32
{
    Circle    = 0u,
    Rectangle = 1u,
    Global    = 2u,
};

// std430 twin of Components::TerrainRuleCondition (mirror GESurfaceRuleCondition in
// terrain_surface_rules.glsl). Conditions live in their own flat buffer, addressed by the
// row's ConditionBase, rather than as an array member of the row: a dynamically indexed
// array inside a struct read out of an SSBO makes glslang spill the whole struct to
// per-invocation scratch. 32 bytes, all scalars.
struct SurfaceRuleConditionGpu
{
    uint32 Kind;            // Components::TerrainRuleConditionKind
    uint32 Curve;           // Components::TerrainRuleFalloffCurve
    float  Min;
    float  Max;
    float  Feather;
    float  NoiseFrequency;
    uint32 NoiseSeed;
    uint32 Pad0;
};
static_assert(sizeof(SurfaceRuleConditionGpu) == 32,
              "SurfaceRuleConditionGpu must match terrain_surface_rules.glsl std430 layout");

// std430 twin of one authored row PLUS the volume scope it was authored inside (mirror
// GESurfaceRule). The CPU bake multiplies the volume's shape weight into every row, so a
// FLATTENED row carries its volume's framing; rows are packed in the order
// ApplySplatModifiers walks them — modifier priority, then effect stack order. 64 bytes.
struct SurfaceRuleGpu
{
    uint32 MaterialSlot;   // splat channel, clamped by the packer exactly as the CPU clamps
    float  Strength;
    uint32 Replace;        // bool
    uint32 ConditionCount;

    uint32 ConditionBase;  // first index into the condition buffer
    uint32 Shape;          // SurfaceRuleGpuShape
    float  CenterX;
    float  CenterZ;

    float  Yaw;            // radians
    float  Radius;
    float  RectHalfX;
    float  RectHalfZ;

    float  Falloff;
    float  FalloffInward;
    float  VolumeWeight;
    uint32 Pad0;
};
static_assert(sizeof(SurfaceRuleGpu) == 64,
              "SurfaceRuleGpu must match terrain_surface_rules.glsl std430 layout");

// How a bake's surface-rule rows become the splat pass's two storage bindings.
//
// THE TWO NUMBERS ARE NOT THE SAME NUMBER, and conflating them is the trap this exists to make
// testable. A storage binding may not be zero-sized, so a rules-free bake still allocates and
// binds ONE zero-filled element of each — while the KERNEL is told SurfaceRuleCount = 0 and reads
// neither. Reporting the padded element as a rule instead would hand the kernel a fabricated row
// on every rules-free bake — the commonest bake there is under the atlas opt-in. Today that row
// happens to be harmless (a zero-filled row short-circuits at VolumeWeight <= 0 in
// GE_ApplySurfaceRuleRow), so the split is defence-in-depth: carrying the AUTHORED count keeps
// rules-free correctness independent of that short-circuit surviving future shader edits.
struct SurfaceRuleBindingSizes
{
    size_t RuleBytes = 0;         // never 0 — a zero-sized storage binding is invalid
    size_t ConditionBytes = 0;    // never 0, same reason
    uint32 KernelRuleCount = 0;   // what the push carries: the AUTHORED count, padding excluded
    bool RulesArePadding = false; // true when the row buffer holds only the zero-fill element
    bool ConditionsArePadding = false;
};

inline SurfaceRuleBindingSizes ComputeSurfaceRuleBindingSizes(size_t ruleCount,
                                                              size_t conditionCount)
{
    SurfaceRuleBindingSizes s{};
    s.RulesArePadding = (ruleCount == 0);
    s.ConditionsArePadding = (conditionCount == 0);
    s.RuleBytes = (ruleCount == 0 ? size_t{1} : ruleCount) * sizeof(SurfaceRuleGpu);
    s.ConditionBytes =
        (conditionCount == 0 ? size_t{1} : conditionCount) * sizeof(SurfaceRuleConditionGpu);
    s.KernelRuleCount = static_cast<uint32>(ruleCount);
    return s;
}

// Push constants for one tile's bake dispatch (mirror PushConstants in
// terrain_height_bake.comp). 80 bytes, under the 128-byte guaranteed minimum.
struct TerrainHeightBakePush
{
    int32  RectMinX;   // tile-local interior sample rect (inclusive origin)
    int32  RectMinZ;
    uint32 RectW;      // rect width  in samples (maxX - minX + 1)
    uint32 RectH;      // rect height in samples

    float OriginX;     // tile world origin (XZ)
    float OriginZ;
    float TileSize;    // tile world size (square)
    float HeightScale;

    float  BaseFreq;   // the base value noise's stack (Terrain::TerrainNoiseStack)
    float  BaseGain;
    uint32 BaseOctaves;
    uint32 BaseSeed;

    uint32 Slot;       // atlas slot for this tile
    uint32 ModifierCount;
    uint32 TileRes;    // interior samples per tile axis
    uint32 SlotStride;

    uint32 SlotsPerRow;
    // Surface rule rows in the row buffer (SPLAT kernel only; 0 for height/normal). Rules
    // composite on top of the procedural classification in buffer order.
    uint32 SurfaceRuleCount;

    // The TERRAIN entity's world Y — not the tile's, which OriginX/OriginZ describe. A baked
    // sample is read back as `normalized * HeightScale + TerrainOriginY`, so the flatten branch
    // measures its world-space target against this, mirroring NormalizedHeightForWorldY on the
    // CPU arm.
    float TerrainOriginY;
    float BaseAmplitude; // the base stack's Amplitude (Terrain::TerrainNoiseStack)
};
static_assert(sizeof(TerrainHeightBakePush) == 80, "TerrainHeightBakePush must match terrain_height_bake.comp push_constant layout");

// One tile's GPU height-bake request. The TerrainModifierSystem records these (with the
// tile-local sample rect it would have CPU-baked) onto the service when the GPU path is
// eligible; the TerrainExtractionSystem resolves the tile's atlas slot and hands them to
// TerrainRenderFeature::QueueGpuHeightBake. Interior-sample rect is inclusive.
struct GpuHeightBakeTileRequest
{
    int32  TileX = 0;        // tile grid coord (may be negative); extraction resolves the atlas slot
    int32  TileZ = 0;
    int32  RectMinX = 0;     // tile-local interior sample rect (inclusive)
    int32  RectMinZ = 0;
    int32  RectMaxX = 0;
    int32  RectMaxZ = 0;
    float  OriginX = 0.0f;   // tile world origin (XZ)
    float  OriginZ = 0.0f;
    float  TileSize = 0.0f;  // tile world size (square)
};

// One resolved tile dispatch: the push constants (slot + rect + params) plus the tile index
// (so a settle readback can route the result back to the right CPU tile heightfield).
struct GpuHeightBakeDispatch
{
    TerrainHeightBakePush Push;
    int32 TileX = 0;   // tile grid coord (for settle-readback routing back to the CPU tile)
    int32 TileZ = 0;
};

// A whole bake's GPU work: the priority-sorted modifier rows (shared by every tile) plus
// the per-tile requests. Recorded by TerrainModifierSystem, drained by TerrainExtractionSystem
// in the same Extraction phase. Cleared after the extraction system consumes it.
struct GpuHeightBakeBatch
{
    std::vector<ModifierGpu> Modifiers;              // priority-sorted; may be empty (base-only re-bake)
    std::vector<GpuHeightBakeTileRequest> Tiles;
    float HeightScale = 0.0f;
    // The terrain entity's world Y, shared by every tile of the bake (the tiles differ in XZ
    // only). Feeds TerrainHeightBakePush::TerrainOriginY so the kernel's flatten branch reads
    // world-space targets the way the CPU bake does.
    float TerrainOriginY = 0.0f;
    float BaseFreq = 0.0f;
    float BaseGain = 0.0f;
    uint32 BaseOctaves = 0;
    uint32 BaseSeed = 0;
    float BaseAmplitude = 0.0f;
    // Whether the splat pass may run for this bake. False when the bake carries a splat writer
    // the kernel has no twin for (paint, or a surface-rules volume on a spline shape): those keep
    // the CPU splat until settle.
    bool SplatEligible = false;
    // Surface rule rows the splat kernel composites on top of the procedural classification,
    // already flattened into (modifier priority, then stack order) with each row carrying its
    // volume's scope. Empty when no rules volume is in the bake.
    std::vector<SurfaceRuleGpu> SurfaceRules;
    std::vector<SurfaceRuleConditionGpu> SurfaceRuleConditions;
    bool Settle = false;     // stroke-settle bake -> its tiles need a GPU->CPU readback afterwards
    bool Pending = false;    // set by the modifier system, cleared by the extraction system

    void Clear()
    {
        Modifiers.clear();
        Tiles.clear();
        HeightScale = 0.0f;
        TerrainOriginY = 0.0f;
        BaseFreq = 0.0f;
        BaseGain = 0.0f;
        BaseOctaves = 0;
        BaseSeed = 0;
        BaseAmplitude = 0.0f;
        SplatEligible = false;
        SurfaceRules.clear();
        SurfaceRuleConditions.clear();
        Settle = false;
        Pending = false;
    }
};

} // namespace GameEngine::TerrainECS
