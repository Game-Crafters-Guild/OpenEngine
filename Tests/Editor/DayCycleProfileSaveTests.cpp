// The shipped Day Cycle profiles and the save rule, over a real asset manager with a project source
// and an editor source holding the shipped profiles: a save writes only into the project source, and
// a shipped profile offers a copy saved into the project.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/DayCycleProfile.h"
#include "DayCycle/DayCycleProfileSave.h"
#include "DayCycle/DayCycleShippedProfiles.h"

#include "StagedTestPaths.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Editor;

namespace
{
constexpr float kRadiansPerDegree = 0.017453292519943295f;

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

class DayCycleProfileSaveTests : public ::testing::Test
{
  protected:
    std::filesystem::path Root;
    AssetManager Assets;

    void SetUp() override
    {
        Root = TestUtils::MakeUniqueTempDirectory("ge_daycycle_save");
        std::error_code ec;
        std::filesystem::create_directories(Root / "Project", ec);
        std::filesystem::create_directories(Root / "Editor" / "DayCycles", ec);
        for (const char* file : {"Default.daycycle.json", "ClearDay.daycycle.json"})
            std::filesystem::copy_file(TestPaths::ExecutableDirectory() / "Assets" / "DayCycles" / file,
                                       Root / "Editor" / "DayCycles" / file, ec);
        ASSERT_TRUE(Assets.Initialize(Root / "Project", nullptr, Root / "AssetDatabase.assetdb", Root / "Cache"));
        AssetSourceDesc editor{};
        editor.Alias = "editor";
        editor.Root = Root / "Editor";
        ASSERT_TRUE(Assets.RegisterSource(editor));
    }

    void TearDown() override
    {
        Assets.Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(Root, ec);
    }

    std::filesystem::path ShippedDefault() const { return Root / "Editor" / "DayCycles" / "Default.daycycle.json"; }

    DayCycleProfileData Read(const std::filesystem::path& path) const
    {
        DayCycleProfileData data;
        std::string error;
        EXPECT_TRUE(ParseDayCycleProfile(ReadText(path), data, error)) << path << ": " << error;
        return data;
    }
};
} // namespace

TEST_F(DayCycleProfileSaveTests, TheShippedDefaultDipsThroughCivilTwilightAndClearDayIsIdentity)
{
    const DayCycleProfileData dusk = Read(ShippedDefault());
    const DayCycleTrackKeys* exposure = dusk.Find(DayCycleTrack::ExposureOffset);
    ASSERT_NE(exposure, nullptr);
    EXPECT_NEAR(EvaluateDayCycleTrack(*exposure, 0.0f, true), 0.0f, 1e-5f);
    EXPECT_NEAR(EvaluateDayCycleTrack(*exposure, -6.0f * kRadiansPerDegree, true), -1.5f, 1e-5f);
    EXPECT_NEAR(EvaluateDayCycleTrack(*exposure, -11.0f * kRadiansPerDegree, true), 0.0f, 1e-5f);
    EXPECT_NEAR(EvaluateDayCycleTrack(*exposure, 30.0f * kRadiansPerDegree, true), 0.0f, 1e-5f);
    EXPECT_NEAR(EvaluateDayCycleTrack(*exposure, -40.0f * kRadiansPerDegree, true), 0.0f, 1e-5f);

    const DayCycleProfileData clear = Read(Root / "Editor" / "DayCycles" / "ClearDay.daycycle.json");
    const DayCycleTrackKeys* flat = clear.Find(DayCycleTrack::ExposureOffset);
    ASSERT_NE(flat, nullptr) << "Clear day keys the exposure, so it cancels a dusk below it";
    for (const float degrees : {-60.0f, -6.0f, 0.0f, 45.0f})
        EXPECT_NEAR(EvaluateDayCycleTrack(*flat, degrees * kRadiansPerDegree, false), 0.0f, 1e-5f);
    ASSERT_FALSE(ShippedDayCycleProfiles(Assets).empty());
    EXPECT_EQ(ShippedDayCycleProfiles(Assets).front().filename(), "Default.daycycle.json");
}

TEST_F(DayCycleProfileSaveTests, AShippedProfileOffersOnlyACopyToTheProject)
{
    EXPECT_EQ(DayCycleSaveActionFor(Assets, ShippedDefault()), DayCycleSaveAction::SaveCopyToProject);
    const std::string before = ReadText(ShippedDefault());
    DayCycleProfileData data = Read(ShippedDefault());
    data.Tracks.front().Keys.Keys[1].Value = -2.0f;

    std::string error;
    EXPECT_FALSE(SaveDayCycleProfile(Assets, ShippedDefault(), data, error));
    EXPECT_NE(error.find("Save a copy to the project"), std::string::npos) << error;
    EXPECT_EQ(ReadText(ShippedDefault()), before) << "a save wrote into the editor source";

    const std::filesystem::path copy = SaveDayCycleProfileCopy(Assets, data, error);
    ASSERT_FALSE(copy.empty()) << error;
    EXPECT_EQ(copy.parent_path(), Root / "Project" / "DayCycles");
    EXPECT_EQ(DayCycleSaveActionFor(Assets, copy), DayCycleSaveAction::SaveInPlace);
    EXPECT_FLOAT_EQ(Read(copy).Tracks.front().Keys.Keys[1].Value, -2.0f);
}

TEST_F(DayCycleProfileSaveTests, AProjectProfileSavesInPlace)
{
    const std::filesystem::path path = Root / "Project" / "DayCycles" / "Marsh.daycycle.json";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary) << ReadText(ShippedDefault());
    EXPECT_EQ(DayCycleSaveActionFor(Assets, path), DayCycleSaveAction::SaveInPlace);

    DayCycleProfileData data = Read(path);
    data.Tracks.front().Keys.Keys[1].Value = -0.75f;
    std::string error;
    ASSERT_TRUE(SaveDayCycleProfile(Assets, path, data, error)) << error;
    EXPECT_FLOAT_EQ(Read(path).Tracks.front().Keys.Keys[1].Value, -0.75f);
}
