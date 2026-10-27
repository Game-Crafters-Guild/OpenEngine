#include "Engine/UI/FontResolver.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetRegistry.h"
#include "TestTempDir.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Engine::UI;
namespace fs = std::filesystem;

namespace
{

class FontResolverTests : public ::testing::Test
{
  protected:
    void SetUp() override { ASSERT_TRUE(m_Registry.Initialize()); }
    void TearDown() override { m_Registry.Shutdown(); }

    AssetMetadata RegisterFont(const char* filename, const char* name)
    {
        const fs::path path = m_Root.Path() / filename;
        std::ofstream file(path, std::ios::binary);
        file << "font bytes";
        file.close();
        EXPECT_TRUE(file.good());

        AssetMetadata metadata(GUID::Generate(), AssetType::Font, path);
        metadata.Name = name;
        EXPECT_TRUE(m_Registry.RegisterAssetMetadata(metadata));
        AssetMetadata registered;
        EXPECT_TRUE(m_Registry.TryGetAssetMetadata(metadata.Guid, registered));
        EXPECT_EQ(registered.Name, name);
        EXPECT_TRUE(fs::exists(registered.Path));
        return registered;
    }

    void ExpectResolved(const FontFamilyIndex& index, const AssetMetadata& font,
                        const std::string& family, int weight = 400,
                        FontStyle style = FontStyle::Normal)
    {
        fs::path resolved;
        GUID guid;
        ASSERT_TRUE(index.TryResolveFontAssetPath(family, weight, style, resolved, nullptr, &guid))
            << family;
        EXPECT_EQ(resolved, font.Path);
        EXPECT_EQ(guid, font.Guid);
    }

    const TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("ge_font_family")};
    AssetRegistry m_Registry;
};

TEST_F(FontResolverTests, SpacedFamilyUsesPreservedAssetName)
{
    const auto font = RegisterFont("robotomono-regular.ttf", "RobotoMono-Regular");
    ASSERT_EQ(font.Path.filename().string(), "robotomono-regular.ttf");
    FontFamilyIndex index(m_Registry);
    index.Rebuild();

    ExpectResolved(index, font, "Roboto Mono");
    ExpectResolved(index, font, "  \"ROBOTO   MONO\"  ");
    ExpectResolved(index, font, "RobotoMono");
    ExpectResolved(index, font, "RobotoMono-Regular");
}

TEST_F(FontResolverTests, MixedCasePathResolvesTheSameFamilyAliases)
{
    const auto font = RegisterFont("RobotoMono-Regular.ttf", "RobotoMono-Regular");
    FontFamilyIndex index(m_Registry);
    index.Rebuild();

    ExpectResolved(index, font, "Roboto Mono");
    ExpectResolved(index, font, "RobotoMono");
    ExpectResolved(index, font, "RobotoMono-Regular");
}

TEST_F(FontResolverTests, SpacedFamilySelectsTheRequestedWeightAndStyle)
{
    const std::array fonts{
        RegisterFont("robotomono-regular.ttf", "RobotoMono-Regular"),
        RegisterFont("robotomono-bold.ttf", "RobotoMono-Bold"),
        RegisterFont("robotomono-italic.ttf", "RobotoMono-Italic"),
        RegisterFont("robotomono-bolditalic.ttf", "RobotoMono-BoldItalic")};
    FontFamilyIndex index(m_Registry);
    index.Rebuild();

    ExpectResolved(index, fonts[0], "Roboto Mono", 400, FontStyle::Normal);
    ExpectResolved(index, fonts[1], "Roboto Mono", 700, FontStyle::Normal);
    ExpectResolved(index, fonts[2], "Roboto Mono", 400, FontStyle::Italic);
    ExpectResolved(index, fonts[3], "Roboto Mono", 700, FontStyle::Italic);
}

TEST_F(FontResolverTests, ResolverReadsTheRegisteredFileForASpacedFamily)
{
    const auto font = RegisterFont("robotomono-regular.ttf", "RobotoMono-Regular");
    FontResolver resolver(m_Registry);
    resolver.SetAllowSystemFonts(false);
    EXPECT_TRUE(resolver.TryIsFamilyResolvable("Roboto Mono"));

    FontBytes bytes;
    int callbackCount = 0;
    resolver.ResolveAsync("Roboto Mono", 400, FontStyle::Normal, FontVariant::Normal,
                          [&bytes, &callbackCount](FontBytes result)
                          {
                              bytes = std::move(result);
                              ++callbackCount;
                          });

    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(std::string(bytes.bytes.begin(), bytes.bytes.end()), "font bytes");
    EXPECT_EQ(bytes.assetGuid, font.Guid);
    EXPECT_EQ(bytes.faceIndex, 0);
}

TEST_F(FontResolverTests, AssetNameKeepsDotsInTheFamily)
{
    const auto font = RegisterFont("example.font-regular.ttf", "Example.Font-Regular");
    FontFamilyIndex index(m_Registry);
    index.Rebuild();

    ExpectResolved(index, font, "Example.Font");
    fs::path resolved;
    EXPECT_FALSE(index.TryResolveFontAssetPath("Example", 400, FontStyle::Normal, resolved));
}

TEST_F(FontResolverTests, UnrelatedWordBoundariesDoNotMatch)
{
    RegisterFont("robotomono-regular.ttf", "RobotoMono-Regular");
    FontFamilyIndex index(m_Registry);
    index.Rebuild();

    fs::path resolved;
    EXPECT_FALSE(index.TryResolveFontAssetPath("Robo to Mono", 400, FontStyle::Normal, resolved));
}

} // namespace
