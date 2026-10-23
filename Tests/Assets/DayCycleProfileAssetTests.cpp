// The Day Cycle profile asset (.daycycle.json): the extension reaches the type, the
// loader reads the declared tracks and refuses what the format does not hold, a profile written out
// reads back the same, and a reload publishes a new profile while a reader holds the old one.

#include <gtest/gtest.h>

#include "AssetCore/AssetTypes.h"
#include "Assets/DayCycleProfile.h"

#include "TestTempDir.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace GameEngine;

namespace
{

const GUID kProfileGuid("6d1f3a52-8b47-4c0e-a9d3-5e7f9b2c4a18");

// The Default profile's dusk dip: 0 at -11 degrees, -1.5 stops at -6, 0 at sunset, Smooth and flat.
constexpr const char* kDuskDip = R"({
  "name": "Dusk",
  "description": "The dip through civil twilight.",
  "tracks": { "exposureOffset": { "keys": "-11,0,0,0,1,1;-6,-1.5,0,0,1,1;0,0,0,0,1,1" } }
})";

std::filesystem::path WriteProfile(const char* directory, const std::string& text)
{
    const std::filesystem::path dir = TestUtils::MakeUniqueTempDirectory(directory);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "Dusk.daycycle.json";
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

std::string RefusalOf(const std::string& text)
{
    DayCycleProfileData data;
    std::string error;
    EXPECT_FALSE(ParseDayCycleProfile(text, data, error)) << text;
    return error;
}

} // namespace

TEST(DayCycleProfileAsset, TheCompoundExtensionReachesTheType)
{
    EXPECT_EQ(GetAssetTypeFromExtension(GetCompoundExtensionFromPath("DayCycles/Dusk.daycycle.json")),
              AssetType::DayCycleProfile);
}

// An asset's name takes a compound extension off whole, and a plain one as before.
TEST(DayCycleProfileAsset, TheAssetNameTakesTheCompoundExtensionOffWhole)
{
    EXPECT_EQ(GetAssetNameFromPath("DayCycles/Marsh.daycycle.json"), "Marsh");
    EXPECT_EQ(GetAssetNameFromPath("C:\\Project\\Rigs\\Hero.profile.json"), "Hero");
    EXPECT_EQ(GetAssetNameFromPath("Textures/Rock.PNG"), "Rock");
    EXPECT_EQ(GetAssetNameFromPath("Notes/README"), "README");
    EXPECT_EQ(GetAssetNameFromPath(".gitignore"), ".gitignore") << "a name that is all extension stays whole";
}

TEST(DayCycleProfileAsset, LoadsADeclaredTrack)
{
    DayCycleProfile asset(kProfileGuid, WriteProfile("ge_daycycle_load", kDuskDip));
    ASSERT_TRUE(asset.Load());
    const std::shared_ptr<const DayCycleProfileData> data = asset.GetData();
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->Name, "Dusk");
    EXPECT_EQ(data->Description, "The dip through civil twilight.");
    const DayCycleTrackKeys* exposure = data->Find(DayCycleTrack::ExposureOffset);
    ASSERT_NE(exposure, nullptr);
    ASSERT_EQ(exposure->Keys.KeyCount, 3u);
    EXPECT_FLOAT_EQ(exposure->Keys.Keys[1].Time, -6.0f);
    EXPECT_FLOAT_EQ(exposure->Keys.Keys[1].Value, -1.5f);
    EXPECT_EQ(exposure->Keys.Keys[1].Interp, Math::CurveInterp::Smooth);
    EXPECT_EQ(exposure->Keys.Keys[1].TangentMode, Math::CurveTangentMode::Flat);
    EXPECT_FALSE(exposure->SeparateDusk);
}

TEST(DayCycleProfileAsset, RefusesATrackTheTableDoesNotDeclareAndNamesIt)
{
    const std::string error =
        RefusalOf(R"({ "name": "Marsh", "tracks": { "fogDensity": { "keys": "0,1,0,0,1,1" } } })");
    EXPECT_NE(error.find("'fogDensity' is not a Day Cycle track"), std::string::npos) << error;
    EXPECT_NE(error.find("exposureOffset"), std::string::npos) << "the refusal lists the declared tracks: " << error;
}

TEST(DayCycleProfileAsset, RefusesKeysThatAreNotInAscendingElevation)
{
    const std::string error =
        RefusalOf(R"({ "tracks": { "exposureOffset": { "keys": "-6,-1.5,0,0,1,1;-11,0,0,0,1,1" } } })");
    EXPECT_NE(error.find("not in ascending elevation"), std::string::npos) << error;
}

// The format has one version and no version field: a file that declares one is another format.
TEST(DayCycleProfileAsset, RefusesASecondFormatVersion)
{
    const std::string error = RefusalOf(R"({ "version": 2, "name": "Dusk", "tracks": {} })");
    EXPECT_NE(error.find("'version'"), std::string::npos) << error;
}

TEST(DayCycleProfileAsset, AWrittenProfileReadsBackTheSame)
{
    DayCycleProfileData written;
    written.Name = "Split dusk";
    written.Description = "A dawn and a dusk of their own.";
    DayCycleTrackKeys& track = written.Tracks.emplace_back();
    track.Track = DayCycleTrack::ExposureOffset;
    track.Keys.TryInsert({-11.0f, 0.0f, 0.0f, 0.0f, Math::CurveInterp::Smooth, Math::CurveTangentMode::Flat});
    track.Keys.TryInsert({-6.0f, -1.5f, 0.25f, -0.125f, Math::CurveInterp::Smooth, Math::CurveTangentMode::Broken});
    track.SeparateDusk = true;
    track.DuskKeys.TryInsert({-18.0f, -0.5f, 0.0f, 0.0f, Math::CurveInterp::Linear, Math::CurveTangentMode::Auto});
    track.DuskKeys.TryInsert({3.0f, 0.1f, 0.0f, 0.0f, Math::CurveInterp::Step, Math::CurveTangentMode::Auto});

    DayCycleProfileData read;
    std::string error;
    ASSERT_TRUE(ParseDayCycleProfile(WriteDayCycleProfile(written), read, error)) << error;
    EXPECT_EQ(read.Name, written.Name);
    EXPECT_EQ(read.Description, written.Description);
    ASSERT_EQ(read.Tracks.size(), 1u);
    const DayCycleTrackKeys& back = read.Tracks.front();
    EXPECT_TRUE(back.SeparateDusk);
    ASSERT_EQ(back.Keys.KeyCount, track.Keys.KeyCount);
    ASSERT_EQ(back.DuskKeys.KeyCount, track.DuskKeys.KeyCount);
    for (uint8 i = 0; i < track.Keys.KeyCount; ++i)
    {
        EXPECT_EQ(back.Keys.Keys[i].Time, track.Keys.Keys[i].Time);
        EXPECT_EQ(back.Keys.Keys[i].Value, track.Keys.Keys[i].Value);
        EXPECT_EQ(back.Keys.Keys[i].InTangent, track.Keys.Keys[i].InTangent);
        EXPECT_EQ(back.Keys.Keys[i].OutTangent, track.Keys.Keys[i].OutTangent);
        EXPECT_EQ(back.Keys.Keys[i].Interp, track.Keys.Keys[i].Interp);
        EXPECT_EQ(back.Keys.Keys[i].TangentMode, track.Keys.Keys[i].TangentMode);
    }
    for (uint8 i = 0; i < track.DuskKeys.KeyCount; ++i)
    {
        EXPECT_EQ(back.DuskKeys.Keys[i].Time, track.DuskKeys.Keys[i].Time);
        EXPECT_EQ(back.DuskKeys.Keys[i].Value, track.DuskKeys.Keys[i].Value);
        EXPECT_EQ(back.DuskKeys.Keys[i].Interp, track.DuskKeys.Keys[i].Interp);
    }
}

// A reader on another thread keeps reading whatever profile it took while the file is reloaded under
// it: every reload publishes a new object, the one held is never changed or freed, and a file that
// does not parse leaves the last good profile published.
TEST(DayCycleProfileAsset, AReloadPublishesANewProfileWhileAReaderHoldsTheOld)
{
    const std::filesystem::path path = WriteProfile("ge_daycycle_reload", kDuskDip);
    DayCycleProfile asset(kProfileGuid, path);
    ASSERT_TRUE(asset.Load());
    const std::shared_ptr<const DayCycleProfileData> held = asset.GetData();
    ASSERT_NE(held, nullptr);

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::atomic<bool> readerSawNull{false};
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire))
        {
            const std::shared_ptr<const DayCycleProfileData> now = asset.GetData();
            if (!now || now->Tracks.empty())
                readerSawNull.store(true);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    constexpr int kReloads = 50;
    for (int i = 0; i < kReloads; ++i)
    {
        std::string text = kDuskDip;
        text.replace(text.find("\"Dusk\""), 6, "\"Dusk " + std::to_string(i) + "\"");
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        ASSERT_EQ(asset.Reload(), ReloadOutcome::Reloaded) << "reload " << i;
    }
    std::ofstream(path, std::ios::binary | std::ios::trunc) << R"({ "tracks": { "fog": {} } })";
    EXPECT_EQ(asset.Reload(), ReloadOutcome::Failed);

    stop.store(true, std::memory_order_release);
    reader.join();

    EXPECT_GT(reads.load(), 0);
    EXPECT_FALSE(readerSawNull.load()) << "a reload left no profile published while a reader looked";
    EXPECT_EQ(held->Name, "Dusk") << "the profile a reader held changed under it";
    const std::shared_ptr<const DayCycleProfileData> latest = asset.GetData();
    ASSERT_NE(latest, nullptr);
    EXPECT_NE(latest.get(), held.get());
    EXPECT_EQ(latest->Name, "Dusk " + std::to_string(kReloads - 1)) << "the refused file replaced the last good one";
}

// Each key field is one number, read once: a field that is not a whole finite number, an elevation
// outside -90 to 90 degrees, and an interpolation or tangent mode outside its list are refused, and
// the refusal names the key and the field.
TEST(DayCycleProfileAsset, RefusesAKeyFieldThatIsNotAFiniteNumberInItsRange)
{
    const struct
    {
        const char* Keys;
        const char* Named;
    } cases[] = {
        {"-10abc,1;5,2", "key 1's elevation"},
        {"0x10,1;20,2", "key 1's elevation"},
        {"+10,1;20,2", "key 1's elevation"},
        {"0,abc", "key 1's value"},
        {"0,nan", "key 1's value"},
        {"0,1e39", "key 1's value"},
        {"-200,0;200,1", "outside -90 to 90"},
        {"0,1,0,0,7,1", "key 1's interpolation"},
        {"0,1,0,0,1,9", "key 1's tangentMode"},
        {"0,1,0,0,1,1,5", "7 fields"},
    };
    for (const auto& c : cases)
    {
        const std::string error = RefusalOf(std::string(R"({ "tracks": { "exposureOffset": { "keys": ")") + c.Keys +
                                            "\" } } }");
        EXPECT_NE(error.find(c.Named), std::string::npos) << c.Keys << ": " << error;
    }
}

// JSON leaves a repeated key to the reader; the profile refuses it, so a track never silently
// loses one of its two definitions.
TEST(DayCycleProfileAsset, RefusesATrackNamedTwice)
{
    const std::string error = RefusalOf(R"({ "tracks": { "exposureOffset": { "keys": "0,0" },
                                                          "exposureOffset": { "keys": "0,-1" } } })");
    EXPECT_NE(error.find("'exposureOffset' is written twice"), std::string::npos) << error;
}
