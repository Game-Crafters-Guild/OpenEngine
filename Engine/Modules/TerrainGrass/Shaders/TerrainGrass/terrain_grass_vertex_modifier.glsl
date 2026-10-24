// Procedural terrain grass blade vertex modifier.
// Uses TerrainGPUParams + splatmap red channel (terrain grass layer) to place
// tapered blades only where the terrain material says grass is present.

#extension GL_EXT_nonuniform_qualifier : require

// Procedural grass has no tangent vertex attribute (VertexFlags::None), so reuse the
// adapter's unused tangent location. Keep locations below 16 for WebGPU compatibility. The full
// profile carries the blade's ROOT GROUND COLOUR in the same location, beside the wind gate: one
// colour per blade, which the surface blends the blade's base into (#4206), resolved by the
// placement compute and read from the blade instance. The compat profile resolves that colour per
// fragment in the surface and carries the wind gate alone.
#if defined(GE_COMPAT_PROFILE)
layout(location = 3) flat out float vGrassWindStrength;
#else
layout(location = 3) flat out vec4 vGrassBlade; // x = wind gate, yzw = root ground colour
#endif
// The field coverage at the blade's site, for the surface's canopy occlusion. Location 4 is the
// adapter's vertex-colour slot, which procedural blades never set (HAS_COLOR).
layout(location = 4) flat out float vGrassCoverage;

layout(location = 0) in vec2 aBlade; // x = side -1..1, y = height fraction 0..1

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

struct GrassBladeInstance
{
    float WorldX;
    float WorldZ;
    float RootY;
    float BladeHeight;
    float BladeWidth;
    float Yaw;
    uint RandAndCoverage; // grass_clump.glsl owns the layout (per-blade random, field coverage)
    uint BladeShape; // grass_clump.glsl owns the bit layout (tilt, twist, tuft fan)
    uint TerrainIndex;
    // Terrain surface normal baked by the placement compute for atlas terrains (world nx/nz; ny
    // reconstructed). 0 for non-atlas terrains, which re-sample the normal below (path unchanged).
    float NormalX;
    float NormalZ;
    uint Flags; // LOD index + clump colour code; grass_clump.glsl owns the bit layout
    // The ground colour under the root, resolved once per blade by the placement compute on the
    // full profile (grass_root_ground_color.glsl); grass_clump.glsl owns the packing. 0 on compat.
    uint RootColor;
};

// TerrainGPUParams::Flags bit1 (mirror Terrain::kTerrainFlagAtlasBacked): height/normal come from
// the atlas, not a unified bindless heightmap/normalmap (both bindless indices are 0 on this path).
const uint kTerrainFlagAtlasBacked = 2u;

layout(std430, set = 2, binding = 3) readonly buffer TerrainGrassInstances
{
    GrassBladeInstance grassInstances[];
};

// The per-view budget fit shortens the AUTHORED range, and the placement compute spawns against
// the shortened one. This is the draw's copy of that scale so the distance band below can be built
// on the same radius: keyed to the authored range instead, the band is still mid-ramp where the
// last blades stand and the blade-to-ground handover never completes, which reads as a hard edge
// at the field boundary. Carries an INSTANCE name, so reflection reports it as `GrassDraw`.
layout(std140, set = 2, binding = 7) uniform GrassDrawParamsBuffer
{
    float RangeFitScale; // plan.Params.FarRadius / summary.MaxRange; 1 = the fit changed nothing
    float _Pad0;
    float _Pad1;
    float _Pad2;
} GrassDraw;

// Full profile only: this stage's taps go through GE_BTEX, and the compat half of that header
// declares a biased sampler helper, which GLSL does not allow in a vertex shader (no implicit
// LOD here). The compat arm below reads named bindings instead and needs nothing from it.
#if !defined(GE_COMPAT_PROFILE)
#include "Includes/bindless_textures.glsl"
#endif
#include "TerrainGrass/grass_clump.glsl"
#include "TerrainGrass/grass_wind_volumes.glsl"
// CBT_LatticeTextureUV: where the unified height and normal maps are read for a terrain UV.
#include "CBT/cbt_atlas.glsl"

#if defined(GE_COMPAT_PROFILE)
// WebGPU exposes no binding array; the two data maps this stage reads are named set-2 bindings.
// Bindings continue the surface's block (20-25) so one layout serves the whole material.
// Separate texture + sampler, sharing the surface's sampler at binding 28 (one binding, both
// stages) — see the surface for why a combined sampler2D is not affordable here.
layout(set = 2, binding = 26) uniform texture2D gGrassVtxHeightmap;
layout(set = 2, binding = 27) uniform texture2D gGrassVtxNormalmap;
layout(set = 2, binding = 28) uniform sampler gGrassMapSampler;
#endif

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

vec3 permute3(vec3 x)
{
    return mod(((x * 34.0) + 1.0) * x, 289.0);
}

float snoise(vec2 v)
{
    const vec4 C = vec4(0.211324865405187, 0.366025403784439,
                       -0.577350269189626, 0.024390243902439);
    vec2 i = floor(v + dot(v, C.yy));
    vec2 x0 = v - i + dot(i, C.xx);
    vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;

    i = mod(i, 289.0);
    vec3 p = permute3(permute3(i.y + vec3(0.0, i1.y, 1.0))
        + i.x + vec3(0.0, i1.x, 1.0));

    vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy),
        dot(x12.zw, x12.zw)), 0.0);
    m = m * m;
    m = m * m;

    vec3 x = 2.0 * fract(p * C.www) - 1.0;
    vec3 h = abs(x) - 0.5;
    vec3 ox = floor(x + 0.5);
    vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);

    vec3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

const mat2 kWindRot = mat2(0.86, 0.50, -0.50, 0.86) * 1.93;

// Mirror of kGrassLod1RadiusFraction in TerrainGrass/GrassPlacementModel.h: LOD 1 begins at this
// fraction of the FITTED range, and the surface's far-LOD colour blend ramps from there to that
// same range. Fitted, not authored, on both sides: the CPU derives GrassPlacementPlan::
// Lod1StartDistance from the fitted radius and hands it to the compute as Place.Bounds.z, so a band
// built on the authored radius would start the colour blend somewhere the geometry LOD does not.
const float kGrassLod1RadiusFraction = 0.18;
// Mirror of kGrassMinPixelWidth in terrain_grass_place.comp: the projected width of the whole leaf,
// in pixels, the floor holds a blade to.
const float kGrassMinPixelWidth = 1.5;
// The lean a blade takes toward one of its faces, in the same angle units as the wind's resting
// lean: what breaks one field-wide lean direction into many (a field leaning one way draws as a
// starburst straight down).
const float kGrassLeanJitter = 0.3;
// The largest tilt a blade takes at its root once its tuft's fan is added: past it the base lies on
// the ground. Mirror of Components::kMaxTerrainGrassBladeTilt.
const float kGrassMaxBladeTilt = 1.2;
// The root sinks slightly INTO the terrain along its normal rather than standing on it: a blade
// that starts exactly at the surface still shows a lit sliver of its own base through the ground
// silhouette at grazing angles. Negative = embed.
const float kGrassRootEmbed = -0.01;

// The authored per-terrain wind seed as a noise-domain offset, in the SCALED domain each field
// samples in. Every noise field in the wind path takes it, so two terrains authored with different
// seeds get uncorrelated gusts, cross-deflection and flutter instead of one field in common. A
// field that skipped it would be byte-identical on every terrain and quietly pin the others to it.
// Seed 0 is the unoffset field.
// The literals carry the `f` suffix because this block is lifted verbatim into the host tests: an
// unsuffixed literal is a float in GLSL but a double in C++, where it narrows and MSVC's C4244
// turns it into a build error (GlslShim.h).
// GE_SHARED_WIND_SEED_OFFSET_BEGIN
vec2 windSeedOffset(float seed)
{
    return vec2(seed * 31.7f, seed * -17.3f);
}
// GE_SHARED_WIND_SEED_OFFSET_END

float sfbm(vec2 p)
{
    uint terrainIdx = grassInstances[uint(gl_InstanceIndex)].TerrainIndex;
    TerrainParamsEntry tp = terrains[terrainIdx];
    p += windSeedOffset(tp.GrassWindSeed);
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; ++i)
    {
        v += a * snoise(p);
        p = kWindRot * p;
        a *= 0.5;
    }
    return v * 0.5 + 0.5;
}

struct WindField
{
    vec2 dir;
    float strength;
    float detail;
};

// Cross-wind deflection of the authored direction by the field's cross noise. `flow` advects the
// noise, so vec2(0) samples the unadvected field — the flowing variant's flow == 0 slice. It
// varies with position and never with time; at any other instant the flowing field has translated
// away from it.
vec2 windDeflectedDir(vec2 dir, vec2 p, float gustScale, vec2 flow, float seed)
{
    float cross = snoise(p * gustScale * 1.7 - flow * 0.85 + vec2(-31.0, 23.0)
                         + windSeedOffset(seed));
    vec2 crossDir = vec2(-dir.y, dir.x);
    return normalize(dir + crossDir * (cross * 0.55));
}

// timeSeconds is the endpoint's animation clock (inst.deformationTimeSeconds), whose per-frame
// advance is capped (Time.cpp). Wall time must never drive wind: a stalled frame loop would resume
// by rendering the whole elapsed phase gap in a single frame, and a captured frame's wind would
// not be part of the capture.
WindField sampleWindField(TerrainParamsEntry tp, vec2 p, float timeSeconds)
{
    vec2 dir = vec2(cos(tp.GrassWindDirection), sin(tp.GrassWindDirection));
    vec2 flow = dir * (timeSeconds * tp.GrassWindGustSpeed);

    float gustScale = max(tp.GrassWindGustScale, 0.0001);
    float broad = sfbm(p * gustScale - flow);
    float detail = snoise(p * gustScale * 3.1 - flow * 1.7 + vec2(17.0, -9.0)
                          + windSeedOffset(tp.GrassWindSeed));

    WindField w;
    w.dir = windDeflectedDir(dir, p, gustScale, flow, tp.GrassWindSeed);
    w.strength = clamp(0.28 + broad * 0.62 + detail * 0.10, 0.0, 1.0);
    w.detail = detail;
    return w;
}

// The RESTING lean direction: the authored direction deflected by the same cross noise the wind
// field uses, sampled UNADVECTED (flow = 0), so it varies per position but never per frame. The
// resting pose must not ride the animated field: with WindStrength authored to zero the field
// still flows, and a static-magnitude lean along a flowing direction is frame-to-frame churn on
// every blade (the ~11%-of-pixels shimmer this replaced).
vec2 restingLeanDir(TerrainParamsEntry tp, vec2 p)
{
    vec2 dir = vec2(cos(tp.GrassWindDirection), sin(tp.GrassWindDirection));
    return windDeflectedDir(dir, p, max(tp.GrassWindGustScale, 0.0001), vec2(0.0),
                            tp.GrassWindSeed);
}

// A unified height or normal map read at a terrain UV: those maps are terrain lattices
// (CBT_LatticeTextureUV), so the blade's root follows the surface the terrain draws.
#if defined(GE_COMPAT_PROFILE)
#define GRASS_VTX_LATTICE_TAP(mapIndex, mapTex, uv) \
    textureLod(sampler2D(mapTex, gGrassMapSampler), \
               CBT_LatticeTextureUV(uv, vec2(textureSize(sampler2D(mapTex, gGrassMapSampler), 0))), 0.0)
#else
#define GRASS_VTX_LATTICE_TAP(mapIndex, mapTex, uv) \
    textureLod(GE_BTEX(mapIndex, GE_TS_CLAMP), \
               CBT_LatticeTextureUV(uv, vec2(textureSize(GE_BTEX(mapIndex, GE_TS_CLAMP), 0))), 0.0)
#endif

float sampleHeight01(TerrainParamsEntry tp, vec2 uv)
{
    if (tp.HeightmapBindless == 0u)
        return 0.0;
    return GRASS_VTX_LATTICE_TAP(tp.HeightmapBindless, gGrassVtxHeightmap, uv).r;
}

vec3 sampleTerrainNormalWS(TerrainParamsEntry tp, vec2 uv)
{
    if (tp.NormalmapBindless != 0u)
    {
        vec2 nXZ = GRASS_VTX_LATTICE_TAP(tp.NormalmapBindless, gGrassVtxNormalmap, uv).rg;
        float ny = sqrt(max(1.0 - dot(nXZ, nXZ), 0.0));
        return normalize(vec3(nXZ.x, ny, nXZ.y));
    }

    if (tp.HeightmapBindless == 0u)
        return vec3(0.0, 1.0, 0.0);

    float texelSize = tp.TexelSize;
    float hL = sampleHeight01(tp, uv + vec2(-texelSize, 0.0));
    float hR = sampleHeight01(tp, uv + vec2( texelSize, 0.0));
    float hD = sampleHeight01(tp, uv + vec2(0.0, -texelSize));
    float hU = sampleHeight01(tp, uv + vec2(0.0,  texelSize));
    float spacingX = max(tp.WorldSizeX * texelSize * 2.0, 0.0001);
    float normalScaleX = tp.HeightScale / spacingX;
    return normalize(vec3(
        (hL - hR) * normalScaleX,
        1.0,
        (hD - hU) * normalScaleX
    ));
}

// THE BLADE'S SHAPE at one vertex: the spine at angle tilt + bend t^2 + droop t^4 from the terrain
// normal over the height fraction t, the leaf's side bent with the spine and twisted about it, the
// edge offset across it, the rounded shading normal at that edge, and the point this vertex takes
// its GROUND COLOUR from. Lifted verbatim into the host suite (GrassBladeVertexTests), so its
// literals carry the `f` suffix and it reads no buffer, texture or global.
// GE_SHARED_GRASS_BLADE_VERTEX_BEGIN
// A blade's leaf is never wider than this fraction of its height, so a short blade cannot become a
// square whatever BladeWidth asks for. A leaf is 3 to 8 mm wide on a 0.4 to 0.8 m blade, so a
// fifth is well above any leaf and only intervenes at extreme aspect ratios.
const float kGrassMaxBladeAspect = 0.2f;
// The tip droop as a share of the bend: a cantilever loaded by its own weight bends more toward
// its free end than a uniform curvature does.
const float kGrassBladeDroopRatio = 0.6f;
// How far the shading normal turns about the spine at each edge of the leaf: a shallow channel, as a
// grass leaf is. Shading only: the strip stays two vertices wide and the hardware interpolates the
// two edge normals across it, so the normal turns smoothly from one edge to the other and the leaf
// shades as a rounded surface. A wider turn splits a blade into a sunlit half and a dark half.
const float kGrassBladeFoldRadians = 0.34906585f; // 20 degrees
// Projected leaf width in pixels over which the fold fades in: none at 3 px, all of it from 8 px.
const float kGrassFoldFadePixels0 = 3.0f;
const float kGrassFoldFadePixels1 = 8.0f;

struct GrassBladeVertex
{
    vec3 Position;          // world position of this vertex
    vec3 Tangent;           // unit spine direction at this height
    vec3 Normal;            // unit folded shading normal at this vertex's edge (unflipped)
};

// The spine's angle from the terrain normal at height fraction t, in radians.
float GrassBladeSpineAngle(float tilt, float bend, float t)
{
    float t2 = t * t;
    return tilt + bend * t2 + bend * kGrassBladeDroopRatio * t2 * t2;
}

// The angle of the chord from the root to height fraction t: the spine angle's mean over [0, t].
// A vertex placed along it lies on the arc the angle law describes, to first order in how far the
// angle turns over the blade, with one sine and cosine instead of a sum over the rows below it.
float GrassBladeChordAngle(float tilt, float bend, float t)
{
    float t2 = t * t;
    return tilt + bend * t2 * (1.0f / 3.0f) + bend * kGrassBladeDroopRatio * t2 * t2 * 0.2f;
}

// The largest angle the chord from the root to the tip may take from the terrain normal: the tip
// stays above the ground plane, short of horizontal, in the strongest gust at a tuft's edge.
const float kGrassMaxTipChordRadians = 1.4f; // 80 degrees

// The tilt a blade of this bend may take: what is left of the tip's chord angle after the bend
// takes its share (GrassBladeChordAngle at t = 1). Applied after the fan, the wind and the per-blade
// draws are summed.
float GrassBladeCappedTilt(float tilt, float bend)
{
    float bendShare = bend * (1.0f / 3.0f) + bend * kGrassBladeDroopRatio * 0.2f;
    return clamp(tilt, 0.0f, max(kGrassMaxTipChordRadians - bendShare, 0.0f));
}

// The leaf's width at height fraction t as a fraction of its base width: parallel sides up to
// taperStart, then a smooth taper to a point at the tip. taperStart stays below 1
// (kMaxTerrainGrassBladeTaperStart), where smoothstep's span would be empty.
float GrassBladeWidthProfile(float taperStart, float t)
{
    float s = smoothstep(taperStart, 1.0f, t);
    return 1.0f - s * s;
}

// Half the leaf's width at height fraction t, in metres: the strip spans this on each side of the
// spine. `bladeWidth` is the whole leaf's width at its base (TerrainGrass::BladeWidth), capped at
// kGrassMaxBladeAspect of the height.
float GrassBladeHalfWidth(float bladeWidth, float bladeHeight, float taperStart, float t)
{
    return 0.5f * min(bladeWidth, bladeHeight * kGrassMaxBladeAspect) * GrassBladeWidthProfile(taperStart, t);
}

// The pixel floor and the fold's fade, per vertex. The placement measures how wide a blade projects
// from its ROOT facing (`sideDir`, terrain_grass_place.comp) and hands over the half-width that
// keeps the leaf kGrassMinPixelWidth wide there; the twist turns the face along the blade, so the
// floor is rescaled by how far this vertex's face is turned toward the camera against the root's.
// A blade widened while edge-on at its root keeps a thin edge where it twists face-on, instead of
// opening into a leaf several times its width. The fold fades out as the leaf's projected width
// falls toward a few pixels, where its two shades would only draw noise.
struct GrassBladeFloor
{
    float MinHalfWidth;  // half-width that projects to the pixel floor at the root facing; 0 = none
    float MaxExpansion;  // ceiling on the widening, a multiple of the blade's own half-width
    float RootPixels;    // projected width in pixels of the whole authored leaf at the root facing; 0 = unknown
    vec3 CameraPos;      // world
};

// Projected pixels of the vertex's face against the root's: |side x view| is how far a side
// direction is turned across the view. For a unit side and any view vector w, |side x w|^2 is
// |w|^2 - (side . w)^2, so the ratio needs no normalize and no cross product. The root edge has a
// floor that stays above zero for a zero-length or underflowing view, so the ratio is always finite.
float GrassBladeFacingRatio(vec3 rootSide, vec3 vertexSide, vec3 view)
{
    float viewSq = dot(view, view);
    float rootDot = dot(rootSide, view);
    float vertexDot = dot(vertexSide, view);
    float rootEdgeSq = max(viewSq - rootDot * rootDot, 1e-6f * viewSq + 1e-12f);
    return sqrt(max(viewSq - vertexDot * vertexDot, 0.0f) / rootEdgeSq);
}

// `bendDir` and `sideDir` are unit vectors in the terrain's tangent plane; `side` is the vertex's
// edge, -1 or +1, and `halfWidth` the half-width at this height before the pixel floor.
// `twistRadians` is the torsion over the whole blade, applied in proportion to t.
GrassBladeVertex GrassBuildBladeVertex(vec3 root, vec3 terrainNormal, vec3 bendDir, vec3 sideDir,
                                       float tilt, float bend, float twistRadians, float t,
                                       float bladeHeight, float side, float halfWidth,
                                       GrassBladeFloor pixelFloor)
{
    float angle = GrassBladeSpineAngle(tilt, bend, t);
    float chord = GrassBladeChordAngle(tilt, bend, t);
    float ca = cos(angle);
    float sa = sin(angle);
    vec3 spine = root + (terrainNormal * cos(chord) + bendDir * sin(chord)) * (t * bladeHeight);
    vec3 tangent = terrainNormal * ca + bendDir * sa;

    // The side direction bends WITH the spine: rotated about the bend axis by the spine's own
    // angle, so it stays perpendicular to the tangent however the blade leans relative to its face.
    vec3 axis = cross(terrainNormal, bendDir);
    vec3 bentSide = sideDir * ca + cross(axis, sideDir) * sa + axis * (dot(axis, sideDir) * (1.0f - ca));
    // Then the leaf's torsion, about the spine.
    float twist = twistRadians * t;
    vec3 twistedSide = bentSide * cos(twist) + cross(tangent, bentSide) * sin(twist);
    vec3 face = cross(twistedSide, tangent);

    float facing = GrassBladeFacingRatio(sideDir, twistedSide, spine - pixelFloor.CameraPos);
    float floorHalf = pixelFloor.MinHalfWidth / max(facing, 1e-3f);
    float width = max(halfWidth, min(floorHalf, halfWidth * pixelFloor.MaxExpansion));
    float pixels = pixelFloor.RootPixels > 0.0f ? pixelFloor.RootPixels * facing : 1e6f;
    float fold = kGrassBladeFoldRadians * smoothstep(kGrassFoldFadePixels0, kGrassFoldFadePixels1, pixels);

    GrassBladeVertex bv;
    bv.Position = spine + twistedSide * (side * width);
    bv.Tangent = normalize(tangent);
    // The face normal rotated about the tangent by +fold at the edge side = -1 and by -fold at
    // side = +1: rotating `face` about the tangent by an angle f gives face cos f + twistedSide sin f.
    bv.Normal = normalize(face * cos(fold) - twistedSide * (side * sin(fold)));
    return bv;
}
// GE_SHARED_GRASS_BLADE_VERTEX_END

vec3 tangentPlaneDir(vec3 dir, vec3 normalWS)
{
    vec3 projected = dir - normalWS * dot(dir, normalWS);
    float len2 = dot(projected, projected);
    if (len2 > 0.0001)
        return projected * inversesqrt(len2);
    return normalize(cross(abs(normalWS.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), normalWS));
}

void ModifyVertex(inout VertexOutput v, InstanceData inst)
{
    uint instanceIdx = uint(gl_InstanceIndex);
    GrassBladeInstance blade = grassInstances[instanceIdx];
    uint terrainIdx = blade.TerrainIndex;
    TerrainParamsEntry tp = terrains[terrainIdx];

    float worldX = blade.WorldX;
    float worldZ = blade.WorldZ;
    vec2 terrainUV = clamp(vec2(
        (worldX - tp.WorldOriginX) / max(tp.WorldSizeX, 0.0001),
        (worldZ - tp.WorldOriginZ) / max(tp.WorldSizeZ, 0.0001)
    ), vec2(0.0), vec2(1.0));
    // Atlas terrains have no unified heightmap/normalmap (both bindless indices are 0): the
    // placement compute already snapped RootY through the atlas and baked the surface normal into
    // the instance, so use those directly. Non-atlas terrains re-sample exactly as before.
    bool atlasBacked = (tp.Flags & kTerrainFlagAtlasBacked) != 0u;
    float rootY = blade.RootY;
    if (tp.HeightmapBindless != 0u)
        rootY = tp.WorldOriginY + sampleHeight01(tp, terrainUV) * tp.HeightScale;
    vec3 terrainNormal;
    if (atlasBacked)
    {
        float ny = sqrt(max(1.0 - blade.NormalX * blade.NormalX - blade.NormalZ * blade.NormalZ, 0.0));
        terrainNormal = normalize(vec3(blade.NormalX, ny, blade.NormalZ));
    }
    else
    {
        terrainNormal = sampleTerrainNormalWS(tp, terrainUV);
    }
    float r = GrassUnpackRand(blade.RandAndCoverage);
    uint shape = blade.BladeShape;
    vec3 sideDir = tangentPlaneDir(vec3(cos(blade.Yaw), 0.0, sin(blade.Yaw)), terrainNormal);

    tp = grassApplyWindVolumes(tp, vec3(worldX, rootY, worldZ));
    // Every tier samples the animated field: the wind is what makes a meadow read as a meadow, and
    // a distance band that holds a constant lean instead reads as a painted backdrop behind the
    // grass that moves. The field is sampled at the blade ROOT, so it is one value per blade and
    // the far tier's reduced blade already pays it over half as many vertices. Volumes modify the
    // terrain entry first so direction, gust, and lean all see the same resolved wind.
    WindField wind = sampleWindField(tp, vec2(worldX, worldZ), inst.deformationTimeSeconds);
    vec2 leanDirStatic = restingLeanDir(tp, vec2(worldX, worldZ));

    float bladeHeight = max(0.0, blade.BladeHeight);
    // The whole leaf's width at its base (a card's edge for a textured card).
    float bladeWidth = max(0.0, blade.BladeWidth);
    // Half-width this blade may not project narrower than, sized once per blade by the placement
    // compute and carried as a multiple of half the width above. Taken from the AUTHORED width,
    // before the aspect cap, because the floor is a screen-space quantity: it is the same number of
    // world metres whichever width the blade goes on to take. 0 = no floor.
    float minWidthScale = GrassMinWidthScaleFromFlags(blade.Flags);
    float minWidth = 0.5 * bladeWidth * minWidthScale;
    float t = clamp(aBlade.y, 0.0, 1.0);
    bool texturedCard = (tp.GrassEnabled & 4u) != 0u;
    if (texturedCard)
        bladeHeight = max(bladeWidth, 0.001);
    float width = texturedCard ? 0.5 * bladeWidth
                               : GrassBladeHalfWidth(bladeWidth, bladeHeight, tp.GrassBladeTaperStart, t);
    float flutterPhase =
        snoise(vec2(worldX, worldZ) * 0.37 + windSeedOffset(tp.GrassWindSeed)
               + inst.deformationTimeSeconds * max(0.0, tp.GrassWindFlutterSpeed));
    float windStrength = max(0.0, tp.GrassWindStrength) * 0.45;
    // Flutter rides the ANIMATED field direction, so it must carry the authored strength or a
    // meadow at WindStrength 0 still animates at the component's non-zero default FlutterAmount.
    // Same gate as the surface's wind shading: identity at strength >= 1, zero at 0.
    float windGate = clamp(tp.GrassWindStrength, 0.0, 1.0);
    // Per-blade ±4% lean variation from data that is a pure function of the blade's site.
    // NEVER from gl_InstanceIndex: the compacted slot a blade lands in is reallocated by a
    // subgroup atomicAdd every frame, so anything derived from it re-rolls per frame and reads
    // as field-wide jitter on a static camera.
    float bladeLean = mix(0.96, 1.04, hash11(r + blade.Yaw * 0.37));
    // Lean composed as a VECTOR: the resting pose along the static per-site direction, the wind
    // and flutter response along the animated field direction. A scalar sum applied along the
    // animated direction would swing the resting pose with the field and double-count the gusts.
    // Zero resolved wind keeps grass still, including inside calm volumes.
    vec2 leanVec = leanDirStatic * tp.GrassWindRestingLean
        + wind.dir * (wind.strength * windStrength
                      + tp.GrassWindFlutterAmount * windGate * (wind.detail + flutterPhase * 0.35));
    float leanMag = length(leanVec);

    // The blade's own shape, from its site: tilt and bend about the authored values, a twist in
    // one direction or the other, a lean toward one of its faces, and, in a tuft, a fan away from
    // the tuft's site that grows with the distance from it. A textured card is a picture of
    // blades, so it keeps the wind's lean alone and stays flat.
    float tilt = 0.0;
    float bendShape = 0.0;
    float twistRadians = 0.0;
    vec2 shapeLean = vec2(0.0);
    if (!texturedCard)
    {
        tilt = tp.GrassBladeTilt * mix(0.6, 1.4, GrassBladeShapeTiltRand(shape));
        bendShape = tp.GrassBladeBend * mix(0.6, 1.4, GrassBladeShapeBendRand(shape));
        float twistSigned = GrassBladeShapeTwistSigned(shape);
        twistRadians = tp.GrassBladeTwist * 6.28318530718 * (twistSigned < 0.0 ? -1.0 : 1.0)
            * (0.5 + abs(twistSigned));
        vec3 faceDir = cross(sideDir, terrainNormal);
        shapeLean = faceDir.xz * (kGrassLeanJitter * GrassBladeShapeJitterSign(shape));
        if (tp.GrassClumpSize > 0.0)
        {
            float distanceCells = GrassBladeShapeDistanceCells(shape);
            float fanTurn = GrassBladeShapeFanTurn(shape) * 6.28318530718;
            float fanAngle = tp.GrassTuftFan * distanceCells;
            shapeLean += vec2(cos(fanTurn), sin(fanTurn)) * fanAngle;
            tilt += fanAngle;
        }
    }
    // One lean plane per blade, from the sum of every reason to lean: the shape's above and the
    // wind's. A windless field keeps its blades where their shape puts them, and a gust swings the
    // plane toward the wind as its share of the sum grows.
    vec2 planeVec = shapeLean + leanVec;
    float planeLen = length(planeVec);
    vec2 leanDir2 = planeLen > 0.0001 ? planeVec / planeLen : leanDirStatic;
    vec3 bendDir = tangentPlaneDir(vec3(leanDir2.x, 0.0, leanDir2.y), terrainNormal);
    // The wind bends the blade on top of its shape. The sign of a backward gust lives in the
    // direction, so the magnitude clamp keeps only an upper bound.
    float bend = min(bendShape + leanMag * bladeLean, 1.15);
    tilt = GrassBladeCappedTilt(min(tilt, kGrassMaxBladeTilt), bend);

    vec3 root = vec3(worldX, rootY, worldZ) + terrainNormal * kGrassRootEmbed;
    // Hold the blade to a minimum projected width, per vertex, inside GrassBuildBladeVertex: the
    // floor lifts the TAPERED width where it has gone sub-pixel, rescaled by how far that vertex's
    // twisted face is turned toward the camera. The expansion ceiling is a multiple of the same
    // width, which keeps the tip a point (zero times anything is zero). Where the blade already
    // exceeds the floor, or there is no floor (minWidth 0), the width is unchanged.
    GrassBladeFloor pixelFloor;
    pixelFloor.MinHalfWidth = minWidth;
    pixelFloor.MaxExpansion = kGrassMaxWidthExpansion;
    pixelFloor.RootPixels = minWidthScale > 0.0 ? kGrassMinPixelWidth / minWidthScale : 0.0;
    pixelFloor.CameraPos = Cam.uCameraPos.xyz;
    GrassBladeVertex bladeVertex = GrassBuildBladeVertex(root, terrainNormal, bendDir, sideDir, tilt,
                                                         bend, twistRadians, t, bladeHeight,
                                                         aBlade.x, width, pixelFloor);
    vGrassCoverage = GrassUnpackCoverage(blade.RandAndCoverage);
#if defined(GE_COMPAT_PROFILE)
    vGrassWindStrength = windGate;
#else
    // Every vertex of the blade reads the same instance word, so the flat varying carries one
    // ground colour for the whole blade whichever vertex provokes a triangle.
    vGrassBlade = vec4(windGate, GrassUnpackRootColor(blade.RootColor));
#endif

    // Distance LOD term the surface reads: 1 at the camera, 0 at the range placement actually
    // spawned to. It drives the blade-to-ground COLOUR blend, which keeps the far field from
    // reading as noise. It is NOT an opacity fade — the surface no longer routes it into alpha —
    // because blades leave by not being spawned and the placement compute scales the outermost
    // slots to nothing. The band is derived from the range rather than being its own knob, so there
    // is one distance control on this component, not two that can disagree — and it is the FITTED
    // range, the same one the compute used, not the authored ceiling.
    float dist = distance(Cam.uCameraPos.xz, root.xz);
    float fieldRange = max(tp.GrassRange * GrassDraw.RangeFitScale, 1.0);
    float lodStart = fieldRange * kGrassLod1RadiusFraction;
    float farLodTerm = 1.0 - smoothstep(lodStart, max(lodStart + 1.0, fieldRange), dist);

    v.position = bladeVertex.Position;
    v.normal = bladeVertex.Normal;
    v.uv0 = vec2(t, aBlade.x * 0.5 + 0.5);
    // Three of the four components carry packed payloads — grass_clump.glsl owns the whole
    // layout and states why each field is sized the way it is.
    //
    // The terrain normal goes through here rather than being resampled in the fragment stage
    // because this stage can resolve it on every terrain and that one cannot: the fragment side
    // has no heightmap-gradient fallback, so on a heightmap-only terrain it answers world-up and
    // would reintroduce exactly the base-versus-ground mismatch the settle exists to remove. It is
    // also the FOOT's normal that the surface wants, which is constant along a blade; a fragment
    // resample would drift with the bend.
    v.custom0 = vec4(GrassPackFarLodAndGust(farLodTerm, wind.strength),
                     r,
                     GrassPackTerrainNormal(terrainNormal),
                     GrassPackTerrainAndClump(terrainIdx, GrassClumpCodeFromFlags(blade.Flags)));
}
