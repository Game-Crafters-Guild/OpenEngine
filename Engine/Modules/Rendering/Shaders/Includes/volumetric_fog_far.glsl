// The volumetric fog past the froxel grid's far plane: the analytic continuation the
// VolumetricFog composite gives distant geometry and sky (volumetric_fog_composite.frag),
// shared with the blended surfaces that take the fog by their own distance
// (volumetric_fog_forward.glsl).
// The includer declares the parameter block (instance GE_VOLUMETRIC_FOG), the irradiance
// cube (GE_VOLUMETRIC_FOG_IRRADIANCE), and includes volumetric_fog_view.glsl and
// volumetric_fog_phase.glsl first.
#ifndef GE_VOLUMETRIC_FOG_FAR_GLSL
#define GE_VOLUMETRIC_FOG_FAR_GLSL

// How the irradiance cube is read (one level); a shader that discards before reading it defines an
// explicit-level read, which takes no derivative.
#ifndef GE_VOLUMETRIC_FOG_IRRADIANCE_TAP
#define GE_VOLUMETRIC_FOG_IRRADIANCE_TAP(s, coords) GE_TAP_LOD0(s, coords)
#endif

// Antiderivative of exp(-max(s,0)/H) in s, made continuous at s=0 (G(0)=0): below
// the fog base density is constant (slope 1), above it decays exponentially.
float FogHeightG(float s, float H)
{
    return s < 0.0 ? s : H * (1.0 - exp(-s / H));
}

// Analytic exponential height-fog continuation past the froxel grid's far plane.
// The grid covers [0, maxDistance]; geometry/sky beyond it would otherwise get no
// fog. The optical depth of the SAME density model the media pass uses
// (density0 * exp(-(h-base)/falloff)) has a closed form along the ray, so this is
// evaluated analytically — NOT ray-marched — which avoids step banding entirely.
// Anchored to the grid's last-slice (scatter, transmittance): a zero-length
// segment at the boundary reproduces the grid value, so the join is seamless.
vec4 FogExtendBeyondGrid(vec4 gridFog, vec2 uv, float sceneDistance)
{
    const float kFarRange = 40000.0; // cap for sky / degenerate far depths
    // Feather width of the fog->sky join (VolumetricFogEffect.SkyFade); min keeps
    // the smoothstep well-formed so 0 reads as a near-hard horizon edge.
    float skyFadeBand = max(GE_VOLUMETRIC_FOG.uLocalVolumeParams.z, 0.02);

    float maxDist = GE_VOLUMETRIC_FOG.uVolumeParams.x;
    float farEnd = min(sceneDistance, maxDist + kFarRange);
    float seg = farEnd - maxDist;
    if (seg <= 0.0)
        return gridFog;

    vec3 ro = FogRayOriginFromUv(uv);
    vec3 rd = FogWorldRayFromUv(uv);
    float base = GE_VOLUMETRIC_FOG.uHeightParams.x;
    float H = max(GE_VOLUMETRIC_FOG.uHeightParams.y, 0.01);
    float density0 = max(GE_VOLUMETRIC_FOG.uAlbedoDensity.w, 0.0);

    // Closed-form optical depth over [maxDist, farEnd]: density0 * segLength *
    // (average of exp(-max(h-base,0)/H) along the segment), the average being the
    // analytic integral G(yEnd)-G(yStart) over the height delta.
    float ya = (ro + rd * maxDist).y - base;
    float yb = (ro + rd * farEnd).y - base;
    float dy = yb - ya;
    float heightIntegral = abs(dy) < 1.0e-4
        ? exp(-max(ya, 0.0) / H)
        : (FogHeightG(yb, H) - FogHeightG(ya, H)) / dy;
    float Tfar = exp(-density0 * seg * max(heightIntegral, 0.0));

    // Representative in-scatter color (sun + GI ambient), constant across the
    // segment -> smooth on screen. Same model as the lighting pass so near and far
    // fog read identically.
    vec3 sunDir = normalize(GE_VOLUMETRIC_FOG.uSunDirection.xyz);
    vec3 direct = GE_VOLUMETRIC_FOG.uSunColor.rgb * FogHenyeyGreenstein(dot(rd, sunDir), GE_VOLUMETRIC_FOG.uEmissionAnisotropy.w);
    vec3 ambient;
    if (GE_VOLUMETRIC_FOG.uDensityParams.w > 0.5)
    {
        // GE_TAP_LOD0: WGSL's uniformity analysis cannot prove this branch
        // uniform (naga routes the UBO read through a private) and allows
        // implicit-derivative sampling only in uniform flow, so the compat arm
        // needs the explicit level. The irradiance cube is mip-less, so level 0
        // is the same fetch either way.
        vec3 skyIrr = GE_VOLUMETRIC_FOG_IRRADIANCE_TAP(GE_VOLUMETRIC_FOG_IRRADIANCE, vec3(0.0, 1.0, 0.0)).rgb;
        vec3 groundIrr = GE_VOLUMETRIC_FOG_IRRADIANCE_TAP(GE_VOLUMETRIC_FOG_IRRADIANCE, vec3(0.0, -1.0, 0.0)).rgb;
        ambient = mix(groundIrr, skyIrr, clamp(0.5 * (ya + yb) / H, 0.0, 1.0)) * GE_VOLUMETRIC_FOG.uAmbientColor.rgb;
    }
    else
    {
        ambient = GE_VOLUMETRIC_FOG.uAmbientColor.rgb * 0.18;
    }
    vec3 fogColor = GE_VOLUMETRIC_FOG.uAlbedoDensity.rgb * (direct + ambient) + GE_VOLUMETRIC_FOG.uEmissionAnisotropy.rgb;

    // Full fog beyond the grid: the near grid band plus the analytic far term.
    vec3 outRgb = gridFog.rgb + fogColor * ((1.0 - Tfar) * gridFog.a);
    float outTrans = gridFog.a * Tfar;

    // Feather the WHOLE fog->sky join above the horizon. Fading both the near grid
    // band and the far term (not just the far term) removes the residual horizon
    // band, and a quintic smootherstep ease gives a gradient with no perceptible
    // edge. Rays at/below the horizon (rd.y <= 0) keep full fog, so ground and
    // geometry are untouched; only the sky clears toward the zenith.
    float u = clamp(max(rd.y, 0.0) / skyFadeBand, 0.0, 1.0);
    float skyKeep = 1.0 - u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
    return vec4(outRgb * skyKeep, mix(1.0, outTrans, skyKeep));
}

#endif // GE_VOLUMETRIC_FOG_FAR_GLSL
