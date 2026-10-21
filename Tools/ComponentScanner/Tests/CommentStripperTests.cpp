#include "CommentStripper.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>

using namespace GameEngine::ComponentScanner;

TEST(CommentStripper, RemovesLineCommentButKeepsCode)
{
    const std::string out = CommentStripper::Strip("int x; // comment with ; and }\nint y;").Text;
    EXPECT_EQ(std::string::npos, out.find("comment"));
    EXPECT_NE(std::string::npos, out.find("int x;"));
    EXPECT_NE(std::string::npos, out.find("int y;"));
}

TEST(CommentStripper, RemovesBlockCommentIncludingItsBracesAndSemicolons)
{
    const std::string out = CommentStripper::Strip("int /* ; } */ x;").Text;
    EXPECT_EQ(1, std::count(out.begin(), out.end(), ';')); // only the real terminator survives
    EXPECT_EQ(std::string::npos, out.find('}'));
    EXPECT_NE(std::string::npos, out.find('x'));
}

TEST(CommentStripper, RemovesStringLiteralContentsSoBracesCannotConfuseTheParser)
{
    const std::string out = CommentStripper::Strip("const char* s = \"a;b}c{\";").Text;
    EXPECT_EQ(std::string::npos, out.find("a;b"));
    EXPECT_EQ(std::string::npos, out.find('}'));
}

TEST(CommentStripper, PreservesNewlineCountForStableLineNumbers)
{
    const std::string out = CommentStripper::Strip("a // x\nb // y\nc").Text;
    EXPECT_EQ(2, std::count(out.begin(), out.end(), '\n'));
}

TEST(CommentStripper, DigitSeparatorDoesNotOpenACharacterLiteral)
{
    const std::string out = CommentStripper::Strip("unsigned Mask = 0xFFFF'FFFFu;\nfloat After;").Text;
    EXPECT_NE(std::string::npos, out.find("0xFFFFFFFFu;"));
    EXPECT_NE(std::string::npos, out.find("float After;"));
}

TEST(CommentStripper, PrefixedCharacterLiteralIsStillRemoved)
{
    const std::string out = CommentStripper::Strip("char32_t Brace = U'}';").Text;
    EXPECT_EQ(std::string::npos, out.find('}'));
}

TEST(CommentStripper, PreservesNewlinesInsideALiteral)
{
    // A line continuation inside the string: the source has two newlines.
    const std::string out = CommentStripper::Strip("const char* s = \"a\\\nb\";\nint x;").Text;
    EXPECT_EQ(2, std::count(out.begin(), out.end(), '\n'));
}

// Each quote's separator test costs O(1): a number with 200,000 separators strips in linear time.
// The bound is two orders of magnitude above the linear cost and far below the quadratic one
// (a back-scan over the token at every quote took minutes).
TEST(CommentStripper, StripsALongSeparatedNumberInLinearTime)
{
    constexpr std::size_t kSeparators = 200000;
    constexpr auto kBound = std::chrono::seconds(5);
    std::string source = "1";
    for (std::size_t i = 0; i < kSeparators; ++i)
        source += "'1";

    const auto start = std::chrono::steady_clock::now();
    const std::string out = CommentStripper::Strip(source).Text;
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(std::string(kSeparators + 1, '1'), out);
    EXPECT_LT(elapsed, kBound);
}
