// Surface shader: Standard PBR (unified).
// Always samples textures. When no texture is assigned, 1x1 default
// textures yield identity values so the material params control the result.
// Texture names (albedoMap, normalMap, …) are provided by the adapter's
// bindless slot macros.
//
// The declared texture set. The six ladder names keep their canonical ordinals,
// and heightMap, this surface's own name, packs onto the first free one, 6.
// @texture albedoMap srgb
// @texture normalMap linear
// @texture metallicRoughnessMap linear
// @texture emissiveMap srgb
// @texture aoMap linear
// @texture coatNormalMap linear
// @texture heightMap linear
//
// Parallax (GE_PARALLAX_MARCH): a bound height map displaces uv0 by the relief
// march, and every map samples at the displaced UV through its own tiling with
// textureGrad at the UNDISPLACED footprint, so the march's discontinuities at
// relief edges never pick a wrong mip. The hit then marches toward the primary
// directional light, and the lighting darkens that light's direct term by the
// result. Hex tiling never reaches this path: the keyword is refused while it is on.

// The emission's exposure weight (uParams19.w, emissiveExposureWeight): the forward adapter shows the
// emission as emission x exposure^weight, so 1 is physical and 0 shows it at its authored brightness
// whatever the view's exposure (Adapters/adapter_forward.glsl).
#define GE_SURFACE_EMISSIVE_EXPOSURE_WEIGHT Mat.uParams19.w

#ifdef GE_PARALLAX_MARCH
float GE_ParallaxSampleHeight(vec2 uvHeight, float lod)
{
    return textureLod(GE_USER_TEXTURE(heightMap), uvHeight, lod).r;
}

#include "../Includes/parallax_occlusion.glsl"

// What the relief march hands the rest of the surface: where every map samples, and what the
// lighting and the steps view read.
struct GE_StandardRelief
{
    vec2 uv0;                    // uv0 displaced to the relief hit, repeats
    float primaryLightOcclusion; // the hit's self-shadow toward the primary directional light, 0..1
    int heightSamples;           // height samples the view march and the self-shadow took
#ifdef GE_PARALLAX_WRITES_DEPTH
    float depthMetres;           // metres along the view ray from the polygon to the hit
#endif
#if defined(GE_PARALLAX_WRITES_DEPTH) && defined(GE_PARALLAX_DEPTH_TOLERANCE)
    float stepMetres;            // metres along the view ray one linear step spans
#endif
#ifdef GE_PARALLAX_READS_DEPTH
    bool hidden;                 // the prepass's depth here is something in front of the relief
#endif
};

// The relief at this pixel. reliefDepth rides uParams16.y (LegacyMaterialLanes.h), a fraction of
// one height repeat; 0 skips the march.
GE_StandardRelief GE_StandardParallax(SurfaceInput sIn)
{
    vec4 heightRow0 = sIn.textureST[GE_TEXSLOT_heightMap];
    vec4 heightRow1 = sIn.textureST2[GE_TEXSLOT_heightMap];
    GE_ParallaxFrame frame = GE_ParallaxBuildFrame(sIn.normalWS, sIn.tangentWS.xyz, sIn.positionFootprintX,
                                                   sIn.positionFootprintY, sIn.uvFootprint.xy, sIn.uvFootprint.zw);
    GE_ParallaxRay ray = GE_ParallaxSetupRay(frame, sIn.viewDirWS, Mat.uParams16.y, heightRow0, heightRow1);
    vec2 heightTexels = vec2(textureSize(GE_USER_TEXTURE(heightMap), 0));
    float lod = GE_ParallaxMarchLod(frame, heightRow0, heightRow1, heightTexels, ge_mipBiasParams.x);
    vec2 heightUv = GE_TransformUV(sIn.uv0, heightRow0, heightRow1);
#ifdef GE_PARALLAX_READS_DEPTH
    // The prepass marched and wrote the hit's depth; rebuild the hit from it rather than march again.
    GE_ParallaxHit hit = GE_ParallaxHitAtDepth(ray, sIn.prepassDepthOffset);
    float rayDepthError = ray.depthMetres > 0.0 ? sIn.prepassDepthOffsetError / ray.depthMetres : 0.0;
    GE_ParallaxReliefCheck check = GE_ParallaxCheckRelief(heightUv, ray, hit.rayDepth, rayDepthError, lod);
    hit.fetches = check.fetches;
#else
    GE_ParallaxHit hit = GE_ParallaxMarch(heightUv, ray, lod);
#endif

    GE_StandardRelief relief;
#ifdef GE_PARALLAX_READS_DEPTH
    relief.hidden = !check.onRelief;
#endif
    relief.uv0 = sIn.uv0 + hit.surfaceUvOffset;
    GE_ParallaxShadow shadow = GE_ParallaxSelfShadow(frame, ray, hit, heightUv, sIn.primaryLightDirectionWS,
                                                     heightRow0, heightRow1, heightTexels, ge_mipBiasParams.x);
    relief.primaryLightOcclusion = shadow.light;
    relief.heightSamples = hit.fetches + shadow.fetches;
#ifdef GE_PARALLAX_WRITES_DEPTH
    relief.depthMetres = hit.rayDepth * ray.depthMetres;
#endif
#if defined(GE_PARALLAX_WRITES_DEPTH) && defined(GE_PARALLAX_DEPTH_TOLERANCE)
    relief.stepMetres = ray.depthMetres / float(max(ray.linearSteps, 1));
#endif
    return relief;
}

// One map at uv0 through its own tiling, with the tiling's linear part applied to
// the undisplaced footprint: the transforms are affine, so the gradients are exact.
vec4 GE_StandardSampleAt(GE_MaterialTexture map, vec4 st, vec4 st2, vec2 uv0, vec4 uvFootprint)
{
    vec2 uvPerPixelX = vec2(dot(uvFootprint.xy, st.xy), dot(uvFootprint.xy, st2.xy));
    vec2 uvPerPixelY = vec2(dot(uvFootprint.zw, st.xy), dot(uvFootprint.zw, st2.xy));
    return textureGrad(map, GE_TransformUV(uv0, st, st2), uvPerPixelX, uvPerPixelY);
}
#else
// One map at `uv` through its own tiling and the material's hex blend.
vec4 GE_StandardSampleHex(GE_MaterialTexture map, vec2 uv, vec4 st, vec4 st2)
{
    vec2 uvA, uvB;
    float blend = 0.0;
    GE_ComputeHexBlendUV(uv, st, st2, Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvA, uvB, blend);
    return mix(texture(map, uvA), texture(map, uvB), blend);
}

// The tangent-space normal at `uv` through its own tiling and the hex blend: each sample is decoded,
// then the two are mixed.
vec3 GE_StandardSampleHexNormal(GE_MaterialTexture map, vec2 uv, vec4 st, vec4 st2)
{
    vec2 uvA, uvB;
    float blend = 0.0;
    GE_ComputeHexBlendUV(uv, st, st2, Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvA, uvB, blend);
    return mix(GE_DecodeTangentNormal(texture(map, uvA)), GE_DecodeTangentNormal(texture(map, uvB)), blend);
}
#endif

// The crossfade (material keyword UV1Crossfade): the albedo, metallic-roughness, occlusion and normal
// maps also sample at uv1, and the vertex alpha is the weight of the uv1 samples, not an opacity.
#if defined(GE_UV1_CROSSFADE) && defined(HAS_UV1) && defined(HAS_COLOR) && !defined(GE_PARALLAX_MARCH)
#define GE_STANDARD_UV1_CROSSFADE
#endif

#if defined(GE_STANDARD_UV1_CROSSFADE) && defined(HAS_TANGENT)
// The world normal of tangent-space `tn` in the frame of uv1: T is the rise of uv1.x from screen
// derivatives, B = cross(N, T) times the mesh's handedness. Where the derivatives read zero (the
// compat profile), the uv0 tangent stands in.
vec3 GE_StandardUV1NormalWS(SurfaceInput sIn, vec3 tn)
{
    vec3 N = sIn.TBN[2];
    vec3 dpdx = GE_DFDX(sIn.positionRelWS);
    vec3 dpdy = GE_DFDY(sIn.positionRelWS);
    vec2 duvdx = GE_DFDX(sIn.uv1);
    vec2 duvdy = GE_DFDY(sIn.uv1);
    // The gradient of uv1.x in the surface plane. The determinant's sign keeps it pointing up the
    // gradient on either winding of the screen.
    float determinant = dot(N, cross(dpdx, dpdy));
    vec3 T = (cross(dpdy, N) * duvdx.x + cross(N, dpdx) * duvdy.x) * (determinant < 0.0 ? -1.0 : 1.0);
    T -= N * dot(N, T);
    float lengthSquared = dot(T, T);
    T = lengthSquared > 1.0e-20 ? T * inversesqrt(lengthSquared) : sIn.TBN[0];
    vec3 B = cross(N, T) * (sIn.tangentWS.w < 0.0 ? -1.0 : 1.0);
    return normalize(mat3(T, B, N) * tn);
}
#endif

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

#ifdef GE_PARALLAX_MARCH
    GE_StandardRelief relief = GE_StandardParallax(sIn);
    vec2 reliefUv0 = relief.uv0;
    o.primaryLightOcclusion = relief.primaryLightOcclusion;
  #ifdef GE_PARALLAX_STEPS_VIEW
    o.parallaxHeightSamples = relief.heightSamples;
  #endif
  #ifdef GE_PARALLAX_WRITES_DEPTH
    o.depthOffset = relief.depthMetres;
  #endif
  #if defined(GE_PARALLAX_WRITES_DEPTH) && defined(GE_PARALLAX_DEPTH_TOLERANCE)
    o.depthOffsetStep = relief.stepMetres;
  #endif
  #ifdef GE_PARALLAX_READS_DEPTH
    o.reliefHidden = relief.hidden;
  #endif
    vec4 albedo = GE_StandardSampleAt(albedoMap, sIn.textureST[0], sIn.textureST2[0], reliefUv0, sIn.uvFootprint);
#else
    // Albedo: texture * UBO tint
    vec4 albedo = GE_StandardSampleHex(albedoMap, sIn.uv0, sIn.textureST[0], sIn.textureST2[0]);
  #ifdef GE_STANDARD_UV1_CROSSFADE
    float uv1Weight = sIn.vertexColor.a;
    albedo = mix(albedo, GE_StandardSampleHex(albedoMap, sIn.uv1, sIn.textureST[0], sIn.textureST2[0]), uv1Weight);
  #endif
#endif
    o.baseColor = albedo.rgb * Mat.uBaseColor.rgb * sIn.vertexColor.rgb;
#if defined(GE_PARALLAX_MARCH) && defined(ALPHA_TEST)
    // Coverage stays on the polygon in every pass (see GE_PARALLAX_MARCH): the cutout is read at
    // the undisplaced UV, exactly where the depth prepass and the cascades read it.
    o.opacity = GE_StandardSampleAt(albedoMap, sIn.textureST[0], sIn.textureST2[0], sIn.uv0, sIn.uvFootprint).a
              * Mat.uBaseColor.a * sIn.vertexColor.a;
#elif defined(GE_STANDARD_UV1_CROSSFADE)
    o.opacity = albedo.a * Mat.uBaseColor.a;
#else
    o.opacity = albedo.a * Mat.uBaseColor.a * sIn.vertexColor.a;
#endif

    // Metallic-roughness (glTF convention: G=roughness, B=metallic)
#ifdef GE_PARALLAX_MARCH
    vec4 mr = GE_StandardSampleAt(metallicRoughnessMap, sIn.textureST[2], sIn.textureST2[2], reliefUv0,
                                  sIn.uvFootprint);
#else
    vec4 mr = GE_StandardSampleHex(metallicRoughnessMap, sIn.uv0, sIn.textureST[2], sIn.textureST2[2]);
  #ifdef GE_STANDARD_UV1_CROSSFADE
    mr = mix(mr, GE_StandardSampleHex(metallicRoughnessMap, sIn.uv1, sIn.textureST[2], sIn.textureST2[2]), uv1Weight);
  #endif
#endif
    o.roughness = mr.g * Mat.uParams0.y;
    o.metallic = mr.b * Mat.uParams0.x;

    // OpenPBR base specular control: F0 from IOR, tinted + weighted (always-on;
    // neutral defaults white/1/1.5 reproduce the legacy fixed 0.04 dielectric).
    o.specularColor    = Mat.uParams13.rgb;
    o.specularWeight   = Mat.uParams13.w;
    o.specularIor      = Mat.uParams14.x;
    o.diffuseRoughness = Mat.uParams14.z; // OpenPBR diffuse roughness (Oren-Nayar; 0 = Lambert)

    // Ambient occlusion (slot 4, R channel) + emissive (slot 3, RGB). Both 1x1 defaults are
    // white: AO -> 1.0 (no occlusion) and emissive -> the colour alone, so a material without
    // these maps is driven by its parameters; the adapter consumes o.ao (ambient) and adds
    // o.emissive.
#ifdef GE_PARALLAX_MARCH
    o.ao = GE_StandardSampleAt(aoMap, sIn.textureST[4], sIn.textureST2[4], reliefUv0, sIn.uvFootprint).r;
    vec3 emissiveTex =
        GE_StandardSampleAt(emissiveMap, sIn.textureST[3], sIn.textureST2[3], reliefUv0, sIn.uvFootprint).rgb;
#else
    o.ao = GE_StandardSampleHex(aoMap, sIn.uv0, sIn.textureST[4], sIn.textureST2[4]).r;
  #ifdef GE_STANDARD_UV1_CROSSFADE
    o.ao = mix(o.ao, GE_StandardSampleHex(aoMap, sIn.uv1, sIn.textureST[4], sIn.textureST2[4]).r, uv1Weight);
  #endif
    vec3 emissiveTex = GE_StandardSampleHex(emissiveMap, sIn.uv0, sIn.textureST[3], sIn.textureST2[3]).rgb;
#endif
    // Emission authored in nits: texture x colour x luminance, scaled into scene-linear by the
    // reference-white anchor (OpenPBR emission_color x emission_luminance; an unbound texture
    // samples white). Luminance 0, the default, emits nothing. Stays inside the tonemap, so SDR
    // clamps bright emitters and HDR headroom lifts them.
    o.emissive = emissiveTex * Mat.uParams18.rgb * (Mat.uParams18.w * (1.0 / GE_EMISSION_PAPERWHITE_NITS));

    // Normal mapping (Z reconstructed — see GE_DecodeTangentNormal)
#ifdef HAS_TANGENT
  #ifdef GE_PARALLAX_MARCH
    vec3 tn = GE_DecodeTangentNormal(
        GE_StandardSampleAt(normalMap, sIn.textureST[1], sIn.textureST2[1], reliefUv0, sIn.uvFootprint));
  #else
    vec3 tn = GE_StandardSampleHexNormal(normalMap, sIn.uv0, sIn.textureST[1], sIn.textureST2[1]);
  #endif
    o.normalWS = normalize(sIn.TBN * tn);
  #ifdef GE_STANDARD_UV1_CROSSFADE
    vec3 uv1Normal = GE_StandardUV1NormalWS(
        sIn, GE_StandardSampleHexNormal(normalMap, sIn.uv1, sIn.textureST[1], sIn.textureST2[1]));
    o.normalWS = normalize(mix(o.normalWS, uv1Normal, uv1Weight));
  #endif
#else
    o.normalWS = normalize(sIn.normalWS);
#endif

    // Clear-coat normal mapping (slot 5): shade the coat lobe about its own normal so the
    // coat can carry microstructure (orange-peel, brushed lacquer) the base does not. Reuses
    // the base normal without a tangent frame (or the keyword), so the swap is a no-op there.
#ifdef GE_COAT_NORMAL_ENABLED
  #ifdef HAS_TANGENT
    #ifdef GE_PARALLAX_MARCH
    vec3 ctn = GE_DecodeTangentNormal(
        GE_StandardSampleAt(coatNormalMap, sIn.textureST[5], sIn.textureST2[5], reliefUv0, sIn.uvFootprint));
    #else
    vec3 ctn = GE_StandardSampleHexNormal(coatNormalMap, sIn.uv0, sIn.textureST[5], sIn.textureST2[5]);
    #endif
    o.coatNormalWS = normalize(sIn.TBN * ctn);
  #else
    o.coatNormalWS = o.normalWS;
  #endif
#else
    o.coatNormalWS = o.normalWS;
#endif

#ifdef GE_CLEARCOAT_ENABLED
    o.clearCoat = Mat.uParams9.x;
    o.clearCoatRoughness = Mat.uParams9.y; // GE_MIN_GGX_ALPHA floors the lobe; the full 0..1 slider is live
    o.clearCoatIor = Mat.uParams14.y;      // OpenPBR coat IOR; neutral 1.5 -> F0 0.04
    o.coatDarkening = Mat.uParams9.z;      // wet-look darkening weight (0 = inert)
    o.coatColor = Mat.uParams19.rgb;       // OpenPBR coat medium tint (white = colourless)
#endif

#ifdef GE_SHEEN_ENABLED
    o.sheenColor     = Mat.uParams10.rgb;
    o.sheenRoughness = Mat.uParams10.w; // floored in the BRDF (GE_MIN_SHEEN_ROUGHNESS)
#endif

#ifdef GE_ANISOTROPY_ENABLED
    o.anisotropy = Mat.uParams11.x; // signed [-1..1]; 0 = isotropic
    o.anisotropyRotation = Mat.uParams11.y; // radians; revolves the tangent frame in-plane (0 = aligned with the geometric tangent)
    #ifdef HAS_TANGENT
    o.tangentWS   = sIn.TBN[0]; // T (normalized in the adapter)
    o.bitangentWS = sIn.TBN[1]; // B = cross(N,T)*handedness, sign folded in by the adapter
    #endif
#endif

#ifdef GE_SUBSURFACE_ENABLED
    o.subsurfaceColor = Mat.uParams12.rgb;
    o.thickness       = Mat.uParams12.w;
#endif

#ifdef GE_TRANSMISSION_ENABLED
    o.transmissionColor  = Mat.uParams15.rgb;
    o.transmissionWeight = Mat.uParams15.w;
    o.refractionDistance = Mat.uParams16.x; // world-unit path length for the thick (two-surface) lens
    o.attenuationColor    = Mat.uParams21.rgb; // Beer–Lambert volume colour (thick path only)
    o.attenuationDistance = Mat.uParams21.w;   // reference distance in world units (0 = inert)
#endif

#ifdef GE_IRIDESCENCE_ENABLED
    o.thinFilmThickness = Mat.uParams17.x; // nm
    o.thinFilmIor       = Mat.uParams17.y;
    o.thinFilmWeight    = Mat.uParams17.z;
#endif

#ifdef GE_FUZZ_ENABLED
    o.fuzzColor     = Mat.uParams20.rgb;
    o.fuzzRoughness = Mat.uParams20.w; // floored in the BRDF (GE_MIN_SHEEN_ROUGHNESS)
#endif

    return o;
}
