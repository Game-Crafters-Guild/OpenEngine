// The height fog: the fog between a point of view and a point a segment's length along a
// direction, as transmittance and the light the fog scatters in. Shared by the HeightFog
// post-process (height_fog.frag), which fogs the opaque scene by its depth, and the forward
// passes that fog what they draw by the fragment's own distance (forward_height_fog.glsl), so
// the two cannot drift apart.
//
// The includer declares the HeightFogParams block (height_fog_params_fields.glsl) and defines
// GE_HEIGHT_FOG_SKY_BACKDROP as the sampler the sky colour sources read (the SkyRender node's
// backdrop).
#ifndef GE_HEIGHT_FOG_COMMON_GLSL
#define GE_HEIGHT_FOG_COMMON_GLSL

// The sky's horizon radiance (GE_SkyHorizonColor).
#include "sky_composite.glsl"

const float kInvFourPi = 0.07957747154594767;
const int kFlagDistanceEnabled = 1;
const int kFlagHeightEnabled = 2;
const int kFlagNoiseEnabled = 4;
const int kFlagSkyEnabled = 8;
// HeightFogColorSource (HeightFogParamsUBO.h); the sky sources are GE_SKY_HORIZON_SOURCE_* values.
const int kColorSourceFixed = 0;

float HeightFogSaturate(float x)
{
    return clamp(x, 0.0, 1.0);
}

float HeightFogUnlerp(float a, float b, float x)
{
    return (x - a) / max(b - a, 1e-5);
}

float MaxFogOpticalDepth(float density)
{
    float d = clamp(density, 0.0, 1.0);
    return d * d * 4.0;
}

float FogExtinctionPerWorldUnit(float density)
{
    return MaxFogOpticalDepth(density) / max(HeightFogParams.maxDistance, 1.0);
}

// A viewport UV (0,0 top left) as NDC.
vec2 ViewportUvToNdc(vec2 uv)
{
    return vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
}

float HgPhase(float g, float theta)
{
    float gg = g * g;
    float denom = pow(abs(1.0 + gg - 2.0 * g * theta), 1.5);
    return kInvFourPi * ((1.0 - gg) / max(denom, 1e-5));
}

float RdrPhaseM2(float g, float theta, float sigmaExt, float w0, float w1)
{
    float hg = HgPhase(g, theta) * w0;
    float c = (w1 * sigmaExt);
    float c0 = HgPhase((2.0 / 3.0) * g, theta);
    float c1 = HgPhase((4.0 / 9.0) * g, theta);
    return hg + c * (c0 + c1);
}

float FogHash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float FogValueNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = FogHash(i + vec3(0.0, 0.0, 0.0));
    float n100 = FogHash(i + vec3(1.0, 0.0, 0.0));
    float n010 = FogHash(i + vec3(0.0, 1.0, 0.0));
    float n110 = FogHash(i + vec3(1.0, 1.0, 0.0));
    float n001 = FogHash(i + vec3(0.0, 0.0, 1.0));
    float n101 = FogHash(i + vec3(1.0, 0.0, 1.0));
    float n011 = FogHash(i + vec3(0.0, 1.0, 1.0));
    float n111 = FogHash(i + vec3(1.0, 1.0, 1.0));
    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);
    return mix(nxy0, nxy1, f.z);
}

float FogFbm(vec3 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 3; ++i)
    {
        sum += amp * FogValueNoise(p);
        p = p * 2.03 + vec3(17.0, 31.0, 47.0);
        amp *= 0.5;
    }
    return sum;
}

vec3 ResolveHeightAxis()
{
    vec3 axis = vec3(HeightFogParams.axisX, HeightFogParams.axisY, HeightFogParams.axisZ);
    float axisLen = length(axis);
    if (axisLen <= 1e-5)
        axis = vec3(0.0, 1.0, 0.0);
    else
        axis /= axisLen;
    return axis;
}

float HeightFogGradientT(vec3 originWS, vec3 viewDirWS, float segmentLength, vec2 uv, vec3 sunDir)
{
    if (HeightFogParams.gradientMode == 1)
        return HeightFogSaturate(segmentLength / max(HeightFogParams.maxDistance, 0.01));
    if (HeightFogParams.gradientMode == 2)
    {
        vec3 axis = ResolveHeightAxis();
        vec3 samplePos = originWS + viewDirWS * segmentLength * 0.5;
        return HeightFogSaturate((dot(samplePos, axis) - HeightFogParams.baseHeight) / max(HeightFogParams.transitionLength, 0.01));
    }
    if (HeightFogParams.gradientMode == 3)
        return HeightFogSaturate(1.0 - uv.y);
    if (HeightFogParams.gradientMode == 4)
        return HeightFogSaturate(dot(viewDirWS, normalize(sunDir)) * 0.5 + 0.5);
    return 0.5;
}

vec3 ApplyDualColor(vec3 baseColor, vec3 originWS, vec3 viewDirWS, float segmentLength, vec2 uv, vec3 sunDir)
{
    if (HeightFogParams.gradientMode == 0 || HeightFogParams.gradientStrength <= 1e-5)
        return baseColor;

    float t = HeightFogGradientT(originWS, viewDirWS, segmentLength, uv, sunDir);
    vec3 lowColor = vec3(HeightFogParams.gradientLowR, HeightFogParams.gradientLowG, HeightFogParams.gradientLowB);
    vec3 highColor = vec3(HeightFogParams.gradientHighR, HeightFogParams.gradientHighG, HeightFogParams.gradientHighB);
    vec3 gradientColor = mix(lowColor, highColor, t);
    return mix(baseColor, gradientColor, clamp(HeightFogParams.gradientStrength, 0.0, 1.0));
}

float FogNoiseDensityMultiplier(vec3 worldPos, float segmentLength)
{
    vec3 velocity = vec3(HeightFogParams.noiseVelX, HeightFogParams.noiseVelY, HeightFogParams.noiseVelZ);
    vec3 noiseP = worldPos / max(HeightFogParams.noiseScale, 0.01) + velocity * HeightFogParams.noiseTime;
    float raw = pow(clamp(FogFbm(noiseP), 0.0, 1.0), max(HeightFogParams.noiseContrast, 0.05));
    float low = clamp(HeightFogParams.noiseMin, 0.0, 1.0);
    float high = max(clamp(HeightFogParams.noiseMax, 0.0, 1.0), low + 1e-4);
    float remapped = HeightFogSaturate((raw - low) / (high - low));

    float fade = 1.0;
    if (HeightFogParams.noiseFadeEnd > HeightFogParams.noiseFadeStart + 1e-4)
        fade = 1.0 - smoothstep(HeightFogParams.noiseFadeStart, HeightFogParams.noiseFadeEnd, segmentLength);

    float amount = clamp(HeightFogParams.noiseStrength, 0.0, 1.0) * fade;
    return mix(1.0, remapped * 1.8, amount);
}

float EffectiveHeightFogBase(float segmentLength)
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

float OpticalDepthHeightFog(float extinction, float baseHeight, float heightScale, float cosZenith, float startHeight, float intervalLength)
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

vec3 SampleHeightFogLighting(vec3 viewDir, float extinction, vec3 emissive)
{
    vec3 sunDir = normalize(vec3(HeightFogParams.sunDirX, HeightFogParams.sunDirY, HeightFogParams.sunDirZ));
    vec3 sunColor = vec3(HeightFogParams.sunColorR, HeightFogParams.sunColorG, HeightFogParams.sunColorB) * max(HeightFogParams.sunIntensity, 0.0);
    float theta = dot(sunDir, viewDir);
    float phase = RdrPhaseM2(clamp(HeightFogParams.phase, -0.95, 0.95), theta, extinction,
                             max(HeightFogParams.phaseWeight0, 0.0), max(HeightFogParams.phaseWeight1, 0.0));
    return emissive + sunColor * HeightFogSaturate(phase);
}

// In-scattered colour of the fog along viewDirWS: the sky's horizon radiance in the sky colour
// mode, otherwise the emissive with the gradient and the sun's in-scatter.
vec3 HeightFogColor(vec3 originWS, vec3 viewDirWS, float segmentLength, vec2 uv, float extinction)
{
    if (HeightFogParams.colorSource != kColorSourceFixed)
    {
        vec3 skyHorizonColor = vec3(HeightFogParams.skyHorizonColorR, HeightFogParams.skyHorizonColorG,
                                    HeightFogParams.skyHorizonColorB);
        return GE_SkyHorizonColor(GE_HEIGHT_FOG_SKY_BACKDROP, HeightFogParams.colorSource, viewDirWS,
                                  HeightFogParams.skyAzimuthOffset, HeightFogParams.skyNightAmount,
                                  skyHorizonColor, HeightFogParams.skyHorizonScale);
    }

    vec3 sunDir = vec3(HeightFogParams.sunDirX, HeightFogParams.sunDirY, HeightFogParams.sunDirZ);
    vec3 emissive = vec3(HeightFogParams.emissiveR, HeightFogParams.emissiveG, HeightFogParams.emissiveB);
    emissive = ApplyDualColor(emissive, originWS, viewDirWS, segmentLength, uv, sunDir);
    return SampleHeightFogLighting(viewDirWS, extinction, emissive);
}

void ComputeHeightFog(vec3 originWS, vec3 viewDirWS, float segmentLength, vec2 uv, out vec3 scattering, out float transmittance)
{
    float density = clamp(HeightFogParams.density, 0.0, 1.0);
    float extinction = FogExtinctionPerWorldUnit(density);
    if (extinction <= 1e-7)
    {
        scattering = vec3(0.0);
        transmittance = 1.0;
        return;
    }

    if ((HeightFogParams.flags & kFlagNoiseEnabled) != 0 && HeightFogParams.noiseStrength > 1e-5)
    {
        vec3 samplePos = originWS + viewDirWS * segmentLength * 0.5;
        extinction *= FogNoiseDensityMultiplier(samplePos, segmentLength);
    }

    segmentLength = min(segmentLength, max(HeightFogParams.maxDistance, 0.01));

    float distanceOpticalDepth = 0.0;
    if ((HeightFogParams.flags & kFlagDistanceEnabled) != 0)
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
    if ((HeightFogParams.flags & kFlagHeightEnabled) != 0)
    {
        vec3 axis = ResolveHeightAxis();
        float heightScale = max(HeightFogParams.transitionLength, 0.01) * 0.144765;
        float baseHeight = EffectiveHeightFogBase(segmentLength);
        heightOpticalDepth = OpticalDepthHeightFog(extinction, baseHeight, heightScale,
                                                   dot(viewDirWS, axis), dot(originWS, axis), segmentLength);
    }

    float opticalDepth = 0.0;
    if ((HeightFogParams.flags & kFlagDistanceEnabled) != 0 && (HeightFogParams.flags & kFlagHeightEnabled) != 0)
        opticalDepth = (HeightFogParams.layerMode == 1)
            ? (distanceOpticalDepth + heightOpticalDepth)
            : max(distanceOpticalDepth, heightOpticalDepth);
    else if ((HeightFogParams.flags & kFlagDistanceEnabled) != 0)
        opticalDepth = distanceOpticalDepth;
    else if ((HeightFogParams.flags & kFlagHeightEnabled) != 0)
        opticalDepth = heightOpticalDepth;

    float fogOpacity = clamp(1.0 - exp(-opticalDepth), 0.0, clamp(HeightFogParams.maxOpacity, 0.0, 1.0));
    transmittance = 1.0 - fogOpacity;

    vec3 fogColor = HeightFogColor(originWS, viewDirWS, segmentLength, uv, extinction);
    vec3 albedo = vec3(extinction);
    scattering = (fogColor * albedo - fogColor * albedo * transmittance) / max(extinction, 0.0001);
}

#endif // GE_HEIGHT_FOG_COMMON_GLSL
