#include "Providers/ReplyTextBlocks.h"

#include <gtest/gtest.h>

#include <string>

namespace GameEngine
{
// The first block opens the reply with no break; each later block starts a new paragraph (a
// blank line), counting the newlines the reply already ends with; a block that adds no text
// adds no break, and the next text block still starts its own paragraph.
TEST(ReplyTextBlocksTests, EachLaterBlockStartsAParagraphAndAnEmptyBlockAddsNothing)
{
    ReplyTextBlocks blocks;
    std::string reply;
    blocks.BeginBlock();
    EXPECT_EQ(blocks.Append(reply, "First."), "First.");
    EXPECT_EQ(blocks.Append(reply, " Same block."), " Same block.");

    blocks.BeginBlock();
    EXPECT_EQ(blocks.Append(reply, "Second."), "\n\nSecond.");

    blocks.BeginBlock();
    EXPECT_EQ(blocks.Append(reply, ""), "") << "an empty delta of an empty block";
    blocks.BeginBlock();
    EXPECT_EQ(blocks.Append(reply, "Third."), "\n\nThird.") << "one break, not two, after an empty block";
    EXPECT_EQ(reply, "First. Same block.\n\nSecond.\n\nThird.");
}

TEST(ReplyTextBlocksTests, ABlockAfterTextEndingInNewlinesAddsOnlyTheMissingOnes)
{
    ReplyTextBlocks blocks;
    std::string reply = "A list:\n";
    blocks.BeginBlock();
    EXPECT_EQ(blocks.Append(reply, "Next."), "\nNext.");
    EXPECT_EQ(reply, "A list:\n\nNext.");

    std::string paragraph = "Done.\n\n";
    ReplyTextBlocks again;
    again.BeginBlock();
    EXPECT_EQ(again.Append(paragraph, "Then."), "Then.");
    EXPECT_EQ(paragraph, "Done.\n\nThen.");
}
} // namespace GameEngine
