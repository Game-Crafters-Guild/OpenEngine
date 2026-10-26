// Surface shader for procedural terrain grass.
// Color and wind-darkening are adapted from the referenced Three.js meadow
// shader, but the placement mask comes from the terrain splatmap grass layer.

#extension GL_EXT_nonuniform_qualifier : require

// Location 3 is the adapter's tangent slot, which procedural blades never use (see the vertex
// modifier). The full profile carries the blade's wind gate and its ROOT GROUND COLOUR there; the
// compat profile carries the wind gate alone and resolves the ground colour per fragment below.
#if defined(GE_COMPAT_PROFILE)
layout(location = 3) flat in float vGrassWindStrength;
#else
layout(location = 3) flat in vec4 vGrassBlade; // x = wind gate, yzw = root ground colour
#endif
// The field coverage at the blade's site, one value per blade (the vertex modifier's
// vGrassCoverage).
layout(location = 4) flat in float vGrassCoverage;

#if defined(GE_USER_GRASS_A2C)
// The colour pipeline resolves this surface's alpha by alpha-to-coverage, so its depth-only variant
// (the camera-prepass head) writes the same alpha for the same mask (adapter_forward.glsl).
#define GE_SURFACE_ALPHA_TO_COVERAGE
#endif

#include "Includes/bindless_textures.glsl"
#include "Includes/srgb_transfer.glsl"
#include "Includes/terrain_grass_canopy.glsl"
#include "Includes/terrain_color_map.glsl"
#include "TerrainGrass/grass_clump.glsl"

#if defined(GE_COMPAT_PROFILE)
// The resident-window atlas resolve, then the ground splat resolve built on it — the SAME file the
// placement compute runs, so a blade's colour and its placement mask read one answer. Spelled
// "CBT/..." because the surface is compiled by MaterialBuildService, whose include roots are the
// Assets/Shaders tree; the compute, whose path carries the CBT directory itself, says
// "cbt_atlas.glsl". The full profile resolves the ground in the placement compute
// (grass_root_ground_color.glsl), so only this profile's surface reads the splat.
#include "CBT/cbt_atlas.glsl"
#define GRASS_ATLAS_SET 2
#define GRASS_ATLAS_PARAMS_BINDING 5
#define GRASS_ATLAS_ROWS_BINDING 6
#endif

// --- Compat profile (WebGPU class): named bindings, no descriptor indexing --------------------
// WebGPU exposes no binding array, so every texture this surface reads is a NAMED set-2 binding
// and the CPU resolves the same indices to handles when it fills them.
//
// SEPARATE texture + sampler, not combined sampler2D: the cook splits every combined sampler into
// a texture AND a sampler, and WebGPU allows 16 samplers per stage. Six combined here plus the lit
// path's own overran that, and an overrun does not degrade — the pipeline fails to create and
// nothing draws. One shared repeat sampler is what fits; the CLAMP data lookups clamp their UV in
// place instead.
//
// Two ground layers, the same cap and the same reason as cbt_surface: a blade must read the SAME
// ground answer the terrain surface does, so this mirrors that cap rather than choosing its own.
// Slots past the second wrap onto the last bound one — the weights still blend, the extra layer
// just repeats a neighbour's texture.
#if defined(GE_COMPAT_PROFILE)
const uint kGrassCompatLayers = 2u;

layout(set = 2, binding = 20) uniform texture2D gGrassLayer0Albedo;
layout(set = 2, binding = 21) uniform texture2D gGrassLayer1Albedo;
layout(set = 2, binding = 22) uniform texture2D gGrassSplatmap;
layout(set = 2, binding = 23) uniform texture2D gGrassBladeAlbedo;
layout(set = 2, binding = 24) uniform texture2D gGrassBladeAlpha;
layout(set = 2, binding = 25) uniform texture2D gGrassBladeNormal;
layout(set = 2, binding = 26) uniform texture2D gGrassVtxHeightmap;
layout(set = 2, binding = 27) uniform texture2D gGrassVtxNormalmap;
layout(set = 2, binding = 28) uniform sampler gGrassMapSampler;
// The terrain's unified colour map (TerrainColorMap.h); an atlas-backed terrain's is not bound here.
layout(set = 2, binding = 29) uniform texture2D gGrassColormap;
// TerrainGPUParams::Flags bit (mirror Terrain::kTerrainFlagHasColorMap).
const uint kGrassTerrainFlagHasColorMap = 64u;

// GLSL cannot return an opaque sampler from a function, so the slot walk completes the tap.
vec3 GrassCompatLayerAlbedo(uint slot, vec2 uv, vec2 ddx, vec2 ddy)
{
    if (slot == 0u)
        return textureGrad(sampler2D(gGrassLayer0Albedo, gGrassMapSampler), uv, ddx, ddy).rgb;
    return textureGrad(sampler2D(gGrassLayer1Albedo, gGrassMapSampler), uv, ddx, ddy).rgb;
}

// A terrain lattice map read at a terrain UV (CBT_LatticeTextureUV): explicit LOD, and a UV that
// stays inside the texture in place of the clamp sampler this profile cannot afford.
#define GRASS_COMPAT_LATTICE_TAP(tex, uv) \
    textureLod(sampler2D(tex, gGrassMapSampler), \
               CBT_LatticeTextureUV(uv, vec2(textureSize(sampler2D(tex, gGrassMapSampler), 0))), 0.0)
#endif

// The shared albedo include's three seams. Their defaults are the bindless spelling, so only the
// compat arm differs; see Includes/terrain_material_albedo.glsl for what each one exists for.
#if defined(GE_COMPAT_PROFILE)
// Presence rides a record FLAG here: every bindless index resolves to the unbound sentinel on this
// profile, so an index test reads "untextured" for a bound layer and the ground under every blade
// falls back to its tint. Mirror of kTerrainMaterialFlagHasAlbedo (TerrainMaterialRecord.h).
#define GRASS_MATFLAG_HAS_ALBEDO 8u
#define CBT_MAT_HAS_ALBEDO(mat) (((mat).Flags & GRASS_MATFLAG_HAS_ALBEDO) != 0u)
// Only ALBEDO is bound per ground layer here (a blade needs the ground's colour, not its normal or
// ORM), so the kind is dropped and the ordinal alone selects the binding.
#define CBT_LAYER_SLOT(ord, kind) (min(ord, kGrassCompatLayers - 1u))
#define CBT_LAYER_TAP(slot, tex, uv, ddx, ddy, bias) \
    GrassCompatLayerAlbedo(slot, uv, (ddx) * exp2(bias), (ddy) * exp2(bias))
// One sampler per named binding here, so the clamp-to-edge Planar tap is the same tap.
#define CBT_LAYER_TAP_EDGE(slot, tex, uv, ddx, ddy, bias) CBT_LAYER_TAP(slot, tex, uv, ddx, ddy, bias)
#endif

// The splat taps of TerrainGrass/grass_atlas_splat.glsl, which is written against the bindless
// array this profile does not have. The unified splatmap is bound by name. The ATLAS-resident
// pair is NOT bound on this profile — no CPU binding fills it — so those two answer with zero
// weights, which grassNormalizeLayerWeights turns into the same channel-0 result the file already
// returns when no splat source is bound. An atlas terrain's grass therefore takes the ground's
// first layer here instead of its resolved blend: a documented limitation of this profile, not a
// wrong texel.
#if defined(GE_COMPAT_PROFILE)
#define GRASS_SPLAT_TAP_UNIFIED(idx, uv) GRASS_COMPAT_LATTICE_TAP(gGrassSplatmap, uv)
#define GRASS_SPLAT_TAP_ATLAS_SLOT(idx, uv) vec4(0.0)
#define GRASS_SPLAT_TAP_ATLAS_COARSE(idx, uv) vec4(0.0)
#include "TerrainGrass/grass_atlas_splat.glsl"
#endif

struct TerrainParamsEntry
{
    float WorldOriginX;
    float WorldOriginZ;
    float WorldSizeX;
    float WorldSizeZ;
    float HeightScale;
    float InvHeightmapWidth;
    float InvHeightmapHeight;
    float TexelSize;
    float WorldOriginY;
    uint SplatmapBindless;
    float MaterialTiling;
    uint LayerCount;
    uint LayerRole[4];
    float GridDimHalf;
    uint Flags;
    uint NormalmapBindless;
    uint HeightmapBindless;
    uint GrassEnabled;
    float GrassDensity;
    float GrassBladeHeight;
    float GrassBladeWidth;
    float GrassMaskThreshold;
    float GrassDensityFalloff;
    float GrassRange;
    float GrassWindStrength;
    uint GrassLayerIndex;
    float GrassBrightness;
    float GrassGroundingStrength;
    float GrassRandomScale;
    float GrassRandomBrightness;
    float GrassTranslucency;
    uint GrassRootColor;
    uint GrassTipColor;
    uint GrassBacklightColor;
    float GrassWindDirection;
    float GrassWindGustSpeed;
    float GrassWindGustScale;
    float GrassWindRestingLean;
    float GrassWindFlutterAmount;
    float GrassWindFlutterSpeed;
    float GrassWindSeed;
    uint GrassBladeSegments;
    uint GrassAlbedoBindless;
    uint GrassAlphaBindless;
    uint GrassNormalBindless;
    uint GrassAtlasColumns;
    uint GrassAtlasRows;
    uint GrassAtlasTileCount;
    float GrassAlphaCutoff;
    float GrassNormalStrength;
    float GrassTextureCardsPerSquareMeter;
    float GrassTextureSize;
    float GrassPlacementSeed;
    float GrassBladeTilt;
    float GrassBladeBend;
    float GrassBladeTwist;
    float GrassBladeTaperStart;
    float GrassTuftFan;
    float GrassClumpSize;
    float GrassClumpHeightVariance;
    float GrassClumpAlignment;
    float GrassClumpTaper;
    float GrassHueVariation;
    float GrassCanopyOcclusion;
    float GrassRootFadeStart;
    float GrassRootFadeEnd;
    uint GrassControlBindless;
    uint ColorMapBindless;
    float GrassGroundTintHeight;
};

layout(std430, set = 2, binding = 1) readonly buffer TerrainParamsBuffer
{
    TerrainParamsEntry terrains[];
};

// THE COMPAT PROFILE'S PER-FRAGMENT GROUND RESOLVE. The full profile resolves a blade's ground
// colour once, at its root, in the placement compute (grass_root_ground_color.glsl) and reads it
// from vGrassBlade: a per-fragment resolve at the fragment's own position draws a textured ground's
// pattern up a leaning blade (#4206). This profile resolves per fragment because its placement
// compute has no texture binding array to tap the layer textures through; the surface reads the two
// layer albedos as named bindings (gGrassLayer0Albedo, gGrassLayer1Albedo).
#if defined(GE_COMPAT_PROFILE)
// Never expanded on this profile — the tap seam below selects a named binding instead. Defined
// only because the include requires the symbol; expanding it is a compile error naming this line.
#define CBT_LAYER_TEX(texIdx) GE_COMPAT_PROFILE_HAS_NO_BINDLESS_TEXTURE_ARRAY


// The material record and the ALBEDO resolve run on it — the SAME file cbt_surface.glsl includes,
// so "the terrain's albedo at this point" has one definition and a blade's grounded base lands on
// the colour the ground beside it actually renders. A local restatement of that resolve (a flat
// tint or single-tap approximation) silently drops the variation/hex/side-projection/UV-phase
// terms the ground applies — sample through the include only.
#include "Includes/terrain_material_albedo.glsl"

// The terrain's material table — the SAME buffer cbt_surface.glsl reads, so a blade and the ground
// under it resolve the same RECORD for a given splat channel.
//
// They also share the RESOLVE: Includes/terrain_blend_resolve.glsl folds duplicate roles into one
// contribution and cuts to the same heaviest few on both sides, so the ranking a blade blends by is
// the ranking its ground blends by.
//
// What that does NOT make identical is where the four raw weights come from. The ground picks
// between four sources (planet slope+altitude, the painted planar splat, the resident-window atlas
// splat, and channel 0 when nothing is bound); the blend below resolves the planar three of those
// through grass_atlas_splat.glsl — the same file the placement compute resolves them through — and
// addresses them from world XZ rather than the ground's interpolated uv0. The remaining gap is the
// SPHERE, whose radial slope+altitude blend has no splat to sample and which sources no grass
// today (placement is planar-only).
//
// Nameless on purpose: MaterialBinder keys set-2 buffers on the reflected INSTANCE name, so an
// instance name here would leave the binding unresolvable, and MaterialBinder then drops the draw
// with a throttled Error log — the symptom is invisible grass, not a device loss.
layout(std430, set = 2, binding = 4) readonly buffer TerrainMaterialTableBuffer
{
    TerrainMaterialRecordData TerrainMaterials[];
};

// The blend width, from the single shared definition the ground surface also takes. Included
// before the resolve below, which is parameterized by it.
#include "Includes/terrain_blend_width.glsl"

// The shared resolve — the weight floor, the duplicate-role fold and the soft top-K the ground
// runs. Included after the define above, which is what parameterizes it.
#include "Includes/terrain_blend_resolve.glsl"

// The terrain's own surface normal at a blade's XZ, for the triplanar projection weights the shared
// albedo resolve blends its taps by. Same textures, same decode, same resolve the GROUND taps at
// that point: the pre-baked R16G16 world nx/nz normalmap on a planar terrain, the resident-window
// normal atlas (or its coarse field) on an atlas-backed one.
//
// That includes the coarse->slot upgrade CROSSFADE: for the ~0.4 s (kAtlasUpgradeFadeSeconds) after
// a tile becomes resident, cbt_surface mixes the coarse normal into the slot normal by the row's
// Fade, and so does this. The weight comes from GrassAtlas_SlotUpgradeWeight on the row THIS resolve
// read, so the blade and the ground move on one clock rather than two that could drift.
//
// Only called when a contributing material is TEXTURED. An untextured record resolves to its tint
// through the value-noise variation and never touches the projection weights, so the shipped
// untextured palette pays nothing for this.
//
// The ATLAS normal pair is not bound on this profile — no CPU binding fills it, the same limitation
// the splat seams document — so an atlas terrain falls through to the flat normal below.
vec3 grassTerrainNormalWS(TerrainParamsEntry tp, vec2 uv)
{
    if (tp.NormalmapBindless != 0u)
    {
        vec2 nXZ = GRASS_COMPAT_LATTICE_TAP(gGrassVtxNormalmap, uv).rg;
        return normalize(vec3(nXZ.x, sqrt(max(1.0 - dot(nXZ, nXZ), 0.0)), nXZ.y));
    }
    return vec3(0.0, 1.0, 0.0);
}

// The ground colour under a blade: the terrain's own albedo at the blade's XZ, resolved by the same
// expression on the same records with the same projection weights the ground uses.
//
// footprintWS is this fragment's world footprint, the argument the variation's Nyquist fade takes.
// It is the BLADE's footprint rather than the ground's — a near-vertical ribbon seen at a grazing
// angle spans differently from the ground under it — which matters only inside the fade band, at
// 0.35 to 1.2 noise cells per pixel. At the shipped VariationScale of 0.03 /m that band starts at a
// ~12 m footprint, and a blade fragment's is centimetres out to the placement range, so both sides
// sit at fade 1.0. A material authored with metre-scale cells would reach it and the two would fade
// differently.
//
// Evaluated HERE, in uniform control flow: EvaluateSurface calls this before any discard, and
// fwidth() inside the per-material loop below would be undefined where that loop diverges.
vec3 sampleTerrainBaseColor(TerrainParamsEntry tp, vec3 posWS, vec3 posRelWS)
{
    float footprintWS = length(fwidth(posWS));

    vec2 uv = clamp(vec2(
        (posWS.x - tp.WorldOriginX) / max(tp.WorldSizeX, 0.0001),
        (posWS.z - tp.WorldOriginZ) / max(tp.WorldSizeZ, 0.0001)
    ), vec2(0.0), vec2(1.0));

    // The blade's ground weights, through the shared resolve: the unified splatmap on a planar
    // terrain, the resident-window atlas slot (or its coarse field) on an atlas-backed one, and
    // channel 0 when nothing is bound. An atlas terrain carries SplatmapBindless == 0 by
    // construction, so reading that field alone answered channel 0 for the whole world.
    //
    // SHOWING the coarse->slot upgrade crossfade, unlike the placement compute: this is the colour
    // the ground beside the blade is rendering, and the ground spends kAtlasUpgradeFadeSeconds
    // arriving at it. Leading it there is a visible seam for the length of that window.
    vec4 weights = GrassAtlas_ResolveSplat(tp.Flags, tp.SplatmapBindless, uv, true);

    // The ground's own resolve, on the ground's own roles — resolved BEFORE the blend for the same
    // reason cbt_surface does it there: from here on a weight belongs to a material, not a channel.
    uint roles[4] = uint[4](tp.LayerRole[0], tp.LayerRole[1], tp.LayerRole[2],
                            tp.LayerRole[3]);
    weights = CBT_ResolveBlendWeights(weights, roles);

    // Whether any surviving material is textured, decided BEFORE the projection context is built so
    // the terrain-normal tap is paid for only where a projection weight is actually read.
    //
    // This is NOT dynamically uniform: the weights come from the splat, so two pixels of a quad
    // straddling a material boundary can disagree. That is why the normal taps it gates are
    // textureLod at level 0 — an implicit-LOD tap in divergent flow has undefined derivatives —
    // and it is the same level the placement compute reads the normal at, so a blade's shading
    // normal source and its placement's agree.
    bool anyTextured = false;
    for (int i = 0; i < 4; ++i)
        anyTextured = anyTextured || (weights[i] > 0.0 && TerrainMaterials[roles[i]].AlbedoTex != 0u);

    // `uv` is the ground's footprint UV under the blade, which a Planar material samples at.
    CBTTriplanarCtx tc = CBT_BuildTriplanarCtx(
        posRelWS, anyTextured ? grassTerrainNormalWS(tp, uv) : vec3(0.0, 1.0, 0.0), uv);
    float uvTiling = CBT_MaterialUVTiling(tp.MaterialTiling);

    vec3 color = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        // Floored, folded and cut-away channels come back EXACTLY zero, which is what makes this a
        // `> 0.0` test: a merely-small weight would still cost a fetch.
        if (weights[i] > 0.0)
        {
            TerrainMaterialRecordData mat = TerrainMaterials[roles[i]];
            CBTMaterialUV muv = CBT_MaterialUVs(mat, tc, uvTiling);
            // The blend ordinal, not the role: it selects the named binding on a profile with no
            // descriptor indexing, and is ignored on the bindless one.
            color += CBT_MaterialAlbedo(uint(i), mat, tc, muv, posWS, footprintWS) * weights[i];
        }
    }
    // The colour map, mixed in as the ground mixes it (cbt_surface.glsl). The atlas map is not bound
    // on this profile, so an atlas-backed terrain's blades read none, as its ground does.
    if ((tp.Flags & kGrassTerrainFlagHasColorMap) != 0u && !grassAtlasBacked(tp.Flags))
    {
        color = TerrainApplyColorMap(color, GRASS_COMPAT_LATTICE_TAP(gGrassColormap, uv));
    }
    return color;
}
#endif // GE_COMPAT_PROFILE

// THE COLOUR CONTRACT. Tip Color, Root Color and Backlight Color are stored as the picker's sRGB
// bytes (0xAARRGGBB), the same bytes the inspector swatch displays, and are decoded to linear albedo
// here, on read, with the engine's one sRGB curve. So the swatch an author picks is the albedo the
// blade renders before lighting. The literals carry the `f` suffix because the block is lifted into
// the host tests (GlslShim.h).
// GE_SHARED_GRASS_COLOR_DECODE_BEGIN
vec3 grassColorFromArgb(uint argb)
{
    return SRGBToLinear(vec3(
        float((argb >> 16) & 0xFFu),
        float((argb >> 8) & 0xFFu),
        float(argb & 0xFFu)
    ) * (1.0f / 255.0f));
}
// GE_SHARED_GRASS_COLOR_DECODE_END

float grassHash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

#if defined(GE_USER_GRASS_DITHER) || defined(GE_USER_GRASS_A2C)
// Jimenez interleaved gradient noise: an even per-pixel threshold distribution with
// no lattice structure at screen-door scales. Deliberately static (no temporal
// term): without TAA a rotating pattern reads as crawling noise, and rotating it
// only under TAA needs a per-view TAA signal the pinned grass pipeline does not
// receive today.
float grassDitherThreshold(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}
#endif

#if defined(GE_USER_GRASS_A2C)
// Alpha-to-coverage quantizes alpha to sampleCount+1 coverage levels; +/- half of
// this amplitude of per-pixel dither on the alpha breaks those bands where a grass
// TEXTURE authors a soft edge. Geometric blades are alpha 1.0 and never reach it —
// the endpoint guard below is what keeps their interiors solid. Sized for 4x MSAA
// (1/4); at 8x it dithers slightly wider than one level, which reads as texture,
// not error.
//
// OPEN: the editor now defaults to 2x, where one coverage band is 0.5, so whether one
// amplitude serves every sample count is unmeasured. A first attempt sited its ROI where
// 2x produced no partially-covered pixels at all, which cannot answer the question.
const float kGrassCoverageDitherAmp = 0.25;
#endif

// Distance-settled blade normals. Near the camera a blade shades with the rounded normal the vertex
// stage builds across its width, so it reads as a leaf. Far
// away that variation is sub-pixel and feeds specular aliasing rather than form, so it gives way
// to the TERRAIN normal, shading the field as one lit surface with the ground it stands on.
// Fraction of the far-LOD band over which that handover completes, so it holds as GrassRange is
// authored rather than being a second distance control that can disagree with the first.
// The literals from here to the specular weight carry the `f` suffix because the block is compiled
// as C++ by the host tests (GlslShim.h).
// GE_SHARED_GRASS_BLADE_LOBE_BEGIN
// THE CANOPY GRADIENT: the blade's own colour from its root end (GrassBladeCanopyRoot) at its root
// to TipColor at its tip, over the whole blade. Fixed, and independent of the authored ground
// handover below.
float grassCanopyRise(float t)
{
    return smoothstep(0.0f, 1.0f, t);
}

// THE CANOPY'S ROOT END. An authored Root Color is the canopy's root. Under Auto with Splat Root
// Color on (`rootFromGround`), the canopy starts from the ground the blade grows out of, so the
// ground handover and the canopy gradient make one ramp from the ground into the leaf: the blade
// reads root, then leaf. A root darker than the ground, as the tip in shade is, would show as a
// band above the handover, darker than the ground beside the blade. Grounding Strength sets how much
// of the ground the canopy starts from: 1 starts it from the ground, 0 from the tip in shade, so at
// 0 no part of the blade takes the ground, as on an authored root. Under Auto without Splat Root
// Color there is no ground to start from, and `rootColor` is the tip in shade
// (kTerrainGrassAutoRootLuminance in TerrainGrass.h).
vec3 GrassBladeCanopyRoot(vec3 rootColor, vec3 groundColor, bool rootFromGround, float groundingStrength)
{
    return rootFromGround ? mix(rootColor, groundColor, groundingStrength) : rootColor;
}

// THE CANOPY GRADIENT AT HEIGHT t, from its root end (GrassBladeCanopyRoot) to the tip. Under
// `rootFromGround` the whole root end, the ground's share and the tip in shade alike, gives way to the
// tip by groundTintHeight: the gradient is one ramp from the root end to the tip, so it never goes
// below the darker of the two, on any ground and at any Grounding Strength. At groundTintHeight 1 the
// rise is the canopy rise itself, so the gradient is the plain mix, bit for bit. groundTintHeight
// stays in [kMinTerrainGrassGroundTintHeight, 1] (TerrainGrass.h): above 1 the root end would still
// show at the tip.
vec3 GrassBladeCanopy(vec3 rootColor, vec3 groundColor, vec3 tip, bool rootFromGround, float groundingStrength,
                      float groundTintHeight, float t)
{
    float rise = rootFromGround ? smoothstep(0.0f, groundTintHeight, t) : grassCanopyRise(t);
    return mix(GrassBladeCanopyRoot(rootColor, groundColor, rootFromGround, groundingStrength), tip, rise);
}

const float kGrassNormalSettleRange = 0.55f;
// Height fraction over which the same handover completes along the BLADE: fully settled onto the
// terrain normal at the root, entirely the blade's own normal above this. It runs the WHOLE blade,
// independent of the authored colour span (RootFadeStart / RootFadeEnd), which is a material
// lever and must not compress the lighting handover.
//
// Spanning the whole blade is what keeps a dark band off the lower-middle of one. Under a low sun
// the settle and the blade's own normal pull opposite ways -- a near-horizontal blade normal
// catches more of a 15-degree sun than the terrain normal does -- so a range that ends early hands
// the fragment to its own normal while the colour is still mostly ground, and the two disagree
// across the gap. Measured into a 15-degree sun at eye height, blade/ground luma by height
// fraction, over the marker mask:
//
//   0-5%  5-12  12-20  20-30  30-42  42-55  55-70  70-85  85-100
//   0.95  0.89  0.75   0.75   0.99   1.31   1.63   1.93   2.16     at 0.55
//   0.96  0.96  0.95   0.92   0.96   1.19   1.54   1.90   2.15     at 1.0
//
// The trough at 12-30% is the band. Over the full blade a ~4% shallow remains at 20-30% -- about
// 1.3 levels at luma 30, below what reads as a band.
//
// The cost of the whole-blade settle is a HIGH sun. A blade settled onto the terrain's up-dominant
// normal catches an overhead sun that its own near-horizontal normal does not, so it brightens a
// noon field by up to 3.1 levels and moves blade-over-ground at noon from 1.103 to 1.148 -- away
// from the contact identity the settle exists to serve. Into a low sun it costs 0.9 to 1.5 levels.
// The light through the blade (GrassBladeTranslucency) is what lights the lower blade into a low
// sun. The noon brightening is a look call, not a wash.
//
// It does not share a value with the far-LOD range above: one is a fraction of the far-LOD band
// and the other a fraction of blade height, tuned against different measurements.
const float kGrassNormalSettleBladeRange = 1.0f;
// THE LEAF'S FORM. The vertex stage shades a blade as a shallow rounded channel: the two edge
// normals are the face normal turned by kGrassBladeFoldRadians about the spine, one each way, and
// the hardware interpolates between them across the width. That is the blade's whole form; it
// settles onto the terrain normal below like any other blade normal.
//
// Gloss follows the normal detail it belongs to: a lobe justified by per-blade normal variation has
// to widen as that variation settles out, or it survives as glitter on a surface that no longer has
// the geometry to earn it: a near blade is at roughness 0.55, the settled canopy is matte.
//
// THE LEAF'S SHINE. The leaf colour is the leaf as it looks face-on, its shine included, so above
// the ground handover the lobe carries no face-on reflection of its own: only the Fresnel rise, the
// pale sheen a waxy leaf takes on toward grazing angles and into a low sun. The root reflects as the
// ground it is coloured as, keeping the ground's reflection for longer inside the colour's handover
// (kGrassReflectionHandoverSharpness), and the far field keeps the settled lobe, the full dielectric
// reflectance.
const float kGrassBladeRoughness = 0.55f;
const float kGrassCanopyRoughness = 1.0f;

// THE GROUND HANDOVER: how far this fragment's ALBEDO has left the ground it grows out of. 0 below
// RootFadeStart, where the blade IS the ground, and 1 from RootFadeEnd up, where it is entirely its
// canopy color. This is a material lever, authored on the component, and it moves the color only. It
// does NOT shape the canopy: the canopy's own root-to-tip gradient (grassCanopyRise) runs the whole
// blade whatever span is authored here, so a short handover cannot flatten the upper blade into one
// color. The shading-normal settle also runs the whole blade: the lighting handover is the physical
// part, and a shorter span for it is what put a dark band across the lower-middle of backlit blades
// (kGrassNormalSettleBladeRange carries that measurement). The clamp pass keeps RootFadeEnd above
// RootFadeStart, which smoothstep requires.
//
// The clamp pass also keeps RootFadeEnd at or below the tip, so the tip is always the canopy's color.
float GrassBladeColourRise(float rootFadeStart, float rootFadeEnd, float t)
{
    return smoothstep(rootFadeStart, rootFadeEnd, t);
}

// How far the blade's colour has left the ground at this height: the handover (GrassBladeColourRise)
// at Grounding Strength 1, and 1 from the root up at Grounding Strength 0, where the blade is its own
// colour all the way down.
float GrassBladeGroundedRise(float rise, float groundingStrength)
{
    return mix(1.0f, rise, groundingStrength);
}

// How late inside the colour's handover the reflection leaves the ground's: the reflection's rise is
// the colour's rise to this power. Both reach 1 at the handover's end, so above it the blade reflects
// as leaf. Inside the handover the blade keeps the ground's reflection longer than the ground's colour:
// with the two rising together the saturated leaf colour shows at the base before the contact has
// faded into the ground. The power and the handover's span are chosen together: 8 over the default
// span (RootFadeEnd 0.275) keeps the base's saturation against the ground within 5 % of a blade with
// the full face-on reflection at every measured pose, and changes from the base's glaze to the leaf
// more softly than a higher power.
const float kGrassReflectionHandoverSharpness = 8.0f;

// How far the blade's reflection has left the ground's at this height: 0 at RootFadeStart, 1 where the
// colour's handover (GrassBladeColourRise) ends and above, rising later inside it.
float GrassBladeReflectionRise(float rootFadeStart, float rootFadeEnd, float t)
{
    return pow(GrassBladeColourRise(rootFadeStart, rootFadeEnd, t), kGrassReflectionHandoverSharpness);
}

// The weight on the blade's face-on reflectance (SurfaceOutput.specularWeight, which scales F0 only).
// `rise` is the reflection's ground handover (GrassBladeReflectionRise through GrassBladeGroundedRise),
// `farSettle` the far-LOD settle. 0 on the part of a near blade that reflects as leaf, 1 where it
// reflects as ground and across the settled far field.
float GrassBladeSpecularWeight(float rise, float farSettle)
{
    return 1.0f - rise * (1.0f - farSettle);
}
// GE_SHARED_GRASS_BLADE_LOBE_END

// LIGHT THROUGH THE BLADE. A blade is a thin leaf: some of the light that falls on it leaves through
// its far face (the lighting model's diffuse transmission, GE_DiffuseTransmissionFarFace). So a face
// turned from the sun is lit by the light through the leaf instead of going dark, and a field seen
// against the sun glows at its tips. The authored colour stays what a sunlit face reflects; the
// light through the leaf is light it scatters instead of absorbing, never more than it reflects, so
// a back-lit blade is never brighter than the same blade lit from the front.
//
// Translucency is the far face's light over the near face's at the tip: 1 for a thin leaf (as much
// through it as off it), 0 for an opaque blade. It falls to nothing toward the thick root; it starts
// at the root rather than part way up so the lower middle of a back-lit blade has light through it
// as well. The literals carry the `f` suffix because the block is lifted into the host tests
// (GlslShim.h).
// GE_SHARED_GRASS_TRANSLUCENCY_BEGIN
const float kGrassMaxTranslucency = 1.0f;

// The far face's light over the near face's at height fraction t.
float GrassBladeTranslucency(float translucency, float t)
{
    return clamp(translucency, 0.0f, kGrassMaxTranslucency) * smoothstep(0.05f, 1.0f, t);
}

// The albedo of the light through the blade: Backlight Color's hue and saturation at the given
// albedo's luminance. A brighter or darker blade, its root-to-tip tone and its canopy shade carry
// over into the light through it.
vec3 GrassTransmittedAlbedo(vec3 albedo, vec3 backlightColor)
{
    const vec3 kLuma = vec3(0.2126f, 0.7152f, 0.0722f);
    return backlightColor * (dot(albedo, kLuma) / max(dot(backlightColor, kLuma), 1e-4f));
}

// What the blade hands the lighting model. Its diffuse splits the scattered light between the two
// faces (the far face takes FarFaceShare of it), so the scattering albedo is the authored colour
// times (1 + translucency): the near face then reflects the authored colour exactly and the far face
// sends translucency times it, in the back-light's colour. Under the sky, which lights both faces,
// the leaf gathers through both. The scattering albedo never passes 1 in any channel (a leaf cannot
// scatter more light than reaches it), so a pale colour lets less light through: above a linear 0.5
// in its brightest channel the light through is held to what keeps that channel at 1.
struct GrassLeafLight
{
    vec3 Albedo;        // the scattering albedo, both faces together
    float FarFaceShare; // the share of it that leaves through the far face
    vec3 FarFaceAlbedo; // the albedo of the light through the leaf
};

GrassLeafLight GrassBladeLeafLight(vec3 color, vec3 backlightColor, float translucency, float t)
{
    float brightest = max(color.x, max(color.y, color.z));
    float through = min(GrassBladeTranslucency(translucency, t),
                        (1.0f - min(brightest, 1.0f)) / max(brightest, 1e-4f));
    GrassLeafLight leaf;
    leaf.Albedo = color * (1.0f + through);
    leaf.FarFaceShare = through / (1.0f + through);
    leaf.FarFaceAlbedo = GrassTransmittedAlbedo(leaf.Albedo, backlightColor);
    return leaf;
}
// GE_SHARED_GRASS_TRANSLUCENCY_END

uint grassAtlasTile(TerrainParamsEntry tp, float tint)
{
    uint cols = max(tp.GrassAtlasColumns, 1u);
    uint rows = max(tp.GrassAtlasRows, 1u);
    uint maxTiles = max(cols * rows, 1u);
    uint tileCount = clamp(tp.GrassAtlasTileCount, 1u, maxTiles);
    return min(uint(floor(fract(tint) * float(tileCount))), tileCount - 1u);
}

vec2 grassAtlasUV(TerrainParamsEntry tp, uint tile, float side, float heightT, float tint)
{
    uint cols = max(tp.GrassAtlasColumns, 1u);
    uint rows = max(tp.GrassAtlasRows, 1u);
    uint maxTiles = max(cols * rows, 1u);
    tile = min(tile, maxTiles - 1u);
    uint tx = tile % cols;
    uint ty = tile / cols;
    vec2 localUv = vec2(clamp(side, 0.0, 1.0), 1.0 - clamp(heightT, 0.0, 1.0));
    float flip = grassHash11(tint + 17.0) < 0.5 ? -1.0 : 1.0;
    localUv.x = flip < 0.0 ? 1.0 - localUv.x : localUv.x;
    localUv.x = clamp(localUv.x + (grassHash11(tint + 29.0) - 0.5) * 0.18, 0.0, 1.0);
    return (vec2(float(tx), float(ty)) + localUv) / vec2(float(cols), float(rows));
}

vec3 applyGrassNormalMap(TerrainParamsEntry tp, vec3 normalWS, vec3 viewDirWS, vec2 uv,
                         vec2 ddx, vec2 ddy)
{
    if (tp.GrassNormalBindless == 0u || tp.GrassNormalStrength <= 0.0)
        return normalWS;

    // Z reconstructed — see GE_DecodeTangentNormal (uniform for RGB8 and BC5 payloads).
#if defined(GE_COMPAT_PROFILE)
    // Gradients come from the caller: this function early-returns above, so nothing here is in
    // uniform control flow and WGSL rejects a derivative taken locally.
    vec3 packed = GE_DecodeTangentNormal(
        textureGrad(sampler2D(gGrassBladeNormal, gGrassMapSampler), uv, ddx, ddy));
#else
    vec3 packed = GE_DecodeTangentNormal(texture(GE_BTEX(tp.GrassNormalBindless, GE_TS_REPEAT), uv, ge_mipBiasParams.x));
#endif
    vec3 n = normalize(normalWS);
    vec3 tangent = normalize(cross(abs(n.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n));
    vec3 bitangent = normalize(cross(n, tangent));
    vec3 mapped = normalize(tangent * packed.x + bitangent * packed.y + n * max(packed.z, 0.08));
    mapped = faceforward(mapped, -normalize(viewDirWS), n);
    return normalize(mix(n, mapped, clamp(tp.GrassNormalStrength, 0.0, 2.0)));
}

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

    // Distance LOD, 1 at the camera and 0 at the placement range. It drives the blade-to-
    // ground COLOUR blend only: alpha is not distance-driven, because blades leave the field
    // by not being spawned and the placement compute already scales the outermost slots to
    // nothing. Fading a blade out while still rasterising and blending it is the overdraw the
    // placement rework exists to remove.
    float farLodTerm = GrassUnpackFarLod(sIn.custom0.x);
    float tint = sIn.custom0.y;
    float gust = GrassUnpackGust(sIn.custom0.x);
    // The ground's own normal at the blade's FOOT, carried from the vertex stage.
    vec3 terrainNormal = GrassUnpackTerrainNormal(sIn.custom0.z);
    int terrainIdx = int(GrassUnpackTerrainIndex(sIn.custom0.w));
    float clumpRand = GrassUnpackClumpRand(sIn.custom0.w);
    TerrainParamsEntry tp = terrains[terrainIdx];
    float brightness = max(tp.GrassBrightness, 0.0);
    float groundingStrength = clamp(tp.GrassGroundingStrength, 0.0, 1.0);
    float t = clamp(sIn.uv0.x, 0.0, 1.0);
    float side = clamp(sIn.uv0.y, 0.0, 1.0);
    float farLod = 1.0 - clamp(farLodTerm, 0.0, 1.0);
    float bladeRise = GrassBladeColourRise(tp.GrassRootFadeStart, tp.GrassRootFadeEnd, t);

    // The ground the blade grows out of: ONE colour per blade on the full profile, resolved at its
    // root by the placement compute, so no part of the ground's pattern can be drawn up the blade.
#if defined(GE_COMPAT_PROFILE)
    vec3 terrainColor = sampleTerrainBaseColor(tp, sIn.positionWS, sIn.positionRelWS);
    float windGate = vGrassWindStrength;
#else
    vec3 terrainColor = vGrassBlade.yzw;
    float windGate = vGrassBlade.x;
#endif
    vec3 rootColor = grassColorFromArgb(tp.GrassRootColor);
    vec3 tip = grassColorFromArgb(tp.GrassTipColor);
    vec3 backlightColor = grassColorFromArgb(tp.GrassBacklightColor);
    // The colour the blade's base hands over from: the ground under it (Splat Root Color, the
    // default), or the authored RootColor for a field that does not take its ground's colour.
    vec3 groundedEnd = ((tp.GrassEnabled & 2u) != 0u) ? terrainColor : rootColor;

    // Bit 1 is Splat Root Color, bit 5 an automatic Root Color (kTerrainGrassBit* in TerrainTypes.h).
    bool rootFromGround = (tp.GrassEnabled & 34u) == 34u;
    vec3 col = GrassBladeCanopy(rootColor, groundedEnd, tip, rootFromGround, groundingStrength,
                                tp.GrassGroundTintHeight, t);
    uint atlasTile = grassAtlasTile(tp, tint);
    vec2 textureUv = grassAtlasUV(tp, atlasTile, side, t, tint);
    // Hoisted here, the last uniform-flow point before the blade-texture branches below: WGSL
    // rejects an implicit derivative inside them. The TAAU bias folds in, since textureGrad takes
    // no bias of its own. Zero on the profile that samples with an implicit derivative and never
    // reads these.
#if defined(GE_COMPAT_PROFILE)
    vec2 bladeGradScale = vec2(exp2(ge_mipBiasParams.x));
    vec2 bladeDdx = dFdx(textureUv) * bladeGradScale;
    vec2 bladeDdy = dFdy(textureUv) * bladeGradScale;
#else
    const vec2 bladeDdx = vec2(0.0);
    const vec2 bladeDdy = vec2(0.0);
#endif
    float textureAlpha = 1.0;
    if (tp.GrassAlbedoBindless != 0u)
    {
#if defined(GE_COMPAT_PROFILE)
        vec4 texel =
            textureGrad(sampler2D(gGrassBladeAlbedo, gGrassMapSampler), textureUv, bladeDdx, bladeDdy);
#else
        vec4 texel = texture(GE_BTEX(tp.GrassAlbedoBindless, GE_TS_REPEAT), textureUv, ge_mipBiasParams.x);
#endif
        col *= texel.rgb;
        textureAlpha *= clamp(texel.a, 0.0, 1.0);
    }
    if (tp.GrassAlphaBindless != 0u)
    {
#if defined(GE_COMPAT_PROFILE)
        float alpha =
            textureGrad(sampler2D(gGrassBladeAlpha, gGrassMapSampler), textureUv, bladeDdx, bladeDdy).r;
#else
        float alpha = texture(GE_BTEX(tp.GrassAlphaBindless, GE_TS_REPEAT), textureUv, ge_mipBiasParams.x).r;
#endif
        textureAlpha *= clamp(alpha, 0.0, 1.0);
    }
#if !defined(GE_USER_GRASS_OPAQUE)
    // The card cutout, on the texture-card bit (bit2) rather than the alpha bit: a card blade is a
    // quad, so the texture's alpha is the only thing that gives it its shape. Geometric ribbons
    // leave this bit clear and are shaped by their geometry.
    //
    // KEYWORD-gated, not merely runtime-gated. A discard anywhere in a fragment module is a static
    // property of the pipeline: the hardware cannot retire the depth test before the shader has run
    // if the shader might yet kill the fragment, so an OpKill costs early-Z on every fragment the
    // module ever shades, including the ones that provably never reach it. The GRASS_OPAQUE variant
    // is the view where this test is unreachable by construction — it is compiled only when no
    // active grass row carries soft alpha, which leaves textureAlpha at 1.0, and a cutoff clamped
    // to [0,1] can never exceed it. Branching around a kill does not buy early-Z back; not
    // compiling it does.
    bool textureGrass = (tp.GrassEnabled & 4u) != 0u;
    if (textureGrass && textureAlpha < clamp(tp.GrassAlphaCutoff, 0.0, 1.0))
        discard;
#endif

    // The blade's own authored alpha, and nothing else: the sampled texture alpha. Geometric grass
    // binds no texture, so this is exactly 1.0 and the blade body is opaque end to end — its shape
    // is geometry, and nothing below may punch holes in it. Distance does not enter — see
    // farLodTerm. How a sub-1.0 alpha reaches the framebuffer is the material's GRASS_DITHER /
    // GRASS_A2C keyword's decision, at the end.
    float opacity = textureAlpha;

    float viewFacing = abs(dot(normalize(sIn.normalWS), normalize(sIn.viewDirWS)));
    float fresnelTips = pow(clamp(1.0 - viewFacing, 0.0, 1.0), 2.2);
    vec3 lodColor = mix(groundedEnd, tip, smoothstep(0.08, 0.92, fresnelTips));
    col = mix(col, lodColor, smoothstep(0.15, 0.85, farLod) * 0.85);
    // Hue jitter, at two granularities from ONE authored amount: per-clump moves whole tufts
    // apart, per-blade breaks up the inside of one. Both rotate about the achromatic axis, so they
    // change WHICH green a blade is without changing how bright it is — the value-only spread the
    // two Brightness knobs give reads as one colour lit unevenly rather than as many plants.
    // Applied BEFORE the root grounding below, so the base still resolves exactly onto the ground
    // colour instead of onto a rotated version of it.
    // The per-blade draw is rehashed off `tint` rather than used raw: `tint` is the instance's
    // width random, and hue tracking width would read as wide blades being systematically yellower.
    float hueVariation = clamp(tp.GrassHueVariation, 0.0, 1.0);
    float hueTurn = (clumpRand - 0.5) * 2.0 * hueVariation
        + (grassHash11(tint + 41.0) - 0.5) * 2.0 * hueVariation * kGrassBladeHueShare;
    col = GrassHueShift(col, hueTurn * kGrassMaxHueShiftRadians);
    // The two granularities sum: at the maximum the rotation reaches +/-42 degrees, which can push
    // a saturated channel negative. Never emit negative albedo.
    col = max(col, vec3(0.0));
    // Gust shading is WIND shading: its amplitude follows the authored wind strength, reaching
    // the full 0.66-1.08 swing at strength >= 1 and vanishing at 0. Ungated it kept modulating a
    // windless meadow — the gust field flows regardless of strength, so a static scene shimmered.
    // The vertex stage supplies the spatially resolved strength, including calm volumes.
    float windShade = groundingStrength * windGate;
    col *= mix(1.0, mix(0.66, 1.08, gust), windShade);
    // Both brightness terms belong to the CANOPY end of the schedule below, which is why they are
    // applied here, before it. The grounded end is the terrain and nothing else: scaling a colour
    // that has already resolved onto the ground would put per-blade variation back into the one
    // place a blade is supposed to disappear into its surroundings, and with RandomBrightness at
    // its shipped 0.42 the roots would spread +/-42% around the ground they stand in.
    // Rehashed off `tint` for the reason the hue draw is: `tint` is the width random, and a twisted
    // blade changes width along its length, so brightness tracking width would read as a rule.
    col *= max(0.0, 1.0 + (grassHash11(tint + 53.0) * 2.0 - 1.0) * tp.GrassRandomBrightness);
    col *= brightness;

    // THE GROUNDED END. A blade's root belongs to the ground it stands in, not to the canopy, so
    // the two ends of the handover are the terrain's own resolved colour and the fully modulated
    // canopy colour above, and the blade's base is the single mix between them on the handover.
    //
    // At t = 0 with a non-negative RootFadeStart the mix is entirely the grounded end, so with
    // Splat Root Color on the root albedo IS the terrain's own resolved colour — a structural
    // guarantee at the default rather
    // than the product of numbers that happen to multiply out, and a blade emerges from the ground
    // instead of merely standing near it. A negative RootFadeStart is the deliberate exception: it
    // starts the blend below the root, so the root carries some tip colour and a value step sits on
    // the contact line by choice.
    //
    // By default the DEPARTURE spans just over the lowest quarter of the blade, into a canopy that is still
    // near RootColor there, so the step from the ground is small; a whole-blade span instead
    // carries the ground colour halfway up the blade.
    //
    // The canopy shade on the contact is the ground's too: the canopy shade at the end of this function
    // carries why.
    //
    // GroundingStrength still turns the whole grounding read off: at 0 the weight is 1 everywhere and
    // the blade is its canopy chain from root to tip.
    float groundedRise = GrassBladeGroundedRise(bladeRise, groundingStrength);
    col = mix(groundedEnd, col, groundedRise);

    float terrainFade = smoothstep(0.28, 0.92, farLod);
    col = mix(col, terrainColor, terrainFade * 0.92);

    // The blade's OWN normal, turned to face the viewer. Blades are drawn two-sided, so half of
    // them present the face whose normal points away; shading that face on its unflipped normal
    // puts the lit side of the field on whichever blades happen to be yawed toward the sun.
    vec3 bladeNormal = normalize(sIn.normalWS);
    bladeNormal = faceforward(bladeNormal, -normalize(sIn.viewDirWS), bladeNormal);

    float canopyOcclusion = GrassBladeCanopyOcclusion(tp.GrassCanopyOcclusion, vGrassCoverage, t);
    o.metallic = 0.0;
    // ONE settle term with two reasons to be settled, so the blade cannot end up shaded as a
    // ribbon in either place the field needs it shaded as ground.
    //
    // Both settle onto `terrainNormal`, the GROUND'S OWN normal at this blade's foot, so the
    // grounded end of a blade takes the same shading normal as the ground beside it and its DIRECT
    // term matches under any sun. Settling onto anything else — including a fixed near-up vector —
    // reintroduces an angle between a fragment coloured as terrain and the terrain it sits on, and
    // the lower the sun the more of the base's light that angle costs.
    //
    // The NORMAL is all this settles. A root is not therefore indistinguishable from the ground:
    // under a low sun a departure survives even with the contact's canopy shade taken out
    // entirely -- measured with that shade pinned open, the 0.028-0.075 band's mean reaches
    // 1.00 while its darkest quarter stays at 0.867. What is left is micro-normal mismatch between
    // a blade base and the ground under a grazing sun.
    //
    // With DISTANCE, because per-blade normal variation is sub-pixel out there and feeds specular
    // aliasing rather than form.
    //
    // Along the BLADE, because a fragment coloured as ground only READS as ground if it is LIT as
    // ground. Colouring a fragment as the terrain while lighting it as a near-vertical ribbon is
    // what makes the contact read wrong. The blade term here is the whole-blade smoothstep,
    // complemented. It stays on the whole blade when the authored colour span is shorter, as the
    // shipped quarter span is: the colour is a material choice, the lighting handover is not. What
    // spanning the whole blade costs under a high sun is measured on kGrassNormalSettleBladeRange.
    // Settling here also gives the blade its dark-root, bright-tip structure out of geometry rather
    // than out of a second shade ramp.
    float farSettle = smoothstep(0.0, kGrassNormalSettleRange, farLod);
    float normalSettle = max(farSettle, 1.0 - smoothstep(0.0, kGrassNormalSettleBladeRange, t));
    // How much of the blade's own normal survives here: its form strength, faded out by the settle.
    // Folding the two into one weight is an algebraic identity, not an approximation —
    // mix(mix(C, B, f), C, s) == mix(C, B, f * (1 - s)) — so this is one mix and one normalize
    // where the nested form needed two of each, and at f = 1 it reduces exactly to the
    // mix(bladeNormal, terrainNormal, settle) it replaces.
    float bladeWeight = 1.0 - normalSettle;
    o.roughness = mix(kGrassCanopyRoughness, kGrassBladeRoughness, bladeWeight);
    float reflectionRise = GrassBladeReflectionRise(tp.GrassRootFadeStart, tp.GrassRootFadeEnd, t);
    o.specularWeight = GrassBladeSpecularWeight(GrassBladeGroundedRise(reflectionRise, groundingStrength), farSettle);
    vec3 grassNormal = normalize(mix(terrainNormal, bladeNormal, bladeWeight));
    // No brightness is handed back for the blade normal's departure from the terrain normal: the
    // fold and the arched upper blade face the sky, and a noon field measured at 1.00x its ground
    // with nothing handed back (P1, sun overhead, grass over the same frame at density 0).
    // The canopy shade: the light the field keeps off its own lower blades, sun and sky alike. It is
    // the share of the light that reaches the blade (lightVisibility), so it dims the diffuse, the
    // specular and the light through the blade alike. TWO-SIDED by construction: the ground under the
    // field takes the same depth (cbt_surface.glsl, through the same include), so at the root the
    // blade and its ground are shaded alike and the base stays the ground's colour. A blade-side
    // shade alone dims the base while nothing dims the ground beside it, which lands as a step at the
    // contact line rather than as shadow.
    o.baseColor = col;
    o.lightVisibility = canopyOcclusion;
#if defined(GE_DIFFUSE_TRANSMISSION_ENABLED)
    GrassLeafLight leaf = GrassBladeLeafLight(o.baseColor, backlightColor, tp.GrassTranslucency, t);
    o.baseColor = leaf.Albedo;
    o.diffuseTransmission = leaf.FarFaceShare;
    o.diffuseTransmissionAlbedo = leaf.FarFaceAlbedo;
#endif
    o.normalWS = applyGrassNormalMap(tp, grassNormal, sIn.viewDirWS, textureUv, bladeDdx, bladeDdy);
#if defined(GE_USER_GRASS_DITHER)
    // Screen-door: the soft alpha becomes a per-pixel coverage test, so surviving
    // fragments are fully opaque and the pipeline's depth write stays valid.
    if (opacity < grassDitherThreshold(gl_FragCoord.xy))
        discard;
    o.opacity = 1.0;
#elif defined(GE_USER_GRASS_A2C)
    // Alpha-to-coverage: the pipeline maps this alpha onto the MSAA sample mask.
    // Endpoints stay exact — dithering a fully-opaque fragment would knock MSAA
    // samples out of solid blade interiors as speckle.
    o.opacity = (opacity >= 1.0 || opacity <= 0.0)
        ? opacity
        : clamp(opacity + (grassDitherThreshold(gl_FragCoord.xy) - 0.5) * kGrassCoverageDitherAmp,
                0.0, 1.0);
#else
    o.opacity = opacity;
#endif
    return o;
}
