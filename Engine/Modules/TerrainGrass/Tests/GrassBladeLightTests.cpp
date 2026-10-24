// The light through a grass blade, executed on the SHIPPED shader code: the lighting model's far-face
// lobe (GE_DiffuseTransmissionFarFace in Includes/standard_pbr.glsl) and the grass surface's
// translucency profile and transmitted albedo (terrain_grass_surface.glsl), with the canopy shade the
// blade base takes (Includes/terrain_grass_canopy.glsl). The blocks are extracted verbatim at build
// time (ExtractShaderBlock.cmake) and compiled here through GlslShim.h, so an edit to a shader is an
// edit to what these tests measure.
//
// Three properties are under test. A sunlit face reflects the authored colour, whatever the
// translucency. A blade lit from behind never sends more light toward the viewer than the same blade
// lit from the front. And a fixture blade seen against a low sun reads brighter at its tip than at
// its root, where an opaque blade reads darker at its tip.

#include <gtest/gtest.h>

#include "GlslShim.h"

#include "Terrain/TerrainTypes.h"

#include <algorithm>
#include <cmath>

namespace
{
namespace Shader
{
using namespace GameEngine::GlslShim;
using GameEngine::GlslShim::clamp;
using GameEngine::GlslShim::dot;
using GameEngine::GlslShim::max;
using GameEngine::GlslShim::min;
using GameEngine::GlslShim::pow;
using GameEngine::GlslShim::smoothstep;
// The lighting model's own constant, declared beside the block in standard_pbr.glsl.
const float GE_PI = 3.14159265359f;
#include "GrassDiffuseTransmissionExtracted.h"
#include "GrassDielectricFresnelExtracted.h"
#include "GrassBladeLobeExtracted.h"
#include "GrassTranslucencyExtracted.h"
#include "GrassCanopyExtracted.h"
} // namespace Shader

using Shader::vec3;

constexpr float kInvPi = 1.0f / 3.14159265359f;

float Luma(const vec3& c)
{
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

// The default palette in linear light (TerrainGrass.h): the tip, the automatic root at 0.4 of the
// tip's luminance, and the back-light. Beside them a pale stylized leaf, a dark one and an orange
// back-light, so the rule is checked away from the defaults too.
const vec3 kTip(0.19f, 0.30f, 0.075f);
const vec3 kRoot(0.076f, 0.12f, 0.03f);
const vec3 kBacklight(0.50f, 0.62f, 0.12f);
const vec3 kAlbedos[] = {kTip, kRoot, vec3(0.55f, 0.75f, 0.30f), vec3(0.04f, 0.07f, 0.02f),
                         vec3(0.95f, 0.98f, 0.90f)};
const vec3 kBacklights[] = {kBacklight, vec3(0.9f, 0.35f, 0.05f), vec3(1.0f, 1.0f, 1.0f)};

// The near face's diffuse reflection per unit of light, at diffuse roughness 0 (Lambert): what the
// lighting model's diffuse lobe keeps after the far face takes its share.
vec3 NearFace(const vec3& albedo, float transmission, float NdotL)
{
    return albedo * (Shader::GE_DiffuseTransmissionNearFaceShare(transmission) * std::max(NdotL, 0.0f) * kInvPi);
}

} // namespace

// The light a blade sends toward the viewer from one light, per unit of light, from the lighting
// model's two diffuse lobes on what the surface hands it: the near face's reflection and the far
// face's light from a light behind it.
namespace
{
float LeafLuma(const Shader::GrassLeafLight& leaf, float NdotL)
{
    return Luma(NearFace(leaf.Albedo, leaf.FarFaceShare, NdotL)
                + Shader::GE_DiffuseTransmissionFarFace(leaf.FarFaceAlbedo, leaf.FarFaceShare, NdotL));
}
} // namespace

// A sunlit face reflects the authored colour exactly, whatever the translucency: the light through a
// blade is light it scatters instead of absorbing, not light taken from the colour the author picked.
TEST(GrassBladeLight, ASunlitFaceReflectsTheAuthoredColour)
{
    for (const vec3& albedo : kAlbedos)
    {
        for (float translucency : {0.0f, 0.5f, 1.0f})
        {
            for (float t : {0.0f, 0.5f, 1.0f})
            {
                const Shader::GrassLeafLight leaf =
                    Shader::GrassBladeLeafLight(albedo, kBacklight, translucency, t);
                EXPECT_NEAR(LeafLuma(leaf, 0.7f), Luma(albedo) * 0.7f * kInvPi, 1e-6f)
                    << "translucency " << translucency << " t " << t;
            }
        }
    }
}

// The leaf never scatters more light than reaches it: its scattering albedo stays at or below 1 in
// every channel, for every albedo, at every height and across the Translucency dial, and a pale leaf
// lets less light through rather than more.
TEST(GrassBladeLight, TheLeafNeverScattersMoreLightThanReachesIt)
{
    for (const vec3& albedo : kAlbedos)
    {
        for (float translucency : {0.0f, 0.5f, 1.0f, 3.0f})
        {
            for (float t : {0.0f, 0.5f, 1.0f})
            {
                const Shader::GrassLeafLight leaf =
                    Shader::GrassBladeLeafLight(albedo, kBacklight, translucency, t);
                EXPECT_LE(leaf.Albedo.x, 1.0f + 1e-6f) << "albedo luma " << Luma(albedo) << " t " << t;
                EXPECT_LE(leaf.Albedo.y, 1.0f + 1e-6f) << "albedo luma " << Luma(albedo) << " t " << t;
                EXPECT_LE(leaf.Albedo.z, 1.0f + 1e-6f) << "albedo luma " << Luma(albedo) << " t " << t;
            }
        }
    }
}

// A blade lit from behind sends no more light toward the viewer than the same blade lit from the
// front, at every height, for every albedo and back-light colour, and across the Translucency dial,
// including a value past its ceiling.
TEST(GrassBladeLight, ABackLitBladeIsNeverBrighterThanTheSameBladeLitFromTheFront)
{
    for (const vec3& albedo : kAlbedos)
    {
        for (const vec3& backlight : kBacklights)
        {
            for (float translucency : {0.0f, 0.4f, 1.0f, 3.0f})
            {
                for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
                {
                    const Shader::GrassLeafLight leaf =
                        Shader::GrassBladeLeafLight(albedo, backlight, translucency, t);
                    for (float cosine : {0.2f, 0.6f, 1.0f})
                    {
                        EXPECT_LE(LeafLuma(leaf, -cosine), LeafLuma(leaf, cosine) * (1.0f + 1e-5f) + 1e-7f)
                            << "translucency " << translucency << " t " << t << " cosine " << cosine
                            << " albedo luma " << Luma(albedo) << " back-light luma " << Luma(backlight);
                    }
                }
            }
        }
    }
}

// The far face takes light only from behind, in the back-light's colour, and at the tip of a thin leaf
// it sends as much light as the near face does.
TEST(GrassBladeLight, TheFarFaceIsLitFromBehindInTheBacklightsColour)
{
    const Shader::GrassLeafLight leaf = Shader::GrassBladeLeafLight(kTip, kBacklight, 1.0f, 1.0f);
    EXPECT_EQ(Luma(Shader::GE_DiffuseTransmissionFarFace(leaf.FarFaceAlbedo, leaf.FarFaceShare, 0.5f)), 0.0f);
    EXPECT_NEAR(LeafLuma(leaf, -0.5f), LeafLuma(leaf, 0.5f), 1e-6f);
    EXPECT_NEAR(leaf.FarFaceAlbedo.x / leaf.FarFaceAlbedo.y, kBacklight.x / kBacklight.y, 1e-4f);
    EXPECT_NEAR(leaf.FarFaceAlbedo.z / leaf.FarFaceAlbedo.y, kBacklight.z / kBacklight.y, 1e-4f);
}

// The light through grows from nothing at the thick root to Translucency at the thin tip, and
// Translucency stops at a thin leaf.
TEST(GrassBladeLight, TheLightThroughGrowsFromTheRootToTheTipAndStopsAtAThinLeaf)
{
    EXPECT_FLOAT_EQ(Shader::GrassBladeTranslucency(1.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(Shader::GrassBladeTranslucency(0.6f, 1.0f), 0.6f);
    float previous = 0.0f;
    for (float t = 0.05f; t <= 1.0f; t += 0.05f)
    {
        const float through = Shader::GrassBladeTranslucency(1.0f, t);
        EXPECT_GE(through, previous) << "t " << t;
        previous = through;
    }
    EXPECT_FLOAT_EQ(Shader::kGrassMaxTranslucency, 1.0f);
    EXPECT_FLOAT_EQ(Shader::GrassBladeTranslucency(3.0f, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(Shader::GrassBladeTranslucency(-1.0f, 1.0f), 0.0f);
}

namespace
{
// A fixture blade of the default meadow, upright, seen face on from eye height, its base 10 % and its
// tip 90 % of the way up. The shading normal at the base is the ground's, settled there as the
// surface settles the root; at the tip it is the blade's face, turned toward the viewer. The sun is
// 15 degrees above the horizon, in front of the face (lit) or behind it (back-lit); the sky lights
// both ends alike with a fifth of the sun's light on a horizontal surface.
struct FixtureEnd
{
    vec3 Albedo;
    float Height;
    float SunCosine; // the shading normal against the direction to the sun
};

constexpr float kSunElevationCosine = 0.9659258f; // cos 15 degrees: the face-on cosine of the sun
constexpr float kSunElevationSine = 0.2588190f;   // sin 15 degrees: the ground's cosine
constexpr float kSkyLight = 0.2f;

// The meadow's canopy depth: 21 blades per square metre, 36 mm leaves, 0.72 m blades, Canopy Shade 1.
float MeadowCanopyDepth()
{
    return std::min(21.0f * 0.036f * 0.72f * GameEngine::Terrain::kTerrainGrassCanopyOcclusionScale,
                    GameEngine::Terrain::kTerrainGrassCanopyOcclusionMax);
}

float FixtureLuma(const FixtureEnd& end, float translucency)
{
    const float shade = Shader::GrassBladeCanopyOcclusion(MeadowCanopyDepth(), 1.0f, end.Height);
    const Shader::GrassLeafLight leaf =
        Shader::GrassBladeLeafLight(end.Albedo * shade, kBacklight, translucency, end.Height);
    return LeafLuma(leaf, end.SunCosine) + Luma(leaf.Albedo) * kSkyLight * kInvPi;
}

// The sun's light alone, without the sky.
float FixtureSunLuma(const FixtureEnd& end, float translucency)
{
    const float shade = Shader::GrassBladeCanopyOcclusion(MeadowCanopyDepth(), 1.0f, end.Height);
    return LeafLuma(Shader::GrassBladeLeafLight(end.Albedo * shade, kBacklight, translucency, end.Height),
                    end.SunCosine);
}

float FixtureTipOverBase(float sunFaceCosine, float translucency)
{
    const FixtureEnd base{kRoot, 0.1f, kSunElevationSine};
    const FixtureEnd tip{kTip, 0.9f, sunFaceCosine};
    return FixtureLuma(tip, translucency) / FixtureLuma(base, translucency);
}
} // namespace

// The shipped Translucency, read from the GPU parameters: this suite links the rendering library
// only, not the engine's component headers. The component's default is pinned to this mirror by
// RenderPipelineDeclareTests.GrassGpuParamDefaultsMatchTheComponent.
const float kDefaultTranslucency = GameEngine::Terrain::TerrainGPUParams{}.GrassTranslucency;

// A grass leaf passes less light than it reflects: at the default Translucency the tip of the
// default blade sends less of its light through the far face than off the near one. At a
// Translucency of 1 the two are equal, and under the sky, which lights both faces, the tip is then
// twice as bright as its colour: the pale near-white tips of a field under a low sun.
TEST(GrassBladeLight, TheDefaultLeafPassesLessLightThanItReflects)
{
    const Shader::GrassLeafLight leaf = Shader::GrassBladeLeafLight(kTip, kBacklight, kDefaultTranslucency, 1.0f);
    EXPECT_LT(leaf.FarFaceShare, 0.5f) << "the far face's share of the tip's scattered light";
    EXPECT_LT(Luma(leaf.Albedo), 1.75f * Luma(kTip)) << "the tip's scattering albedo under the sky";
}

// The fixture blade against a low sun reads brighter at its tip than at its root by more than a
// fifth with the default Translucency, where an opaque blade reads darker at its tip; lit from the
// front, the tip stays at least as bright as the base.
TEST(GrassBladeLight, AFixtureBladeAgainstTheSunGlowsAtItsTip)
{
    const float backLit = FixtureTipOverBase(-kSunElevationCosine, kDefaultTranslucency);
    const float opaque = FixtureTipOverBase(-kSunElevationCosine, 0.0f);
    const float lit = FixtureTipOverBase(kSunElevationCosine, kDefaultTranslucency);
    EXPECT_GT(backLit, 1.2f) << "back-lit tip over base";
    // The tip's own albedo is brighter than the root's, so an opaque blade's tip is brighter too; the
    // light through the leaf is what makes it glow on top of that.
    EXPECT_GT(backLit, 1.25f * opaque) << "an opaque blade against the sun " << opaque;
    EXPECT_GE(lit, 1.0f) << "lit tip over base";
    // The sun alone: the tip's light is the light through the leaf, and it outshines the base's.
    const FixtureEnd base{kRoot, 0.1f, kSunElevationSine};
    const FixtureEnd tip{kTip, 0.9f, -kSunElevationCosine};
    EXPECT_GT(FixtureSunLuma(tip, kDefaultTranslucency), 1.2f * FixtureSunLuma(base, kDefaultTranslucency))
        << "the back-lit tip's sunlight over the base's";
}

// The blade's specular weight: the leaf part of a near blade carries no face-on reflection of its own,
// because its colour is the leaf as it looks face-on, shine included. The root, coloured as the ground,
// and the far field, settled onto the ground, keep the full lobe, the far field exactly.
TEST(GrassBladeLight, TheLeafCarriesNoFaceOnReflectionAndTheRootAndFarFieldKeepTheLobe)
{
    for (float farLod = 0.0f; farLod <= 1.0f; farLod += 0.05f)
    {
        const float farSettle = Shader::smoothstep(0.0f, Shader::kGrassNormalSettleRange, farLod);
        EXPECT_EQ(Shader::GrassBladeSpecularWeight(0.0f, farSettle), 1.0f) << "the root, far LOD " << farLod;
        float previous = 1.0f;
        for (float rise = 0.0f; rise <= 1.0f; rise += 0.05f)
        {
            const float weight = Shader::GrassBladeSpecularWeight(rise, farSettle);
            if (farLod >= Shader::kGrassNormalSettleRange)
                EXPECT_EQ(weight, 1.0f) << "the far field, far LOD " << farLod << " rise " << rise;
            EXPECT_LE(weight, previous) << "far LOD " << farLod << " rise " << rise;
            previous = weight;
        }
    }
    for (float rise = 0.0f; rise <= 1.0f; rise += 0.05f)
    {
        EXPECT_EQ(Shader::GrassBladeSpecularWeight(rise, 1.0f), 1.0f) << "the far settle, rise " << rise;
        EXPECT_LE(Shader::GrassBladeSpecularWeight(rise, 0.25f), Shader::GrassBladeSpecularWeight(rise, 0.5f))
            << "the weight returns toward the lobe as the far field settles, rise " << rise;
    }
    EXPECT_EQ(Shader::GrassBladeSpecularWeight(1.0f, 0.0f), 0.0f) << "the leaf part at the near LOD";
    // The weight follows the colour: at Grounding Strength 0 the root is coloured as leaf and
    // reflects as leaf; at 1 it is coloured as ground and keeps the lobe.
    EXPECT_EQ(Shader::GrassBladeSpecularWeight(Shader::GrassBladeGroundedRise(0.0f, 0.0f), 0.0f), 0.0f)
        << "the near root at Grounding Strength 0";
    EXPECT_EQ(Shader::GrassBladeSpecularWeight(Shader::GrassBladeGroundedRise(0.0f, 1.0f), 0.0f), 1.0f)
        << "the near root at Grounding Strength 1";
}

// The blade's albedo before lighting under Auto with Splat Root Color on, at height fraction t over
// the shipped Root Fade, composed as the surface composes it: the canopy from its root end to the
// tip, then the ground handover over it.
static Shader::vec3 AutoRootAlbedo(const Shader::vec3& ground, const Shader::vec3& tip, const Shader::vec3& autoRoot,
                            float t, float groundingStrength)
{
    const GameEngine::Terrain::TerrainGPUParams shipped{};
    const Shader::vec3 canopy = Shader::mix(
        Shader::GrassBladeCanopyRoot(autoRoot, ground, true, groundingStrength), tip, Shader::grassCanopyRise(t));
    const float colourRise = Shader::GrassBladeColourRise(shipped.GrassRootFadeStart, shipped.GrassRootFadeEnd, t);
    return Shader::mix(ground, canopy, Shader::GrassBladeGroundedRise(colourRise, groundingStrength));
}

// An automatic root reads root, then leaf: with Splat Root Color on, the blade's albedo rises from
// the ground at its base to the tip with no band darker than the ground between them. Grounding
// Strength keeps its meaning: at 0 the root is the tip in shade and the blade takes no ground, at 1
// the root is the ground. The ground is the untextured grass layer's turf green, the tip the default
// leaf green, the root under Auto the tip at 40 % in linear light (TerrainGrassAutoRootColor).
TEST(GrassBladeLight, AnAutomaticRootRisesFromTheGroundIntoTheLeafWithNoDip)
{
    const Shader::vec3 ground{0.10f, 0.16f, 0.035f};
    const Shader::vec3 tip{0.19f, 0.30f, 0.075f};
    const Shader::vec3 autoRoot = tip * 0.4f;
    float previous = Luma(ground);
    for (float t = 0.0f; t <= 1.0f; t += 0.01f)
    {
        const float value = Luma(AutoRootAlbedo(ground, tip, autoRoot, t, 1.0f));
        EXPECT_GE(value, previous - 1e-6f) << "the albedo darkens at t " << t;
        previous = value;
    }
    EXPECT_NEAR(previous, Luma(tip), 1e-5f) << "the tip is the leaf";

    const Shader::vec3 groundedRoot = AutoRootAlbedo(ground, tip, autoRoot, 0.0f, 1.0f);
    EXPECT_NEAR(groundedRoot.x, ground.x, 1e-6f) << "Grounding Strength 1: the root is the ground";
    EXPECT_NEAR(groundedRoot.y, ground.y, 1e-6f) << "Grounding Strength 1: the root is the ground";
    EXPECT_NEAR(groundedRoot.z, ground.z, 1e-6f) << "Grounding Strength 1: the root is the ground";
    const Shader::vec3 ungroundedRoot = AutoRootAlbedo(ground, tip, autoRoot, 0.0f, 0.0f);
    EXPECT_NEAR(ungroundedRoot.x, autoRoot.x, 1e-6f) << "Grounding Strength 0: the root is the tip in shade";
    EXPECT_NEAR(ungroundedRoot.y, autoRoot.y, 1e-6f) << "Grounding Strength 0: the root is the tip in shade";
    EXPECT_NEAR(ungroundedRoot.z, autoRoot.z, 1e-6f) << "Grounding Strength 0: the root is the tip in shade";
}

// The colour's handover ends exactly at the authored Root Fade end, for any authored span.
TEST(GrassBladeLight, TheColourEndsAtTheAuthoredEnd)
{
    const float spans[][2] = {{0.0f, 0.275f}, {0.0f, 0.25f}, {0.1f, 0.6f}, {-0.2f, 0.4f}};
    for (const auto& span : spans)
    {
        EXPECT_EQ(Shader::GrassBladeColourRise(span[0], span[1], span[1]), 1.0f) << "at the end " << span[1];
        EXPECT_LT(Shader::GrassBladeColourRise(span[0], span[1], span[1] - 0.005f), 1.0f) << "below the end " << span[1];
    }
}

// The reflection leaves the ground inside the colour's handover and nowhere else. The handover ends at
// the authored end (0.275 at the shipped span); from there up the blade reflects as leaf, as it is
// coloured. Inside the handover the blade keeps more of the ground's reflection than it
// keeps of the ground's colour, so the saturated leaf colour does not show at the base before the
// contact fades, and the reflection still leaves gradually: no tenth of the authored span takes more
// than 0.4 of the way from the ground's glaze to the leaf.
TEST(GrassBladeLight, TheReflectionLeavesTheGroundWithinTheColoursHandover)
{
    const float handoverEnd = 0.275f;
    EXPECT_EQ(Shader::GrassBladeColourRise(0.0f, handoverEnd, handoverEnd), 1.0f) << "the colour at the handover's end";
    EXPECT_EQ(Shader::GrassBladeColourRise(0.0f, 1.0f, 1.0f), 1.0f) << "the tip, with the span reaching it";
    for (float t = handoverEnd; t <= 1.0f; t += 0.05f)
        for (float groundingStrength : {0.0f, 0.5f, 1.0f})
            for (float farSettle : {0.0f, 0.5f, 1.0f})
            {
                const float rise = Shader::GrassBladeReflectionRise(0.0f, handoverEnd, t);
                EXPECT_EQ(Shader::GrassBladeSpecularWeight(Shader::GrassBladeGroundedRise(rise, groundingStrength), farSettle),
                          Shader::GrassBladeSpecularWeight(Shader::GrassBladeGroundedRise(1.0f, groundingStrength), farSettle))
                    << "above the handover, t " << t << " Grounding Strength " << groundingStrength << " far settle "
                    << farSettle;
            }
    EXPECT_EQ(Shader::GrassBladeReflectionRise(0.0f, handoverEnd, 0.0f), 0.0f) << "the root";
    for (float t = 0.025f; t < handoverEnd - 0.01f; t += 0.025f)
        EXPECT_LT(Shader::GrassBladeReflectionRise(0.0f, handoverEnd, t), Shader::GrassBladeColourRise(0.0f, handoverEnd, t))
            << "inside the handover, t " << t;
    for (float t = 0.0f; t < handoverEnd; t += 0.0025f)
        EXPECT_LE(Shader::GrassBladeReflectionRise(0.0f, handoverEnd, t + 0.025f) - Shader::GrassBladeReflectionRise(0.0f, handoverEnd, t), 0.4f)
            << "the glaze-to-leaf handover over a tenth of the span from t " << t;
    EXPECT_EQ(Shader::GrassBladeReflectionRise(-0.5f, -0.25f, 0.0f), 1.0f) << "a span ending below the root";
}

// The weight scales the face-on reflectance only: the leaf part reflects nothing face-on and still
// rises to the full Schlick term toward grazing, (1 - cos 80 degrees)^5 = 0.3853 at 80 degrees, under
// both the sun's Fresnel (F82, with the white edge tint a dielectric passes) and plain Schlick. A
// weight that also scaled the grazing reflection would take that rise away.
TEST(GrassBladeLight, TheSpecularWeightScalesTheFaceOnReflectionOnly)
{
    const vec3 white(1.0f, 1.0f, 1.0f);
    const vec3 leafF0 = Shader::GE_DielectricF0(Shader::GrassBladeSpecularWeight(1.0f, 0.0f), white, 1.5f);
    const vec3 rootF0 = Shader::GE_DielectricF0(Shader::GrassBladeSpecularWeight(0.0f, 0.0f), white, 1.5f);
    const float cos80 = std::cos(80.0f * 3.14159265359f / 180.0f);
    EXPECT_EQ(Shader::GE_SchlickFresnel(leafF0, 1.0f).y, 0.0f) << "the leaf part face-on";
    EXPECT_NEAR(Shader::GE_SchlickFresnel(leafF0, cos80).y, 0.3853f, 1e-4f) << "the leaf part at 80 degrees";
    EXPECT_NEAR(Shader::GE_SchlickFresnel(rootF0, 1.0f).y, 0.04f, 1e-6f) << "the root face-on";
    EXPECT_EQ(Shader::GE_FresnelF82(leafF0, white, 1.0f).y, 0.0f) << "the sun's Fresnel on the leaf part face-on";
    EXPECT_NEAR(Shader::GE_FresnelF82(leafF0, white, cos80).y, 0.3853f, 1e-4f)
        << "the sun's Fresnel on the leaf part at 80 degrees";
}
