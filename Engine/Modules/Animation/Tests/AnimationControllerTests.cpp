#include "Animation/AnimationController.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Animation;

TEST(AnimationController, LoadsStatesParametersAndTransitions)
{
    const std::string text = R"json({
      "schemaVersion": 1,
      "assetType": "AnimationController",
      "entryState": "Idle",
      "parameters": [
        {"name": "Speed", "type": "Float", "defaultFloat": 0.0},
        {"name": "Grounded", "type": "Bool", "defaultBool": true}
      ],
      "states": [
        {"name": "Idle", "motion": {"assetGuid": "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", "type": "Animation", "speed": 1.0, "loop": true}},
        {"name": "Move", "blendTree": {"type": "Blend1D", "parameterX": "Speed", "children": [
          {"thresholdX": 0.0, "motion": {"assetGuid": "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb", "type": "Animation"}},
          {"thresholdX": 1.0, "motion": {"assetGuid": "cccccccc-cccc-cccc-cccc-cccccccccccc", "type": "Animation"}}
        ]}}
      ],
      "transitions": [
        {"fromState": "Idle", "toState": "Move", "durationSeconds": 0.15, "conditions": [
          {"parameter": "Speed", "comparison": "Greater", "floatValue": 0.1}
        ]}
      ]
    })json";

    AnimationController controller(GUID(), std::filesystem::path("test://controller.animcontroller"));
    Vector<uint8> data(text.begin(), text.end());
    ASSERT_TRUE(controller.LoadFromData(data));

    EXPECT_EQ(controller.EntryState(), "Idle");
    ASSERT_EQ(controller.Parameters().size(), 2u);
    EXPECT_EQ(controller.Parameters()[1].Type, AnimationParameterType::Bool);
    ASSERT_NE(controller.FindState("Move"), nullptr);
    EXPECT_EQ(controller.FindState("Move")->BlendTree.Type, BlendTreeType::Blend1D);
    EXPECT_EQ(controller.FindState("Move")->BlendTree.Children.size(), 2u);
    ASSERT_EQ(controller.Transitions().size(), 1u);
    EXPECT_EQ(controller.Transitions()[0].Conditions[0].Comparison, AnimationTransitionComparison::Greater);
}

TEST(AnimationController, LoadsEmptyStubWithoutFakeIdle)
{
    const std::string text = R"json({
      "schemaVersion": 1,
      "assetType": "AnimationController",
      "entryState": "",
      "parameters": [],
      "states": [],
      "transitions": []
    })json";

    AnimationController controller(GUID(), std::filesystem::path("test://empty.animcontroller"));
    Vector<uint8> data(text.begin(), text.end());
    ASSERT_TRUE(controller.LoadFromData(data));
    EXPECT_TRUE(controller.EntryState().empty());
    EXPECT_TRUE(controller.Parameters().empty());
    EXPECT_TRUE(controller.States().empty());
    EXPECT_TRUE(controller.Transitions().empty());
}

// A controller that is not JSON, or whose field holds a value of the wrong type, is refused with
// the file and the place or the field named, and never throws: the web build has no exception
// catching.
TEST(AnimationController, AnUnreadableFileIsRefusedNamingThePlaceOrTheField)
{
    std::vector<std::string> lines;
    bool truncatedLoaded = true;
    bool mistypedLoaded = true;
    bool arrayLoaded = true;
    {
        TestLog::ScopedLogCapture capture(&lines);
        const std::string truncatedText = R"({"name": [)";
        const std::string mistypedText = R"({"transitions": [{"fromState": "Idle", "toState": "Run", "canInterrupt": "no"}]})";
        AnimationController truncated(GUID(), std::filesystem::path("Animations/Truncated.animcontroller"));
        truncatedLoaded = truncated.LoadFromData(Vector<uint8>(truncatedText.begin(), truncatedText.end()));
        AnimationController mistyped(GUID(), std::filesystem::path("Animations/Mistyped.animcontroller"));
        mistypedLoaded = mistyped.LoadFromData(Vector<uint8>(mistypedText.begin(), mistypedText.end()));
        const std::string arrayText = "[]";
        AnimationController array(GUID(), std::filesystem::path("Animations/Array.animcontroller"));
        arrayLoaded = array.LoadFromData(Vector<uint8>(arrayText.begin(), arrayText.end()));
    }
    EXPECT_FALSE(truncatedLoaded);
    EXPECT_FALSE(mistypedLoaded);
    EXPECT_FALSE(arrayLoaded);
    EXPECT_NE(TestLog::FirstLineContaining(lines, "'Animations/Array.animcontroller' is not loaded: its JSON is not an object"), "");
    EXPECT_NE(TestLog::FirstLineContaining(
                  lines, "'Animations/Truncated.animcontroller' is not loaded: it is not JSON (parse error at line 1, column"),
              "");
    EXPECT_NE(TestLog::FirstLineContaining(lines, "'Animations/Mistyped.animcontroller' is not loaded: 'transitions[0].canInterrupt' is a string; write it as true or false"), "");
}
