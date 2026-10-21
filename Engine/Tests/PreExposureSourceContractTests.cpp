// Scene colour is stored at the frame's pre-exposure P (pre_exposure_snapshot.comp). A pass that
// writes radiance into it without multiplying by P renders wrong by a factor of P, and a reader that
// compares it with an absolute value without dividing by P changes its look with the exposure; the
// pixel tests only see that once P is not 1. These contracts pin the closed lists from the source.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::string ReadSource(const char* relativePath)
{
    std::ifstream in(std::filesystem::path(GE_RENDERER_REPO_ROOT) / relativePath);
    EXPECT_TRUE(in.is_open()) << relativePath;
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

bool ReadsThePreExposure(const std::string& source)
{
    return source.find("GE_PreExposure()") != std::string::npos ||
           source.find("GE_PreExposureHistoryScale()") != std::string::npos;
}

std::string StripWhitespace(const std::string& text)
{
    std::string out;
    std::copy_if(text.begin(), text.end(), std::back_inserter(out),
                 [](unsigned char c) { return std::isspace(c) == 0; });
    return out;
}

// Whether `source` holds `code`, ignoring whitespace: a reformat does not change the tokens.
bool ContainsTokens(const std::string& source, const std::string& code)
{
    return StripWhitespace(source).find(StripWhitespace(code)) != std::string::npos;
}

nlohmann::json ReadGraph(const char* relativePath)
{
    return nlohmann::json::parse(ReadSource(relativePath));
}

constexpr const char* kRenderingShaders = "Engine/Modules/Rendering/Shaders/";
} // namespace

TEST(PreExposureSourceContract, EveryWriterStoresItsRadianceAtThePreExposure)
{
    const std::vector<const char*> writers = {
        "Engine/Modules/Rendering/Shaders/Adapters/adapter_forward.glsl",
        "Engine/Modules/Rendering/Shaders/Includes/ibl.glsl",
        "Engine/Modules/Rendering/Shaders/Ocean/ocean_surface.glsl",
        "Engine/Modules/Rendering/Shaders/sky_render.frag",
        "Engine/Modules/Rendering/Shaders/sky_stars_billboard.frag",
        "Engine/Modules/Rendering/Shaders/clear_color_backdrop.frag",
        "Engine/Modules/Rendering/Shaders/cluster_debug_overlay.frag",
        "Engine/Modules/Rendering/Shaders/atmospheric_cloud_layer.frag",
        "Engine/Modules/VolumetricClouds/Shaders/volumetric_clouds_composite.frag",
        "Engine/Modules/VolumetricFog/Shaders/volumetric_fog_composite.frag",
        "Engine/Modules/Rendering/Shaders/Ocean/ocean_underwater.frag",
        "Engine/Modules/Rendering/Shaders/Ocean/ocean_spray_gpu.frag",
        "Engine/Modules/Rendering/Shaders/lens_flare.frag",
        "Engine/Modules/Rendering/Shaders/sun_glare.frag",
        "Engine/Modules/Rendering/Shaders/forwardplus_lighting.frag",
        "Packages/fidelityfx-dof/Assets/Shaders/ffx_dof_composite.comp",
    };
    for (const char* path : writers)
        EXPECT_TRUE(ReadsThePreExposure(ReadSource(path))) << path << " writes scene colour without P";
}

TEST(PreExposureSourceContract, EveryAbsoluteReaderDividesByThePreExposure)
{
    const std::vector<const char*> readers = {
        "auto_exposure_histogram.comp", "tonemap.frag", "bloom_threshold.frag", "halation_prefilter.frag",
        "hdr_color_fx.frag", "fog_glow_fast.frag", "fog_glow_prefilter.frag", "fog_glow_downsample.frag",
        "taa_resolve.comp", "taa_resolve_upscale.comp", "ScreenSpaceReflections/sssr_intersect.comp",
        "ScreenSpaceReflections/sssr_reproject.comp", "ScreenSpaceReflections/sssr_temporal.comp",
    };
    for (const char* name : readers)
    {
        const std::string path = std::string(kRenderingShaders) + name;
        EXPECT_TRUE(ReadsThePreExposure(ReadSource(path.c_str()))) << path;
    }
    EXPECT_TRUE(ReadsThePreExposure(ReadSource("Packages/fidelityfx-dof/Assets/Shaders/ffx_dof_blur.comp")));
}

TEST(PreExposureSourceContract, TheHistoriesAreRescaledByTheRatioOfPreExposures)
{
    for (const char* name : {"taa_resolve.comp", "taa_resolve_upscale.comp",
                             "ScreenSpaceReflections/sssr_intersect.comp",
                             "ScreenSpaceReflections/sssr_reproject.comp"})
    {
        const std::string path = std::string(kRenderingShaders) + name;
        EXPECT_NE(ReadSource(path.c_str()).find("GE_PreExposureHistoryScale()"), std::string::npos)
            << path << " reads a history stored at the previous frame's P";
    }
}

TEST(PreExposureSourceContract, AbsoluteStoresNeverReadThePreExposure)
{
    const std::vector<const char*> absolute = {
        "Engine/Modules/Rendering/Shaders/sky_view_lut.comp",
        "Engine/Modules/Rendering/Shaders/sky_multiscatter_lut.comp",
        "Engine/Modules/Rendering/Shaders/sky_transmittance_lut.comp",
        "Engine/Modules/Rendering/Shaders/sky_capture_cube.frag",
        "Engine/Modules/Rendering/Shaders/ddgi_blend.comp",
        "Engine/Modules/VolumetricFog/Shaders/volumetric_fog_light.comp",
        "Engine/Modules/VolumetricFog/Shaders/volumetric_fog_integrate.comp",
    };
    for (const char* path : absolute)
        EXPECT_FALSE(ReadsThePreExposure(ReadSource(path))) << path << " is an absolute store";
}

TEST(PreExposureSourceContract, TheShippedGraphsSnapshotTheViewsExposureBeforeAnyWriter)
{
    for (const char* path : {"Engine/Assets/RenderPipelines/ForwardPlus.rendergraph",
                             "Apps/WebLibrary/Assets/RenderPipelines/Web.rendergraph"})
    {
        SCOPED_TRACE(path);
        const nlohmann::json graph = ReadGraph(path);
        int snapshot = -1, sky = -1, index = 0;
        for (const auto& pass : graph["passes"])
        {
            const std::string id = pass.value("id", std::string());
            if (id == "PreExposureSnapshot")
            {
                snapshot = index;
                EXPECT_EQ(pass["buffers"]["state"], "ExposureHistory");
                EXPECT_EQ(pass["bufferUsages"]["state"], "StorageReadWrite");
                // P follows the view's exposure: the static scale and the auto-exposure flag come
                // from the view's post-process settings, as the tonemap's do.
                EXPECT_TRUE(pass.value("ppOverrides", false));
                EXPECT_TRUE(pass["pushConstants"].contains("exposure"));
                EXPECT_TRUE(pass["pushConstants"].contains("useAutoExposure"));
            }
            if (id == "Sky")
                sky = index;
            ++index;
        }
        ASSERT_GE(snapshot, 0);
        EXPECT_LT(snapshot, sky) << "the snapshot must precede the first scene colour writer";
        EXPECT_TRUE(graph["resources"]["SceneColor"].value("preExposed", false));
    }
}

TEST(PreExposureSourceContract, EveryFullscreenPassOnAPreExposureShaderBindsTheExposureHistory)
{
    const std::vector<std::string> shaders = {
        "Shaders/atmospheric_cloud_layer.shaderpkg", "Shaders/volumetric_clouds_composite.shaderpkg",
        "Shaders/fog_glow_fast.shaderpkg",
        "Shaders/fog_glow_prefilter.shaderpkg",      "Shaders/fog_glow_downsample.shaderpkg",
        "Shaders/bloom_threshold.shaderpkg",         "Shaders/halation_prefilter.shaderpkg",
        "Shaders/hdr_color_fx.shaderpkg",            "Shaders/tonemap.shaderpkg",
        "Shaders/forwardplus_lighting.shaderpkg",
    };
    const nlohmann::json graph = ReadGraph("Engine/Assets/RenderPipelines/ForwardPlus.rendergraph");
    int bound = 0;
    for (const auto& pass : graph["passes"])
    {
        const std::string shader = pass.value("shaderPkg", std::string());
        if (std::find(shaders.begin(), shaders.end(), shader) == shaders.end())
            continue;
        SCOPED_TRACE(pass.value("id", std::string()));
        ASSERT_TRUE(pass.contains("buffers"));
        EXPECT_EQ(pass["buffers"]["uExposure"], "ExposureHistory");
        EXPECT_EQ(pass["bufferUsages"]["uExposure"], "Storage");
        ++bound;
    }
    EXPECT_EQ(bound, 17) << "fog glow fast and prefilter, 7 fog glow downsamples, 2 bloom thresholds, halation, "
                            "colour FX, tonemap, cloud layer, clouds composite, forward+ lighting";
}

TEST(PreExposureSourceContract, TheExposureHistoryLayoutIsDeclaredOnce)
{
    // Every pass binds the block through Includes/exposure_history.glsl; a copy of the layout drifts
    // when the state changes (ExposureReadbackFeature::State mirrors it on the CPU).
    const std::filesystem::path root(GE_RENDERER_REPO_ROOT);
    const std::filesystem::path owner = root / "Engine/Modules/Rendering/Shaders/Includes/exposure_history.glsl";
    std::vector<std::string> copies;
    for (const char* dir : {"Engine/Modules", "Packages"})
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root / dir))
        {
            const std::string ext = entry.path().extension().string();
            if (!entry.is_regular_file() || (ext != ".glsl" && ext != ".frag" && ext != ".comp" && ext != ".vert"))
                continue;
            if (std::filesystem::equivalent(entry.path(), owner))
                continue;
            std::ifstream in(entry.path());
            std::stringstream text;
            text << in.rdbuf();
            if (text.str().find("float previousPreExposure;") != std::string::npos)
                copies.push_back(entry.path().string());
        }
    }
    EXPECT_TRUE(copies.empty()) << copies.size() << " shader(s) copy the ExposureHistory layout, first: "
                                << (copies.empty() ? std::string() : copies.front());
}

// Every shading variant of the forward adapter, unlit particles included, stores its colour at P in
// the fragment stage, where it writes scene colour. The unlit particles' vertex factor is the
// display-referred 1/exposure only: P applied there as well would store their colour at P squared.
TEST(PreExposureSourceContract, TheForwardAdapterStoresEveryShadingVariantAtThePreExposure)
{
    const std::string adapter = ReadSource("Engine/Modules/Rendering/Shaders/Adapters/adapter_forward.glsl");
    EXPECT_NE(adapter.find("oColor = vec4(finalColor * GE_PreExposure(), alpha);"), std::string::npos)
        << "the forward adapter multiplies its colour by P where it writes it";
    const std::string particles = ReadSource("Engine/Modules/Rendering/Shaders/Particles/particle_vertex.glsl");
    const size_t factor = particles.find("v.particleColorScale = ");
    ASSERT_NE(factor, std::string::npos);
    const std::string line = particles.substr(factor, particles.find(';', factor) - factor);
    EXPECT_EQ(line.find("GE_PreExposure"), std::string::npos) << "the vertex factor stays 1/exposure: " << line;
}

// The double-scale hazard: a scene-colour grab is stored at P, and the forward adapter multiplies its
// output by P. A grab folded into the shading must be read back to absolute first (glass
// transmission, ocean refraction), and a grab passed straight through must bypass the multiply (the
// glass hidden behind an opaque occluder); either mistake stores the background at P squared.
TEST(PreExposureSourceContract, SceneColourGrabsAreScaledByThePreExposureOnce)
{
    const std::string ibl = ReadSource("Engine/Modules/Rendering/Shaders/Includes/ibl.glsl");
    EXPECT_TRUE(ContainsTokens(ibl, "textureLod(ge_sceneColor, clamp(ssUV, 0.0, 1.0), 0.0).rgb / GE_PreExposure()"))
        << "glass transmission reads its grab back to absolute";
    EXPECT_TRUE(ContainsTokens(ibl, "textureLod(ge_sceneColor, uvE, 0.0).rgb / GE_PreExposure()"))
        << "the transmission's edge sample reads its grab back to absolute";
    const std::string ocean = ReadSource("Engine/Modules/Rendering/Shaders/Ocean/ocean_surface.glsl");
    EXPECT_TRUE(ContainsTokens(ocean, ": texture(uOceanSceneColor, grabUV).rgb) / GE_PreExposure();"))
        << "ocean refraction reads its grab back to absolute";
    const std::string adapter = ReadSource("Engine/Modules/Rendering/Shaders/Adapters/adapter_forward.glsl");
    EXPECT_TRUE(ContainsTokens(
        adapter, "oColor = vec4(texelFetch(ge_sceneColor, ivec2(gl_FragCoord.xy), 0).rgb, alpha); return;"))
        << "an occluded glass fragment passes the grab through, bypassing the multiply by P";
}

// The night tint reads the scene, not the exposure (Includes/night_tint.glsl): under Auto exposure its
// gate takes the meter's reading (before the clamps and the curve) and its dark field takes the pixel's
// stored luminance with the pre-exposure divided out. The helpers are probed on the device
// (NightTintTests); this pins the tonemap's arguments to them.
TEST(PreExposureSourceContract, TheTonemapGivesTheTintTheMetersReadingAndTheScenesLuminance)
{
    const std::string tonemap = ReadSource("Engine/Modules/Rendering/Shaders/tonemap.frag");
    EXPECT_TRUE(ContainsTokens(tonemap, "float meteredNits = pc.useAutoExposure != 0 ? GE_MeteredNits() : -1.0;"))
        << "under Auto exposure the gate must read the meter's reading";
    EXPECT_TRUE(ContainsTokens(tonemap,
                               "float adaptationNits = GE_NightTintAdaptationNits(meteredNits, exposure * GE_PreExposure());"))
        << "the gate's fallback is the luminance the applied exposure maps to mid-grey";
    EXPECT_TRUE(ContainsTokens(tonemap, "float sceneNits = GE_NightTintSceneNits(dot(storedColor, kY), GE_PreExposure());"))
        << "the dark field reads the stored luminance with the pre-exposure divided out";
    EXPECT_TRUE(ContainsTokens(tonemap, "lin = ApplyNightTint(lin, adaptationNits, uv, stored);"))
        << "the tint takes the stored colour, not the exposed one";
}
