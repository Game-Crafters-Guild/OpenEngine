// What a build ships for a scene. The export walks the dependency graph from each
// built scene, so an asset the scene loads but the walk never reaches is missing from
// the package, and the packaged Player fails to load the scene (#2652). A scene names
// the prefabs its [prefab ...] instances spawn in [resource] headers.

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Engine/Build/AssetCollector.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{

void WriteTextFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

class BuildSceneCollection : public ::testing::Test
{
  protected:
    void SetUp() override { fs::create_directories(m_ProjectAssets); }

    void TearDown() override { m_AssetManager.Shutdown(); }

    void Mount()
    {
        ASSERT_TRUE(m_AssetManager.Initialize(m_ProjectAssets, nullptr,
                                              m_Root.Path() / "project" / "AssetDatabase.assetdb",
                                              m_Root.Path() / "project" / ".Cache" / "AssetDatabase"));
        m_AssetManager.WaitForStartupScan();
    }

    GUID GuidOf(const fs::path& file) { return m_AssetManager.GetRegistry().GetAssetGUID(file); }

    const TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("ge_build_scene_collect")};
    const fs::path m_ProjectAssets = m_Root.Path() / "project" / "Assets";
    AssetManager m_AssetManager;
};

std::string Prefab(const std::string& name, const std::string& texturePath)
{
    return "[prefab name=\"" + name + "\" version=1]\n"
           "[entity id=\"root\"]\n"
           "Name.value = \"" + name + "\"\n"
           "MeshRenderer.material = [path=\"" + texturePath + "\"]\n";
}

} // namespace

TEST_F(BuildSceneCollection, PrefabsAScenesInstancesSpawnShipWithWhatTheyReference)
{
    // A path-only reference: the resource names its prefab by path only, and the prefab
    // names its own content. A second resource carries the GUID the editor's save heals in.
    WriteTextFile(m_ProjectAssets / "Textures" / "Well.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Textures" / "Rock.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Prefabs" / "Env" / "Well.prefab", Prefab("Well", "Textures/Well.png"));
    WriteTextFile(m_ProjectAssets / "Prefabs" / "Rock.prefab", Prefab("Rock", "Textures/Rock.png"));
    WriteTextFile(m_ProjectAssets / "Scenes" / "Lake.scene", "[scene name=\"Lake\" version=1]\n");
    Mount();
    const GUID wellGuid = GuidOf(m_ProjectAssets / "Prefabs" / "Env" / "Well.prefab");
    const GUID rockGuid = GuidOf(m_ProjectAssets / "Prefabs" / "Rock.prefab");
    const GUID wellTextureGuid = GuidOf(m_ProjectAssets / "Textures" / "Well.png");
    const GUID rockTextureGuid = GuidOf(m_ProjectAssets / "Textures" / "Rock.png");
    ASSERT_FALSE(wellGuid.IsNull());
    ASSERT_FALSE(rockGuid.IsNull());
    ASSERT_FALSE(wellTextureGuid.IsNull());
    ASSERT_FALSE(rockTextureGuid.IsNull());
    WriteTextFile(m_ProjectAssets / "Scenes" / "Lake.scene",
                  "[scene name=\"Lake\" version=1]\n"
                  "[resource id=\"well\" path=\"Prefabs/Env/Well.prefab\"]\n"
                  "[resource id=\"rock\" path=\"Prefabs/Rock.prefab\" guid=\"" + rockGuid.ToString() + "\"]\n"
                  "\n"
                  "[prefab id=\"well_0\" source=\"well\"]\n"
                  "Transform.position = (0, 0, 0)\n"
                  "\n"
                  "[prefab id=\"rock_0\" source=\"rock\"]\n"
                  "Transform.position = (4, 0, 0)\n");

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Lake.scene"});

    const AssetManifestEntry* well = manifest.FindByGuid(wellGuid);
    ASSERT_NE(well, nullptr) << "the prefab a resource names by path did not ship";
    EXPECT_EQ(well->outputPath.generic_string(), "Assets/Prefabs/Env/Well.prefab");
    const AssetManifestEntry* rock = manifest.FindByGuid(rockGuid);
    ASSERT_NE(rock, nullptr) << "the prefab a resource names by GUID did not ship";
    EXPECT_EQ(rock->outputPath.generic_string(), "Assets/Prefabs/Rock.prefab");
    EXPECT_NE(manifest.FindByGuid(wellTextureGuid), nullptr) << "what the spawned prefab names did not ship";
    EXPECT_NE(manifest.FindByGuid(rockTextureGuid), nullptr) << "what the spawned prefab names did not ship";
    EXPECT_TRUE(manifest.unresolvedDependencies.empty());
}

TEST_F(BuildSceneCollection, ResourceAScenesFieldNamesByIdShips)
{
    // `#id` field values resolve through the [resource] table, so the header is the
    // only place the scene names the asset.
    WriteTextFile(m_ProjectAssets / "Textures" / "Ground.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Scenes" / "Field.scene",
                  "[scene name=\"Field\" version=1]\n"
                  "[resource id=\"ground\" path=\"Textures/Ground.png\"]\n"
                  "\n"
                  "[entity id=\"e_1\"]\n"
                  "MeshRenderer.material = #ground\n");
    Mount();
    const GUID groundGuid = GuidOf(m_ProjectAssets / "Textures" / "Ground.png");
    ASSERT_FALSE(groundGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Field.scene"});
    EXPECT_NE(manifest.FindByGuid(groundGuid), nullptr) << "the asset a #id field names did not ship";
}

TEST_F(BuildSceneCollection, ResourceWithAStaleGuidShipsByItsPath)
{
    // The load tries a resource's GUID first and its path when the registry does not
    // know the GUID (the asset was deleted and recreated, or the database rebuilt), so
    // the editor and the packaged Player load the prefab by its path. It has to ship.
    WriteTextFile(m_ProjectAssets / "Textures" / "Well.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Prefabs" / "Well.prefab", Prefab("Well", "Textures/Well.png"));
    WriteTextFile(m_ProjectAssets / "Scenes" / "Lake.scene", "[scene name=\"Lake\" version=1]\n");
    Mount();
    const GUID wellGuid = GuidOf(m_ProjectAssets / "Prefabs" / "Well.prefab");
    const GUID wellTextureGuid = GuidOf(m_ProjectAssets / "Textures" / "Well.png");
    ASSERT_FALSE(wellGuid.IsNull());
    ASSERT_FALSE(wellTextureGuid.IsNull());
    const GUID staleGuid("0badc0de-0000-4000-8000-000000000001");
    ASSERT_NE(staleGuid, wellGuid);
    WriteTextFile(m_ProjectAssets / "Scenes" / "Lake.scene",
                  "[scene name=\"Lake\" version=1]\n"
                  "[resource id=\"well\" path=\"Prefabs/Well.prefab\" guid=\"" + staleGuid.ToString() + "\"]\n"
                  "\n"
                  "[prefab id=\"well_0\" source=\"well\"]\n"
                  "Transform.position = (0, 0, 0)\n");

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Lake.scene"});

    EXPECT_NE(manifest.FindByGuid(wellGuid), nullptr) << "the prefab the load finds by path did not ship";
    EXPECT_NE(manifest.FindByGuid(wellTextureGuid), nullptr) << "what the spawned prefab names did not ship";
    // The stale GUID itself names nothing, and the build still reports it.
    ASSERT_EQ(manifest.unresolvedDependencies.size(), 1u);
    EXPECT_EQ(manifest.unresolvedDependencies[0], staleGuid);
}

TEST_F(BuildSceneCollection, FieldWithAStaleGuidShipsByItsPath)
{
    // The same fallback for a `[path=... guid=...]` field value (TryResolveAssetReference).
    WriteTextFile(m_ProjectAssets / "Textures" / "Ground.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Scenes" / "Field.scene", "[scene name=\"Field\" version=1]\n");
    Mount();
    const GUID groundGuid = GuidOf(m_ProjectAssets / "Textures" / "Ground.png");
    ASSERT_FALSE(groundGuid.IsNull());
    WriteTextFile(m_ProjectAssets / "Scenes" / "Field.scene",
                  "[scene name=\"Field\" version=1]\n"
                  "\n"
                  "[entity id=\"e_1\"]\n"
                  "MeshRenderer.material = [path=\"Textures/Ground.png\" guid=\"0badc0de-0000-4000-8000-000000000002\"]\n");

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Field.scene"});
    EXPECT_NE(manifest.FindByGuid(groundGuid), nullptr) << "the asset the load finds by path did not ship";
}

TEST_F(BuildSceneCollection, InstanceOverridesBeforeAnyEntityShip)
{
    // A scene made only of prefab instances: an override on an instance's root names
    // an asset the prefab does not, and nothing else in the scene names it.
    WriteTextFile(m_ProjectAssets / "Textures" / "Well.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Textures" / "Override.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Prefabs" / "Well.prefab", Prefab("Well", "Textures/Well.png"));
    WriteTextFile(m_ProjectAssets / "Scenes" / "Lake.scene",
                  "[scene name=\"Lake\" version=1]\n"
                  "[resource id=\"well\" path=\"Prefabs/Well.prefab\"]\n"
                  "\n"
                  "[prefab id=\"well_0\" source=\"well\"]\n"
                  "MeshRenderer.material = [path=\"Textures/Override.png\"]\n");
    Mount();
    const GUID overrideGuid = GuidOf(m_ProjectAssets / "Textures" / "Override.png");
    ASSERT_FALSE(overrideGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Lake.scene"});
    EXPECT_NE(manifest.FindByGuid(overrideGuid), nullptr) << "the asset an instance override names did not ship";
}

TEST_F(BuildSceneCollection, SheetImportedBesideTheImporterShips)
{
    // The runtime resolves an @import against the importing sheet's folder first, so a
    // sheet next to it ships; without it every rule of the theme is missing.
    WriteTextFile(m_ProjectAssets / "UI" / "Theme.css", ".panel { color: #fff; }\n");
    WriteTextFile(m_ProjectAssets / "UI" / "Menu.css", "@import \"Theme.css\";\n.menu { width: 10px; }\n");
    WriteTextFile(m_ProjectAssets / "Scenes" / "Menu.scene",
                  "[scene name=\"Menu\" version=1]\n"
                  "[entity id=\"ui\"]\n"
                  "Name.value = \"UI\"\n"
                  "UIDocument.Style = [path=\"UI/Menu.css\"]\n");
    Mount();
    const GUID menuGuid = GuidOf(m_ProjectAssets / "UI" / "Menu.css");
    const GUID themeGuid = GuidOf(m_ProjectAssets / "UI" / "Theme.css");
    ASSERT_FALSE(menuGuid.IsNull());
    ASSERT_FALSE(themeGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Menu.scene"});

    ASSERT_NE(manifest.FindByGuid(menuGuid), nullptr) << "the sheet the scene names did not ship";
    const AssetManifestEntry* theme = manifest.FindByGuid(themeGuid);
    ASSERT_NE(theme, nullptr) << "the sheet Menu.css imports did not ship";
    EXPECT_EQ(theme->outputPath.generic_string(), "Assets/UI/Theme.css");
}

TEST_F(BuildSceneCollection, SheetImportedByAssetPathFromASubfolderShips)
{
    // Not beside the importer: the runtime resolves the @import through the asset mounts.
    WriteTextFile(m_ProjectAssets / "UI" / "Theme.css", ".panel { color: #fff; }\n");
    WriteTextFile(m_ProjectAssets / "UI" / "Menus" / "Menu.css",
                  "@import \"UI/Theme.css\";\n.menu { width: 10px; }\n");
    WriteTextFile(m_ProjectAssets / "Scenes" / "Menu.scene",
                  "[scene name=\"Menu\" version=1]\n"
                  "[entity id=\"ui\"]\n"
                  "Name.value = \"UI\"\n"
                  "UIDocument.Style = [path=\"UI/Menus/Menu.css\"]\n");
    Mount();
    const GUID themeGuid = GuidOf(m_ProjectAssets / "UI" / "Theme.css");
    ASSERT_FALSE(themeGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Menu.scene"});

    EXPECT_NE(manifest.FindByGuid(themeGuid), nullptr) << "the sheet Menu.css imports by asset path did not ship";
}

TEST_F(BuildSceneCollection, ImportByUrlShipsOnlyTheSheetTheRuntimeLoads)
{
    // The runtime loads the sheet beside the importer. The same name at the asset root does not ship.
    WriteTextFile(m_ProjectAssets / "Theme.css", ".root { color: #000; }\n");
    WriteTextFile(m_ProjectAssets / "UI" / "Theme.css", ".panel { color: #fff; }\n");
    WriteTextFile(m_ProjectAssets / "UI" / "Menu.css", "@import url(Theme.css);\n.menu { width: 10px; }\n");
    WriteTextFile(m_ProjectAssets / "Scenes" / "Menu.scene",
                  "[scene name=\"Menu\" version=1]\n"
                  "[entity id=\"ui\"]\n"
                  "Name.value = \"UI\"\n"
                  "UIDocument.Style = [path=\"UI/Menu.css\"]\n");
    Mount();
    const GUID besideGuid = GuidOf(m_ProjectAssets / "UI" / "Theme.css");
    const GUID rootGuid = GuidOf(m_ProjectAssets / "Theme.css");
    ASSERT_FALSE(besideGuid.IsNull());
    ASSERT_FALSE(rootGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Menu.scene"});

    EXPECT_NE(manifest.FindByGuid(besideGuid), nullptr) << "the sheet the runtime imports did not ship";
    EXPECT_EQ(manifest.FindByGuid(rootGuid), nullptr) << "a sheet the runtime does not import shipped";
}

TEST_F(BuildSceneCollection, ImportsAndIncludesReadBeforeTheWalkShip)
{
    // Edges read with no current manager resolve next to the file only. The walk reads them again
    // with its manager, so an import and an include under the asset root still ship.
    WriteTextFile(m_ProjectAssets / "UI" / "Theme.css", ".panel { color: #fff; }\n");
    WriteTextFile(m_ProjectAssets / "UI" / "Menus" / "Menu.css",
                  "@import \"UI/Theme.css\";\n.menu { width: 10px; }\n");
    WriteTextFile(m_ProjectAssets / "Shared" / "Noise.glsl", "float Noise() { return 0.5; }\n");
    WriteTextFile(m_ProjectAssets / "Materials" / "Surfaces" / "Stone.glsl",
                  "#include \"Shared/Noise.glsl\"\nvoid EvaluateSurface() {}\n");
    WriteTextFile(m_ProjectAssets / "Materials" / "Stone.mat",
                  "{ \"surfaceShader\": \"Materials/Surfaces/Stone.glsl\" }\n");
    WriteTextFile(m_ProjectAssets / "Scenes" / "Menu.scene",
                  "[scene name=\"Menu\" version=1]\n"
                  "[entity id=\"ui\"]\n"
                  "Name.value = \"UI\"\n"
                  "UIDocument.Style = [path=\"UI/Menus/Menu.css\"]\n"
                  "[entity id=\"rock\"]\n"
                  "Name.value = \"Rock\"\n"
                  "MeshRenderer.material = [path=\"Materials/Stone.mat\"]\n");
    Mount();
    const GUID menuGuid = GuidOf(m_ProjectAssets / "UI" / "Menus" / "Menu.css");
    const GUID surfaceGuid = GuidOf(m_ProjectAssets / "Materials" / "Surfaces" / "Stone.glsl");
    const GUID themeGuid = GuidOf(m_ProjectAssets / "UI" / "Theme.css");
    const GUID noiseGuid = GuidOf(m_ProjectAssets / "Shared" / "Noise.glsl");
    ASSERT_FALSE(themeGuid.IsNull());
    ASSERT_FALSE(noiseGuid.IsNull());
    ASSERT_EQ(AssetManager::GetThreadCurrent(), nullptr);
    m_AssetManager.GetRegistry().GetDependencies(menuGuid);
    m_AssetManager.GetRegistry().GetDependencies(surfaceGuid);

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Menu.scene"});

    EXPECT_NE(manifest.FindByGuid(themeGuid), nullptr) << "the sheet Menu.css imports by asset path did not ship";
    EXPECT_NE(manifest.FindByGuid(noiseGuid), nullptr) << "the include under the asset root did not ship";
}

TEST_F(BuildSceneCollection, IncludesOfAMaterialSurfaceShip)
{
    // A surface includes a file next to it, and a file by its path under the asset root: the
    // compiler's includer finds both, so both ship.
    WriteTextFile(m_ProjectAssets / "Materials" / "Surfaces" / "Relief.glsl", "float Relief() { return 1.0; }\n");
    WriteTextFile(m_ProjectAssets / "Shared" / "Noise.glsl", "float Noise() { return 0.5; }\n");
    WriteTextFile(m_ProjectAssets / "Materials" / "Surfaces" / "Stone.glsl",
                  "#include \"Relief.glsl\"\n#include \"Shared/Noise.glsl\"\n#include \"Missing.glsl\"\n"
                  "void EvaluateSurface() {}\n");
    WriteTextFile(m_ProjectAssets / "Materials" / "Stone.mat",
                  "{ \"surfaceShader\": \"Materials/Surfaces/Stone.glsl\" }\n");
    WriteTextFile(m_ProjectAssets / "Scenes" / "Lake.scene",
                  "[scene name=\"Lake\" version=1]\n"
                  "[entity id=\"rock\"]\n"
                  "Name.value = \"Rock\"\n"
                  "MeshRenderer.material = [path=\"Materials/Stone.mat\"]\n");
    Mount();
    const GUID surfaceGuid = GuidOf(m_ProjectAssets / "Materials" / "Surfaces" / "Stone.glsl");
    const GUID reliefGuid = GuidOf(m_ProjectAssets / "Materials" / "Surfaces" / "Relief.glsl");
    const GUID noiseGuid = GuidOf(m_ProjectAssets / "Shared" / "Noise.glsl");
    ASSERT_FALSE(reliefGuid.IsNull());
    ASSERT_FALSE(noiseGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Lake.scene"});

    ASSERT_NE(manifest.FindByGuid(surfaceGuid), nullptr) << "the surface the material names did not ship";
    EXPECT_NE(manifest.FindByGuid(reliefGuid), nullptr) << "the include next to the surface did not ship";
    EXPECT_NE(manifest.FindByGuid(noiseGuid), nullptr) << "the include under the asset root did not ship";
}
