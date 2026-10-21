#include <gtest/gtest.h>

#include <string>
#include <vector>
#include "LogCapture.h"

#include "Animation/HumanBone.h"
#include "Animation/HumanoidRig.h"
#include "Animation/RetargetMap.h"

#include "AssetCore/GUID.h"

#include <filesystem>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

void FillMap(RetargetMap& map)
{
    map.SetSourceRigRef(GUID("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));
    map.SetTargetRigRef(GUID("11111111-2222-3333-4444-555555555555"));

    auto& cm = map.ChainMapMutable();
    {
        ChainPairing cp;
        cp.Kind = ChainKind::LeftArm;
        cp.FK.RotationMode = FKRotationMode::Transport;
        cp.FK.RotationAlpha = 0.75f;
        cp.FK.TranslationMode = FKTranslationMode::PerBoneScale;
        cp.IK.Enabled = true;
        cp.IK.BlendToSource = 0.5f;
        cp.IK.Extension = 0.1f;
        cp.FootLock.Enabled = false;
        cm.push_back(cp);
    }
    {
        ChainPairing cp;
        cp.Kind = ChainKind::LeftLeg;
        cp.FK.RotationMode = FKRotationMode::OneToOne;
        cp.IK.Enabled = true;
        cp.IK.BlendToSource = 1.0f;
        cp.FootLock.Enabled = true;
        cp.FootLock.SpeedThreshold = 0.08f;
        cp.FootLock.LockBlend = 0.9f;
        cm.push_back(cp);
    }

    auto& os = map.OpStackMutable();
    {
        OpStackEntry e;
        e.OpName = "FootLockOp";
        e.Params = nlohmann::json{ {"foot", "left"}, {"snapToFloor", true}, {"floorOffset", 0.01f} };
        os.push_back(e);
    }
    {
        OpStackEntry e;
        e.OpName = "AttachmentPassthroughOp";
        e.Params = nlohmann::json{ {"bones", nlohmann::json::array({"CloakAnchor", "HairTip"})} };
        os.push_back(e);
    }
}

} // namespace

TEST(RetargetMapTest, FKRotationModeStringRoundTrip)
{
    EXPECT_EQ(FKRotationModeFromString("Transport"), FKRotationMode::Transport);
    EXPECT_EQ(FKRotationModeFromString("SlerpAlongArc"), FKRotationMode::SlerpAlongArc);
    EXPECT_EQ(FKRotationModeFromString("garbage"), FKRotationMode::OneToOne);
    EXPECT_STREQ(FKRotationModeToString(FKRotationMode::SlerpAlongArc), "SlerpAlongArc");
}

TEST(RetargetMapTest, FKTranslationModeStringRoundTrip)
{
    EXPECT_EQ(FKTranslationModeFromString("PerBoneScale"), FKTranslationMode::PerBoneScale);
    EXPECT_EQ(FKTranslationModeFromString("UniformChainScale"), FKTranslationMode::UniformChainScale);
    EXPECT_EQ(FKTranslationModeFromString("garbage"), FKTranslationMode::None);
}

TEST(RetargetMapTest, RoundTripPreservesAllFields)
{
    RetargetMap original(GUID(), std::filesystem::path("test://example.retargetmap.json"));
    FillMap(original);

    Vector<uint8> bytes;
    ASSERT_TRUE(original.SaveToData(bytes));

    RetargetMap loaded(GUID(), std::filesystem::path("test://example.retargetmap.json"));
    ASSERT_TRUE(loaded.LoadFromData(bytes));

    EXPECT_EQ(original.SourceRigRef().ToString(), loaded.SourceRigRef().ToString());
    EXPECT_EQ(original.TargetRigRef().ToString(), loaded.TargetRigRef().ToString());

    ASSERT_EQ(original.ChainMap().size(), loaded.ChainMap().size());
    for (size_t i = 0; i < original.ChainMap().size(); ++i)
    {
        const auto& a = original.ChainMap()[i];
        const auto& b = loaded.ChainMap()[i];
        EXPECT_EQ(a.Kind, b.Kind);
        EXPECT_EQ(a.FK.RotationMode, b.FK.RotationMode);
        EXPECT_FLOAT_EQ(a.FK.RotationAlpha, b.FK.RotationAlpha);
        EXPECT_EQ(a.FK.TranslationMode, b.FK.TranslationMode);
        EXPECT_EQ(a.IK.Enabled, b.IK.Enabled);
        EXPECT_FLOAT_EQ(a.IK.BlendToSource, b.IK.BlendToSource);
        EXPECT_FLOAT_EQ(a.IK.Extension, b.IK.Extension);
        EXPECT_EQ(a.FootLock.Enabled, b.FootLock.Enabled);
        EXPECT_FLOAT_EQ(a.FootLock.SpeedThreshold, b.FootLock.SpeedThreshold);
        EXPECT_FLOAT_EQ(a.FootLock.LockBlend, b.FootLock.LockBlend);
    }

    ASSERT_EQ(original.OpStack().size(), loaded.OpStack().size());
    for (size_t i = 0; i < original.OpStack().size(); ++i)
    {
        EXPECT_EQ(original.OpStack()[i].OpName, loaded.OpStack()[i].OpName);
        // Params survive as nlohmann::json: compare via canonical dumps.
        EXPECT_EQ(original.OpStack()[i].Params.dump(), loaded.OpStack()[i].Params.dump());
    }
}

TEST(RetargetMapTest, IsBrokenWhenSourceRigRefIsNull)
{
    RetargetMap map(GUID(), std::filesystem::path("test://x.retargetmap.json"));
    EXPECT_TRUE(map.IsBroken()); // both null
    map.SetTargetRigRef(GUID("11111111-2222-3333-4444-555555555555"));
    EXPECT_TRUE(map.IsBroken()); // source still null
    map.SetSourceRigRef(GUID("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));
    EXPECT_FALSE(map.IsBroken());
}

TEST(RetargetMapTest, OpStackParamsSurviveRoundTrip)
{
    RetargetMap m(GUID(), std::filesystem::path("test://op.retargetmap.json"));
    m.SetSourceRigRef(GUID("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));
    m.SetTargetRigRef(GUID("11111111-2222-3333-4444-555555555555"));

    OpStackEntry e;
    e.OpName = "WeirdOp";
    e.Params = nlohmann::json{
        {"nested", nlohmann::json{ {"deep", nlohmann::json::array({1, 2, 3}) } }},
        {"flag", true},
        {"text", "hello"}
    };
    m.OpStackMutable().push_back(std::move(e));

    Vector<uint8> bytes;
    ASSERT_TRUE(m.SaveToData(bytes));

    RetargetMap loaded(GUID(), std::filesystem::path("test://op.retargetmap.json"));
    ASSERT_TRUE(loaded.LoadFromData(bytes));

    ASSERT_EQ(loaded.OpStack().size(), 1u);
    EXPECT_EQ(loaded.OpStack()[0].OpName, "WeirdOp");
    EXPECT_EQ(loaded.OpStack()[0].Params.value("flag", false), true);
    EXPECT_EQ(loaded.OpStack()[0].Params.value("text", std::string()), "hello");
    auto inner = loaded.OpStack()[0].Params.value("nested", nlohmann::json{});
    ASSERT_TRUE(inner.contains("deep"));
    EXPECT_EQ(inner["deep"].size(), 3u);
}

// TODO Phase 3: integration test for "hot-reload during evaluation race"
// requires HumanoidRetargetSystem (added in Phase 3) to drive the eviction
// path. Currently exercising only the asset-side serialization contract.

// A retarget map that is not JSON, or whose field holds a value of the wrong type, is refused with
// the file and the place or the field named, and never throws: the web build has no exception
// catching.
TEST(RetargetMapTest, AnUnreadableFileIsRefusedNamingThePlaceOrTheField)
{
    std::vector<std::string> lines;
    bool truncatedLoaded = true;
    bool mistypedLoaded = true;
    {
        TestLog::ScopedLogCapture capture(&lines);
        const std::string truncatedText = R"({"name": [)";
        const std::string mistypedText = R"({"chainMap": [{"kind": "Spine", "ik": {"enabled": "yes"}}]})";
        RetargetMap truncated(GUID(), std::filesystem::path("Animations/Truncated.retargetmap.json"));
        truncatedLoaded = truncated.LoadFromData(Vector<uint8>(truncatedText.begin(), truncatedText.end()));
        RetargetMap mistyped(GUID(), std::filesystem::path("Animations/Mistyped.retargetmap.json"));
        mistypedLoaded = mistyped.LoadFromData(Vector<uint8>(mistypedText.begin(), mistypedText.end()));
    }
    EXPECT_FALSE(truncatedLoaded);
    EXPECT_FALSE(mistypedLoaded);
    EXPECT_NE(TestLog::FirstLineContaining(
                  lines, "'Animations/Truncated.retargetmap.json' is not loaded: it is not JSON (parse error at line 1, column"),
              "");
    EXPECT_NE(TestLog::FirstLineContaining(lines, "'Animations/Mistyped.retargetmap.json' is not loaded: 'chainMap[0].ik.enabled' is a string; write it as true or false"), "");
}
