#include <gtest/gtest.h>

#include "Types/PathUtils.h"

#include <filesystem>
#include <string>

using namespace GameEngine;
namespace fs = std::filesystem;

TEST(PathUtilsTests, AFileInsideTheRootIsSpelledRelativeToIt)
{
    EXPECT_EQ(RelativePathUnderRoot("/proj/Shaders/Surfaces/mine.glsl", "/proj/Shaders"),
              fs::path("Surfaces/mine.glsl"));
}

TEST(PathUtilsTests, BothSidesAreNormalisedBeforeComparing)
{
    EXPECT_EQ(RelativePathUnderRoot("/proj/Shaders/../Shaders/./a.glsl", "/proj/Shaders/"),
              fs::path("a.glsl"));
}

TEST(PathUtilsTests, OutsideTheRootIsEmpty)
{
    EXPECT_TRUE(RelativePathUnderRoot("/elsewhere/a.glsl", "/proj/Shaders").empty());
    EXPECT_TRUE(RelativePathUnderRoot("/proj/a.glsl", "/proj/Shaders").empty())
        << "a sibling of the root walks out through ..";
    EXPECT_TRUE(RelativePathUnderRoot("Surfaces/a.glsl", "/proj/Shaders").empty())
        << "a relative path shares no root with an absolute one";
}

TEST(PathUtilsTests, TheRootItselfIsNotUnderTheRoot)
{
    EXPECT_TRUE(RelativePathUnderRoot("/proj/Shaders", "/proj/Shaders").empty());
    EXPECT_TRUE(RelativePathUnderRoot("/proj/Shaders/", "/proj/Shaders").empty());
}

TEST(PathUtilsTests, AnEmptyRootContainsNothing)
{
    EXPECT_TRUE(RelativePathUnderRoot("/proj/Shaders/a.glsl", "").empty());
}

TEST(PathUtilsTests, ANameThatStartsWithTwoDotsIsNotAWalkOut)
{
    EXPECT_EQ(RelativePathUnderRoot("/proj/Shaders/..hidden.glsl", "/proj/Shaders"),
              fs::path("..hidden.glsl"));
}

TEST(PathUtilsTests, AUtf8StringBecomesThePathOfTheSameCharacters)
{
    const std::string utf8 = "Gr\xC3\xB6\xC3\x9F" "e/caf\xC3\xA9.txt"; // "Größe/café.txt" in UTF-8
    const fs::path path = PathFromUtf8(utf8);
    EXPECT_EQ(path.filename(), fs::path(u8"caf\u00E9.txt"));
    EXPECT_EQ(path.parent_path(), fs::path(u8"Gr\u00F6\u00DFe"));
}

// The active code page on Windows (1252 on a Western system) has no character for 日本, so a
// conversion through it throws.
TEST(PathUtilsTests, APathBecomesUtf8WithForwardSlashesForAnyCharacters)
{
    const fs::path path = fs::path(u8"\u65E5\u672C") / u8"caf\u00E9.txt";
    EXPECT_EQ(PathToUtf8(path), "\xE6\x97\xA5\xE6\x9C\xAC/caf\xC3\xA9.txt"); // "日本/café.txt" in UTF-8
}
