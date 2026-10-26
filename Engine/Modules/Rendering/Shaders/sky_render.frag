#version 450

#include "Includes/compat_profile.glsl"
#include "Includes/moon_disc.glsl"
#include "Includes/screen_position.glsl"
#include "Includes/sky_composite.glsl"
#include "Includes/sun_disc.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform SkyUBO
{
    vec3  cameraPositionWS;   float exposureEV;
    vec3  cameraRightWS;      float sunSize2D;
    vec3  cameraUpWS;         float moonSize2D;
    vec3  cameraForwardWS;    float skyPan2D;
    vec3  sunDirectionWS;     float sunAngularRadius;
    vec3  moonDirectionWS;    float moonIntensity;
    float moonAngularRadius;
    float skyRotateAroundZenithRadians;
    float moonUboPad1;
    float moonUboPad2;
    vec3  sunColor;           float sunIntensity;
    float timeOfDayHours;     float skyPad0;
    float viewportAspect;     float tanHalfFovY;
    vec2  viewportResolution; float nightSkyBlend; float skyTimeSeconds;
    vec4  starModel;    // x = limiting magnitude, y = highlight compression, z = scintillation, w = twinkle speed
    vec4  skyboxParams; // x = HDRI rotation radians; y = HDRI intensity if >= 0, else the procedural sky
    vec4  fallingStarParams; // the falling stars, drawn by the star billboard (sky_stars_billboard.vert)
    vec4  gradientSkyTop;    // .rgb = scene-linear top color; .w >= 0.5 -> render gradient sky
    vec4  gradientSkyHorizon;
    vec4  gradientSkyBottom;
} uSky;

layout(set = 0, binding = 1) uniform sampler2D uSkyViewLUT;

#include "Includes/sky_atmosphere_ubo.glsl"
#include "Includes/sky_transmittance_lut.glsl"

layout(set = 0, binding = 3) uniform sampler2D uTransLUT;
layout(set = 0, binding = 4) uniform sampler2D uMoonFullTexture;
// The view's exposure state (ExposureHistory). Scene colour is stored at the frame's
// pre-exposure P, so everything this pass writes is multiplied by P with the sky's own trim.
#define GE_EXPOSURE_HISTORY_BINDING 5
#include "Includes/exposure_history.glsl"

// The multiply that lands sky radiance in scene colour: the sky's exposure trim and P.
float SkyStoreScale()
{
    return exp2(uSky.exposureEV) * GE_PreExposure();
}

#ifdef GE_SKY_HEIGHT_FOG
// The height fog over the sky (sky_render_fog, drawn for a view whose height fog is active): the sky
// is what the far plane shows, so it takes the fog's sky fill where it is drawn, in the direction
// and at the far plane the view's own projection gives the pixel, and every surface drawn over it
// takes its own fog in its own shader.
layout(set = 0, binding = 6, std140) uniform ViewParamsBlock
{
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#define GE_VIEWPARAMS_CORE_ONLY 1
#include "Includes/view_params_fields.glsl"
#undef GE_VIEWPARAMS_CORE_ONLY
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;
layout(set = 0, binding = 7, std140) uniform HeightFogParamsBlock
{
#include "Includes/height_fog_params_fields.glsl"
} HeightFogParams;
// The backdrop the fog's sky colour reads: the texture uSkyViewLUT binds, through a repeating sampler,
// as every other reader of the fog's sky colour takes it.
layout(set = 0, binding = 8) uniform sampler2D uHeightFogBackdrop;
#define GE_HEIGHT_FOG_BACKDROP uHeightFogBackdrop
#include "Includes/height_fog_term.glsl"
#endif

// The sky pass's output: `stored`, the sky's radiance already at the store scale, with the height
// fog's sky fill over it when the view's fog is drawn here. The in-scatter is absolute and lands at
// the pre-exposure P.
#ifdef GE_SKY_HEIGHT_FOG
vec4 SkyOutput(vec3 stored)
{
    vec2 ndc = vec2(vUV.x * 2.0 - 1.0, (1.0 - vUV.y) * 2.0 - 1.0);
    vec4 farPoint = ViewParams.ge_invProj * vec4(ndc, 0.0, 1.0);
    farPoint.xyz /= max(farPoint.w, 1e-6);
    vec3 viewDirWS = normalize(transpose(mat3(ViewParams.ge_view)) * normalize(farPoint.xyz));
    vec3 fogScattering;
    float fogTransmittance;
    GE_HeightFogSky(ViewParams.ge_cameraPosWS.xyz, viewDirWS, ViewParams.ge_nearFar.y, vUV, fogScattering,
                    fogTransmittance);
    return vec4(stored * fogTransmittance + fogScattering * GE_PreExposure(), 1.0);
}
#else
#define SkyOutput(stored) vec4((stored), 1.0)
#endif

const float PI = 3.14159265359;

// Sky-view LUT height (matches Config::skyViewLutHeight); the sampled elevation
// is clamped to a half-texel inset so the warped poles never read past the edge.
const float kSkyViewLutHeight = 256.0;

// The sky-view LUT's radiance toward `dirWS`, in the planet-local horizon frame (east, up, north).
vec3 SkyViewLutRadiance(vec3 dirWS, vec3 east, vec3 up, vec3 north)
{
    vec3 dLocal = vec3(dot(dirWS, east), dot(dirWS, up), dot(dirWS, north));
    vec2 skyUv = GE_SkyDirToViewLutUv(dLocal);
    skyUv.y = clamp(skyUv.y, 0.5 / kSkyViewLutHeight, 1.0 - 0.5 / kSkyViewLutHeight);
    return GE_TAP_LOD0(uSkyViewLUT, skyUv).rgb;
}

vec2 EncodeDirToLatLongUv(vec3 dirWS)
{
    float u = atan(dirWS.z, dirWS.x) / (2.0 * PI) + 0.5;
    float v = acos(clamp(dirWS.y, -1.0, 1.0)) / PI;
    return vec2(u, v);
}

vec3 RotateAroundY(vec3 dirWS, float radians)
{
    float s = sin(radians);
    float c = cos(radians);
    return vec3(c * dirWS.x - s * dirWS.z,
                dirWS.y,
                s * dirWS.x + c * dirWS.z);
}

// Stylized 3-color gradient sky: blend Top/Horizon/Bottom by the view direction's world +Y
// component (up -> Top, horizon -> Horizon, down -> Bottom). Branchless; colors are already
// scene-linear (intensity folded in on the CPU). Shared by the perspective and ortho paths.
vec3 GradientSkyColor(vec3 dirWS)
{
    float up   = max(dirWS.y, 0.0);
    float down = max(-dirWS.y, 0.0);
    float mid  = 1.0 - up - down;
    return uSky.gradientSkyTop.rgb * up
         + uSky.gradientSkyHorizon.rgb * mid
         + uSky.gradientSkyBottom.rgb * down;
}

// Transmittance from the camera to the top of the atmosphere along dirWS; 0 below the horizon.
vec3 TransmittanceAlongView(vec3 dirWS)
{
    vec3 planetCenter = vec3(0.0, -uAtmos.planetRadius, 0.0);
    vec3 camPos = uSky.cameraPositionWS;
    float r = length(camPos - planetCenter);
    vec3 up = (camPos - planetCenter) / r;
    float mu = clamp(dot(dirWS, up), -1.0, 1.0);
    return GE_SkyTransmittanceToTop(uTransLUT, r, mu, uAtmos.planetRadius, uAtmos.atmosphereRadius);
}

// Sample the same HDR backdrop source as the perspective sky for one virtual
// sky direction. Orthographic rendering still lays those samples out as a
// screen-space gradient below, but the samples must remain on the atmosphere's
// physical scene-linear scale so a single camera EV exposes every projection.
vec3 SampleOrthoBackdropDirection(vec3 dirWS)
{
    float hdriIntensity = uSky.skyboxParams.y;
    if (hdriIntensity >= 0.0)
    {
        return GE_TAP_LOD0(uSkyViewLUT,
                       EncodeDirToLatLongUv(RotateAroundY(dirWS, uSky.skyboxParams.x))).rgb *
               hdriIntensity;
    }

    vec3 planetCenter = vec3(0.0, -uAtmos.planetRadius, 0.0);
    vec3 camPos = uSky.cameraPositionWS;
    vec3 up = normalize(camPos - planetCenter);

    vec3 northRef = vec3(0.0, 0.0, 1.0);
    if (abs(dot(northRef, up)) > 0.98)
        northRef = vec3(1.0, 0.0, 0.0);
    vec3 east = normalize(cross(up, northRef));
    vec3 north = normalize(cross(east, up));

    vec3 dLocal = vec3(dot(dirWS, east), dot(dirWS, up), dot(dirWS, north));
    vec2 skyUv = GE_SkyDirToViewLutUv(dLocal);
    skyUv.y = clamp(skyUv.y, 0.5 / kSkyViewLutHeight, 1.0 - 0.5 / kSkyViewLutHeight);
    vec3 skyColor = GE_TAP_LOD0(uSkyViewLUT, skyUv).rgb;

    bool applyStylizedGround = (uAtmos.belowHorizonMode == 2u);
    return GE_ApplySkyGroundAndNight(skyColor, dirWS, dot(dirWS, up),
                                     clamp(uSky.nightSkyBlend, 0.0, 1.0),
                                     applyStylizedGround,
                                     uAtmos.groundAlbedo, uAtmos.groundBrightness,
                                     uAtmos.groundNightColor, uAtmos.belowHorizonDarkness,
                                     uAtmos.belowHorizonDarkColor,
                                     uAtmos.belowHorizonBlendSharpness,
                                     uAtmos.nightSkyHorizonColor);
}

// 2D sky backdrop: simple screen-vertical gradient through HDR zenith / horizon /
// lower-sky samples, plus a sun tag at the sun's elevation. Stars are drawn by
// the billboard pass in their own ortho branch. This deliberately bypasses the
// per-pixel atmospheric ray simulation, which has no meaningful per-pixel
// angular variation in an orthographic projection, while keeping its radiance.
void RenderOrthoBackdrop(vec2 ndc)
{
    float nightAmount = clamp(uSky.nightSkyBlend, 0.0, 1.0);
    vec2 skyNdc = vec2(ndc.x, ndc.y - clamp(uSky.skyPan2D, -1.0, 1.0));

    // t: 0 at screen top, 0.5 at horizon (screen middle), 1 at bottom
    float t = clamp((1.0 - skyNdc.y) * 0.5, 0.0, 1.0);

    // Gradient sky: a stylized vertical gradient with NO celestial bodies. Map the screen-vertical
    // t (top -> Top, horizon -> Horizon, bottom -> Bottom) to the gradient colors and emit before
    // the sun disk / moon compositing below (matches the perspective path's early-out).
    if (uSky.gradientSkyTop.w >= 0.5)
    {
        vec3 grad = (t < 0.5)
            ? mix(uSky.gradientSkyTop.rgb, uSky.gradientSkyHorizon.rgb, smoothstep(0.0, 0.5, t))
            : mix(uSky.gradientSkyHorizon.rgb, uSky.gradientSkyBottom.rgb, smoothstep(0.5, 1.0, t));
        oColor = SkyOutput(grad * SkyStoreScale());
        return;
    }

    // Build a stable virtual horizon direction from the camera orientation.
    // Top/bottom views have a forward vector parallel to planet-up, so fall
    // back through the camera's up/right axes before using local north.
    vec3 planetCenter = vec3(0.0, -uAtmos.planetRadius, 0.0);
    vec3 planetUp = normalize(uSky.cameraPositionWS - planetCenter);
    vec3 horizonDir = uSky.cameraForwardWS -
                      planetUp * dot(uSky.cameraForwardWS, planetUp);
    if (dot(horizonDir, horizonDir) < 1e-6)
        horizonDir = uSky.cameraUpWS - planetUp * dot(uSky.cameraUpWS, planetUp);
    if (dot(horizonDir, horizonDir) < 1e-6)
        horizonDir = uSky.cameraRightWS - planetUp * dot(uSky.cameraRightWS, planetUp);
    if (dot(horizonDir, horizonDir) < 1e-6)
    {
        vec3 northRef = abs(planetUp.z) < 0.98 ? vec3(0.0, 0.0, 1.0)
                                               : vec3(1.0, 0.0, 0.0);
        horizonDir = northRef - planetUp * dot(northRef, planetUp);
    }
    horizonDir = normalize(horizonDir);

    // These anchors now come from the same HDR atmosphere/HDRI sampled by the
    // perspective path. The old hand-authored ~0..1 palette sat roughly seven
    // stops below a sky driven by a physical-lux sun, forcing incompatible EVs.
    vec3 zenithCol = SampleOrthoBackdropDirection(planetUp);
    vec3 horizonCol = SampleOrthoBackdropDirection(horizonDir);
    vec3 groundCol = SampleOrthoBackdropDirection(-planetUp);

    vec3 col = (t < 0.5)
        ? mix(zenithCol, horizonCol, smoothstep(0.0, 0.5, t))
        : mix(horizonCol, groundCol, smoothstep(0.5, 1.0, t));

    float rimWidth = max(mix(uAtmos.groundHorizonDayCosWidth,
                             uAtmos.groundHorizonNightCosWidth,
                             nightAmount) * 3.0, 0.015);
    float rimMask = 1.0 - smoothstep(0.0, rimWidth, abs(skyNdc.y));
    col = mix(col, horizonCol, clamp(rimMask, 0.0, 1.0));
    vec3 sunDir = normalize(uSky.sunDirectionWS);

    // Sun disk: evaluate against a virtual sky direction, like sampling a
    // cubemap/sky dome through the camera. This keeps the orthographic world
    // projection, but lets camera orbit move the celestial disk left/right.
    if (uSky.sunAngularRadius > 0.0 && sunDir.y > -0.18 && nightAmount < 0.98)
    {
        float aspect = max(uSky.viewportAspect, 0.001);
        const float virtualTanHalfFovY = 1.0;
        vec3 basisRight = normalize(uSky.cameraRightWS);
        vec3 basisUp    = normalize(uSky.cameraUpWS);
        vec3 basisFwd   = normalize(uSky.cameraForwardWS);
        float sunRadius = uSky.sunAngularRadius * max(uSky.sunSize2D, 0.1);
        // The 2D backdrop is a stylised screen-space tag, not the physical disc: it draws the sun
        // in NDC, so this is a screen fraction and not a solid angle. It is scaled off
        // sunAngularRadius only to have one place the sun's size comes from, so the multiplier
        // must move with that constant or the tag silently resizes. sunAngularRadius already
        // carries SkyEnvironment::SunSize, so the two compose and SunSize2D trims the tag on top
        // of the sun's size — as moonSize2D does below.
        // The 0.055 NDC floor applies to that PRODUCT, not to SunSize2D alone: 16 * 0.004651 =
        // 0.0744 NDC at SunSize 1 and SunSize2D 1, so the tag stops shrinking once
        // SunSize * SunSize2D drops under ~0.74 and both dials are inert below that combined
        // value.
        float effectiveRadius = max(sunRadius * 16.0, 0.055);
        float disc = 0.0;
        float ramp = 0.0;
        if (uSky.skyRotateAroundZenithRadians > 0.5)
        {
            // Editor 2D mode keeps the sun visible as a circular screen orbit.
            vec2 sunScreen = vec2(dot(sunDir, basisRight),
                                  dot(sunDir, basisUp));
            const float maxSunScreenRadius = 0.82;
            float sunScreenLen = length(sunScreen);
            if (sunScreenLen > maxSunScreenRadius)
                sunScreen *= maxSunScreenRadius / sunScreenLen;
            vec2 sunNDC = vec2(sunScreen.x / aspect, sunScreen.y);
            vec2 d = vec2((skyNdc.x - sunNDC.x) * aspect, skyNdc.y - sunNDC.y);
            float r2 = dot(d, d);
            disc = exp(-r2 / max(effectiveRadius * effectiveRadius, 1e-6));
            ramp = exp(-r2 / max(effectiveRadius * effectiveRadius * 26.0, 1e-6));
        }
        else
        {
            // Generic orthographic mode moves the sun with the camera-facing
            // sky basis, but measures the disk in screen space so side views
            // do not squash the sun into an ellipse.
            vec2 sunScreen = vec2(dot(sunDir, basisRight),
                                  dot(sunDir, basisUp));
            vec2 sunNDC = vec2(sunScreen.x / aspect, sunScreen.y);
            vec2 d = vec2((skyNdc.x - sunNDC.x) * aspect, skyNdc.y - sunNDC.y);
            float sunForward = dot(sunDir, basisFwd);
            float frontFade = smoothstep(-0.05, 0.05, sunForward);
            float r2 = dot(d, d);
            disc = exp(-r2 / max(effectiveRadius * effectiveRadius, 1e-6)) * frontFade;
            ramp = exp(-r2 / max(effectiveRadius * effectiveRadius * 26.0, 1e-6)) * frontFade;
        }
        float horizonFade = smoothstep(-0.05, 0.05, sunDir.y);
        float horizonMask = smoothstep(-0.006, 0.006, skyNdc.y);
        float dayFade = 1.0 - nightAmount;
        float sunsetT = 1.0 - smoothstep(0.04, 0.55, sunDir.y);
        vec3 warmRampColor = mix(uSky.sunColor, vec3(1.0, 0.48, 0.18), sunsetT);
        float rampStrength = clamp(uSky.sunIntensity * 0.018, 0.0, 1.2);
        col += warmRampColor * (rampStrength * horizonFade * dayFade) * ramp * horizonMask;
        col += uSky.sunColor * (uSky.sunIntensity * 0.4 * horizonFade * dayFade) * disc * horizonMask;
    }

    vec3 moonDir = normalize(uSky.moonDirectionWS);
    float moonAboveHorizon = smoothstep(-0.18, 0.02, moonDir.y);
    if (uSky.moonIntensity > 0.0 && moonAboveHorizon > 0.0)
    {
        float aspect = max(uSky.viewportAspect, 0.001);
        vec3 basisRight = normalize(uSky.cameraRightWS);
        vec3 basisUp    = normalize(uSky.cameraUpWS);
        vec2 moonScreen = vec2(dot(moonDir, basisRight),
                               dot(moonDir, basisUp));
        if (uSky.skyRotateAroundZenithRadians > 0.5)
        {
            const float maxMoonScreenRadius = 0.82;
            float moonScreenLen = length(moonScreen);
            if (moonScreenLen > maxMoonScreenRadius)
                moonScreen *= maxMoonScreenRadius / moonScreenLen;
        }

        vec2 moonNDC = vec2(moonScreen.x / aspect, moonScreen.y);
        float moonRadius = max(uSky.moonAngularRadius * 6.0 * max(uSky.moonSize2D, 0.1), 0.05);
        vec2 d = vec2((skyNdc.x - moonNDC.x) * aspect, skyNdc.y - moonNDC.y);
        float diskDist = length(d) / moonRadius;
        float moonHorizonMask = smoothstep(-0.006, 0.006, skyNdc.y);
        float moonDisk = GE_MoonDiscProfile(diskDist) * moonHorizonMask;
        if (moonDisk > 0.0)
        {
            vec2 moonUv = d / moonRadius * 0.5 + 0.5;
            moonUv.y = 1.0 - moonUv.y;
            vec4 moonAlbedo = GE_TAP_LOD0(uMoonFullTexture, moonUv);
            // The backdrop's disc frame is the screen's: right, and up (moon UV runs down it).
            vec3 sunInDisc = GE_MoonSunInDiscFrame(normalize(uSky.sunDirectionWS), moonDir, basisRight, basisUp);
            col += GE_MoonDiscRadiance(moonAlbedo, moonUv, diskDist, sunInDisc, uSky.moonIntensity,
                                       uSky.moonUboPad2) *
                   moonHorizonMask * moonAboveHorizon;
        }
    }

    oColor = SkyOutput(col * SkyStoreScale());
}

void main()
{
    vec2 ndc = GE_ViewportUVToYUpNdc(vUV);

    float thfov = uSky.tanHalfFovY;
    float aspect = max(uSky.viewportAspect, 0.001);

    // tanHalfFovY <= 0 is the host-side sentinel for orthographic projection.
    // Switch to a 2D backdrop instead of the per-pixel atmospheric simulation,
    // which produces a uniform color in ortho (all rays are parallel).
    if (thfov <= 0.0)
    {
        RenderOrthoBackdrop(ndc);
        return;
    }

    vec3 basisRight = normalize(uSky.cameraRightWS);
    vec3 basisUp    = normalize(uSky.cameraUpWS);
    vec3 basisFwd   = normalize(uSky.cameraForwardWS);

    vec3 dirView = normalize(vec3(ndc.x * aspect * thfov,
                                  ndc.y * thfov,
                                  -1.0));
    vec3 dirWS = normalize(dirView.x * basisRight +
                           dirView.y * basisUp +
                          (-dirView.z) * basisFwd);

    // Gradient sky: stylized 3-color vertical gradient, no atmosphere / sun disc / stars.
    if (uSky.gradientSkyTop.w >= 0.5)
    {
        oColor = SkyOutput(GradientSkyColor(dirWS) * SkyStoreScale());
        return;
    }

    float hdriIntensity = uSky.skyboxParams.y;
    if (hdriIntensity >= 0.0)
    {
        vec3 envColor = GE_TAP_LOD0(uSkyViewLUT, EncodeDirToLatLongUv(RotateAroundY(dirWS, uSky.skyboxParams.x))).rgb;
        oColor = SkyOutput(envColor * hdriIntensity * SkyStoreScale());
        return;
    }

    // Planet-relative up for stable zenith across camera movement
    vec3 planetCenter = vec3(0.0, -uAtmos.planetRadius, 0.0);
    vec3 camPos       = uSky.cameraPositionWS;
    float camR        = length(camPos - planetCenter);
    vec3 up           = (camPos - planetCenter) / max(camR, 1e-5);

    float viewZenithCos = dot(dirWS, up);

    vec3 sunDir = normalize(uSky.sunDirectionWS);

    float nightAmount = clamp(uSky.nightSkyBlend, 0.0, 1.0);

    vec3 northRef = vec3(0.0, 0.0, 1.0);
    if (abs(dot(northRef, up)) > 0.98)
        northRef = vec3(1.0, 0.0, 0.0);
    vec3 east = normalize(cross(up, northRef));
    vec3 north = normalize(cross(east, up));

    // The moon's disc on the sky: its direction, drawn radius and frame.
    bool moonShown = uSky.moonIntensity > 0.0;
    vec3 moonDir = normalize(uSky.moonDirectionWS);
    float moonR = max(1e-6, uSky.moonAngularRadius > 0.0 ? uSky.moonAngularRadius : 0.026);
    vec3 discRight;
    vec3 discUp;
    GE_MoonDiscFrame(moonDir, discRight, discUp);

    vec3 skyColor = SkyViewLutRadiance(dirWS, east, up, north);

    // ---- Sun disk ----
    // Drawn at the sun's apparent angular radius and given the radiance that makes the drawn
    // profile carry the sun's irradiance (E / omega_drawn), attenuated along the view ray by
    // the same transmittance the sky itself is built from. The disc's size and its brightness
    // are therefore one decision, not two: nothing here scales the sun by elevation, because
    // the atmosphere's own optical depth is what reddens and dims it at sunset.
    float muSun = dot(dirWS, sunDir);
    float sunRadius = uSky.sunAngularRadius;

    // The planet hides the disc where the view ray meets it: the transmittance below is 0 there,
    // which sets the disc on the true horizon at any altitude.
    float sunDisk = 0.0;
    if (sunRadius > 0.0 && muSun > 0.0)
    {
        float angle = acos(clamp(muSun, -1.0, 1.0));
        sunDisk = clamp(GE_SunDiscProfile(angle, sunRadius), 0.0, 1.0);
        sunDisk *= (1.0 - nightAmount);
    }

    vec3 transmittance = TransmittanceAlongView(dirWS);

    // The sky's single store factor (its exposure trim and P); the disc's peak is bounded against
    // the value it puts in the fp16 target, so it has to know the multiply that lands it there.
    float exposure = SkyStoreScale();
    float sunPeakRadiance = GE_SunDiscPeakRadiance(uSky.sunIntensity, sunRadius, exposure);
    vec3 sunRadiance = uSky.sunColor * sunPeakRadiance * sunDisk * transmittance;

    // ---- Moon disk: lit toward the sun, at the real moon's surface radiance, earthshine on the dark limb ----
    vec3 moonRadiance = vec3(0.0);
    float moonCover = 0.0;
    if (moonShown)
    {
        float muMoon = dot(dirWS, moonDir);
        if (muMoon > cos(moonR * 1.02))
        {
            float tDisk = sqrt(max(0.0, 2.0 * (1.0 - muMoon))) / moonR;
            vec3 perp = dirWS - muMoon * moonDir;
            vec2 moonUv = vec2(dot(perp, discRight) / moonR * 0.5 + 0.5,
                               0.5 - dot(perp, discUp) / moonR * 0.5);
            vec4 moonAlbedo = GE_TAP_LOD0(uMoonFullTexture, moonUv);
            vec3 sunInDisc = GE_MoonSunInDiscFrame(sunDir, moonDir, discRight, discUp);

            // The planet hides the disc where the view ray meets it (transmittance 0), and the
            // air reddens and dims it at moonrise exactly as it does the sun.
            moonRadiance = GE_MoonDiscRadiance(moonAlbedo, moonUv, tDisk, sunInDisc, uSky.moonIntensity,
                                               uSky.moonUboPad2) *
                           transmittance;
            moonCover = GE_MoonDiscCover(tDisk, moonAlbedo.a);
        }
    }
    // Under the disc the sky is the sky around it (GE_MoonSkyUnderDisc): four rim taps, disc pixels only.
    if (moonCover > 0.0)
    {
        vec3 rimSky = vec3(0.0);
        for (int k = 0; k < 4; ++k)
            rimSky += SkyViewLutRadiance(GE_MoonRimDirection(moonDir, discRight, discUp, moonR, k), east, up, north);
        skyColor = GE_MoonSkyUnderDisc(skyColor, 0.25 * rimSky, moonCover, transmittance);
    }

    // ---- Ground / below-horizon blend + night-sky gradient ----
    // Extracted into sky_composite.glsl so the IBL capture (sky_capture_cube.frag)
    // composites the sky identically. The bright discs (sun/moon/stars) stay below,
    // inline, so the capture can stop after this call.
    //
    // Below-horizon mode selects whether the stylized ground composite runs on read:
    //   ContinueHorizon (0) / PlanetGround (1): the LUT already holds the right
    //     below-horizon color (folded horizon or lit planet floor), so apply NO
    //     stylized ground — sample the LUT straight (applyGround = false).
    //   StylizedGround (2): the raw LUT trends black below, so paint the stylized
    //     ground/dark blend over it (applyGround = true) — the current look.
    // The night gradient above the horizon applies in every mode.
    bool applyStylizedGround = (uAtmos.belowHorizonMode == 2u);
    skyColor = GE_ApplySkyGroundAndNight(skyColor, dirWS, viewZenithCos, nightAmount,
                                         applyStylizedGround,
                                         uAtmos.groundAlbedo, uAtmos.groundBrightness,
                                         uAtmos.groundNightColor, uAtmos.belowHorizonDarkness,
                                         uAtmos.belowHorizonDarkColor, uAtmos.belowHorizonBlendSharpness,
                                         uAtmos.nightSkyHorizonColor);

    // PlanetGround composites a sharp lit floor per-pixel (the LUT stores only the
    // folded horizon-haze backdrop for this mode). The per-fragment ray-sphere hit
    // resolves the grazing planet edge at full resolution -- no LUT magnification.
    // GE_CompositeGroundFloor is shared with the IBL capture so the ground a camera
    // sees and the ground bounce a surface receives cannot drift apart.
    //
    // GE_CompositeGroundFloor is the shared visible+IBL floor. Mean sky
    // radiance is a per-pixel cost here; hoist it to the UBO when a scene
    // actually pins PlanetGround.
    if (uAtmos.belowHorizonMode == 1u && viewZenithCos < 0.005)
    {
        skyColor = GE_CompositeGroundFloor(skyColor, uTransLUT,
                                           GE_SkyDiffuseMeanRadiance(uSkyViewLUT, kSkyViewLutHeight),
                                           camPos, dirWS, planetCenter, uAtmos.planetRadius,
                                           uAtmos.atmosphereRadius, camR,
                                           viewZenithCos,
                                           uAtmos.groundAlbedo, uAtmos.groundBrightness,
                                           sunDir, uSky.sunColor, uSky.sunIntensity,
                                           uAtmos.betaRayleigh, uAtmos.rayleighScaleHeight,
                                           uAtmos.groundHazeStrength,
                                           uAtmos.groundNightColor, nightAmount);
    }

    // Horizon rim is a stylized overlay: the rim ramp painted as a band straddling
    // the horizon. Only StylizedGround wants it. The physical modes (ContinueHorizon
    // / PlanetGround) keep the atmospheric horizon clean, so no rim band there.
    if (uAtmos.belowHorizonMode == 2u)
    {
        float nightRimT = clamp(uSky.nightSkyBlend, 0.0, 1.0);
        float rimW = max(mix(uAtmos.groundHorizonDayCosWidth, uAtmos.groundHorizonNightCosWidth, nightRimT), 1e-5);
        float rimMask = 1.0 - smoothstep(0.0, rimW, abs(viewZenithCos));
        vec3 rimCol = mix(uAtmos.groundHorizonColor, uAtmos.groundHorizonNightColor, nightRimT);
        skyColor = mix(skyColor, rimCol, clamp(rimMask, 0.0, 1.0));
    }

    vec3 finalColor = skyColor + sunRadiance + moonRadiance;

    oColor = SkyOutput(finalColor * exposure);
}
