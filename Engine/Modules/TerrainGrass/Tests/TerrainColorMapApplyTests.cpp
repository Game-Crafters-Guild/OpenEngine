// The terrain colour map's apply (Includes/terrain_color_map.glsl), compiled from the shipped shader
// through GlslShim.h: the expression the ground and the blade root both run. And the host mirror of
// the grass field coverage (Terrain/TerrainGrassCoverage.h) the colour map's ground write uses at
// bake, held to the shader's own block (Includes/terrain_grass_canopy.glsl).

#include "Terrain/TerrainGrassCoverage.h"

#include "GlslShim.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>

namespace TerrainColorMapShader
{
using namespace GameEngine::GlslShim;
#include "TerrainColorMapExtracted.h"
} // namespace TerrainColorMapShader

namespace GrassCanopyShaderForColorMap
{
using namespace GameEngine::GlslShim;
#include "GrassCanopyExtracted.h"
} // namespace GrassCanopyShaderForColorMap

namespace
{
using GameEngine::GlslShim::vec3;
using GameEngine::GlslShim::vec4;

bool SameBits(float a, float b)
{
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

// No map binds no texture and the shader reads vec4(0); an all-zero texel reads the same. Both leave
// the ground's colour bit for bit, so a terrain whose map holds nothing renders as one with no map.
TEST(TerrainColorMapApply, AZeroTexelLeavesTheGroundBitForBit)
{
    const std::array<vec3, 4> grounds = {vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f),
                                         vec3(0.10f, 0.16f, 0.035f), vec3(1e-7f, 0.5f, 0.9999f)};
    for (const vec3& ground : grounds)
    {
        const vec3 out = TerrainColorMapShader::TerrainApplyColorMap(ground, vec4(0.0f, 0.0f, 0.0f, 0.0f));
        EXPECT_TRUE(SameBits(out.x, ground.x) && SameBits(out.y, ground.y) && SameBits(out.z, ground.z));
    }
}

// A texel holding colour c at weight w (stored c x w and w) mixes the ground toward c by w: the
// untextured turf (0.10, 0.16, 0.035) under the soil (0.10, 0.075, 0.045) at 0.55 is
// (0.100, 0.11325, 0.0405), and at weight 1 the ground is the soil.
TEST(TerrainColorMapApply, AKnownColourAndWeightMixesTheGround)
{
    const vec3 turf(0.10f, 0.16f, 0.035f);
    const vec3 soil(0.10f, 0.075f, 0.045f);
    for (const float w : {0.25f, 0.55f, 1.0f})
    {
        const vec3 out = TerrainColorMapShader::TerrainApplyColorMap(turf, vec4(soil.x * w, soil.y * w, soil.z * w, w));
        EXPECT_NEAR(out.x, turf.x + (soil.x - turf.x) * w, 1e-6f) << w;
        EXPECT_NEAR(out.y, turf.y + (soil.y - turf.y) * w, 1e-6f) << w;
        EXPECT_NEAR(out.z, turf.z + (soil.z - turf.z) * w, 1e-6f) << w;
    }
    const vec3 half = TerrainColorMapShader::TerrainApplyColorMap(
        turf, vec4(soil.x * 0.55f, soil.y * 0.55f, soil.z * 0.55f, 0.55f));
    EXPECT_NEAR(half.y, 0.11325f, 1e-5f);
    EXPECT_NEAR(half.z, 0.0405f, 1e-5f);
}

// The host coverage the bake writes the field's ground by is the shader's: a sweep of splat weights,
// thresholds and both splat states, compared to the shipped block.
TEST(TerrainColorMapGroundWrite, TheHostCoverageIsTheShadersCoverage)
{
    for (const bool hasSplat : {false, true})
        for (const float threshold : {0.0f, 0.3f, 0.45f, 0.8f})
            for (unsigned layer = 0; layer < 4; ++layer)
                for (int a = 0; a <= 10; ++a)
                    for (int b = 0; b <= 10 - a; ++b)
                    {
                        const float w0 = a / 10.0f, w1 = b / 10.0f, w2 = (10 - a - b) / 20.0f;
                        const float w3 = 1.0f - w0 - w1 - w2;
                        const float host[4] = {w0, w1, w2, w3};
                        const float shader = GrassCanopyShaderForColorMap::GrassSplatCoverage(
                            vec4(w0, w1, w2, w3), layer, threshold, hasSplat);
                        EXPECT_FLOAT_EQ(GameEngine::Terrain::GrassSplatCoverage(host, layer, threshold, hasSplat),
                                        shader)
                            << hasSplat << " " << threshold << " " << layer << " " << a << "," << b;
                    }
    for (const float density : {0.0f, 0.4f, 1.0f})
        for (const float height : {0.0f, 0.5f, 1.0f})
            EXPECT_FLOAT_EQ(GameEngine::Terrain::GrassFieldCoverage(0.8f, height, density),
                            GrassCanopyShaderForColorMap::GrassFieldCoverage(
                                0.8f, GameEngine::GlslShim::vec2(height, density)));
}

} // namespace
