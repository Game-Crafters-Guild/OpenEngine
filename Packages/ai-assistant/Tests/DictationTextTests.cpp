#include "DictationText.h"

#include <gtest/gtest.h>

using namespace GameEngine;

TEST(DictationText, CountsWordsAcrossSpacesAndLineBreaks)
{
    EXPECT_EQ(CountDictationWords(""), 0u);
    EXPECT_EQ(CountDictationWords("  "), 0u);
    EXPECT_EQ(CountDictationWords("test one two"), 3u);
    EXPECT_EQ(CountDictationWords(" test  one\ntwo "), 3u);
}

// Clearing the prompt mid-dictation skips the words said so far: later transcripts of the same
// session, revised or extended, bring back only the words spoken after the clear.
TEST(DictationText, SkipsTheWordsOfClearedText)
{
    EXPECT_EQ(DictationTextAfterWords("test one two", 0), "test one two");
    EXPECT_EQ(DictationTextAfterWords("test one two three", 3), "three");
    EXPECT_EQ(DictationTextAfterWords("test won too three four", 3), "three four");
    EXPECT_EQ(DictationTextAfterWords("test one two", 3), "");
    EXPECT_EQ(DictationTextAfterWords("test one", 5), "");
}
