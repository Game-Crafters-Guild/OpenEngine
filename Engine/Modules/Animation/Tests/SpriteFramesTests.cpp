#include "Animation/SpriteFrames.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Animation;

TEST(SpriteFrames, LoadsAndSamplesFrames)
{
    const std::string text = R"json({
      "schemaVersion": 1,
      "assetType": "SpriteFrames",
      "animations": [{
        "name": "Run",
        "loop": true,
        "frames": [
          {"name": "A", "textureGuid": "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", "durationSeconds": 0.1},
          {"name": "B", "textureGuid": "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb", "durationSeconds": 0.1}
        ]
      }]
    })json";

    SpriteFrames frames(GUID(), std::filesystem::path("test://run.spriteframes"));
    Vector<uint8> data(text.begin(), text.end());
    ASSERT_TRUE(frames.LoadFromData(data));

    const SpriteFrame* frame = frames.Sample("Run", 0.15f);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->Name, "B");

    frame = frames.Sample("Run", 0.25f);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->Name, "A");
}

// A sprite frames file that is not JSON, or whose field holds a value of the wrong type, is refused with
// the file and the place or the field named, and never throws: the web build has no exception
// catching.
TEST(SpriteFrames, AnUnreadableFileIsRefusedNamingThePlaceOrTheField)
{
    std::vector<std::string> lines;
    bool truncatedLoaded = true;
    bool mistypedLoaded = true;
    bool arrayLoaded = true;
    {
        TestLog::ScopedLogCapture capture(&lines);
        const std::string truncatedText = R"({"name": [)";
        const std::string mistypedText = R"({"animations": [{"name": "Walk", "frames": [{"durationSeconds": "fast"}]}]})";
        SpriteFrames truncated(GUID(), std::filesystem::path("Animations/Truncated.spriteframes"));
        truncatedLoaded = truncated.LoadFromData(Vector<uint8>(truncatedText.begin(), truncatedText.end()));
        SpriteFrames mistyped(GUID(), std::filesystem::path("Animations/Mistyped.spriteframes"));
        mistypedLoaded = mistyped.LoadFromData(Vector<uint8>(mistypedText.begin(), mistypedText.end()));
        const std::string arrayText = "[]";
        SpriteFrames array(GUID(), std::filesystem::path("Animations/Array.spriteframes"));
        arrayLoaded = array.LoadFromData(Vector<uint8>(arrayText.begin(), arrayText.end()));
    }
    EXPECT_FALSE(truncatedLoaded);
    EXPECT_FALSE(mistypedLoaded);
    EXPECT_FALSE(arrayLoaded);
    EXPECT_NE(TestLog::FirstLineContaining(lines, "'Animations/Array.spriteframes' is not loaded: its JSON is not an object"), "");
    EXPECT_NE(TestLog::FirstLineContaining(
                  lines, "'Animations/Truncated.spriteframes' is not loaded: it is not JSON (parse error at line 1, column"),
              "");
    EXPECT_NE(TestLog::FirstLineContaining(lines, "'Animations/Mistyped.spriteframes' is not loaded: 'animations[0].frames[0].durationSeconds' is a string; write it as a number"), "");
}
