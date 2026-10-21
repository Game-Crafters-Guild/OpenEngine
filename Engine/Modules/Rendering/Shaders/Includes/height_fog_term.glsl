// The height fog term: the fog between the camera and a surface (the distance and height optical
// depths, the in-scatter color) and the fog over the sky in a direction (the sky fill). Any shader
// that shades a point the fog covers includes it, so the fog has one implementation.
//
// The includer declares, before this file:
//   - the HeightFogParams UBO block (height_fog_params_fields.glsl) under the instance name
//     HeightFogParams;
//   - the sampler2D the sky pass draws its backdrop from (the sky-view LUT or the HDRI), with
//     GE_HEIGHT_FOG_BACKDROP defined as its name; read only when HeightFogParams.colorSource names
//     a backdrop.
//
// Units and conventions: positions and directions are world space (left-handed, +Y up; the height
// axis is HeightFogParams.axis, +Y when unset); lengths are meters; directions are unit vectors.
// The term takes a direction and a distance, not a depth: reconstructing them from the depth
// buffer (reverse-Z) is the caller's business. In-scatter is absolute scene-linear radiance; a
// caller that stores scene color at the frame's pre-exposure multiplies it by GE_PreExposure().
#ifndef GE_HEIGHT_FOG_TERM_GLSL
#define GE_HEIGHT_FOG_TERM_GLSL

#include "sky_composite.glsl"

#ifndef GE_HEIGHT_FOG_BACKDROP
#error "define GE_HEIGHT_FOG_BACKDROP as the sky backdrop sampler's name before including height_fog_term.glsl"
#endif

const float GE_HEIGHT_FOG_INV_FOUR_PI = 0.07957747154594767;
const int GE_HEIGHT_FOG_FLAG_DISTANCE = 1;
const int GE_HEIGHT_FOG_FLAG_HEIGHT = 2;
const int GE_HEIGHT_FOG_FLAG_NOISE = 4;
const int GE_HEIGHT_FOG_FLAG_SKY = 8;
// HeightFogColorSource (HeightFogParamsUBO.h); the sky sources are GE_SKY_HORIZON_SOURCE_* values.
const int GE_HEIGHT_FOG_COLOR_FIXED = 0;

float GE_HeightFogSaturate(float x)
{
    return clamp(x, 0.0, 1.0);
}

float GE_HeightFogUnlerp(float a, float b, float x)
{
    return (x - a) / max(b - a, 1e-5);
}

// The optical depth the fog reaches at maxDistance: density is artist-scaled in [0, 1].
float GE_HeightFogMaxOpticalDepth(float density)
{
    float d = clamp(density, 0.0, 1.0);
    return d * d * 4.0;
}

float GE_HeightFogExtinction(float density)
{
    return GE_HeightFogMaxOpticalDepth(density) / max(HeightFogParams.maxDistance, 1.0);
}

float GE_HeightFogHgPhase(float g, float theta)
{
    float gg = g * g;
    float denom = pow(abs(1.0 + gg - 2.0 * g * theta), 1.5);
    return GE_HEIGHT_FOG_INV_FOUR_PI * ((1.0 - gg) / max(denom, 1e-5));
}

float GE_HeightFogRdrPhaseM2(float g, float theta, float sigmaExt, float w0, float w1)
{
    float hg = GE_HeightFogHgPhase(g, theta) * w0;
    float c = (w1 * sigmaExt);
    float c0 = GE_HeightFogHgPhase((2.0 / 3.0) * g, theta);
    float c1 = GE_HeightFogHgPhase((4.0 / 9.0) * g, theta);
    return hg + c * (c0 + c1);
}

float GE_HeightFogHash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float GE_HeightFogValueNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = GE_HeightFogHash(i + vec3(0.0, 0.0, 0.0));
    float n100 = GE_HeightFogHash(i + vec3(1.0, 0.0, 0.0));
    float n010 = GE_HeightFogHash(i + vec3(0.0, 1.0, 0.0));
    float n110 = GE_HeightFogHash(i + vec3(1.0, 1.0, 0.0));
    float n001 = GE_HeightFogHash(i + vec3(0.0, 0.0, 1.0));
    float n101 = GE_HeightFogHash(i + vec3(1.0, 0.0, 1.0));
    float n011 = GE_HeightFogHash(i + vec3(0.0, 1.0, 1.0));
    float n111 = GE_HeightFogHash(i + vec3(1.0, 1.0, 1.0));
    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);
    return mix(nxy0, nxy1, f.z);
}

float GE_HeightFogFbm(vec3 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 3; ++i)
    {
        sum += amp * GE_HeightFogValueNoise(p);
        p = p * 2.03 + vec3(17.0, 31.0, 47.0);
        amp *= 0.5;
    }
    return sum;
}

vec3 GE_HeightFogAxis()
{
    vec3 axis = vec3(HeightFogParams.axisX, HeightFogParams.axisY, HeightFogParams.axisZ);
    float axisLen = length(axis);
    if (axisLen <= 1e-5)
        axis = vec3(0.0, 1.0, 0.0);
    else
        axis /= axisLen;
    return axis;
}

float GE_HeightFogGradientT(vec3 originWS, vec3 viewDirWS, float segmentLength, vec2 uv, vec3 sunDir)
{
    if (HeightFogParams.gradientMode == 1)
        return GE_HeightFogSaturate(segmentLength / max(HeightFogParams.maxDistance, 0.01));
    if (HeightFogParams.gradientMode == 2)
    {
        vec3 axis = GE_HeightFogAxis();
        vec3 samplePos = originWS + viewDirWS * segmentLength * 0.5;
        return GE_HeightFogSaturate((dot(samplePos, axis) - HeightFogParams.baseHeight) /
                                    max(HeightFogParams.transitionLength, 0.01));
    }
    if (HeightFogParams.gradientMode == 3)
        return GE_HeightFogSaturate(1.0 - uv.y);
    if (HeightFogParams.gradientMode == 4)
        return GE_HeightFogSaturate(dot(viewDirWS, normalize(sunDir)) * 0.5 + 0.5);
    return 0.5;
}

vec3 GE_HeightFogDualColor(vec3 baseColor, vec3 originWS, vec3 viewDirWS, float segmentLength, vec2 uv, vec3 sunDir)
{
    if (HeightFogParams.gradientMode == 0 || HeightFogParams.gradientStrength <= 1e-5)
        return baseColor;

    float t = GE_HeightFogGradientT(originWS, viewDirWS, segmentLength, uv, sunDir);
    vec3 lowColor = vec3(HeightFogParams.gradientLowR, HeightFogParams.gradientLowG, HeightFogParams.gradientLowB);
    vec3 highColor = vec3(HeightFogParams.gradientHighR, HeightFogParams.gradientHighG, HeightFogParams.gradientHighB);
    vec3 gradientColor = mix(lowColor, highColor, t);
    return mix(baseColor, gradientColor, clamp(HeightFogParams.gradientStrength, 0.0, 1.0));
}

float GE_HeightFogNoiseDensity(vec3 worldPos, float segmentLength)
{
    vec3 velocity = vec3(HeightFogParams.noiseVelX, HeightFogParams.noiseVelY, HeightFogParams.noiseVelZ);
    vec3 noiseP = worldPos / max(HeightFogParams.noiseScale, 0.01) + velocity * HeightFogParams.noiseTime;
    float raw = pow(clamp(GE_HeightFogFbm(noiseP), 0.0, 1.0), max(HeightFogParams.noiseContrast, 0.05));
    float low = clamp(HeightFogParams.noiseMin, 0.0, 1.0);
    float high = max(clamp(HeightFogParams.noiseMax, 0.0, 1.0), low + 1e-4);
    float remapped = GE_HeightFogSaturate((raw - low) / (high - low));

    float fade = 1.0;
    if (HeightFogParams.noiseFadeEnd > HeightFogParams.noiseFadeStart + 1e-4)
        fade = 1.0 - smoothstep(HeightFogParams.noiseFadeStart, HeightFogParams.noiseFadeEnd, segmentLength);

    float amount = clamp(HeightFogParams.noiseStrength, 0.0, 1.0) * fade;
    return mix(1.0, remapped * 1.8, amount);
}

float GE_HeightFogEffectiveBase(float segmentLength)
{
    float baseHeight = HeightFogParams.baseHeight;
    if (abs(HeightFogParams.horizonHeightOffset) <= 1e-5)
        return baseHeight;

    if (HeightFogParams.horizonHeightBlendEnd <= HeightFogParams.horizonHeightBlendStart + 1e-4)
        return baseHeight;

    float t = smoothstep(HeightFogParams.horizonHeightBlendStart,
                         HeightFogParams.horizonHeightBlendEnd,
                         segmentLength);
    return baseHeight + HeightFogParams.horizonHeightOffset * t;
}

float GE_HeightFogHeightOpticalDepth(float extinction, float baseHeight, float heightScale, float cosZenith,
                                     float startHeight, float intervalLength)
{
    float H = max(heightScale, 0.01);
    float rcpH = 1.0 / H;
    float z = cosZenith;
    float absZ = max(abs(z), 0.001);
    float rcpAbsZ = 1.0 / absZ;

    float endHeight = startHeight + intervalLength * z;
    float minHeight = min(startHeight, endHeight);
    float h = max(minHeight - baseHeight, 0.0);

    float homFogDist = clamp((baseHeight - minHeight) * rcpAbsZ, 0.0, intervalLength);
    float expFogDist = intervalLength - homFogDist;
    float expFogMult = exp(-h * rcpH) * (1.0 - exp(-expFogDist * absZ * rcpH)) * (rcpAbsZ * H);
    return extinction * (homFogDist + expFogMult);
}

vec3 GE_HeightFogLighting(vec3 viewDir, float extinction, vec3 emissive)
{
    vec3 sunDir = normalize(vec3(HeightFogParams.sunDirX, HeightFogParams.sunDirY, HeightFogParams.sunDirZ));
    vec3 sunColor = vec3(HeightFogParams.sunColorR, HeightFogParams.sunColorG, HeightFogParams.sunColorB) *
                    max(HeightFogParams.sunIntensity, 0.0);
    float theta = dot(sunDir, viewDir);
    float phase = GE_HeightFogRdrPhaseM2(clamp(HeightFogParams.phase, -0.95, 0.95), theta, extinction,
                                         max(HeightFogParams.phaseWeight0, 0.0), max(HeightFogParams.phaseWeight1, 0.0));
    return emissive + sunColor * GE_HeightFogSaturate(phase);
}

// In-scattered color of the fog along viewDirWS: the sky's horizon radiance in the sky color
// mode, otherwise the emissive with the gradient and the sun's in-scatter. Absolute scene-linear.
vec3 GE_HeightFogColor(vec3 originWS, vec3 viewDirWS, float segmentLength, vec2 uv, float extinction)
{
    if (HeightFogParams.colorSource != GE_HEIGHT_FOG_COLOR_FIXED)
    {
        vec3 skyHorizonColor = vec3(HeightFogParams.skyHorizonColorR, HeightFogParams.skyHorizonColorG,
                                    HeightFogParams.skyHorizonColorB);
        return GE_SkyHorizonColor(GE_HEIGHT_FOG_BACKDROP, HeightFogParams.colorSource, viewDirWS,
                                  HeightFogParams.skyAzimuthOffset, HeightFogParams.skyNightAmount,
                                  skyHorizonColor, HeightFogParams.skyHorizonScale);
    }

    vec3 sunDir = vec3(HeightFogParams.sunDirX, HeightFogParams.sunDirY, HeightFogParams.sunDirZ);
    vec3 emissive = vec3(HeightFogParams.emissiveR, HeightFogParams.emissiveG, HeightFogParams.emissiveB);
    emissive = GE_HeightFogDualColor(emissive, originWS, viewDirWS, segmentLength, uv, sunDir);
    return GE_HeightFogLighting(viewDirWS, extinction, emissive);
}

// The fog's strength k: HeightFogParams.intensity in [0, 1], or 0 when the fog is off (the intensity
// or the density at 0). Every result of the term is scaled by it.
float GE_HeightFogStrength()
{
    float k = clamp(HeightFogParams.intensity, 0.0, 1.0);
    return (k <= 1e-5 || HeightFogParams.density <= 1e-5) ? 0.0 : k;
}

/// The fog between the camera and a surface: what reaches the camera of the surface's own light,
/// and the fog's light added on the way.
/// @param originWS      the camera position, world space.
/// @param viewDirWS     the unit direction from the camera to the surface, world space.
/// @param segmentLength the distance from the camera to the surface, meters.
/// @param uv            the screen position in [0, 1]; read only by the screen-space gradient mode.
/// @param scattering    S: the in-scattered radiance, absolute scene-linear, scaled by the strength.
/// @param transmittance T: the fraction of the surface's radiance that reaches the camera, [0, 1],
///                      blended toward 1 by the strength.
/// The fogged radiance is radiance * T + S (S times the pre-exposure in a pre-exposed target).
void GE_HeightFogSurface(vec3 originWS, vec3 viewDirWS, float segmentLength, vec2 uv,
                         out vec3 scattering, out float transmittance)
{
    float strength = GE_HeightFogStrength();
    if (strength <= 0.0)
    {
        scattering = vec3(0.0);
        transmittance = 1.0;
        return;
    }

    float density = clamp(HeightFogParams.density, 0.0, 1.0);
    float extinction = GE_HeightFogExtinction(density);
    if (extinction <= 1e-7)
    {
        scattering = vec3(0.0);
        transmittance = 1.0;
        return;
    }

    if ((HeightFogParams.flags & GE_HEIGHT_FOG_FLAG_NOISE) != 0 && HeightFogParams.noiseStrength > 1e-5)
    {
        vec3 samplePos = originWS + viewDirWS * segmentLength * 0.5;
        extinction *= GE_HeightFogNoiseDensity(samplePos, segmentLength);
    }

    segmentLength = min(segmentLength, max(HeightFogParams.maxDistance, 0.01));

    float distanceOpticalDepth = 0.0;
    if ((HeightFogParams.flags & GE_HEIGHT_FOG_FLAG_DISTANCE) != 0)
    {
        float smoothLength = max(HeightFogParams.smoothLength, 0.0001);
        float x = max(0.0, segmentLength - max(HeightFogParams.minDistance, 0.0));
        if (x < smoothLength)
        {
            float invS = 1.0 / smoothLength;
            distanceOpticalDepth = invS * invS * x * x * x - 0.5 * invS * invS * invS * x * x * x * x;
        }
        else
        {
            distanceOpticalDepth = (x - smoothLength) + 0.5 * smoothLength;
        }
        distanceOpticalDepth *= extinction;
    }

    float heightOpticalDepth = 0.0;
    if ((HeightFogParams.flags & GE_HEIGHT_FOG_FLAG_HEIGHT) != 0)
    {
        vec3 axis = GE_HeightFogAxis();
        float heightScale = max(HeightFogParams.transitionLength, 0.01) * 0.144765;
        float baseHeight = GE_HeightFogEffectiveBase(segmentLength);
        heightOpticalDepth = GE_HeightFogHeightOpticalDepth(extinction, baseHeight, heightScale,
                                                            dot(viewDirWS, axis), dot(originWS, axis), segmentLength);
    }

    float opticalDepth = 0.0;
    if ((HeightFogParams.flags & GE_HEIGHT_FOG_FLAG_DISTANCE) != 0 && (HeightFogParams.flags & GE_HEIGHT_FOG_FLAG_HEIGHT) != 0)
        opticalDepth = (HeightFogParams.layerMode == 1)
            ? (distanceOpticalDepth + heightOpticalDepth)
            : max(distanceOpticalDepth, heightOpticalDepth);
    else if ((HeightFogParams.flags & GE_HEIGHT_FOG_FLAG_DISTANCE) != 0)
        opticalDepth = distanceOpticalDepth;
    else if ((HeightFogParams.flags & GE_HEIGHT_FOG_FLAG_HEIGHT) != 0)
        opticalDepth = heightOpticalDepth;

    float fogOpacity = clamp(1.0 - exp(-opticalDepth), 0.0, clamp(HeightFogParams.maxOpacity, 0.0, 1.0));
    transmittance = 1.0 - fogOpacity;

    vec3 fogColor = GE_HeightFogColor(originWS, viewDirWS, segmentLength, uv, extinction);
    vec3 albedo = vec3(extinction);
    scattering = (fogColor * albedo - fogColor * albedo * transmittance) / max(extinction, 0.0001);

    scattering *= strength;
    transmittance = mix(1.0, transmittance, strength);
}

/// The sky fill: how much of the fog color the sky takes in a direction, dense at the horizon and
/// clear above skyFillEnd, scaled by the strength; 0 when the sky fill or the fog is off.
/// @param viewDirWS the unit view direction, world space.
/// @return the fill amount in [0, maxOpacity].
float GE_HeightFogSkyAmount(vec3 viewDirWS)
{
    float strength = GE_HeightFogStrength();
    if ((HeightFogParams.flags & GE_HEIGHT_FOG_FLAG_SKY) == 0 || strength <= 0.0)
        return 0.0;
    vec3 axis = GE_HeightFogAxis();
    float axisDot = dot(viewDirWS, axis);
    float s = max(0.0, axisDot + HeightFogParams.skyHorizonOffset);
    s = GE_HeightFogSaturate(GE_HeightFogUnlerp(HeightFogParams.skyFillStart, HeightFogParams.skyFillEnd, s));
    s = smoothstep(1.0, 0.0, pow(s, max(HeightFogParams.skyPower, 0.001)));
    float bottom = pow(GE_HeightFogSaturate(-axisDot), max(HeightFogParams.skyPower, 0.001)) *
                   clamp(HeightFogParams.skyBottomStrength, 0.0, 1.0);
    s = max(s, bottom);
    float skyAmount = s * (1.0 - exp(-GE_HeightFogMaxOpticalDepth(HeightFogParams.density)));
    return strength * min(skyAmount, clamp(HeightFogParams.maxOpacity, 0.0, 1.0));
}

/// The fog over the sky in a direction: the sky fill as an in-scatter and a transmittance.
/// @param originWS      the camera position, world space.
/// @param viewDirWS     the unit view direction, world space.
/// @param farPlane      the view's far-plane distance, meters. The sky stands at
///                      max(farPlane, maxDistance): the distance and the height color gradients
///                      read that length.
/// @param uv            the screen position in [0, 1]; read only by the screen-space gradient mode.
/// @param scattering    S: the fog color times the fill, absolute scene-linear.
/// @param transmittance T: 1 - the fill. With no fill, S is 0 and T is 1 exactly.
void GE_HeightFogSky(vec3 originWS, vec3 viewDirWS, float farPlane, vec2 uv,
                     out vec3 scattering, out float transmittance)
{
    float viewLength = max(farPlane, HeightFogParams.maxDistance);
    float skyAmount = GE_HeightFogSkyAmount(viewDirWS);
    if (skyAmount <= 0.0)
    {
        scattering = vec3(0.0);
        transmittance = 1.0;
        return;
    }
    vec3 fogColor = GE_HeightFogColor(originWS, viewDirWS, viewLength, uv,
                                      GE_HeightFogExtinction(HeightFogParams.density));
    scattering = fogColor * skyAmount;
    transmittance = 1.0 - skyAmount;
}

#endif // GE_HEIGHT_FOG_TERM_GLSL
