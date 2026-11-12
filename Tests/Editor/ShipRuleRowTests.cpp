// The Inspector's Ship in builds row: what it shows for a selection (an asset's own rule, or the
// folder rule it inherits and where that comes from), and the undoable write that sets or clears
// an asset's asset database key or a folder's .assetfolder file.

#include "Inspectors/ShipRules/SetShipRuleCommand.h"
#include "Inspectors/ShipRules/ShipRuleSelection.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Types/PathUtils.h"

#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Editor;
namespace fs = std::filesystem;

namespace
{

void WriteTextFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string ReadTextFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

class ShipRuleRowTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
        // A mount with its own store: an asset's rule is a kv key there, as in a project.
        s_Project = TestUtils::MakeUniqueTempDirectory("ship_rule_row");
        s_Root = s_Project / "Assets";
        WriteTextFile(s_Root / "Data" / ShipRules::kFolderFileName, R"({"ship": "always"})");
        WriteTextFile(s_Root / "Data" / "units.json", "{}");
        WriteTextFile(s_Root / "Data" / "maps.json", "{}");
        AssetSourceDesc source{};
        source.Alias = "shiprulerow";
        source.Root = s_Root;
        source.AuthoritativeDbFile = s_Project / "AssetDatabase.assetdb";
        source.RegisterFileWatcher = false;
        ASSERT_TRUE(engine.GetAssetManager().RegisterSource(source));
        for (const char* file : {"units.json", "maps.json"})
            ASSERT_FALSE(engine.GetAssetManager().ResolveAssetGuid(s_Root / "Data" / file).IsNull()) << file;
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_Project, ec);
    }

    static AssetRegistry& Registry() { return EngineCore::GetInstance().GetAssetManager().GetRegistry(); }

    static ShipRuleSelection::State Describe(const std::vector<fs::path>& paths)
    {
        ShipRuleSelection selection;
        EXPECT_TRUE(ShipRuleSelection::TryRead(EngineCore::GetInstance().GetAssetManager(), s_Root, paths, selection));
        return selection.Describe();
    }

    static bool TryGetOwnRule(const fs::path& asset, std::string& outStored)
    {
        return Registry().TryGetMetaValue(asset, std::string(ShipRules::kAssetMetaKey), outStored);
    }

    static fs::path s_Project;
    static fs::path s_Root;
};

fs::path ShipRuleRowTests::s_Project;
fs::path ShipRuleRowTests::s_Root;

TEST_F(ShipRuleRowTests, AnAssetSetHereWritesItsKeyAndUndoLeavesNoKey)
{
    const fs::path units = s_Root / "Data" / "units.json";
    ShipRuleSelection::State state = Describe({units});
    EXPECT_EQ(state.SelectedIndex, -1);
    EXPECT_EQ(state.NoSelectionText, "Always (from Data/)");
    EXPECT_FALSE(state.AnyOwn);

    SetShipRuleCommand setNever({units}, ShipRule::Never, nullptr);
    setNever.Do();
    std::string stored;
    ASSERT_TRUE(TryGetOwnRule(units, stored));
    EXPECT_EQ(stored, "never");
    state = Describe({units});
    EXPECT_EQ(state.SelectedIndex, 2);
    EXPECT_EQ(state.Caption, "Set here. Folder Data/ says Always.");
    EXPECT_TRUE(state.AnyOwn);

    // No rule before the command is no key after its undo, not an empty value.
    setNever.Undo();
    EXPECT_FALSE(TryGetOwnRule(units, stored)) << stored;
    EXPECT_EQ(Describe({units}).NoSelectionText, "Always (from Data/)");

    setNever.Redo();
    SetShipRuleCommand clear({units}, std::nullopt, nullptr);
    clear.Do();
    EXPECT_FALSE(TryGetOwnRule(units, stored)) << stored;
    clear.Undo();
    ASSERT_TRUE(TryGetOwnRule(units, stored));
    EXPECT_EQ(stored, "never");
    setNever.Undo();
}

TEST_F(ShipRuleRowTests, AFileMadeThisSessionHasARow)
{
    // The fixture's mount has no file watcher, so the registry does not hold the new file.
    const fs::path made = s_Root / "Loose" / "made_now.json";
    WriteTextFile(made, "{}");
    ASSERT_TRUE(Registry().GetAssetGUID(made).IsNull());
    EXPECT_EQ(Describe({made}).NoSelectionText, "Inferred (default)");
    EXPECT_FALSE(Registry().GetAssetGUID(made).IsNull());
}

TEST_F(ShipRuleRowTests, AFolderRuleIsItsFolderFileAndClearingDeletesIt)
{
    const fs::path data = s_Root / "Data";
    const fs::path folderFile = data / ShipRules::kFolderFileName;
    ShipRuleSelection::State state = Describe({data});
    EXPECT_EQ(state.SelectedIndex, 1);
    EXPECT_EQ(state.Caption, "Set here. The default is Inferred.");

    SetShipRuleCommand clear({data}, std::nullopt, nullptr);
    clear.Do();
    EXPECT_FALSE(fs::exists(folderFile));
    EXPECT_EQ(Describe({data / "units.json"}).NoSelectionText, "Inferred (default)");
    clear.Undo();
    EXPECT_EQ(ReadTextFile(folderFile), R"({"ship": "always"})");
}

TEST_F(ShipRuleRowTests, AMixedSelectionCountsEachRuleAndSetsAllInOneStep)
{
    const std::vector<fs::path> both = {s_Root / "Data" / "units.json", s_Root / "Data" / "maps.json"};
    SetShipRuleCommand setNever({both[0]}, ShipRule::Never, nullptr);
    setNever.Do();
    ShipRuleSelection::State state = Describe(both);
    EXPECT_EQ(state.NoSelectionText, "Mixed");
    EXPECT_EQ(state.Caption, "1 Always, 1 Never.");

    SetShipRuleCommand setAlways(both, ShipRule::Always, nullptr);
    setAlways.Do();
    EXPECT_EQ(Describe(both).SelectedIndex, 1);
    setAlways.Undo();
    std::string stored;
    ASSERT_TRUE(TryGetOwnRule(both[0], stored));
    EXPECT_EQ(stored, "never");
    EXPECT_FALSE(TryGetOwnRule(both[1], stored)) << stored;
    setNever.Undo();
}

// The row reads a folder rule by its UTF-8 path, so a folder name that is not ASCII gives its rule.
TEST_F(ShipRuleRowTests, AFolderWithANameThatIsNotAsciiAndAnAssetInItShowTheFolderRule)
{
    const fs::path folder(u8"\u4E2D");
    const std::string folderName = PathToUtf8(folder);
    const fs::path asset = s_Root / folder / "wall.json";
    WriteTextFile(s_Root / folder / ShipRules::kFolderFileName, R"({"ship": "never"})");
    WriteTextFile(asset, "{}");
    EXPECT_EQ(Describe({s_Root / folder}).SelectedIndex, 2);
#if defined(_WIN32)
    GTEST_SKIP() << "asset registration converts paths through the code page (#4246)";
#else
    EXPECT_EQ(Describe({asset}).NoSelectionText, "Never (from " + folderName + "/)");

    SetShipRuleCommand setAlways({asset}, ShipRule::Always, nullptr);
    setAlways.Do();
    EXPECT_EQ(Describe({asset}).Caption, "Set here. Folder " + folderName + "/ says Never.");
    setAlways.Undo();
#endif
}
