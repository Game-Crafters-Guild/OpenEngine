// The terrain's receiveShadows reaches the fragment gate. The CBT draw has no instance record, so the
// vertex modifier sets the instance flags from the terrain params row. The flag mapping is the
// shader's own block (extracted at build time); CBTLayoutTests checks the wiring in the shader source.

#include <gtest/gtest.h>

#include "GlslShim.h"
#include "Terrain/TerrainTypes.h"

#include <cstdint>

namespace
{
namespace Shader
{
using GameEngine::GlslShim::uint;
#include "CBTInstanceFlagsExtracted.h"
} // namespace Shader

// The bit adapter_forward.glsl reads for its receive-shadows gate (vInstanceFlags & 2u).
constexpr uint32_t kInstanceFlagReceiveShadows = 1u << 1;

// Every terrain params flag except receive shadows.
constexpr uint32_t kTerrainFlagsWithoutReceive =
    GameEngine::Terrain::kTerrainFlagAtlasBacked | GameEngine::Terrain::kTerrainFlagHasHeightmap |
    GameEngine::Terrain::kTerrainFlagHasNormalmap | GameEngine::Terrain::kTerrainFlagHasSplatmap |
    GameEngine::Terrain::kTerrainFlagHasGrassControls | GameEngine::Terrain::kTerrainFlagHasColorMap;
} // namespace

TEST(CBTInstanceFlags, ATerrainThatReceivesShadowsSetsTheGateBit)
{
    EXPECT_EQ(Shader::CBT_InstanceFlags(GameEngine::Terrain::kTerrainFlagReceiveShadows) &
                  kInstanceFlagReceiveShadows,
              kInstanceFlagReceiveShadows);
    EXPECT_EQ(Shader::CBT_InstanceFlags(GameEngine::Terrain::kTerrainFlagReceiveShadows |
                                        kTerrainFlagsWithoutReceive) &
                  kInstanceFlagReceiveShadows,
              kInstanceFlagReceiveShadows);
}

TEST(CBTInstanceFlags, ATerrainWithReceiveShadowsOffClearsTheGateBit)
{
    EXPECT_EQ(Shader::CBT_InstanceFlags(0u) & kInstanceFlagReceiveShadows, 0u);
    // The atlas-backed bit has the same value as the instance receive bit: it must not pass through.
    EXPECT_EQ(Shader::CBT_InstanceFlags(kTerrainFlagsWithoutReceive) & kInstanceFlagReceiveShadows, 0u);
}
