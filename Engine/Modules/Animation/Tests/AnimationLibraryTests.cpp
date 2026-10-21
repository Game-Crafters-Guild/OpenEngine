#include "Animation/AnimationLibrary.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Animation;

TEST(AnimationLibrary, LoadsNamedEntriesAndFindsByName)
{
    const std::string text = R"json({
      "schemaVersion": 1,
      "assetType": "AnimationLibrary",
      "animations": [
        {
          "name": "Idle",
          "assetGuid": "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
          "type": "Animation",
          "defaultBlendSeconds": 0.15,
          "loop": true
        },
        {
          "name": "Cinematic",
          "assetGuid": "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb",
          "type": "Timeline",
          "loop": false
        }
      ]
    })json";

    AnimationLibrary library(GUID(), std::filesystem::path("test://library.animlib"));
    Vector<uint8> data(text.begin(), text.end());
    ASSERT_TRUE(library.LoadFromData(data));

    const auto* idle = library.Find("Idle");
    ASSERT_NE(idle, nullptr);
    EXPECT_EQ(idle->AssetGuid, GUID("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"));
    EXPECT_EQ(idle->Type, AssetType::Animation);
    EXPECT_FLOAT_EQ(idle->DefaultBlendSeconds, 0.15f);
    EXPECT_TRUE(idle->Loop);

    const auto* cinematic = library.Find("Cinematic");
    ASSERT_NE(cinematic, nullptr);
    EXPECT_EQ(cinematic->Type, AssetType::Timeline);
    EXPECT_FALSE(cinematic->Loop);
    EXPECT_EQ(library.Find("Missing"), nullptr);
}

TEST(AnimationLibrary, SavesRoundTripData)
{
    AnimationLibrary library(GUID(), std::filesystem::path("test://library.animlib"));
    library.SetEntriesForTest({
        AnimationLibraryEntry{
            "Run",
            GUID("cccccccc-cccc-cccc-cccc-cccccccccccc"),
            AssetType::Animation,
            0.05f,
            true
        }
    });

    Vector<uint8> data;
    ASSERT_TRUE(library.SaveToData(data));

    AnimationLibrary loaded(GUID(), std::filesystem::path("test://loaded.animlib"));
    ASSERT_TRUE(loaded.LoadFromData(data));
    const auto* run = loaded.Find("Run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run->AssetGuid, GUID("cccccccc-cccc-cccc-cccc-cccccccccccc"));
    EXPECT_EQ(run->Type, AssetType::Animation);
    EXPECT_FLOAT_EQ(run->DefaultBlendSeconds, 0.05f);
}

// A library that is not JSON, or whose entry holds a value of the wrong type, is refused with the
// file and the place or the field named, and never throws: the web build has no exception catching.
TEST(AnimationLibrary, AnUnreadableLibraryIsRefusedNamingThePlaceOrTheField)
{
    std::vector<std::string> lines;
    bool truncatedLoaded = true;
    bool mistypedLoaded = true;
    bool arrayLoaded = true;
    {
        TestLog::ScopedLogCapture capture(&lines);
        const std::string truncatedText = R"({"clips": [)";
        const std::string mistypedText = R"({"animations": [{"name": "Idle", "loop": "yes"}]})";
        AnimationLibrary truncated(GUID(), std::filesystem::path("Animations/Truncated.animlib"));
        truncatedLoaded = truncated.LoadFromData(Vector<uint8>(truncatedText.begin(), truncatedText.end()));
        AnimationLibrary mistyped(GUID(), std::filesystem::path("Animations/Mistyped.animlib"));
        mistypedLoaded = mistyped.LoadFromData(Vector<uint8>(mistypedText.begin(), mistypedText.end()));
        const std::string arrayText = "[1, 2]";
        AnimationLibrary array(GUID(), std::filesystem::path("Animations/Array.animlib"));
        arrayLoaded = array.LoadFromData(Vector<uint8>(arrayText.begin(), arrayText.end()));
    }

    EXPECT_FALSE(truncatedLoaded);
    EXPECT_FALSE(mistypedLoaded);
    EXPECT_FALSE(arrayLoaded);
    EXPECT_NE(TestLog::FirstLineContaining(lines, "'Animations/Truncated.animlib' is not loaded: it is not JSON (parse error at line 1, column"),
              "");
    EXPECT_NE(TestLog::FirstLineContaining(lines, "'Animations/Mistyped.animlib' is not loaded: 'animations[0].loop' is a string; write it as true or false"),
              "");
    EXPECT_NE(TestLog::FirstLineContaining(lines, "'Animations/Array.animlib' is not loaded: its JSON is not an object"),
              "");
}

// A refusal quotes the text the parse last read, as much as a log line can carry: an unterminated
// string runs to the end of the file, and its whole text would make one line of the file's size.
TEST(AnimationLibrary, ARefusalQuotesAShortExcerptOfAnUnterminatedString)
{
    constexpr size_t kMebibyte = 1024u * 1024u;
    const std::string text = R"({"animations": [{"name": ")" + std::string(kMebibyte, 'x');
    std::vector<std::string> lines;
    bool loaded = true;
    {
        TestLog::ScopedLogCapture capture(&lines);
        AnimationLibrary unterminated(GUID(), std::filesystem::path("Animations/Unterminated.animlib"));
        loaded = unterminated.LoadFromData(Vector<uint8>(text.begin(), text.end()));
    }

    EXPECT_FALSE(loaded);
    const std::string refusal =
        TestLog::FirstLineContaining(lines, "'Animations/Unterminated.animlib' is not loaded: it is not JSON");
    ASSERT_NE(refusal, "");
    EXPECT_LT(refusal.size(), 400u) << refusal.substr(0, 400);
    EXPECT_NE(refusal.find("bytes)"), std::string::npos) << refusal.substr(0, 400);
}
