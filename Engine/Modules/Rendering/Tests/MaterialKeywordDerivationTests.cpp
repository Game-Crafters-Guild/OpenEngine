// The invariant these pin: a variant key carries EXACTLY ONE vertex-modifier keyword, chosen by
// reading the resolved modifier file, for both shipped forms. The two defines select mutually
// exclusive adapter paths, so a key holding both names a variant no derivation produces.
// Pipeline keywords can arrive carrying the other form; the derivation normalises them rather
// than merging on top. A reference that resolves to nothing — including one whose build context
// is empty — still yields a keyword, so "a modifier implies one of the two bits" holds for the
// depth classifiers and the extraction path that read them.

#include <gtest/gtest.h>

#include "Rendering/Materials/MaterialKeywordDerivation.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifndef EZTREE_PACKAGE_SHADERS_DIR
#error "EZTREE_PACKAGE_SHADERS_DIR must be defined by CMake (target_compile_definitions)"
#endif
#ifndef RENDERING_SOURCE_DIR
#error "RENDERING_SOURCE_DIR must be defined by CMake (rendering_test_shader_paths)"
#endif
#ifndef TERRAIN_GRASS_SHADER_SOURCE_DIR
#error "TERRAIN_GRASS_SHADER_SOURCE_DIR must be defined by CMake (target_compile_definitions)"
#endif

namespace
{
namespace fs = std::filesystem;

using GameEngine::Rendering::ApplyParallaxKeyword;
using GameEngine::Rendering::ApplyVertexModifierKeyword;
using GameEngine::Rendering::GenerateDefines;
using GameEngine::Rendering::HasKeyword;
using GameEngine::Rendering::MaterialBuildContext;
using GameEngine::Rendering::MaterialKeyword;
using GameEngine::Rendering::ShaderVariantKey;

// TerrainGrassRenderFeature references its modifier as "TerrainGrass/<file>", so the root it
// resolves against is the PARENT of the grass shader directory.
fs::path GrassShaderRoot()
{
    return fs::path(TERRAIN_GRASS_SHADER_SOURCE_DIR).parent_path();
}

// Both shipped modifiers live off a shader root rather than beside a .material, which is the
// case a materialDir-only resolve would miss.
MaterialBuildContext ShippedRootsContext()
{
    MaterialBuildContext ctx{};
    ctx.PackageShaderDirs = {fs::path(EZTREE_PACKAGE_SHADERS_DIR), GrassShaderRoot()};
    return ctx;
}

// The invariant the whole change exists to enforce.
::testing::AssertionResult HasExactlyOneModifierKeyword(const ShaderVariantKey& key)
{
    const bool simple = HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexMod);
    const bool extended = HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexOutputMod);
    if (simple && extended)
        return ::testing::AssertionFailure()
               << "both HasVertexMod and HasVertexOutputMod are set: the variant would compile "
                  "with HAS_VERTEX_MODIFIER and HAS_VERTEX_OUTPUT_MODIFIER at once";
    if (!simple && !extended)
        return ::testing::AssertionFailure() << "neither modifier keyword is set";
    return ::testing::AssertionSuccess();
}
} // namespace

// The shipped grass modifier: `void ModifyVertex(inout VertexOutput, InstanceData)`.
TEST(MaterialKeywordDerivation, ShippedGrassModifierDerivesExtendedFormAlone)
{
    ASSERT_TRUE(fs::exists(GrassShaderRoot() / "TerrainGrass" / "terrain_grass_vertex_modifier.glsl"))
        << "fixture missing under " << GrassShaderRoot().string();

    ShaderVariantKey key{};
    ApplyVertexModifierKeyword(key, "TerrainGrass/terrain_grass_vertex_modifier.glsl",
                               /*materialDir=*/{}, ShippedRootsContext());

    EXPECT_TRUE(HasExactlyOneModifierKeyword(key));
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexOutputMod));
}

// The shipped EZ Tree wind modifier: `vec3 ModifyVertex(vec3, InstanceData)`. It also reads
// Mat.uParams1/uParams2, which only the simple form's MaterialParams SSBO declaration provides.
TEST(MaterialKeywordDerivation, ShippedEZTreeWindModifierDerivesSimpleFormAlone)
{
    ASSERT_TRUE(fs::exists(fs::path(EZTREE_PACKAGE_SHADERS_DIR) / "VertexModifiers" /
                           "ez_tree_wind.glsl"))
        << "fixture missing under " << EZTREE_PACKAGE_SHADERS_DIR;

    ShaderVariantKey key{};
    ApplyVertexModifierKeyword(key, "VertexModifiers/ez_tree_wind.glsl",
                               /*materialDir=*/{}, ShippedRootsContext());

    EXPECT_TRUE(HasExactlyOneModifierKeyword(key));
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexMod));
}

// The co-emit reached the compiler because pipeline keywords carried the other form in.
// The derivation is the authority, so it must clear rather than merge.
TEST(MaterialKeywordDerivation, ClearsAStaleFormCarriedInByPipelineKeywords)
{
    ShaderVariantKey key{};
    key.materialKeywords = MaterialKeyword::ForwardPlus | MaterialKeyword::HasVertexMod;

    ApplyVertexModifierKeyword(key, "TerrainGrass/terrain_grass_vertex_modifier.glsl",
                               /*materialDir=*/{}, ShippedRootsContext());

    EXPECT_TRUE(HasExactlyOneModifierKeyword(key));
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexOutputMod));
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::ForwardPlus))
        << "unrelated pipeline keywords must survive";
}

// A material that dropped its modifier must not keep the keyword from the key it is rebuilt over.
TEST(MaterialKeywordDerivation, EmptyReferenceClearsBothForms)
{
    ShaderVariantKey key{};
    key.materialKeywords = MaterialKeyword::AlphaTest | MaterialKeyword::HasVertexOutputMod;

    ApplyVertexModifierKeyword(key, /*vertexModifierRef=*/"", /*materialDir=*/{},
                               ShippedRootsContext());

    EXPECT_FALSE(HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexMod));
    EXPECT_FALSE(HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexOutputMod));
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::AlphaTest));
}

// An unresolvable reference takes the simple form — the behaviour MaterialBuildService's
// detector fall-through already had. It must stay ONE form, not none and not both.
TEST(MaterialKeywordDerivation, UnresolvableReferenceTakesTheSimpleForm)
{
    ShaderVariantKey key{};
    ApplyVertexModifierKeyword(key, "NoSuchDir/no_such_modifier.glsl", /*materialDir=*/{},
                               ShippedRootsContext());

    EXPECT_TRUE(HasExactlyOneModifierKeyword(key));
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexMod));
}

// A cold build context (no Engine instance — a unit-test harness) resolves nothing, but a
// document that names a modifier must still come out carrying one. MaterialDepthClassify,
// DepthDrawRecorder and RenderExtractionSystem all branch on
// HasVertexMod || HasVertexOutputMod, so a key with neither would classify a modifier
// material as unmodified geometry.
TEST(MaterialKeywordDerivation, EmptyBuildContextStillYieldsAModifierKeyword)
{
    ShaderVariantKey key{};
    ApplyVertexModifierKeyword(key, "TerrainGrass/terrain_grass_vertex_modifier.glsl",
                               /*materialDir=*/{}, MaterialBuildContext{});

    EXPECT_TRUE(HasExactlyOneModifierKeyword(key));
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexMod));
}

// ---- Parallax: derived from a bound height map on a surface that declares one ----
//
// The keyword is a property of the document's binding AND of the surface's declared `@texture` set.
// The derivation is pure; its callers own the surface read, so these tests hand it the declared
// slots of the shipped surfaces themselves.

namespace
{
using GameEngine::Rendering::DescribeParallaxRefusal;
using GameEngine::Rendering::ParallaxRefusal;
using GameEngine::Rendering::ShaderComposer;
using GameEngine::Rendering::TextureSlotResolution;

TextureSlotResolution ShippedSurfaceSlots(const char* surfaceFile)
{
    const fs::path path = fs::path(RENDERING_SOURCE_DIR) / "Shaders" / "Surfaces" / surfaceFile;
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "cannot read " << path.string();
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return ShaderComposer::ResolveTextureSlots(source);
}

GameEngine::MaterialDocument HeightMappedDocument()
{
    GameEngine::MaterialDocument doc{};
    doc.materialName = "Relief";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.textures["heightMap"] = "__embedded__:0";
    return doc;
}
} // namespace

TEST(MaterialKeywordDerivation, BoundHeightMapOnTheStandardSurfaceDerivesParallax)
{
    const TextureSlotResolution slots = ShippedSurfaceSlots("standard_pbr.glsl");
    ShaderVariantKey key{};
    EXPECT_EQ(ApplyParallaxKeyword(key, HeightMappedDocument(), &slots.DeclaredSlots), ParallaxRefusal::None);
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::Parallax));
    const auto defines = GenerateDefines(key, "StandardPBR");
    EXPECT_NE(std::find(defines.begin(), defines.end(), "GE_PARALLAX_ENABLED"), defines.end());
}

TEST(MaterialKeywordDerivation, UnboundHeightMapClearsParallax)
{
    // The derivation is the authority: a key rebuilt over one that carried the bit loses it, for
    // a slot left empty and for no slot at all.
    const TextureSlotResolution slots = ShippedSurfaceSlots("standard_pbr.glsl");
    auto emptySlot = HeightMappedDocument();
    emptySlot.textures["heightMap"] = "";
    auto noSlot = HeightMappedDocument();
    noSlot.textures.erase("heightMap");
    for (const GameEngine::MaterialDocument* doc : {&emptySlot, &noSlot})
    {
        ShaderVariantKey key{};
        key.materialKeywords = MaterialKeyword::Parallax | MaterialKeyword::ForwardPlus;
        EXPECT_EQ(ApplyParallaxKeyword(key, *doc, &slots.DeclaredSlots), ParallaxRefusal::None);
        EXPECT_FALSE(HasKeyword(key.materialKeywords, MaterialKeyword::Parallax));
        EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::ForwardPlus));
    }
}

TEST(MaterialKeywordDerivation, SurfaceWithoutAHeightSlotIsRefusedNamingTheSurface)
{
    const TextureSlotResolution slots = ShippedSurfaceSlots("triplanar_pbr.glsl");
    ASSERT_FALSE(slots.Rejected);
    ShaderVariantKey key{};
    const ParallaxRefusal refusal = ApplyParallaxKeyword(key, HeightMappedDocument(), &slots.DeclaredSlots);
    EXPECT_EQ(refusal, ParallaxRefusal::SurfaceHasNoHeightMap);
    EXPECT_FALSE(HasKeyword(key.materialKeywords, MaterialKeyword::Parallax));
    EXPECT_EQ(DescribeParallaxRefusal(refusal, "Surfaces/triplanar_pbr"),
              "Surfaces/triplanar_pbr does not use a Height map; bind it on a Standard PBR material");
}

TEST(MaterialKeywordDerivation, HexTilingWithAHeightMapIsRefused)
{
    const TextureSlotResolution slots = ShippedSurfaceSlots("standard_pbr.glsl");
    for (const GameEngine::MaterialValue& hexOn :
         {GameEngine::MaterialValue{1.0f}, GameEngine::MaterialValue{true}, GameEngine::MaterialValue{int32_t{1}}})
    {
        auto doc = HeightMappedDocument();
        doc.properties["hexTiling"] = hexOn;
        ShaderVariantKey key{};
        const ParallaxRefusal refusal = ApplyParallaxKeyword(key, doc, &slots.DeclaredSlots);
        EXPECT_EQ(refusal, ParallaxRefusal::HexTiling);
        EXPECT_FALSE(HasKeyword(key.materialKeywords, MaterialKeyword::Parallax));
        EXPECT_EQ(DescribeParallaxRefusal(refusal, "Surfaces/standard_pbr"),
                  "Parallax cannot combine with hex tiling: turn Hex Tiling off, or remove the Height map");
    }
    auto hexOff = HeightMappedDocument();
    hexOff.properties["hexTiling"] = 0.0f;
    ShaderVariantKey key{};
    EXPECT_EQ(ApplyParallaxKeyword(key, hexOff, &slots.DeclaredSlots), ParallaxRefusal::None);
    EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::Parallax));
}

TEST(MaterialKeywordDerivation, UnresolvedSurfaceRefusesNothing)
{
    // The compose reports an unresolved surface by name; a height-map refusal here would name the
    // wrong cause.
    ShaderVariantKey key{};
    key.materialKeywords = MaterialKeyword::Parallax;
    EXPECT_EQ(ApplyParallaxKeyword(key, HeightMappedDocument(), nullptr), ParallaxRefusal::None);
    EXPECT_FALSE(HasKeyword(key.materialKeywords, MaterialKeyword::Parallax));
    EXPECT_TRUE(DescribeParallaxRefusal(ParallaxRefusal::None, "Surfaces/standard_pbr").empty());
}

TEST(MaterialKeywordDerivation, TheStepsViewReachesOnlyAMaterialThatMarches)
{
    // Turning the debug view on recompiles the parallax variants alone: every other material keeps
    // the variant it already draws, and every other pass keyword passes through untouched.
    using GameEngine::Rendering::NarrowColorPassKeywords;
    const MaterialKeyword pass = MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::Instanced |
                                 MaterialKeyword::GTAO | MaterialKeyword::ParallaxStepsView;
    const MaterialKeyword withoutView = pass & ~MaterialKeyword::ParallaxStepsView;
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::Parallax, pass), pass);
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::Parallax | MaterialKeyword::AlphaTest, pass), pass);
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::None, pass), withoutView);
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::ClearCoat | MaterialKeyword::AlphaTest, pass), withoutView);
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::None, withoutView), withoutView);
}

TEST(MaterialKeywordDerivation, TheReliefDepthReachesOnlyAMarchingMaterialAndNeverTheCompatProfile)
{
    // The world pass adds the relief's depth to every entity draw; only a parallax material keeps it,
    // and a compatibility-profile key never carries it (WGSL has no conservative depth).
    using GameEngine::Rendering::NarrowColorPassKeywords;
    const MaterialKeyword relief = MaterialKeyword::ParallaxDepthOffset | MaterialKeyword::ParallaxDepthFromPrepass |
                                   MaterialKeyword::ParallaxPrepassDepthMultisample;
    const MaterialKeyword pass = MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::Instanced;
    const bool previousCompat = GameEngine::Rendering::IsCompatShaderProfile();
    GameEngine::Rendering::SetCompatShaderProfile(false);
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::Parallax, pass | relief), pass | relief);
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::None, pass | relief), pass);
    GameEngine::Rendering::SetCompatShaderProfile(true);
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::Parallax, pass | relief), pass);
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::Parallax, pass | MaterialKeyword::ParallaxStepsView),
              pass | MaterialKeyword::ParallaxStepsView)
        << "the compat arm still draws the steps view";
    GameEngine::Rendering::SetCompatShaderProfile(previousCompat);
}

TEST(MaterialKeywordDerivation, TheStepsViewKeywordHasItsOwnDefine)
{
    ShaderVariantKey key{};
    key.lightingModel = GameEngine::Rendering::LightingModel::kStandardPBR;
    key.materialKeywords = MaterialKeyword::Parallax | MaterialKeyword::ParallaxStepsView;
    const auto defines = GenerateDefines(key, "StandardPBR");
    EXPECT_NE(std::find(defines.begin(), defines.end(), "GE_PARALLAX_STEPS_VIEW_ENABLED"), defines.end());
    key.materialKeywords = MaterialKeyword::Parallax;
    const auto plain = GenerateDefines(key, "StandardPBR");
    EXPECT_EQ(std::find(plain.begin(), plain.end(), "GE_PARALLAX_STEPS_VIEW_ENABLED"), plain.end());
}

namespace
{
// The blend keyword the fog reads for one colour equation, by the rule the fog applies: straight alpha
// over takes none (c T + S), premultiplied over takes PremultipliedBlend (c T + S a), anything added onto
// the destination AdditiveBlend (c T), the two multiplies MultiplyBlend (mix(1, c, T)) and every other
// equation UnfoggedBlend (no identity colour to fade toward).
MaterialKeyword ExpectedBlendKeyword(GameEngine::MaterialBlendOp op, GameEngine::MaterialBlendFactor src,
                                     GameEngine::MaterialBlendFactor dst)
{
    using GameEngine::MaterialBlendFactor;
    using GameEngine::MaterialBlendOp;
    if (op != MaterialBlendOp::Add)
        return MaterialKeyword::UnfoggedBlend;
    if (dst == MaterialBlendFactor::OneMinusSrcAlpha && src == MaterialBlendFactor::SrcAlpha)
        return MaterialKeyword::None;
    if (dst == MaterialBlendFactor::OneMinusSrcAlpha && src == MaterialBlendFactor::One)
        return MaterialKeyword::PremultipliedBlend;
    if (dst == MaterialBlendFactor::One)
        return MaterialKeyword::AdditiveBlend;
    if ((src == MaterialBlendFactor::DstColor && dst == MaterialBlendFactor::Zero) ||
        (src == MaterialBlendFactor::Zero && dst == MaterialBlendFactor::SrcColor))
        return MaterialKeyword::MultiplyBlend;
    return MaterialKeyword::UnfoggedBlend;
}
} // namespace

// Every colour blend equation a material can author reaches exactly the blend keyword the fog's rule
// gives it, and only a Blend material carries one: an opaque or alpha-tested surface hides what is
// behind it whatever blend its document names.
TEST(MaterialKeywordDerivation, EveryBlendTakesTheFogWeightOfItsComposite)
{
    using GameEngine::MaterialAlphaMode;
    using GameEngine::MaterialBlendFactor;
    using GameEngine::MaterialBlendOp;
    using GameEngine::MaterialBlendState;
    using GameEngine::Rendering::ApplyAlphaModeKeywords;
    constexpr MaterialKeyword kBlendKeywords = MaterialKeyword::PremultipliedBlend | MaterialKeyword::AdditiveBlend |
                                               MaterialKeyword::MultiplyBlend | MaterialKeyword::UnfoggedBlend;
    size_t equations = 0;
    for (int op = 0; op <= static_cast<int>(MaterialBlendOp::Max); ++op)
        for (int src = 0; src <= static_cast<int>(MaterialBlendFactor::AlphaSaturate); ++src)
            for (int dst = 0; dst <= static_cast<int>(MaterialBlendFactor::AlphaSaturate); ++dst)
            {
                MaterialBlendState blend{};
                blend.ColorOp = static_cast<MaterialBlendOp>(op);
                blend.SrcColorFactor = static_cast<MaterialBlendFactor>(src);
                blend.DstColorFactor = static_cast<MaterialBlendFactor>(dst);
                SCOPED_TRACE("op " + std::to_string(op) + " src " + std::to_string(src) + " dst " + std::to_string(dst));
                ShaderVariantKey key{};
                ApplyAlphaModeKeywords(key, MaterialAlphaMode::Blend, blend);
                EXPECT_EQ(key.materialKeywords & kBlendKeywords,
                          ExpectedBlendKeyword(blend.ColorOp, blend.SrcColorFactor, blend.DstColorFactor));
                EXPECT_TRUE(GameEngine::Rendering::HasKeyword(key.materialKeywords, MaterialKeyword::AlphaBlend));
                for (MaterialAlphaMode covering : {MaterialAlphaMode::Opaque, MaterialAlphaMode::Mask})
                {
                    ShaderVariantKey opaque{};
                    ApplyAlphaModeKeywords(opaque, covering, blend);
                    EXPECT_EQ(opaque.materialKeywords & kBlendKeywords, MaterialKeyword::None);
                }
                ++equations;
            }
    EXPECT_EQ(equations, 5u * 15u * 15u);
    // An unauthored blend is the default straight alpha over.
    ShaderVariantKey unauthored{};
    ApplyAlphaModeKeywords(unauthored, MaterialAlphaMode::Blend, std::nullopt);
    EXPECT_EQ(unauthored.materialKeywords & kBlendKeywords, MaterialKeyword::None);
}

// The volumetric fog by a surface's own distance reaches the blended and the transmissive materials;
// every other surface takes it from the composite. The clouds in front of a far surface reach the
// blended materials. The height fog reaches every colour variant.
TEST(MaterialKeywordDerivation, TheVolumetricPassKeywordsReachTheSurfacesDrawnAfterTheirComposites)
{
    using GameEngine::Rendering::NarrowColorPassKeywords;
    const MaterialKeyword pass = MaterialKeyword::ForwardPlus | MaterialKeyword::HeightFog |
                                 MaterialKeyword::VolumetricFog | MaterialKeyword::VolumetricClouds;
    const MaterialKeyword opaquePass = MaterialKeyword::ForwardPlus | MaterialKeyword::HeightFog;
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::AlphaBlend, pass), pass);
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::Transmission, pass), opaquePass | MaterialKeyword::VolumetricFog)
        << "the clouds in front reach blended surfaces only";
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::None, pass), opaquePass);
    EXPECT_EQ(NarrowColorPassKeywords(MaterialKeyword::AlphaTest, pass), opaquePass);
}
