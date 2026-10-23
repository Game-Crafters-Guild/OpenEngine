// What the in-memory ring hands the debug server's get_log: a sequence number on
// every entry, so a caller can ask for what came after the last line it saw, and
// the count of the entries after that cursor the ring overwrote.

#include "Logger/RingBufferSink.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using namespace Logger;

void Log(RingBufferSink& sink, LogLevel level, const std::string& text)
{
    sink.Write(LogMessage(level, text));
}

TEST(RingBufferSinkQuery, AfterSequenceReturnsOnlyNewerEntriesAndCountsOverwrittenOnes)
{
    RingBufferSink sink(4);
    Log(sink, LogLevel::Info, "one");
    Log(sink, LogLevel::Info, "two");
    const auto first = sink.GetMessages({});
    ASSERT_EQ(first.Messages.size(), 2u);
    EXPECT_EQ(first.Messages.back().Sequence, 2u);
    EXPECT_EQ(first.LastSequence, 2u);

    Log(sink, LogLevel::Info, "three");
    const auto delta = sink.GetMessages({.AfterSequence = first.LastSequence});
    ASSERT_EQ(delta.Messages.size(), 1u);
    EXPECT_EQ(delta.Messages[0].Message, "three");
    EXPECT_EQ(delta.Messages[0].Sequence, 3u);
    EXPECT_EQ(delta.Overwritten, 0u);

    // Seven entries through a ring of four: entries 4 to 7 remain, so a caller at 2 lost 3.
    for (const char* text : {"four", "five", "six", "seven"})
        Log(sink, LogLevel::Info, text);
    const auto behind = sink.GetMessages({.AfterSequence = 2});
    EXPECT_EQ(behind.Messages.size(), 4u);
    EXPECT_EQ(behind.Overwritten, 1u);

    sink.Clear();
    Log(sink, LogLevel::Info, "after clear");
    EXPECT_EQ(sink.GetMessages({}).Messages.at(0).Sequence, 8u) << "a cursor stays valid across Clear";
}

} // namespace
