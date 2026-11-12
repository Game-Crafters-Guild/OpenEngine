#include <gtest/gtest.h>

#include <string>
#include <vector>
#include "LogCapture.h"

#include "Animation/HumanBone.h"
#include "Animation/Skeleton/SkeletonProfile.h"

#include "AssetCore/GUID.h"

#include <filesystem>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

// Build a hand-authored 5-bone profile that exercises every field we serialize.
void FillFiveBoneProfile(SkeletonProfile& profile)
{
    profile.SetNameForTest("FiveBone");
    profile.SetDescriptionForTest("Smoke-test profile for round-trip checks");

    std::vector<ProfileBone> bones;

    {
        ProfileBone b;
        b.Bone = HumanBone::Hips;
        b.Parent = HumanBone::None;
        b.RestTranslation = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
        b.RestRotation = Mathematics::Quaternion::Identity();
        b.RestScale = Mathematics::Vector3(1.0f, 1.0f, 1.0f);
        b.LocalForward = Mathematics::Vector3(0.0f, 0.0f, 1.0f);
        b.LocalUp = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
        bones.push_back(b);
    }
    {
        ProfileBone b;
        b.Bone = HumanBone::Spine;
        b.Parent = HumanBone::Hips;
        b.RestTranslation = Mathematics::Vector3(0.0f, 0.15f, 0.0f);
        b.RestRotation = Mathematics::Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
        b.RestScale = Mathematics::Vector3(1.0f, 1.0f, 1.0f);
        b.LocalForward = Mathematics::Vector3(0.0f, 0.0f, 1.0f);
        b.LocalUp = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
        bones.push_back(b);
    }
    {
        ProfileBone b;
        b.Bone = HumanBone::Chest;
        b.Parent = HumanBone::Spine;
        b.RestTranslation = Mathematics::Vector3(0.0f, 0.18f, 0.0f);
        b.RestRotation = Mathematics::Quaternion(0.9659f, 0.0f, 0.2588f, 0.0f); // tiny yaw
        b.RestScale = Mathematics::Vector3(1.0f, 1.0f, 1.0f);
        b.LocalForward = Mathematics::Vector3(0.0f, 0.0f, 1.0f);
        b.LocalUp = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
        bones.push_back(b);
    }
    {
        ProfileBone b;
        b.Bone = HumanBone::Neck;
        b.Parent = HumanBone::Chest;
        b.RestTranslation = Mathematics::Vector3(0.0f, 0.20f, 0.0f);
        b.RestRotation = Mathematics::Quaternion::Identity();
        bones.push_back(b);
    }
    {
        ProfileBone b;
        b.Bone = HumanBone::Head;
        b.Parent = HumanBone::Neck;
        b.RestTranslation = Mathematics::Vector3(0.0f, 0.10f, 0.0f);
        b.RestRotation = Mathematics::Quaternion::Identity();
        bones.push_back(b);
    }

    profile.SetBonesForTest(std::move(bones));
}

bool BonesEqual(const ProfileBone& a, const ProfileBone& b)
{
    if (a.Bone != b.Bone) return false;
    if (a.Parent != b.Parent) return false;
    auto vec3eq = [](const Mathematics::Vector3& x, const Mathematics::Vector3& y)
    {
        return x.x == y.x && x.y == y.y && x.z == y.z;
    };
    if (!vec3eq(a.RestTranslation, b.RestTranslation)) return false;
    if (!vec3eq(a.RestScale, b.RestScale)) return false;
    if (!vec3eq(a.LocalForward, b.LocalForward)) return false;
    if (!vec3eq(a.LocalUp, b.LocalUp)) return false;
    const glm::quat& qa = a.RestRotation.GetGLM();
    const glm::quat& qb = b.RestRotation.GetGLM();
    return qa.w == qb.w && qa.x == qb.x && qa.y == qb.y && qa.z == qb.z;
}

/// Loads a profile of one Hips bone with `fields` added to it; the log lines go to `lines`.
bool LoadHipsBone(SkeletonProfile& profile, const std::string& fields, std::vector<std::string>& lines)
{
    TestLog::ScopedLogCapture capture(&lines);
    const std::string text = R"({"bones": [{"bone": "Hips", )" + fields + "}]}";
    return profile.LoadFromData(Vector<uint8>(text.begin(), text.end()));
}

} // namespace

TEST(SkeletonProfileTest, HumanBoneStringRoundTrip)
{
    EXPECT_STREQ(HumanBoneToString(HumanBone::None), "None");
    EXPECT_STREQ(HumanBoneToString(HumanBone::Hips), "Hips");
    EXPECT_STREQ(HumanBoneToString(HumanBone::RightLittleDistal), "RightLittleDistal");

    EXPECT_EQ(HumanBoneFromString("None"), HumanBone::None);
    EXPECT_EQ(HumanBoneFromString("Hips"), HumanBone::Hips);
    EXPECT_EQ(HumanBoneFromString("RightLittleDistal"), HumanBone::RightLittleDistal);
    EXPECT_EQ(HumanBoneFromString("not-a-real-bone"), HumanBone::None);
}

TEST(SkeletonProfileTest, HumanBoneCountIs56)
{
    // 1 sentinel + 24 body + 30 fingers = 55 named values; Count == 56
    // (one past the last enumerated bone, RightLittleDistal == 55).
    EXPECT_EQ(static_cast<int>(HumanBone::Count), 56);
    EXPECT_EQ(static_cast<int>(HumanBone::RightLittleDistal), 55);
}

TEST(SkeletonProfileTest, RoundTripByteExact)
{
    SkeletonProfile original(GUID(), std::filesystem::path("test://five-bone.profile.json"));
    FillFiveBoneProfile(original);

    std::vector<uint8> bytes;
    ASSERT_TRUE(original.SaveToData(bytes));
    ASSERT_FALSE(bytes.empty());

    SkeletonProfile loaded(GUID(), std::filesystem::path("test://five-bone.profile.json"));
    ASSERT_TRUE(loaded.LoadFromData(bytes));

    // Re-serialize and compare bytes for byte-exact round-trip.
    std::vector<uint8> bytes2;
    ASSERT_TRUE(loaded.SaveToData(bytes2));
    EXPECT_EQ(bytes.size(), bytes2.size());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), bytes2.begin()));

    // And per-field structural compare. Version is always set to the
    // current schema (kSchemaVersion) by the serializer regardless of the
    // in-memory version, so both the original and loaded versions must
    // match the on-disk schema version after one round-trip.
    EXPECT_EQ(original.Name(), loaded.Name());
    EXPECT_EQ(original.Description(), loaded.Description());
    EXPECT_EQ(loaded.Version(), SkeletonProfile::kSchemaVersion);
    ASSERT_EQ(original.Bones().size(), loaded.Bones().size());
    for (size_t i = 0; i < original.Bones().size(); ++i)
    {
        EXPECT_TRUE(BonesEqual(original.Bones()[i], loaded.Bones()[i]))
            << "bone index " << i;
    }
}

TEST(SkeletonProfileTest, MissingVersionDefaultsToZero)
{
    SkeletonProfile p(GUID(), std::filesystem::path("test://no-version.profile.json"));
    const std::string text = R"({"name":"x","bones":[]})";
    Vector<uint8> bytes(text.begin(), text.end());
    ASSERT_TRUE(p.LoadFromData(bytes));
    EXPECT_EQ(p.Version(), 0);
}

TEST(SkeletonProfileTest, BumpedVersionLoadsAnyway)
{
    // Bumped version (newer than supported) logs a warning but parses.
    SkeletonProfile p(GUID(), std::filesystem::path("test://future.profile.json"));
    const std::string text = R"({"version":99,"name":"future","bones":[]})";
    Vector<uint8> bytes(text.begin(), text.end());
    ASSERT_TRUE(p.LoadFromData(bytes));
    EXPECT_EQ(p.Version(), 99);
    EXPECT_EQ(p.Name(), "future");
}

TEST(SkeletonProfileTest, FindBoneReturnsNullForMissing)
{
    SkeletonProfile profile(GUID(), std::filesystem::path("test://five-bone.profile.json"));
    FillFiveBoneProfile(profile);
    EXPECT_NE(profile.FindBone(HumanBone::Hips), nullptr);
    EXPECT_EQ(profile.FindBone(HumanBone::LeftEye), nullptr);
}

// A skeleton profile that is not JSON, or whose field holds a value of the wrong type, is refused with
// the file and the place or the field named, and never throws: the web build has no exception
// catching.
TEST(SkeletonProfileTest, AnUnreadableFileIsRefusedNamingThePlaceOrTheField)
{
    std::vector<std::string> lines;
    bool truncatedLoaded = true;
    bool mistypedLoaded = true;
    {
        TestLog::ScopedLogCapture capture(&lines);
        const std::string truncatedText = R"({"name": [)";
        const std::string mistypedText = R"({"bones": [{"bone": "Hips", "restRotation": [1, 0, "x", 0]}]})";
        SkeletonProfile truncated(GUID(), std::filesystem::path("Animations/Truncated.profile.json"));
        truncatedLoaded = truncated.LoadFromData(Vector<uint8>(truncatedText.begin(), truncatedText.end()));
        SkeletonProfile mistyped(GUID(), std::filesystem::path("Animations/Mistyped.profile.json"));
        mistypedLoaded = mistyped.LoadFromData(Vector<uint8>(mistypedText.begin(), mistypedText.end()));
    }
    EXPECT_FALSE(truncatedLoaded);
    EXPECT_FALSE(mistypedLoaded);
    EXPECT_NE(TestLog::FirstLineContaining(
                  lines, "'Animations/Truncated.profile.json' is not loaded: it is not JSON (parse error at line 1, column"),
              "");
    EXPECT_NE(TestLog::FirstLineContaining(lines, "'Animations/Mistyped.profile.json' is not loaded: 'bones[0].restRotation[2]' is a string; write it as a number"), "");
}

// A vector or rotation field that is present but is not an array of enough numbers is refused with
// the field named. An absent field takes its default.
TEST(SkeletonProfileTest, AVectorFieldThatIsNotEnoughNumbersIsRefusedAndAnAbsentOneTakesItsDefault)
{
    const std::pair<std::string, std::string> cases[] = {
        {R"("restScale": [1, 1])", "'bones[0].restScale' is an array of 2 elements; write it as [x, y, z]"},
        {R"("restScale": "1,1,1")", "'bones[0].restScale' is a string; write it as [x, y, z]"},
        {R"("localUp": 1)", "'bones[0].localUp' is a number; write it as [x, y, z]"},
        {R"("restRotation": [1, 0, 0])", "'bones[0].restRotation' is an array of 3 elements; write it as [w, x, y, z]"},
        {R"("restRotation": {"w": 1})", "'bones[0].restRotation' is an object; write it as [w, x, y, z]"},
    };
    for (const auto& [field, refusal] : cases)
    {
        SkeletonProfile profile(GUID(), std::filesystem::path("Animations/Bad.profile.json"));
        std::vector<std::string> lines;
        EXPECT_FALSE(LoadHipsBone(profile, field, lines)) << field;
        EXPECT_NE(TestLog::FirstLineContaining(lines, refusal), "") << field;
    }

    SkeletonProfile absent(GUID(), std::filesystem::path("Animations/Absent.profile.json"));
    std::vector<std::string> lines;
    EXPECT_TRUE(LoadHipsBone(absent, R"("parent": "None")", lines));
    EXPECT_EQ(TestLog::FirstLineContaining(lines, "is not loaded"), "");
    const ProfileBone* hips = absent.FindBone(HumanBone::Hips);
    ASSERT_NE(hips, nullptr);
    EXPECT_EQ(hips->RestScale.x, 1.0f);
    EXPECT_EQ(hips->LocalUp.y, 1.0f);
}
