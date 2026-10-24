// The Render Layers page's registration shape and its rows' refusals and writes, against a temporary
// workspace: the 32 name rows, the placeholders, a refused duplicate with its message, and a rename that
// reaches the project file and the names a game reads.

#include "Editor/Settings/RenderLayersSettingsPage.h"

#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Engine/Rendering/RenderLayerNames.h"
#include "Rendering/RenderLayersScriptApi.h"

#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

namespace ed = GameEngine::Editor;
namespace gr = GameEngine::Rendering;
namespace fs = std::filesystem;

namespace
{

class RenderLayersSettingsPageTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_PreviousRoot = GameEngine::EngineCore::GetInstance().GetWorkspaceRoot();
        m_Root = GameEngine::TestUtils::MakeUniqueTempDirectory("ge_render_layers_page");
        fs::create_directories(m_Root / ".Editor");
        GameEngine::EngineCore::GetInstance().SetWorkspaceRoot(m_Root);
        ed::RegisterRenderLayersSettingsCategory();
        ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("renderLayers", m_Page));
    }
    void TearDown() override
    {
        GameEngine::EngineCore::GetInstance().SetWorkspaceRoot(m_PreviousRoot);
        gr::SetRenderLayerNames({});
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    void AuthorNames(const std::string& names) const
    {
        std::ofstream file(m_Root / ".Editor" / "ProjectSettings.json");
        file << "{\n  \"schemaVersion\": 1,\n  \"rendering\": {\"lodMode\": \"sse\", \"renderLayerNames\": " << names
             << "}\n}\n";
    }

    const ed::SettingsFieldDescriptor::StringField& Row(size_t layer) const
    {
        return std::get<ed::SettingsFieldDescriptor::StringField>(m_Page.Fields[layer].Control);
    }

    fs::path m_PreviousRoot;
    fs::path m_Root;
    ed::SettingsCategoryDescriptor m_Page;
};

TEST_F(RenderLayersSettingsPageTest, ThirtyTwoNameRowsRegisterUnderProjectSettings)
{
    EXPECT_EQ(m_Page.Title, "Render Layers");
    EXPECT_EQ(m_Page.Group, ed::SettingsCategoryGroup::ProjectSettings);
    EXPECT_NE(m_Page.Description.find("Show Render Layer"), std::string::npos)
        << "the page points at the hierarchy for where a layer is used";
    ASSERT_EQ(m_Page.Fields.size(), 32u);
    for (size_t layer = 0; layer < m_Page.Fields.size(); ++layer)
    {
        EXPECT_EQ(m_Page.Fields[layer].Label, "Layer " + std::to_string(layer));
        ASSERT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::StringField>(m_Page.Fields[layer].Control));
        EXPECT_TRUE(static_cast<bool>(Row(layer).Validate));
    }
    EXPECT_EQ(Row(0).Placeholder, "Default");
    EXPECT_EQ(Row(5).Placeholder, "Layer 5");
}

TEST_F(RenderLayersSettingsPageTest, ARowRefusesADuplicateWithItsMessage)
{
    AuthorNames(R"(["", "Props"])");
    EXPECT_EQ(Row(4).Validate("Props"), "'Props' is already layer 1.");
    EXPECT_EQ(Row(1).Validate("Props"), "") << "a row may keep its own name";
    EXPECT_NE(Row(4).Validate(std::string(64, 'x')).find("at most 63 bytes"), std::string::npos);
    EXPECT_NE(Row(4).Validate("  ").find("only spaces"), std::string::npos);
    EXPECT_EQ(Row(4).Validate("Layer 1"), "'Layer 1' is the default name of layer 1.")
        << "a named layer's default name stays taken";
    EXPECT_EQ(Row(1).Get(), "Props");
    EXPECT_EQ(Row(0).Get(), "") << "an unnamed layer shows its placeholder, not a value";
}

TEST_F(RenderLayersSettingsPageTest, ARenameReachesTheProjectFileAndTheGameReads)
{
    AuthorNames(R"(["", "Props"])");
    Row(2).Set("Foliage");

    std::ifstream file(m_Root / ".Editor" / "ProjectSettings.json");
    const nlohmann::json root = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    ASSERT_TRUE(root.contains("rendering"));
    EXPECT_EQ(root["rendering"]["renderLayerNames"], nlohmann::json::parse(R"(["", "Props", "Foliage"])"));
    EXPECT_EQ(root["rendering"]["lodMode"], "sse") << "the other rendering keys stay";
    EXPECT_EQ(gr::Script::Find("Foliage"), gr::RenderLayerMask::FromLayer(2));
    EXPECT_EQ(gr::Script::NameOf(gr::RenderLayerMask::FromLayer(1)), "Props");
}

} // namespace
