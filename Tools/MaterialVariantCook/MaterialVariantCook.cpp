// MaterialVariantCook — offline compilation of a project's material shader
// variants into a relocatable shader cache.
//
// A runtime without a shader compiler (wasm) can still bring materials up: it
// composes the same source and addresses the same content-derived cache key, so
// every variant this tool wrote is served straight from the cache. What it
// cannot do is compile a variant nobody cooked — so what is enumerated here is
// exactly what that runtime can draw.
//
//     MaterialVariantCook --project <dir> --engine-shaders <dir> [--cache <dir>]
//                         [--package-shaders <dir>]...
//                         [--scan <dir> [--scan-alias <alias>]]... [--verify]
//                         [--web --shadercook <shadercook.py>]
//                         [--particle-row <variant>] [--jobs <count>]
//                         [--reuse-cache <dir>]
//
// The cache root defaults to <project>/.Cache/Shaders — the location the
// runtime probes — so the cooked tree ships by copying the project.
//
// --scan adds another root's materials. A graph material's generated surface
// is named "<alias>/<path under its root>", which the runtime derives from its
// mount table, so a scan root's alias must be the one the runtime mounts it
// under. A root that a package manifest names as its assets folder takes that
// package's alias, derived as the package resolver derives it; --scan-alias
// names any other root. Aliases are spelled and checked as
// AssetManager::RegisterSource spells and checks them. The --project root is
// "project".
//
// --reuse-cache names a shader cache the editor compiled into. A variant whose
// program it holds, still valid as the runtime validates a cache entry, is
// copied into the cook's cache instead of compiled; the cache is
// content-addressed, so the same key is the same program in any root.
//
// A desktop cook also compiles the engine's compute programs that the runtime
// builds from the engine shader tree through the same compile service (the
// terrain grass placement's), reported on the same "ok" lines, so an export
// ships them with the materials.
//
// Beside the programs, the cook writes the cache's program index
// (ShaderProgramIndex, program-index.bin): each cooked variant's build inputs
// mapped to its program, so the runtime that ships the cache loads a variant's
// program without composing its source or hashing its include closure. Every
// run rewrites it from that run's variants.
//
// --verify re-resolves every cooked variant with the compiler switched off,
// which is the same code path the shipped runtime takes, and checks the program
// index serves every variant. A variant that only resolves with a compiler
// present is a cook that did not actually land.
//
// --web cooks the WebGPU-class profile: the compat shader profile (no
// buffer_device_address, no descriptor indexing — the two things naga refuses
// in the desktop material SPIR-V) plus a "<stage>-wgsl" chunk per variant, so
// a browser that ingests no SPIR-V still gets a pipeline. The compat profile is
// a compile input, so a web cache and a desktop cache address different keys
// and can share a cache root without colliding.
//
// --particle-row cooks only the named row of the particle request set
// (Particles::ParticleRenderVariants) for each particle shape instead of the
// whole set: a check that every shape composes and translates, at a fraction
// of the cost. A cache cooked with it is incomplete for particles; an export
// cooks the whole set.
//
// --jobs bounds how many variants compile at once and, under --web, how many
// WGSL translations run at once; the default is one per hardware thread. The
// report lists the variants in the same order whatever the count, because the
// export reads it.

#include "AssetCore/PathNormalization.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetSourceAlias.h"
#include "Assets/MaterialAsset.h"
#include "Assets/Packages/PackageManifest.h"
#include "Assets/Packages/PackageResolver.h"
#include "Engine/Rendering/MaterialPrewarmVariants.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialShaderPackageBuilder.h"
#include "Particles/Rendering/ParticleMaterials.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RendererProfile.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/MaterialKeywordDerivation.h"
#include "Rendering/Materials/MaterialShaderIncludeRoots.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/ShaderCache/ShaderProgramIndex.h"
#include "CBTTerrainECS/CBTTerrainMaterial.h"
#include "EZTreeECS/EZTreeRuntimeMaterials.h"
#include "TerrainGrass/GrassComputePrograms.h"
#include "TerrainGrass/GrassDrawMode.h"
#include "TerrainGrass/GrassMaterialKeywords.h"
#include "Ocean/OceanSurfaceMaterial.h"
#include "Platform/Process.h"
#include "CookWorkers.h"
#include "WebWgslCook.h"
#include "MeshVariantTable.h"
#include "VariantRequests.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// One shader variant of one material: the pass keywords the renderer will ask
// for, and the vertex layout the mesh feeding it carries.
using GameEngine::Tools::MaterialCook::VariantRequest;
using GameEngine::Tools::MaterialCook::ImportedMeshMaterialTarget;
using GameEngine::Tools::MaterialCook::kDepthMaskRows;
using GameEngine::Tools::MaterialCook::kImportedMeshMaterialTargets;
using GameEngine::Tools::MaterialCook::kForwardColor;
using GameEngine::Tools::MaterialCook::kForwardColorIbl;
using GameEngine::Tools::MaterialCook::kMeshVariants;
using GameEngine::Tools::MaterialCook::kPerViewWorldPassKeywords;

// A --scan root, the --scan-alias given for it, and the alias its generated
// surfaces are named under once ResolveScanAlias has settled it.
struct ScanRoot
{
    fs::path Path;
    std::optional<std::string> GivenAlias;
    std::string Alias;
};

struct Options
{
    fs::path Project;
    fs::path EngineShaders;
    fs::path Cache;
    std::vector<fs::path> PackageShaders;
    std::vector<ScanRoot> ScanRoots;
    fs::path ShaderCookScript;
    bool Verify = false;
    bool Web = false;
    // How many variants compile, and under --web translate, at once; 0 = one per hardware thread.
    size_t Jobs = 0;
    // The one particle request-set row each particle shape cooks under --particle-row.
    std::optional<VariantRequest> ParticleRow;
    // A shader cache whose programs the cook takes instead of compiling them again.
    std::optional<fs::path> ReuseCache;
};

// One material the cook is responsible for. A project's .material assets are
// the obvious members; the engine's built-in default PBR document is the
// non-obvious one — every primitive and every model without an assigned
// material draws with it, so a cache without its variants renders nothing even
// though the project looks fully cooked. A target may carry its own variant
// list (built-in contributor materials request one specific keyword set at
// registration, not the mesh-material table); empty means kMeshVariants. Rows are
// expanded with the optional pass keywords (contact shadows) unless the target
// lists exactly what its pass draws with (`ExpandsOptionalPasses` false).
// `ImportedMeshMaterial` marks the documents an imported model's materials
// compose as (the built-in default PBR family): the only ones a mesh with a
// COLOR_0 stream reaches on a cooked-only runtime, so the only ones that cook
// the vertex-colour rows. `Prewarmed` marks a material the runtime prewarms on
// registration (a project's .material and the imported mesh materials): a
// desktop cook adds every key that prewarm asks for.
struct CookSource
{
    std::string Name;
    fs::path MaterialPath;
    MaterialDocument Document;
    std::vector<VariantRequest> Variants;
    bool ImportedMeshMaterial = false;
    bool ExpandsOptionalPasses = true;
    bool Prewarmed = false;
};

// Preparation precedes variant enumeration because graph tags shape the document.
struct CookTarget
{
    std::string Name;
    fs::path MaterialPath;
    Engine::Renderer::MaterialShaderPackageBuilder Builder;
    std::vector<VariantRequest> Variants;
    bool ImportedMeshMaterial = false;
    bool ExpandsOptionalPasses = true;
    bool Prewarmed = false;
};

// The world passes a cooked material's colour pass is drawn with: those the shipped render graphs draw
// with, as the runtime's prewarm resolves them (RenderServices::ResolveWorldPassKeywordsForPrewarm):
// ForwardPlus.rendergraph's (with IBL), and Web.rendergraph's, which is also what prewarm asks for
// before a graph is applied; and every world pass `rows` declares (its ForwardPlus rows), which adds
// the per-view keywords a volume or view setting turns on (GTAO, SSSR, DDGI, contact shadows). The
// colour pass draws a crossfading tail under any of them (it adds LodCrossfade to the view's
// keywords per segment). `rows` are the declared rows before the relief, grab and material keyword
// expansions, so each ForwardPlus row's keywords are a world pass's.
std::vector<MaterialKeyword> PrewarmedWorldPasses(const std::vector<VariantRequest>& rows)
{
    std::vector<MaterialKeyword> worldPasses = {kForwardColor, kForwardColorIbl};
    for (const VariantRequest& row : rows)
        if (HasKeyword(row.PassKeywords, MaterialKeyword::ForwardPlus) &&
            std::find(worldPasses.begin(), worldPasses.end(), row.PassKeywords) == worldPasses.end())
            worldPasses.push_back(row.PassKeywords);
    return worldPasses;
}

// The registration key the runtime prewarms `target` from (MaterialSystem::DeriveRegistrationVariantKey):
// the document's own keywords and the vertex modifier's form, on the standard mesh layout. Registration
// gives a graph surface that reads tangents the tangent layout; the build composes the tangent layout
// for such a surface whatever layout it is asked for, so the standard layout names the same programs.
// The relief march is taken from the height-map binding alone; the build narrows it away from a
// surface that does not march.
ShaderVariantKey PrewarmBaseKey(const CookTarget& target, const MaterialBuildContext& context)
{
    const MaterialDocument& document = target.Builder.GetDocument();
    ShaderVariantKey key = Engine::Renderer::DeriveBaseVariantKey(document);
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    ApplyVertexModifierKeyword(key, document.vertexModifier, target.MaterialPath.parent_path(), context);
    const auto heightMap = document.textures.find(kParallaxHeightMapSlot);
    if (heightMap != document.textures.end() && !heightMap->second.empty())
        key.materialKeywords |= MaterialKeyword::Parallax;
    if (document.customVertexShader)
        ApplyCustomVertexShaderClamp(key);
    return key;
}

// Adds every key the runtime's prewarm of `target` asks for (MaterialPrewarmVariantKeys) under each
// of `worldPasses` (PrewarmedWorldPasses) that `rows` lacks: the LOD-crossfade tails of the colour,
// asset preview and camera-prepass draws, the depth heads, and the registration's own layout. The
// rule stays in MaterialPrewarmVariantKeys; the table declares the per-view world passes it is applied
// under. A world pass with a per-view keyword gets no scene-colour-grab key: the transmissive pass
// never draws with one (kPerViewWorldPassKeywords).
void AddPrewarmVariants(std::vector<VariantRequest>& rows, const CookTarget& target,
                        const MaterialBuildContext& context, const std::vector<MaterialKeyword>& worldPasses)
{
    const MaterialDocument& document = target.Builder.GetDocument();
    const ShaderVariantKey base = PrewarmBaseKey(target, context);
    for (const MaterialKeyword worldPass : worldPasses)
    {
        const bool perView = (worldPass & kPerViewWorldPassKeywords) != MaterialKeyword::None;
        for (const ShaderVariantKey& key : Engine::Renderer::MaterialPrewarmVariantKeys(
                 base, worldPass, document.customVertexShader, Engine::Renderer::DeriveRenderedAlphaMode(document),
                 document.ignoreVertexColor))
        {
            if (perView && HasKeyword(key.materialKeywords, MaterialKeyword::SceneColorGrab))
                continue;
            if (std::any_of(rows.begin(), rows.end(), [&](const VariantRequest& row)
                            { return row.PassKeywords == key.materialKeywords && row.VertexFlags == key.vertexFlags; }))
                continue;
            char name[64];
            std::snprintf(name, sizeof(name), "prewarm-0x%llx+layout-0x%x",
                          static_cast<unsigned long long>(key.materialKeywords),
                          static_cast<unsigned>(key.vertexFlags));
            rows.push_back({name, key.materialKeywords, key.vertexFlags});
        }
    }
}

std::vector<VariantRequest> VariantsFor(const CookTarget& target, const MaterialBuildContext& context, bool web)
{
    const MaterialDocument& document = target.Builder.GetDocument();
    auto rows = GameEngine::Tools::MaterialCook::SelectVariantRequests(
        kMeshVariants, target.Variants, kDepthMaskRows, document.alphaMode);
    if (target.ExpandsOptionalPasses)
        GameEngine::Tools::MaterialCook::ExpandOptionalPassVariants(rows);
    const std::vector<MaterialKeyword> worldPasses = PrewarmedWorldPasses(rows);
    const auto heightMap = document.textures.find(kParallaxHeightMapSlot);
    const bool bindsHeightMap =
        target.Variants.empty() && heightMap != document.textures.end() && !heightMap->second.empty();
    GameEngine::Tools::MaterialCook::ExpandParallaxDepthVariants(rows, bindsHeightMap, web);
    const MaterialKeyword materialKeywords = Engine::Renderer::DeriveBaseVariantKey(document).materialKeywords;
    GameEngine::Tools::MaterialCook::ExpandSceneColorGrabVariants(
        rows, HasKeyword(materialKeywords, MaterialKeyword::Transmission));
    GameEngine::Tools::MaterialCook::AddMaterialKeywords(rows, materialKeywords);
    // A web cook keeps its declared rows: a browser's cook translates every row to WGSL.
    if (!web && target.Prewarmed)
        AddPrewarmVariants(rows, target, context, worldPasses);
    // Last, so every colour row gets its vertex-colour twin: the declared rows, the grab and relief rows,
    // and the prewarm's crossfade tails, which a mesh with a COLOR_0 stream draws with it too.
    GameEngine::Tools::MaterialCook::ExpandVertexColorVariants(
        rows, target.ImportedMeshMaterial && !document.ignoreVertexColor);
    return rows;
}

// The alias the runtime mounts a package under when its manifest names `root`
// as the package's assets folder: SanitizePackageAlias of the manifest's name,
// as PackageResolver derives it. Empty when no manifest above `root` names it.
// A package.json that is not an engine package manifest names no root.
std::string ManifestAlias(const fs::path& root)
{
    const fs::path mountRoot = AssetPaths::NormalizeMountRoot(root);
    for (fs::path directory = mountRoot;; directory = directory.parent_path())
    {
        std::error_code ec;
        const fs::path manifestFile = directory / "package.json";
        PackageManifest manifest;
        std::string notAPackageManifest;
        if (fs::is_regular_file(manifestFile, ec) &&
            TryLoadPackageManifest(manifestFile, manifest, notAPackageManifest) &&
            !manifest.AssetsDir.empty() &&
            AssetPaths::NormalizeMountRoot(directory / manifest.AssetsDir) == mountRoot)
            return SanitizePackageAlias(manifest.Name);
        if (directory == directory.parent_path())
            return {};
    }
}

// Settles a scan root's alias, the given one or its package's, in the spelling
// the runtime registers, and refuses one the runtime could not mount.
bool ResolveScanAlias(ScanRoot& scan, std::string& error)
{
    const std::string alias = scan.GivenAlias ? *scan.GivenAlias : ManifestAlias(scan.Path);
    if (!scan.GivenAlias && alias.empty())
    {
        error = "--scan " + scan.Path.string() +
                ": no package manifest names this folder as its package's assets folder, so the "
                "alias the runtime mounts it under is unknown; pass --scan-alias <alias>";
        return false;
    }
    scan.Alias = AssetPathSyntax::NormalizeAssetSourceAlias(alias);
    if (!AssetPathSyntax::IsValidAssetSourceAlias(scan.Alias))
    {
        error = "--scan " + scan.Path.string() + ": the alias '" + alias +
                "' is not a valid source alias; an alias is lower-cased and must be non-empty "
                "and use only letters, digits, '-' and '_'";
        return false;
    }
    return true;
}

bool ParseOptions(int argc, char** argv, Options& out, std::string& error)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto value = [&](auto& dst) {
            if (i + 1 >= argc)
            {
                error = arg + " needs a value";
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (arg == "--project")
        {
            if (!value(out.Project)) return false;
        }
        else if (arg == "--engine-shaders")
        {
            if (!value(out.EngineShaders)) return false;
        }
        else if (arg == "--cache")
        {
            if (!value(out.Cache)) return false;
        }
        else if (arg == "--package-shaders")
        {
            fs::path p;
            if (!value(p)) return false;
            out.PackageShaders.push_back(std::move(p));
        }
        else if (arg == "--scan")
        {
            ScanRoot scan;
            if (!value(scan.Path)) return false;
            out.ScanRoots.push_back(std::move(scan));
        }
        else if (arg == "--scan-alias")
        {
            if (out.ScanRoots.empty() || out.ScanRoots.back().GivenAlias)
            {
                error = "--scan-alias names the --scan root just before it";
                return false;
            }
            std::string alias;
            if (!value(alias)) return false;
            out.ScanRoots.back().GivenAlias = std::move(alias);
        }
        else if (arg == "--shadercook")
        {
            if (!value(out.ShaderCookScript)) return false;
        }
        else if (arg == "--reuse-cache")
        {
            fs::path cache;
            if (!value(cache)) return false;
            out.ReuseCache = std::move(cache);
        }
        else if (arg == "--verify")
        {
            out.Verify = true;
        }
        else if (arg == "--web")
        {
            out.Web = true;
        }
        else if (arg == "--jobs")
        {
            std::string count;
            if (!value(count)) return false;
            // from_chars takes digits only: no sign, no whitespace, no wrap of "-1".
            size_t parsed = 0;
            const char* last = count.data() + count.size();
            const auto [end, status] = std::from_chars(count.data(), last, parsed);
            if (status != std::errc{} || end != last || parsed == 0)
            {
                error = "--jobs needs a positive whole number (1 or more, digits only), got '" +
                        count + "'";
                return false;
            }
            out.Jobs = parsed;
        }
        else if (arg == "--particle-row")
        {
            std::string name;
            if (!value(name)) return false;
            const auto rows = Particles::ParticleRenderVariants();
            const auto row = std::find_if(rows.begin(), rows.end(),
                                          [&](const Particles::ParticleRenderVariant& r) { return r.Name == name; });
            if (row == rows.end())
            {
                error = "--particle-row " + name +
                        ": not a row of the particle request set; name one such as 'base' or "
                        "'forward-color'";
                return false;
            }
            out.ParticleRow = VariantRequest{row->Name, row->PassKeywords, row->VertexFlags};
        }
        else
        {
            error = "unknown argument: " + arg;
            return false;
        }
    }

    if (out.Project.empty() || out.EngineShaders.empty())
    {
        error = "--project and --engine-shaders are required";
        return false;
    }
    if (!fs::is_directory(out.Project))
    {
        error = "not a directory: " + out.Project.string();
        return false;
    }
    std::unordered_set<std::string> aliases{std::string(kAssetSourceAliasProject)};
    for (ScanRoot& scan : out.ScanRoots)
    {
        if (!fs::is_directory(scan.Path))
        {
            error = "not a directory: " + scan.Path.string();
            return false;
        }
        if (!ResolveScanAlias(scan, error))
            return false;
        if (!aliases.insert(scan.Alias).second)
        {
            error = "--scan " + scan.Path.string() + ": the alias '" + scan.Alias +
                    "' is already taken (the --project root is 'project'); name this root "
                    "with --scan-alias <alias>, the alias the runtime mounts it under";
            return false;
        }
    }
    if (!fs::is_directory(out.EngineShaders))
    {
        error = "not a directory: " + out.EngineShaders.string();
        return false;
    }
    if (out.Web && out.ShaderCookScript.empty())
    {
        error = "--web needs --shadercook <path to Tools/ShaderCook/shadercook.py>";
        return false;
    }
    if (!out.ShaderCookScript.empty() && !fs::is_regular_file(out.ShaderCookScript))
    {
        error = "not a file: " + out.ShaderCookScript.string();
        return false;
    }
    if (out.ReuseCache && !fs::is_directory(*out.ReuseCache))
    {
        error = "not a directory: " + out.ReuseCache->string();
        return false;
    }
    if (out.Cache.empty())
        out.Cache = out.Project / ".Cache" / "Shaders";
    if (out.ReuseCache &&
        AssetPaths::NormalizeMountRoot(*out.ReuseCache) == AssetPaths::NormalizeMountRoot(out.Cache))
    {
        error = "--reuse-cache " + out.ReuseCache->string() +
                ": this is the cook's own cache; name the cache the editor compiled into";
        return false;
    }
    return true;
}

std::vector<fs::path> FindMaterials(const fs::path& project)
{
    std::vector<fs::path> materials;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(project, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
            break;
        // The cache is output, never input.
        if (it->is_directory(ec) && it->path().filename() == ".Cache")
        {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && it->path().extension() == ".material")
            materials.push_back(it->path());
    }
    std::sort(materials.begin(), materials.end());
    return materials;
}

// Whether `path` lies under `root`, both spelled as mount roots are.
bool IsUnderRoot(const fs::path& path, const fs::path& root)
{
    const fs::path relative =
        AssetPaths::NormalizeMountRoot(path).lexically_relative(AssetPaths::NormalizeMountRoot(root));
    return !relative.empty() && *relative.begin() != "..";
}

// The project's materials the cook compiles: every .material under --project and the --scan
// roots. Sorted, each once.
std::vector<fs::path> MaterialsToCook(const Options& options)
{
    std::vector<fs::path> materialPaths = FindMaterials(options.Project);
    for (const ScanRoot& scan : options.ScanRoots)
    {
        std::vector<fs::path> extraMaterials = FindMaterials(scan.Path);
        materialPaths.insert(materialPaths.end(), extraMaterials.begin(), extraMaterials.end());
    }
    std::sort(materialPaths.begin(), materialPaths.end());
    materialPaths.erase(std::unique(materialPaths.begin(), materialPaths.end()),
                        materialPaths.end());
    return materialPaths;
}

// Every material the project can draw with: `materialPaths`, plus the engine's
// built-in default PBR document, whose stand-in path is anchored under the
// cache root exactly as the runtime anchors it — a directory holding no
// shaders, so it can never shadow the project/package/engine resolution chain.
// Preparation materializes graph surfaces and extends the shared include roots.
// Targets whose preparation fails are reported and excluded from compilation.
std::vector<CookTarget> CollectTargets(const std::vector<fs::path>& materialPaths,
                                       MaterialBuildContext& context, bool web,
                                       const std::optional<VariantRequest>& particleRow,
                                       std::vector<std::string>& outErrors)
{
    const fs::path& cacheRoot = context.CacheRoot;
    std::vector<CookSource> targets;
    for (const fs::path& materialPath : materialPaths)
    {
        MaterialAsset asset(GUID::Generate(), materialPath);
        if (!asset.Load())
        {
            std::string message = materialPath.filename().string() + ": unreadable";
            for (const std::string& e : asset.GetErrors())
                message += "\n        " + e;
            outErrors.push_back(std::move(message));
            continue;
        }
        targets.push_back({materialPath.stem().string(), materialPath, asset.GetDocument(), {}, false, true, true});
    }
    // The standard-PBR stand-ins for imported mesh materials, one per alpha mode
    // (kImportedMeshMaterialTargets); VariantsFor appends the depth-mask rows to
    // the Mask one.
    for (const ImportedMeshMaterialTarget& imported : kImportedMeshMaterialTargets)
    {
        MaterialDocument document = MaterialDocument::CreateDefaultPBR(imported.DocumentName);
        document.alphaMode = imported.AlphaMode;
        targets.push_back({imported.Name, cacheRoot / "Synthetic" / imported.File, std::move(document),
                           std::vector<VariantRequest>(imported.Rows.begin(), imported.Rows.end()), true, true,
                           true});
    }

    // Unlit textured placeholder (Polyhaven thumbnail billboards and any other
    // CreateUnlitTextured consumer): registered at runtime per asset, but every
    // instance composes the same source, so one target serves them all.
    targets.push_back({"unlit-textured-placeholder",
                       cacheRoot / "Synthetic" / "unlit_textured.material",
                       MaterialDocument::CreateUnlitTextured("UnlitTextured"), {}});

    // Particle emitters register their materials at runtime from synthetic
    // documents too, and a cooked-only runtime cannot compile the miss. The
    // shapes come from the builder extraction uses, so the composed source (and
    // therefore the variant key) cannot drift; an emitter's values, textures and
    // source material change no program, so one target per shape serves them all.
    // Each shape cooks the particle renderer's own request set, exactly: the
    // late transparent pass draws no optional pass keyword.
    {
        const std::vector<MaterialDocument> shapes = Particles::ParticleRenderMaterialShapes();
        std::vector<VariantRequest> rows;
        if (particleRow)
            rows.push_back(*particleRow);
        else
            for (const Particles::ParticleRenderVariant& variant : Particles::ParticleRenderVariants())
                rows.push_back({variant.Name, variant.PassKeywords, variant.VertexFlags});
        for (size_t index = 0; index < shapes.size(); ++index)
        {
            const std::string name = "particles-" + std::to_string(index);
            targets.push_back({name, cacheRoot / "Synthetic" / (name + ".material"), shapes[index], rows, false, false});
        }
    }

    // The ocean surface registers at runtime from a synthetic document (no
    // .material asset) with exactly one keyword set, so it cooks its own row:
    // the registration pipeline is the only variant the renderer requests.
    // The document and keywords come from the Ocean module so the cook and the
    // runtime cannot drift.
    {
        CookSource ocean{"ocean-surface", cacheRoot / "Synthetic" / "ocean_surface.material",
                         Ocean::BuildOceanSurfaceMaterialDocument(), {}};
        ocean.Variants.push_back({"ocean-forward", Ocean::OceanSurfaceMaterialKeywords(web),
                                  VertexAttributeFlags::None});
        // The fogged surface (OceanSurfaceFogMaterialGuid), drawn by a view whose height fog is active.
        if (!web)
            ocean.Variants.push_back({"ocean-forward-fog",
                                      Ocean::OceanSurfaceMaterialKeywords(web) | MaterialKeyword::HeightFog,
                                      VertexAttributeFlags::None});
        targets.push_back(std::move(ocean));
    }

    // CBT terrain, same shape as the ocean: a synthetic document registered at runtime,
    // taken from the module so the cook and the feature cannot drift. It cannot use the
    // default variant table — customVertexShader forces vertexFlags = None, so every row
    // is one variant rather than four vertex buckets, and the material is not Instanced.
    // The pass re-keys apply here exactly as they do to world geometry: an AO or SSR
    // volume flips the whole colour pass to a keyword set, and a terrain with no cooked
    // row for it disappears rather than degrades.
    {
        using CBTTerrainECS::BuildCBTTerrainMaterialDocument;
        using CBTTerrainECS::CBTTerrainMaterialKeywords;
        const MaterialKeyword base = CBTTerrainMaterialKeywords();
        CookSource terrain{"cbt-terrain", cacheRoot / "Synthetic" / "cbt_terrain.material",
                           BuildCBTTerrainMaterialDocument(), {}};
        // The keyword-less variant registration prewarms before any pass has asked.
        terrain.Variants.push_back({"cbt-base", MaterialKeyword::None,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward", base, VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-sssr",
                                    base | MaterialKeyword::SSSRNormalRoughness,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-gtao", base | MaterialKeyword::GTAO,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-gtao-sssr",
                                    base | MaterialKeyword::GTAO |
                                        MaterialKeyword::SSSRNormalRoughness,
                                    VertexAttributeFlags::None});
        // A DDGIVolume resolved for the view re-keys the colour pass exactly like AO
        // or SSR do (this block's own comment above), so DDGI crosses every row above
        // it the same way GTAO and SSSR already cross each other: on its own, and
        // combined with each of the other two. Missed here, a terrain under a DDGI
        // volume has no cooked row and disappears on a cooked-only runtime — the
        // black-screen report this table exists to prevent, just for a keyword this
        // table had not been extended to yet.
        terrain.Variants.push_back({"cbt-forward-ddgi", base | MaterialKeyword::DDGI,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-ddgi-sssr",
                                    base | MaterialKeyword::DDGI |
                                        MaterialKeyword::SSSRNormalRoughness,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-ddgi-gtao",
                                    base | MaterialKeyword::DDGI | MaterialKeyword::GTAO,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-ddgi-gtao-sssr",
                                    base | MaterialKeyword::DDGI | MaterialKeyword::GTAO |
                                        MaterialKeyword::SSSRNormalRoughness,
                                    VertexAttributeFlags::None});
        // The height fog re-keys the colour pass the same way, in the shipped templates on its own.
        terrain.Variants.push_back({"cbt-forward-fog", base | MaterialKeyword::HeightFog,
                                    VertexAttributeFlags::None});
        targets.push_back(std::move(terrain));
    }

    // Terrain grass, same shape as the terrain above: a synthetic document taken from the module
    // so cook and feature cannot drift. vertexFlags = None, matching what the feature actually
    // submits — the blade card is a bare two-float buffer the vertex modifier expands, not a
    // standard mesh layout, so none of the StandardMesh buckets describe it.
    //
    // Every profile: a browser ships no compiler, and a desktop game that ships this cache serves
    // the material's registration compile, made on the frame that first draws grass, from it.
    {
        using TerrainGrass::GrassDrawMode;
        // The per-mode document TerrainGrassRenderFeature registers, mirrored here because that
        // builder is private to the feature TU. Only the fields the cook keys on are reproduced.
        const auto grassDocFor = [](GrassDrawMode mode) {
            MaterialDocument doc{};
            doc.lightingModel = "StandardPBR";
            doc.surfaceShader = "TerrainGrass/terrain_grass_surface.glsl";
            doc.vertexModifier = "TerrainGrass/terrain_grass_vertex_modifier.glsl";
            doc.doubleSided = true;
            doc.properties["roughness"] = 0.9f;
            switch (mode)
            {
            case GrassDrawMode::Blend:
                doc.materialName = "Terrain/Grass";
                doc.alphaMode = MaterialAlphaMode::Blend;
                break;
            case GrassDrawMode::Opaque:
                doc.materialName = "Terrain/GrassOpaque";
                doc.alphaMode = MaterialAlphaMode::Opaque;
                doc.keywords = {"GRASS_OPAQUE"};
                break;
            case GrassDrawMode::Dither:
                doc.materialName = "Terrain/GrassDither";
                doc.alphaMode = MaterialAlphaMode::Opaque;
                doc.keywords = {"GRASS_DITHER"};
                break;
            case GrassDrawMode::DitherA2C:
                doc.materialName = "Terrain/GrassA2C";
                doc.alphaMode = MaterialAlphaMode::Opaque;
                doc.keywords = {"GRASS_A2C"};
                break;
            }
            return doc;
        };
        // The feature submits the blade draw with HasVertexMod already set, and
        // MaterialBuildService adds HasVertexOutputMod on top (the modifier
        // declares the output capability). The cook must request the same pair or
        // its rows key on a different define set — HAS_VERTEX_MODIFIER missing —
        // and the cooked-only runtime resolves nothing for the draw.
        const MaterialKeyword base =
            TerrainGrass::kGrassMaterialKeywords | MaterialKeyword::HasVertexMod;
        // None resolves to StandardMesh inside the build (MaterialBuildService),
        // which is what the blade draw asks for.
        const VertexAttributeFlags bladeFlags = VertexAttributeFlags::None;
        // All three draw-mode materials: the feature picks one per view at runtime
        // (authored Blend vs Dither, and Dither splits on the view's sample count),
        // so a cooked-only runtime needs every mode it could resolve to.
        struct GrassModeTarget
        {
            GrassDrawMode Mode;
            const char* TargetName;
            const char* FileName;
        };
        constexpr GrassModeTarget kGrassModes[] = {
            {GrassDrawMode::Blend, "terrain-grass", "terrain_grass.material"},
            {GrassDrawMode::Opaque, "terrain-grass-opaque", "terrain_grass_opaque.material"},
            {GrassDrawMode::Dither, "terrain-grass-dither",
             "terrain_grass_dither.material"},
            {GrassDrawMode::DitherA2C, "terrain-grass-a2c",
             "terrain_grass_a2c.material"},
        };
        for (const GrassModeTarget& gm : kGrassModes)
        {
            CookSource grass{gm.TargetName, cacheRoot / "Synthetic" / gm.FileName,
                             grassDocFor(gm.Mode), {}};
            grass.Variants.push_back({"grass-base", MaterialKeyword::None, bladeFlags});
            grass.Variants.push_back({"grass-forward", base, bladeFlags});
            grass.Variants.push_back({"grass-forward-sssr",
                                      base | MaterialKeyword::SSSRNormalRoughness, bladeFlags});
            grass.Variants.push_back(
                {"grass-forward-gtao", base | MaterialKeyword::GTAO, bladeFlags});
            grass.Variants.push_back({"grass-forward-gtao-sssr",
                                      base | MaterialKeyword::GTAO |
                                          MaterialKeyword::SSSRNormalRoughness,
                                      bladeFlags});
            // WorldRenderNode applies DDGI to every material in the view,
            // including each grass draw mode. Cook its combinations with AO
            // and reflections so a compiler-less runtime can resolve them.
            grass.Variants.push_back(
                {"grass-forward-ddgi", base | MaterialKeyword::DDGI, bladeFlags});
            grass.Variants.push_back({"grass-forward-ddgi-sssr",
                                      base | MaterialKeyword::DDGI |
                                          MaterialKeyword::SSSRNormalRoughness,
                                      bladeFlags});
            grass.Variants.push_back({"grass-forward-ddgi-gtao",
                                      base | MaterialKeyword::DDGI | MaterialKeyword::GTAO,
                                      bladeFlags});
            grass.Variants.push_back({"grass-forward-ddgi-gtao-sssr",
                                      base | MaterialKeyword::DDGI | MaterialKeyword::GTAO |
                                          MaterialKeyword::SSSRNormalRoughness,
                                      bladeFlags});
            grass.Variants.push_back(
                {"grass-forward-fog", base | MaterialKeyword::HeightFog, bladeFlags});
            targets.push_back(std::move(grass));
        }
    }

    if (web)
    {
        // Tree Generator runtime materials: extraction always attaches
        // ez_tree_wind (even when wind is off — the shader early-outs) and
        // leaves always use Mask + ez_tree_leaves.glsl. Scanning the package
        // .material files cooks a different intern identity (no modifier,
        // standard_pbr leaves), so the canopy, bark textures, and wind all
        // miss on a compiler-less runtime. Same builders the extractor uses.
        {
            using EZTreeECS::MakeBarkRuntimeMaterialShape;
            using EZTreeECS::MakeLeafRuntimeMaterialShape;
            using EZTreeECS::MakeTrellisRuntimeMaterialShape;
            targets.push_back({"eztree-bark", cacheRoot / "Synthetic" / "eztree_bark.material",
                               MakeBarkRuntimeMaterialShape(), {}});
            targets.push_back({"eztree-leaves", cacheRoot / "Synthetic" / "eztree_leaves.material",
                               MakeLeafRuntimeMaterialShape(), {}});
            targets.push_back({"eztree-trellis", cacheRoot / "Synthetic" / "eztree_trellis.material",
                               MakeTrellisRuntimeMaterialShape(), {}});
        }
    }
    std::vector<CookTarget> preparedTargets;
    preparedTargets.reserve(targets.size());
    for (CookSource& source : targets)
    {
        Engine::Renderer::MaterialShaderPackageBuilder builder(
            std::move(source.Document), source.MaterialPath, source.Name, context);
        if (!builder.IsPrepared())
        {
            for (const std::string& error : builder.GetErrors())
                outErrors.push_back(source.Name + ": " + error);
            continue;
        }
        preparedTargets.push_back({std::move(source.Name), std::move(source.MaterialPath),
                                   std::move(builder), std::move(source.Variants), source.ImportedMeshMaterial,
                                   source.ExpandsOptionalPasses, source.Prewarmed});
    }
    return preparedTargets;
}

// One engine compute program the cook compiles beside the materials: a program the runtime builds
// through the shader compile service from the engine shader tree rather than loading prebuilt.
struct ComputeProgramRow
{
    std::string Target;
    std::string Variant;
    ShaderProgramCompileRequest Request;
};

// The desktop runtime's compute programs, built from the engine shader tree `engineShaders` exactly as
// the runtime builds them: the terrain grass placement's (MakeGrassComputeCompileRequest, the full
// profile). Empty when the tree holds no grass sources. A web cook lists none: the browser loads the
// placement's WGSL that Tools/Web/builtin_shader_cook.py cooks.
std::vector<ComputeProgramRow> ComputeProgramRows(const fs::path& engineShaders, bool web)
{
    std::vector<ComputeProgramRow> rows;
    std::error_code ec;
    if (web || !fs::is_regular_file(engineShaders / "TerrainGrass" / TerrainGrass::kGrassPlaceProgram.ShaderFile, ec))
        return rows;
    for (const TerrainGrass::GrassComputeProgram& program : TerrainGrass::kGrassComputePrograms)
        rows.push_back({"terrain-grass-compute", program.ShaderFile,
                        TerrainGrass::MakeGrassComputeCompileRequest(engineShaders, program, false)});
    return rows;
}

// Resolves `row` into the cache roots `context` names: compiled into its cache root, or served from a
// read-only root. Returns the package path, or empty with `error` set.
std::string BuildComputeProgram(const ComputeProgramRow& row, const MaterialBuildContext& context,
                                std::string& error)
{
    ShaderProgramCompileRequest request = row.Request;
    request.readOnlyCacheRoots = context.ReadOnlyCacheRoots;
    request.cacheRoot = context.CacheRoot;
    ShaderProgramCompileResult result{};
    if (!ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::SpirV, result, &error))
        return {};
    return result.shaderPkgPath.string();
}

constexpr size_t kNoWgslRequest = static_cast<size_t>(-1);

// One variant's result, reported after every WGSL translation has run.
struct VariantOutcome
{
    std::string Label;
    std::string PackagePath;
    std::vector<std::string> BuildErrors;
    bool Built = false;
    // The translation this variant's package needs, or kNoWgslRequest.
    size_t WgslRequest = kNoWgslRequest;
    // Another variant of this run owns the translation of the same package.
    bool SharesTranslation = false;
};

// The logger's drain thread shares the console and writes a message and its
// newline separately, so a result line written in pieces can be split by a log
// line. Each result line, with its indented detail lines, is composed first
// and written with one insertion.
std::string ResultLines(std::string head, const std::vector<std::string>& details)
{
    head += '\n';
    for (const std::string& detail : details)
        head += "        " + detail + '\n';
    return head;
}

// One variant of one prepared target, in the order the cook reports it.
struct CookRow
{
    const CookTarget* Target = nullptr;
    VariantRequest Variant;
};

// What building a row produced that the report and the WGSL translation read. The compiled
// package itself stays on disk; a row keeps the composed source only for the web translation.
struct RowBuild
{
    bool Success = false;
    std::vector<std::string> Errors;
    std::string PackagePath;
    // The key the program index addresses this row's program by (MaterialBuildResult::programIndexKey).
    uint64_t ProgramIndexKey = 0;
    // The files the build read (MaterialBuildResult::includeClosurePaths): the index records them.
    std::vector<std::string> SourcePaths;
    std::string VertexSource;
    std::string FragmentSource;
    std::vector<std::string> Defines;
};

std::vector<CookRow> CookRows(const std::vector<CookTarget>& targets, const MaterialBuildContext& context,
                              bool web)
{
    std::vector<CookRow> rows;
    for (const CookTarget& target : targets)
        for (VariantRequest& variant : VariantsFor(target, context, web))
            rows.push_back({&target, std::move(variant)});
    return rows;
}

RowBuild BuildRow(const CookRow& row, bool web)
{
    MaterialBuildResult result = row.Target->Builder.Build(
        ShaderSourceKind::SpirV, row.Variant.PassKeywords, row.Variant.VertexFlags);
    RowBuild build{result.success, std::move(result.errors), std::move(result.generatedShaderPkgPath),
                   result.programIndexKey, std::move(result.includeClosurePaths)};
    if (web)
    {
        build.VertexSource = std::move(result.composedVertexSource);
        build.FragmentSource = std::move(result.composedFragmentSource);
        build.Defines = std::move(result.composedDefines);
    }
    return build;
}

// Builds every row, `jobs` at a time. A build only reads its prepared target and the build
// context, and the compile service stages each package it writes under its own temporary name
// (SaveShaderPkg), so rows that share a content key may compile concurrently.
std::vector<RowBuild> BuildRows(const std::vector<CookRow>& rows, size_t jobs, bool web)
{
    std::vector<RowBuild> builds(rows.size());
    Tools::RunOnCookWorkers(rows.size(), jobs,
                            [&](size_t index) { builds[index] = BuildRow(rows[index], web); });
    return builds;
}

// Copies the program at `packagePath`, which lies under `fromCache`, to the same key directory
// under `toCache`, and names the copy in `packagePath`. Returns false with `error` naming the
// program when it cannot be copied.
bool CopyIntoCache(std::string& packagePath, const fs::path& fromCache, const fs::path& toCache,
                   std::string& error)
{
    const fs::path relative = AssetPaths::NormalizeMountRoot(packagePath)
                                  .lexically_relative(AssetPaths::NormalizeMountRoot(fromCache));
    const fs::path destination = toCache / relative;
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (!ec)
        fs::copy_file(packagePath, destination, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        error = "could not copy the reused program " + packagePath + " to " + destination.string() +
                ": " + ec.message();
        return false;
    }
    packagePath = destination.string();
    return true;
}

// The program index entry of a built row: its input key and its package's path under `cache`.
// Empty when the package does not lie under the cache (it was not copied in).
std::optional<ShaderProgramIndex::Entry> ProgramIndexEntry(const RowBuild& build, const std::string& packagePath,
                                                           const fs::path& cache)
{
    if (build.ProgramIndexKey == 0 || !IsUnderRoot(packagePath, cache))
        return std::nullopt;
    const fs::path relative =
        AssetPaths::NormalizeMountRoot(packagePath).lexically_relative(AssetPaths::NormalizeMountRoot(cache));
    return ShaderProgramIndex::Entry{build.ProgramIndexKey, relative.generic_string()};
}

// Checks the written index the way the runtime reads it: loaded for this cook's compile inputs,
// every entry found under its key and naming its row's package. Returns the rows it does not serve.
std::vector<std::string> UnservedByProgramIndex(const fs::path& indexPath,
                                                const std::vector<ShaderProgramIndex::Entry>& entries,
                                                const MaterialBuildContext& context)
{
    std::string reason;
    const std::shared_ptr<const ShaderProgramIndex> index = ShaderProgramIndex::Load(indexPath, context, &reason);
    if (!index)
        return {reason};
    std::vector<std::string> unserved;
    for (const ShaderProgramIndex::Entry& entry : entries)
        if (AssetPaths::NormalizeMountRoot(index->Find(entry.InputKey)) !=
            AssetPaths::NormalizeMountRoot(index->CacheRoot() / entry.PackagePath))
            unserved.push_back(entry.PackagePath);
    return unserved;
}

void PrintUsage(std::ostream& out)
{
    out << "usage: MaterialVariantCook --project <dir> --engine-shaders <dir>\n"
           "                           [--cache <dir>] [--package-shaders <dir>]...\n"
           "                           [--scan <dir> [--scan-alias <alias>]]...\n"
           "                           [--verify] [--web --shadercook <path>]\n"
           "                           [--particle-row <variant>] [--jobs <count>]\n"
           "                           [--reuse-cache <dir>]\n"
           "--jobs bounds how many variants compile, and under --web how many WGSL\n"
           "translations run, at once (default: one per hardware thread).\n"
           "--scan-alias names the --scan root before it in generated graph surface\n"
           "names and must be the alias the runtime mounts that root under. Without\n"
           "it, a root that a package manifest names as its assets folder takes that\n"
           "package's alias; any other root needs --scan-alias. An alias is\n"
           "lower-cased and uses letters, digits, '-' and '_'. The --project root\n"
           "is 'project'.\n";
}

} // namespace

int main(int argc, char** argv)
{
    Logger::Log::Initialize(Logger::Log::Config{});

    Options options{};
    std::string error;
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        Logger::Log::Shutdown();
        PrintUsage(std::cout);
        return 0;
    }
    if (!ParseOptions(argc, argv, options, error))
    {
        Logger::Log::Shutdown();
        std::cerr << "MaterialVariantCook: " << error << "\n";
        PrintUsage(std::cerr);
        return 2;
    }
    if (!ShaderCompileService::IsCompilerAvailable())
    {
        Logger::Log::Shutdown();
        std::cerr << "MaterialVariantCook: this build has no shader compiler; nothing can be "
                     "cooked. Build the tool from a desktop configuration with shaderc.\n";
        return 2;
    }

    MaterialBuildContext context{};
    context.AdapterShaderDir = fs::absolute(options.EngineShaders);
    context.ProjectRoots = {fs::absolute(options.Project)};
    context.AssetSourceRoots.push_back(
        {std::string(kAssetSourceAliasProject), AssetPaths::NormalizeMountRoot(options.Project)});
    for (const ScanRoot& scan : options.ScanRoots)
        context.AssetSourceRoots.push_back({scan.Alias, AssetPaths::NormalizeMountRoot(scan.Path)});
    context.PackageShaderDirs = options.PackageShaders;
    // A project's own Shaders/ dir is a package shader root like any other.
    const fs::path projectShaders = fs::absolute(options.Project) / "Shaders";
    if (fs::is_directory(projectShaders))
        context.PackageShaderDirs.push_back(projectShaders);
    context.CacheRoot = fs::absolute(options.Cache);
    // Probed before the cook's own cache, with the runtime's validation of a cached program.
    if (options.ReuseCache)
        context.ReadOnlyCacheRoots = {fs::absolute(*options.ReuseCache)};

    // The compile inputs of the device class the cook serves, set before the
    // first build the way the runtime sets them from the profile its device
    // resolves: a desktop device resolves the full profile, every feature on
    // (the interpolation functions included); a browser resolves the compat
    // profile, which keeps the cooked SPIR-V free of the buffer_device_address
    // and descriptor-indexing constructs naga refuses and of interpolation
    // functions WGSL does not have. An input set differently here from the
    // runtime changes the cache key, and that runtime then misses every
    // program cooked for it.
    Rendering::RendererProfile servedProfile{};
    if (options.Web)
    {
        servedProfile.ActiveTier = Rendering::RendererProfile::Tier::Compatibility;
        servedProfile.UseInterpolationFunctions = false;
    }
    Rendering::ApplyShaderCompileProfile(servedProfile);
    // The SPIR-V target is a compile input too, and a device publishes its own at init
    // (ShaderCompileService::SetMaxSupportedSpirv): 1.6 on Metal and on a Vulkan 1.3 device, which a
    // desktop cook serves. A browser device publishes none, so a web cook keys at the default 1.5.
    // A Vulkan 1.2 device asks for 1.5 and compiles its programs at first use.
    if (!options.Web)
        ShaderCompileService::SetMaxSupportedSpirv(ShaderCompileService::SpirvTarget::Spirv_1_6);

    size_t failed = 0;
    std::vector<std::string> loadErrors;
    const std::vector<CookTarget> targets =
        CollectTargets(MaterialsToCook(options), context, options.Web,
                       options.ParticleRow, loadErrors);
    for (const std::string& e : loadErrors)
    {
        std::cerr << ResultLines("  FAIL " + e, {});
        ++failed;
    }

    // Outside the cache root: the cache ships with the content, and the WGSL
    // chain's intermediates are debugging material, not shipped artifacts. Each
    // translation writes under its own <pid>-<index> directory, so concurrent
    // translations, and concurrent cooks on one machine, never share a file. A
    // failure keeps its directory and names the scratch file it wrote.
    const fs::path wgslScratch = fs::temp_directory_path() / "MaterialVariantCookWeb";
    const std::string processTag = std::to_string(Platform::GetCurrentProcessId());

    const size_t jobs = options.Jobs != 0 ? options.Jobs
                                          : std::max<size_t>(std::thread::hardware_concurrency(), 1);
    // Every row builds its SPIR-V first, `jobs` at a time; the WGSL translations the rows need
    // are collected and run concurrently afterwards, so no translation runs while a build can
    // still rewrite its package. The results are reported in row order whatever order the
    // builds and translations finish in: the export reads these lines.
    // The index is rewritten from this run's rows only, so one left by an earlier cook never ships
    // beside programs it does not describe.
    const fs::path programIndexPath = context.CacheRoot / kShaderProgramIndexFileName;
    std::error_code removeError;
    fs::remove(programIndexPath, removeError);
    const std::vector<CookRow> rows = CookRows(targets, context, options.Web);
    const std::vector<RowBuild> builds = BuildRows(rows, jobs, options.Web);
    size_t reused = 0;
    std::vector<VariantOutcome> outcomes;
    std::vector<Tools::WebWgslCookRequest> wgslRequests;
    std::unordered_map<std::string, size_t> wgslRequestByPackage;
    for (size_t index = 0; index < rows.size(); ++index)
    {
        const CookTarget& target = *rows[index].Target;
        const VariantRequest& variant = rows[index].Variant;
        const RowBuild& result = builds[index];
        VariantOutcome& outcome = outcomes.emplace_back();
        outcome.Label = target.Name + " [" + variant.Name + "]";
        if (!result.Success)
        {
            outcome.BuildErrors = result.Errors;
            continue;
        }
        std::string packagePath = result.PackagePath;
        if (options.ReuseCache && IsUnderRoot(packagePath, *options.ReuseCache))
        {
            std::string copyError;
            if (!CopyIntoCache(packagePath, *options.ReuseCache, context.CacheRoot, copyError))
            {
                outcome.BuildErrors = {copyError};
                continue;
            }
            ++reused;
        }
        outcome.Built = true;
        outcome.PackagePath = packagePath;
        if (!options.Web)
            continue;

        // BuildMaterialToShaderPackage has validated this content key and its
        // include dependencies, so variants that share a package share its
        // translation: it runs once in this run and is never taken from a prior one.
        if (const auto it = wgslRequestByPackage.find(outcome.PackagePath);
            it != wgslRequestByPackage.end())
        {
            outcome.WgslRequest = it->second;
            outcome.SharesTranslation = true;
            continue;
        }
        outcome.WgslRequest = wgslRequests.size();
        Tools::WebWgslCookRequest& wgsl = wgslRequests.emplace_back();
        wgsl.ShaderCookScript = fs::absolute(options.ShaderCookScript);
        wgsl.ScratchDir = wgslScratch / (processTag + "-" + std::to_string(outcome.WgslRequest));
        wgsl.PackagePath = outcome.PackagePath;
        wgsl.VertexSource = result.VertexSource;
        wgsl.FragmentSource = result.FragmentSource;
        wgsl.Defines = result.Defines;
        wgsl.IncludeRoots = BuildMaterialIncludeRoots(context, target.MaterialPath.parent_path());
        wgsl.DebugName = target.Name + "_" + variant.Name;
        wgslRequestByPackage.emplace(outcome.PackagePath, outcome.WgslRequest);
    }

    // No result line prints until the batch ends, so say what is running.
    if (!wgslRequests.empty())
        std::cout << "MaterialVariantCook: translating " << wgslRequests.size() << " WGSL program(s) on "
                  << std::min(jobs, wgslRequests.size()) << " worker(s)\n"
                  << std::flush;
    const std::vector<std::string> wgslErrors = Tools::CookWebWgslBatch(wgslRequests, jobs);

    size_t cooked = 0;
    size_t webTranslationsReused = 0;
    std::vector<ShaderProgramIndex::Entry> programIndexEntries;
    std::unordered_set<std::string> programIndexSourceSet;
    for (size_t index = 0; index < outcomes.size(); ++index)
    {
        const VariantOutcome& outcome = outcomes[index];
        if (!outcome.Built)
        {
            std::cerr << ResultLines("  FAIL " + outcome.Label, outcome.BuildErrors);
            ++failed;
            continue;
        }
        if (outcome.WgslRequest != kNoWgslRequest && !wgslErrors[outcome.WgslRequest].empty())
        {
            std::cerr << ResultLines("  FAIL " + outcome.Label + " WGSL", {wgslErrors[outcome.WgslRequest]});
            ++failed;
            continue;
        }
        if (outcome.SharesTranslation)
            ++webTranslationsReused;
        ++cooked;
        if (std::optional<ShaderProgramIndex::Entry> entry =
                ProgramIndexEntry(builds[index], outcome.PackagePath, context.CacheRoot))
        {
            programIndexEntries.push_back(std::move(*entry));
            programIndexSourceSet.insert(builds[index].SourcePaths.begin(), builds[index].SourcePaths.end());
        }
        std::cout << ResultLines("  ok   " + outcome.Label + " -> " + outcome.PackagePath, {});
    }
    std::string programIndexError;
    const std::vector<fs::path> programIndexSources(programIndexSourceSet.begin(), programIndexSourceSet.end());
    if (!ShaderProgramIndex::Write(programIndexPath, programIndexEntries, programIndexSources, context,
                                   &programIndexError))
    {
        std::cerr << ResultLines("  FAIL program index", {programIndexError});
        ++failed;
    }

    const fs::path engineShaders = fs::absolute(options.EngineShaders);
    const std::vector<ComputeProgramRow> computeRows = ComputeProgramRows(engineShaders, options.Web);
    for (const ComputeProgramRow& row : computeRows)
    {
        std::string programError;
        std::string packagePath = BuildComputeProgram(row, context, programError);
        if (!packagePath.empty() && options.ReuseCache && IsUnderRoot(packagePath, *options.ReuseCache))
        {
            if (CopyIntoCache(packagePath, *options.ReuseCache, context.CacheRoot, programError))
                ++reused;
            else
                packagePath.clear();
        }
        if (packagePath.empty())
        {
            std::cerr << ResultLines("  FAIL " + row.Target + " [" + row.Variant + "]", {programError});
            ++failed;
            continue;
        }
        ++cooked;
        std::cout << ResultLines("  ok   " + row.Target + " [" + row.Variant + "] -> " + packagePath, {});
    }

    const bool verify = options.Verify && failed == 0;
    size_t unserved = 0;
    if (verify)
    {
        // Exactly what the shipped runtime does: resolve every variant with no
        // compiler in reach. A miss here means the cook wrote an entry the
        // runtime does not address — the failure mode this whole path exists to
        // prevent, and one a compiler-present run would have papered over.
        ShaderCompileService::SetCompilerAvailable(false);
        // The cook's own cache is what ships, so it alone must serve every variant.
        context.ReadOnlyCacheRoots.clear();
        const std::vector<RowBuild> resolved = BuildRows(rows, jobs, false);
        for (size_t index = 0; index < rows.size(); ++index)
        {
            const CookTarget& target = *rows[index].Target;
            const VariantRequest& variant = rows[index].Variant;
            const RowBuild& result = resolved[index];
            // The WGSL lives in the package on disk, not in the build
            // result: the compile service's SPIR-V-shaped result carries no
            // WGSL, and a browser build substitutes the chunks at parse
            // time. Checking the shipped artifact is the honest check.
            bool servable = result.Success;
            if (servable && options.Web)
            {
                ShaderPackage pkg{};
                std::string loadError;
                servable = LoadShaderPkg(result.PackagePath, ShaderSourceKind::SpirV, pkg, &loadError)
                           && pkg.wgslStages.count("vs") != 0
                           && pkg.wgslStages.count("fs") != 0;
            }
            if (servable)
                continue;
            std::cerr << ResultLines("  UNSERVED " + target.Name + " [" + variant.Name + "]" +
                                         (result.Success ? " (no WGSL chunks in the package)" : ""),
                                     result.Errors);
            ++unserved;
        }
        for (const ComputeProgramRow& row : computeRows)
        {
            std::string programError;
            if (!BuildComputeProgram(row, context, programError).empty())
                continue;
            std::cerr << ResultLines("  UNSERVED " + row.Target + " [" + row.Variant + "]", {programError});
            ++unserved;
        }
        // The runtime finds a cooked variant through the index before it composes anything.
        for (const std::string& missing : UnservedByProgramIndex(programIndexPath, programIndexEntries, context))
        {
            std::cerr << ResultLines("  UNSERVED program index", {missing});
            ++unserved;
        }
        ShaderCompileService::SetCompilerAvailable(true);
    }

    // The export reads the lines below from this output: every line the cook logged is written
    // before them, and the logger's drain thread is joined, so none can follow them or be cut
    // short when the process exits.
    Logger::Log::Shutdown();
    if (unserved != 0)
    {
        std::cerr << "MaterialVariantCook: " << unserved
                  << " cooked variant(s) are NOT servable without a compiler\n";
        return 1;
    }
    if (verify)
        std::cout << "verify: every cooked variant resolves with no compiler\n";
    std::cout << "MaterialVariantCook: " << cooked << " variant(s) cooked from " << targets.size()
              << " material(s) into " << context.CacheRoot << "\n";
    if (options.ReuseCache)
        std::cout << "MaterialVariantCook: " << reused << " variant(s) reused from "
                  << *options.ReuseCache << " instead of compiled\n";
    if (options.Web)
        std::cout << "MaterialVariantCook: reused " << webTranslationsReused
                  << " shared WGSL translation(s) within this run\n";
    if (failed != 0)
    {
        std::cerr << "MaterialVariantCook: " << failed << " variant(s) failed\n";
        return 1;
    }
    return 0;
}
