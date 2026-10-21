// RenderLayerNames: the project's render layer names, their two transports, and the two reads a game
// calls. Fixtures are raw JSON text, never output of the writer under test.

#include "Engine/Build/GameConfig.h"
#include "Engine/Rendering/RenderLayerNames.h"
#include "Engine/Rendering/RenderLayers.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace gr = GameEngine::Rendering;

namespace
{

class RenderLayerNamesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() /
                 ("ge_render_layer_names_" +
                  std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line()));
        std::filesystem::remove_all(m_Root);
        std::filesystem::create_directories(m_Root / ".Editor");
    }
    void TearDown() override
    {
        gr::SetRenderLayerNames({});
        std::filesystem::remove_all(m_Root);
    }

    void AuthorProjectSettings(const std::string& renderingBody) const
    {
        std::ofstream file(m_Root / ".Editor" / "ProjectSettings.json");
        ASSERT_TRUE(file.is_open());
        file << "{\n  \"schemaVersion\": 1,\n  \"rendering\": " << renderingBody << "\n}\n";
    }

    std::filesystem::path m_Root;
};

TEST_F(RenderLayerNamesTest, ReadsTheAuthoredNamesAndWritesThemBack)
{
    const nlohmann::json rendering =
        nlohmann::json::parse(R"({"renderLayerNames": ["Ground", "Props", "", "Foliage"]})");
    const gr::RenderLayerNames names = gr::RenderLayerNames::ReadFrom(rendering);
    EXPECT_EQ(names.Authored(0), "Ground");
    EXPECT_EQ(names.Authored(1), "Props");
    EXPECT_EQ(names.Authored(2), "");
    EXPECT_EQ(names.Authored(3), "Foliage");

    nlohmann::json written = nlohmann::json::object();
    names.WriteTo(written);
    EXPECT_EQ(written, rendering) << "unnamed layers after the last named one are left out";
}

TEST_F(RenderLayerNamesTest, AnUnnamedLayerShowsItsFallbackName)
{
    const gr::RenderLayerNames names{};
    EXPECT_EQ(names.EffectiveName(0), "Default");
    EXPECT_EQ(names.EffectiveName(7), "Layer 7");
    EXPECT_EQ(names.EffectiveName(31), "Layer 31");

    nlohmann::json written = nlohmann::json::parse(R"({"renderLayerNames": ["Old"]})");
    names.WriteTo(written);
    EXPECT_FALSE(written.contains("renderLayerNames")) << "a table with no name writes no key";
}

TEST_F(RenderLayerNamesTest, RefusesADuplicateAnOverLongNameAndOnlySpaces)
{
    gr::RenderLayerNames names{};
    ASSERT_EQ(names.Set(1, "Props"), "");
    EXPECT_EQ(names.Set(4, "Props"), "'Props' is already layer 1.");
    EXPECT_EQ(names.Set(4, "Layer 2"), "'Layer 2' is the default name of layer 2.") << "a fallback name is taken too";
    EXPECT_NE(names.Set(4, std::string(64, 'a')).find("at most 63 bytes"), std::string::npos);
    EXPECT_EQ(names.Set(4, std::string(63, 'a')), "");
    EXPECT_NE(names.Set(5, "   ").find("only spaces"), std::string::npos);
    EXPECT_EQ(names.Authored(5), "");
    EXPECT_EQ(names.Set(1, "Props"), "") << "a layer keeps its own name";
    EXPECT_EQ(names.Set(1, ""), "") << "an empty name makes the layer unnamed";
    EXPECT_EQ(names.EffectiveName(1), "Layer 1");

    const nlohmann::json rendering =
        nlohmann::json::parse("{\"renderLayerNames\": [\"\", \"" + std::string(64, 'b') + "\", \"Props\", \"Props\"]}");
    const gr::RenderLayerNames read = gr::RenderLayerNames::ReadFrom(rendering);
    EXPECT_EQ(read.Authored(1), "") << "an over-long name in the file is left unnamed";
    EXPECT_EQ(read.Authored(2), "Props");
    EXPECT_EQ(read.Authored(3), "") << "a duplicate in the file is left unnamed";
}

// Another layer's default name stays refused while that layer has a name of its own, so clearing the name
// cannot leave two layers with one name.
TEST_F(RenderLayerNamesTest, ClearingANameNeverLeavesTwoLayersWithOneName)
{
    gr::RenderLayerNames names{};
    ASSERT_EQ(names.Set(2, "Crates"), "");
    EXPECT_EQ(names.Set(4, "Layer 2"), "'Layer 2' is the default name of layer 2.");
    ASSERT_EQ(names.Set(2, ""), "");
    EXPECT_EQ(names.EffectiveName(2), "Layer 2");
    EXPECT_EQ(names.EffectiveName(4), "Layer 4");
    EXPECT_EQ(names.Set(0, "Default"), "") << "a layer may take its own default name";
}

TEST_F(RenderLayerNamesTest, WritingKeepsTheSiblingRenderingKeys)
{
    nlohmann::json rendering = nlohmann::json::parse(R"({"lodMode": "coverage", "aaMode": "taa"})");
    gr::RenderLayerNames names{};
    ASSERT_EQ(names.Set(2, "Foliage"), "");
    names.WriteTo(rendering);
    EXPECT_EQ(rendering["lodMode"], "coverage");
    EXPECT_EQ(rendering["aaMode"], "taa");
    EXPECT_EQ(rendering["renderLayerNames"], nlohmann::json::parse(R"(["", "", "Foliage"])"));
}

TEST_F(RenderLayerNamesTest, GameConfigCarriesTheTableFromTheProjectFile)
{
    AuthorProjectSettings(R"({"lodMode": "sse", "renderLayerNames": ["", "Props"]})");

    GameEngine::GameConfig cooked;
    cooked.renderLayerNames = gr::RenderLayerNames::Load(m_Root);
    const std::filesystem::path configPath = m_Root / "game.config";
    ASSERT_TRUE(GameEngine::SaveGameConfig(configPath, cooked));

    {
        std::ifstream file(configPath);
        const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
        ASSERT_TRUE(doc.contains("rendering"));
        ASSERT_TRUE(doc["rendering"].contains("renderLayerNames"));
        EXPECT_EQ(doc["rendering"]["renderLayerNames"], nlohmann::json::parse(R"(["", "Props"])"));
    }

    const GameEngine::GameConfig shipped = GameEngine::LoadGameConfig(configPath);
    EXPECT_EQ(shipped.renderLayerNames.Authored(1), "Props");
}

TEST_F(RenderLayerNamesTest, TheReadsAnswerFromTheTableAndReturnCopies)
{
    gr::RenderLayerNames names{};
    ASSERT_EQ(names.Set(3, "Props"), "");
    gr::SetRenderLayerNames(names);

    EXPECT_EQ(gr::FindRenderLayer("Props"), 3u);
    EXPECT_EQ(gr::FindRenderLayer("Default"), 0u);
    EXPECT_EQ(gr::FindRenderLayer("Layer 9"), 9u);
    EXPECT_FALSE(gr::FindRenderLayer("props").has_value()) << "the match is case-sensitive";
    EXPECT_FALSE(gr::FindRenderLayer("Missing").has_value()) << "an unknown name is no layer, never layer 0";

    const std::string name = gr::RenderLayerName(3);
    const std::optional<GameEngine::uint32> found = gr::FindRenderLayer("Props");
    ASSERT_EQ(names.Set(3, "Crates"), "");
    gr::SetRenderLayerNames(names);
    EXPECT_EQ(name, "Props") << "a name read before a rename keeps its text";
    EXPECT_EQ(found, 3u);
    EXPECT_EQ(gr::RenderLayerName(3), "Crates");
    EXPECT_FALSE(gr::FindRenderLayer("Props").has_value());
}

} // namespace
