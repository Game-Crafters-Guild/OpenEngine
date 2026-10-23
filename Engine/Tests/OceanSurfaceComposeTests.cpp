// The ocean surface is registered at runtime from a synthetic document, so no
// .material asset names Ocean/ocean_surface.glsl and no other suite composes it:
// an edit to the surface, its vertex modifier or the ocean_common.glsl parameter
// block they share reaches a compiler for the first time when a scene with water
// opens. This runs the shipped document and keywords through the real
// ShaderComposer -> ShaderCompileService path in both shader profiles, which are
// the two the cook and the runtime request.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Ocean/OceanSurfaceMaterial.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "ScopedCompatShaderProfile.h"
#include "StagedTestPaths.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

using TestSupport::ScopedCompatShaderProfile;

class OceanSurfaceCompose : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_EngineShaderDir = TestPaths::StagedRenderingShadersDir();
        if (!fs::exists(m_EngineShaderDir))
            GTEST_SKIP() << "Staged engine shader tree not found: " << m_EngineShaderDir.string();
        if (!ShaderCompileService::IsCompilerAvailable())
            GTEST_SKIP() << "no shader compiler in this build";

        m_CacheRoot = fs::temp_directory_path() / ("ge_ocean_compose_" + GUID::Generate().ToString());
        fs::create_directories(m_CacheRoot);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_CacheRoot, ec);
    }

    // The registration pipeline, exactly as OceanForwardContributor and
    // MaterialVariantCook ask for it; `passKeywords` adds the keywords of the pass the surface draws
    // in (the height fog, the reflection slices).
    MaterialBuildResult BuildOceanSurface(bool compatProfile,
                                          MaterialKeyword passKeywords = MaterialKeyword::None)
    {
        MaterialBuildContext context{};
        context.AdapterShaderDir = m_EngineShaderDir;
        context.CacheRoot = m_CacheRoot / "Shaders";
        return BuildMaterialToShaderPackage(
            Ocean::BuildOceanSurfaceMaterialDocument(), m_CacheRoot / "ocean_surface.material",
            "ocean_surface", context, ShaderSourceKind::SpirV,
            Ocean::OceanSurfaceMaterialKeywords(compatProfile) | passKeywords, VertexAttributeFlags::None);
    }

    // An ocean shader source from the staged tree the composer resolves includes from; empty when
    // the file is missing.
    std::string ReadStagedOceanSource(const char* fileName) const
    {
        std::ifstream file(m_EngineShaderDir / "Ocean" / fileName, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    fs::path m_EngineShaderDir;
    fs::path m_CacheRoot;
};
} // namespace

TEST_F(OceanSurfaceCompose, NativeProfileVariantCompiles)
{
    const MaterialBuildResult built = BuildOceanSurface(false);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    EXPECT_FALSE(built.composedVertexSource.empty());
    EXPECT_FALSE(built.composedFragmentSource.empty());
}

TEST_F(OceanSurfaceCompose, CompatProfileVariantCompiles)
{
    ScopedCompatShaderProfile compat;
    const MaterialBuildResult built = BuildOceanSurface(true);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    EXPECT_FALSE(built.composedVertexSource.empty());
    EXPECT_FALSE(built.composedFragmentSource.empty());
}

// The water reflects the sky once. The ocean blends its own Fresnel reflection (the
// environment cube, the planar capture, the shallow clarity) into its emission, and it keeps
// the IBL keyword because that reflection reads the environment cube, so the composed program
// also runs the standard environment term. The surface that program includes must limit that
// term's lobe to the foam and paint, the reflectances that cover the water; GE_EvaluateIBL
// scaling the lobe by the coverage is IblShaderContract's. The composer leaves includes
// unexpanded, so the surface is read from the staged tree the composer resolved it from: this
// pins the source, it does not execute it. The field is written once, so nothing later in the
// surface overrides the coverage.
TEST_F(OceanSurfaceCompose, NativeProfileWaterTakesNoStandardEnvironmentSpecular)
{
    const MaterialBuildResult built = BuildOceanSurface(false);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    ASSERT_NE(built.composedFragmentSource.find("GE_EvaluateIBL(so,"), std::string::npos)
        << "the native ocean program is expected to run the standard environment term";

    const std::string surface = ReadStagedOceanSource("ocean_surface.glsl");
    ASSERT_FALSE(surface.empty());
    const std::size_t coverage = surface.find("o.environmentSpecularCoverage = albedoCoverage;");
    ASSERT_NE(coverage, std::string::npos)
        << "the ocean must limit the standard environment lobe to its foam and paint coverage";
    std::size_t writes = 0;
    for (std::size_t at = surface.find("o.environmentSpecularCoverage"); at != std::string::npos;
         at = surface.find("o.environmentSpecularCoverage", at + 1))
        ++writes;
    EXPECT_EQ(writes, 1u) << "a second write would override the foam and paint coverage";
    EXPECT_LT(surface.rfind("albedoCoverage = "), coverage)
        << "the coverage must be written after the last write to albedoCoverage";
    EXPECT_GT(surface.find("return o;", coverage), coverage);
}

// The water's reflection is the lobe the scene's reflection sources replace. The native ocean
// weights its reflection itself (Fresnel, Specular, ReflectionStrength, the shallow suppression,
// the underside mirror) and publishes that weight for the environment term to evaluate, so the
// local probe's parallax and the AmbientLight specular fill reach open water; it publishes the
// reflection it emits itself (the planar capture, the procedural sky where the environment is
// faded out), so screen-space reflections replace all of it. It does not sample the environment
// cube itself. The surface is read from the staged tree the composer resolved it from: this pins
// the source, it does not execute it.
TEST_F(OceanSurfaceCompose, NativeProfileWaterPublishesItsReflection)
{
    const MaterialBuildResult built = BuildOceanSurface(false);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    ASSERT_NE(built.composedFragmentSource.find("GE_EvaluateIBL(so,"), std::string::npos)
        << "the native ocean program is expected to run the standard environment term";

    const std::string surface = ReadStagedOceanSource("ocean_surface.glsl");
    ASSERT_FALSE(surface.empty());
    for (const char* field : {"o.environmentReflectionWeight =", "o.emittedReflection ="})
    {
        std::size_t writes = 0;
        for (std::size_t at = surface.find(field); at != std::string::npos; at = surface.find(field, at + 1))
            ++writes;
        EXPECT_EQ(writes, 1u) << field << " must be written once";
    }
    EXPECT_EQ(surface.find("ge_prefilterCube"), std::string::npos)
        << "the water's environment reflection must come from the environment term, not a sample of its own";
}

namespace
{
// The highest colour location the fragment stage of `built` writes, or -1 with no colour output.
int HighestFragmentColorLocation(const MaterialBuildResult& built)
{
    if (!built.package)
        return -1;
    const auto fs = built.package->meta.Stages.find("fs");
    if (fs == built.package->meta.Stages.end())
        return -1;
    int highest = -1;
    for (const auto& output : fs->second.Outputs)
        if (!output.Builtin)
            highest = std::max(highest, static_cast<int>(output.Location));
    return highest;
}
} // namespace

// The cook's rows for the surface with the screen-space reflection slices: the variant compiles
// and its fragment stage writes the slices at locations 1 to 3 (the normal and roughness, the
// replaced weight and the incident radiance), and the variant without the keyword writes the
// colour only. This checks the composed programs, not the draw: that the water draws with the
// slices program is RenderPipelineDeclareTests.WaterSurfaceDrawsTheProgramThatWritesTheSlices.
TEST_F(OceanSurfaceCompose, NativeProfileReflectionSlicesVariantWritesTheSlices)
{
    const MaterialBuildResult withSlices = BuildOceanSurface(false, MaterialKeyword::SSSRNormalRoughness);
    ASSERT_TRUE(withSlices.success) << (withSlices.errors.empty() ? std::string{} : withSlices.errors.front());
    EXPECT_EQ(HighestFragmentColorLocation(withSlices), 3);

    const MaterialBuildResult colorOnly = BuildOceanSurface(false);
    ASSERT_TRUE(colorOnly.success) << (colorOnly.errors.empty() ? std::string{} : colorOnly.errors.front());
    EXPECT_EQ(HighestFragmentColorLocation(colorOnly), 0);
}

// The water reflects the sun once. The native ocean program runs the forward adapter's
// standard lighting, whose sun lobe is the water's one reflection of the sun: GGX at the
// water's roughness with the water's F0, through the same shadow and light loop as every other
// surface. This pins that the surface gives that lobe the water's F0 (0.02 at the default
// indices against the default 0.04), from the larger index over the smaller as the environment
// Fresnel takes it, and that the surface's own emission takes no sunlight: the sun's color
// reaches the emission only as a hue, the tint of the subsurface scatter, so any sun reflection
// the surface adds (sunCol times a lobe) is a second one. The surface is read from the staged
// tree the composer resolved it from: this pins the source, it does not execute it.
TEST_F(OceanSurfaceCompose, NativeProfileWaterReflectsTheSunOnce)
{
    const MaterialBuildResult built = BuildOceanSurface(false);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    ASSERT_NE(built.composedFragmentSource.find("GE_AccumulateClusteredLighting("), std::string::npos)
        << "the native ocean program is expected to run the standard light loop, sun included";

    const std::string surface = ReadStagedOceanSource("ocean_surface.glsl");
    const std::string parameters = ReadStagedOceanSource("ocean_common.glsl");
    ASSERT_FALSE(surface.empty());
    ASSERT_FALSE(parameters.empty());

    const std::size_t ior =
        surface.find("o.specularIor = max(uIorWater, uIorAir) / max(min(uIorWater, uIorAir), 1.0);");
    EXPECT_NE(ior, std::string::npos) << "the standard sun lobe must take the water's F0";
    if (ior != std::string::npos)
    {
        EXPECT_EQ(surface.find("o.specularIor", ior + 1), std::string::npos)
            << "a second write would override the water's F0";
        EXPECT_GT(surface.find("return o;", ior), ior);
    }

    std::size_t emissiveWrites = 0;
    for (std::size_t at = surface.find("o.emissive ="); at != std::string::npos;
         at = surface.find("o.emissive =", at + 1))
        ++emissiveWrites;
    EXPECT_NE(surface.find("o.emissive = body * (1.0 - albedoCoverage);"), std::string::npos)
        << "the open water emits its body and its environment reflection, no sun term";
    EXPECT_EQ(parameters.find("uDirectionalLight"), std::string::npos)
        << "the ocean's own sun glitter dials are gone: the sun reflects through the standard lobe";
    EXPECT_EQ(surface.find("uDirectionalLight"), std::string::npos);
    EXPECT_EQ(emissiveWrites, 2u) << "the body above water and the debug view; no other emission";

    const std::string sunColor = "vec3 sunCol = uSunColor.rgb;";
    const std::string sunHue = "vec3 sunTint = sunCol / max(max(sunCol.r, sunCol.g), max(sunCol.b, 1e-3));";
    const std::size_t sunColorAt = surface.find(sunColor);
    const std::size_t sunHueAt = surface.find(sunHue);
    ASSERT_NE(sunColorAt, std::string::npos) << "the surface reads the sun's color once, as sunCol";
    ASSERT_NE(sunHueAt, std::string::npos) << "the surface takes the sun's hue as sunTint";
    EXPECT_EQ(surface.find("uSunColor"), surface.find("uSunColor", sunColorAt));
    EXPECT_EQ(surface.find("uSunColor", sunColorAt + sunColor.size()), std::string::npos)
        << "a second read of the sun's color adds sunlight to the emission";
    for (std::size_t at = surface.find("sunCol"); at != std::string::npos; at = surface.find("sunCol", at + 1))
    {
        const bool declared = at >= sunColorAt && at < sunColorAt + sunColor.size();
        const bool hue = at >= sunHueAt && at < sunHueAt + sunHue.size();
        EXPECT_TRUE(declared || hue)
            << "sunCol is read outside its hue at offset " << at
            << ": the sun's own light reflects through the standard lobe only";
    }
}

// The water's sun highlight is anti-aliased like every other surface's: the forward adapter
// floors the roughness by the screen-space variance of the normal, which spreads ripples
// smaller than a pixel into a wider highlight instead of sparkling. The ocean variant compiles
// with optimization off, so the SPIR-V names every function it calls and drops the ones it
// does not: the name of the floor's function in the fragment stage means the call survived the
// variant's defines.
TEST_F(OceanSurfaceCompose, NativeProfileWaterKeepsTheSpecularAntiAliasingFloor)
{
    const MaterialBuildResult built = BuildOceanSurface(false);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    ASSERT_NE(built.package, nullptr);
    const auto fragment = built.package->stageBytes.find("fs");
    ASSERT_NE(fragment, built.package->stageBytes.end());
    const std::string name = "GE_ApplySpecularAA(";
    const bool named =
        std::search(fragment->second.begin(), fragment->second.end(), name.begin(), name.end()) != fragment->second.end();
    EXPECT_TRUE(named) << "the native ocean fragment must run the specular anti-aliasing floor";
}
