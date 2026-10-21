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
using GameEngine::GlslShim::smoothstep;
// The lighting model's own constant, declared beside the block in standard_pbr.glsl.
const float GE_PI = 3.14159265359f;
#include "GrassDiffuseTransmissionExtracted.h"
#include "GrassTranslucencyExtracted.h"
#include "GrassCanopyExtracted.h"
} // namespace Shader

using Shader::vec3;

constexpr float kInvPi = 1.0f / 3.14159265359f;

float Luma(const vec3& c)
{
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

// The default palette in linear light (TerrainGrass.h): the tip, the automatic root at 0.65 of the
// tip's luminance, and the back-light. Beside them a pale stylized leaf, a dark one and an orange
// back-light, so the rule is checked away from the defaults too.
const vec3 kTip(0.19f, 0.30f, 0.075f);
const vec3 kRoot(0.1235f, 0.195f, 0.04875f);
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

// The fixture blade against a low sun reads brighter at its tip than at its root by more than a
// fifth with the default Translucency, where an opaque blade reads darker at its tip; lit from the
// front, the tip stays at least as bright as the base.
TEST(GrassBladeLight, AFixtureBladeAgainstTheSunGlowsAtItsTip)
{
    constexpr float kDefaultTranslucency = 1.0f;
    const float backLit = FixtureTipOverBase(-kSunElevationCosine, kDefaultTranslucency);
    const float opaque = FixtureTipOverBase(-kSunElevationCosine, 0.0f);
    const float lit = FixtureTipOverBase(kSunElevationCosine, kDefaultTranslucency);
    EXPECT_GT(backLit, 1.2f) << "back-lit tip over base";
    EXPECT_LT(opaque, 1.1f) << "an opaque blade against the sun";
    EXPECT_GE(lit, 1.0f) << "lit tip over base";
    // The sun alone: the tip's light is the light through the leaf, and it outshines the base's.
    const FixtureEnd base{kRoot, 0.1f, kSunElevationSine};
    const FixtureEnd tip{kTip, 0.9f, -kSunElevationCosine};
    EXPECT_GT(FixtureSunLuma(tip, kDefaultTranslucency), 1.2f * FixtureSunLuma(base, kDefaultTranslucency))
        << "the back-lit tip's sunlight over the base's";
}
