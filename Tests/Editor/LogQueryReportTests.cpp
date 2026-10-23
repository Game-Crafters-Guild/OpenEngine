// get_log's reply: a filtered delta, not a page of the log. The defaults return the
// warnings and errors only, `since` returns only what came after the caller's cursor,
// the answer fits a size cap and says what it left out, and a connection that repeats a
// tail call with nothing new is told "unchanged" instead of getting the same lines.

#include "DebugServer/LogQueryReport.h"

#include "DebugServer/DebugServerReply.h"
#include "Logger/RingBufferSink.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using GameEngine::Editor::DescribeLogQuery;
using GameEngine::Editor::LogPollTracker;
using json = nlohmann::json;

constexpr std::uint32_t kClient = 7;
const LogPollTracker::Clock::time_point kNow{std::chrono::hours(1000)};

void Log(Logger::RingBufferSink& sink, Logger::LogLevel level, const std::string& text)
{
    sink.Write(Logger::LogMessage(level, text));
}

TEST(LogQueryReport, DefaultsToWarningsAndATailThatNamesTheCursor)
{
    Logger::RingBufferSink sink(64);
    Log(sink, Logger::LogLevel::Info, "loaded scene");
    Log(sink, Logger::LogLevel::Warning, "texture missing");
    Log(sink, Logger::LogLevel::Error, "pipeline failed");
    LogPollTracker tracker;

    json reply = DescribeLogQuery(sink, json::object(), kClient, tracker, kNow);

    EXPECT_EQ(reply["lines"], (json{"2 WARN texture missing", "3 ERROR pipeline failed"}));
    EXPECT_EQ(reply["nextSince"], 3);
    EXPECT_EQ(reply["hint"], "pass since=3 on the next call to receive only new lines");
    EXPECT_EQ(reply["omitted"]["filtered"], 1) << "the info line is counted, not returned";
}

TEST(LogQueryReport, SinceReturnsOnlyTheLinesAfterTheCursor)
{
    Logger::RingBufferSink sink(64);
    Log(sink, Logger::LogLevel::Warning, "first");
    LogPollTracker tracker;
    json first = DescribeLogQuery(sink, json::object(), kClient, tracker, kNow);
    Log(sink, Logger::LogLevel::Warning, "second");

    json delta = DescribeLogQuery(sink, json{{"since", first["nextSince"]}}, kClient, tracker, kNow);

    EXPECT_EQ(delta["lines"], (json{"2 WARN second"}));
    EXPECT_EQ(delta["nextSince"], 2);
    EXPECT_FALSE(delta.contains("hint"));
}

TEST(LogQueryReport, KeepsTheNewestLinesWithinTheCapAndNamesTheNarrowerCall)
{
    Logger::RingBufferSink sink(256);
    for (int i = 0; i < 100; ++i)
        Log(sink, Logger::LogLevel::Warning, "warning " + std::to_string(i) + " " + std::string(80, 'x'));
    LogPollTracker tracker;

    json reply = DescribeLogQuery(sink, json::object(), kClient, tracker, kNow);

    std::size_t lineBytes = 0;
    for (const auto& line : reply["lines"])
        lineBytes += line.get<std::string>().size();
    EXPECT_LE(lineBytes, GameEngine::Editor::kLogMaxLineBytes);
    EXPECT_LT(reply.dump().size(), 2048u);
    EXPECT_EQ(reply["lines"].back().get<std::string>().rfind("100 WARN warning 99", 0), 0u) << "the newest line is kept";
    EXPECT_EQ(reply["omitted"]["overCap"], 100 - reply["lines"].size());
    EXPECT_NE(reply["omitted"]["narrower"].get<std::string>().find("minLevel=error"), std::string::npos);
}

TEST(LogQueryReport, ARepeatedTailCallWithNothingNewIsUnchangedForThatConnectionOnly)
{
    Logger::RingBufferSink sink(64);
    Log(sink, Logger::LogLevel::Warning, "texture missing");
    LogPollTracker tracker;
    DescribeLogQuery(sink, json::object(), kClient, tracker, kNow);

    json repeat = DescribeLogQuery(sink, json::object(), kClient, tracker, kNow + std::chrono::seconds(10));
    EXPECT_EQ(repeat, (json{{"unchanged", "unchanged since 16:00:00Z; pass since=1"}, {"nextSince", 1}}));

    json otherConnection = DescribeLogQuery(sink, json::object(), kClient + 1, tracker, kNow);
    EXPECT_TRUE(otherConnection.contains("lines")) << "the state is per connection";

    json afterWindow = DescribeLogQuery(sink, json::object(), kClient, tracker, kNow + std::chrono::minutes(2));
    EXPECT_TRUE(afterWindow.contains("lines")) << "only within the window";

    Log(sink, Logger::LogLevel::Warning, "shader missing");
    json newLine = DescribeLogQuery(sink, json::object(), kClient, tracker, kNow + std::chrono::minutes(2));
    EXPECT_EQ(newLine["lines"].size(), 2u) << "a new line is news";
}

TEST(LogQueryReport, APatternSelectsCaseInsensitivelyAndTheFilteredLinesAreCounted)
{
    Logger::RingBufferSink sink(16);
    Log(sink, Logger::LogLevel::Info, "Shader compiled");
    Log(sink, Logger::LogLevel::Warning, "shader cache miss");
    Log(sink, Logger::LogLevel::Warning, "Texture missing");
    Log(sink, Logger::LogLevel::Error, "Pipeline 7 failed");
    LogPollTracker tracker;

    json reply = DescribeLogQuery(sink, json{{"pattern", "SHADER|pipeline \\d"}}, kClient, tracker, kNow);

    EXPECT_EQ(reply["lines"], (json{"2 WARN shader cache miss", "4 ERROR Pipeline 7 failed"}));
    EXPECT_EQ(reply["omitted"]["filtered"], 2) << "one below the level, one not matching the pattern";
}

TEST(LogQueryReport, AnUnknownLevelIsRefusedNamingTheLevels)
{
    Logger::RingBufferSink sink(8);
    Log(sink, Logger::LogLevel::Info, "loaded scene");
    LogPollTracker tracker;

    const json reply = DescribeLogQuery(sink, json{{"minLevel", "warnings"}}, kClient, tracker, kNow);

    ASSERT_TRUE(GameEngine::Editor::IsRefusal(reply));
    EXPECT_NE(GameEngine::Editor::RefusalReason(reply).find("trace, debug, info, warning, error or critical"),
              std::string::npos);
}

TEST(LogQueryReport, AMalformedOrOverlongPatternIsRefused)
{
    Logger::RingBufferSink sink(8);
    LogPollTracker tracker;
    EXPECT_TRUE(GameEngine::Editor::IsRefusal(DescribeLogQuery(sink, json{{"pattern", "("}}, kClient, tracker, kNow)));

    const std::string overlong(GameEngine::Editor::kLogMaxPatternLength + 1, 'a');
    const json reply = DescribeLogQuery(sink, json{{"pattern", overlong}}, kClient, tracker, kNow);
    ASSERT_TRUE(GameEngine::Editor::IsRefusal(reply));
    EXPECT_NE(GameEngine::Editor::RefusalReason(reply).find("longer than"), std::string::npos);
}

TEST(LogQueryReport, OverwrittenLinesAreReportedOnlyAfterACursor)
{
    Logger::RingBufferSink sink(4);
    for (int i = 0; i < 10; ++i)
        Log(sink, Logger::LogLevel::Warning, "warning " + std::to_string(i));
    LogPollTracker tracker;

    json tail = DescribeLogQuery(sink, json::object(), kClient, tracker, kNow);
    EXPECT_FALSE(tail.contains("omitted")) << "a tail call asked for nothing the ring overwrote";

    json delta = DescribeLogQuery(sink, json{{"since", 2}}, kClient, tracker, kNow);
    EXPECT_EQ(delta["omitted"]["overwritten"], 4) << "entries 3 to 6 are gone; 7 to 10 remain";
}

TEST(LogQueryReport, ACursorAheadOfTheLogAnswersFromItsStartAndSaysSo)
{
    Logger::RingBufferSink sink(16);
    Log(sink, Logger::LogLevel::Warning, "first line of a new session");
    LogPollTracker tracker;

    json reply = DescribeLogQuery(sink, json{{"since", 5000}}, kClient, tracker, kNow);

    EXPECT_EQ(reply["lines"], (json{"1 WARN first line of a new session"}));
    EXPECT_TRUE(reply.contains("restarted"));
    EXPECT_EQ(reply["nextSince"], 1);
}

TEST(LogQueryReport, ALineLongerThanTheCapIsClippedOnACharacterBoundary)
{
    Logger::RingBufferSink sink(4);
    // One ASCII byte in front of the two-byte characters puts the cut the cap computes
    // inside a character: the clip must move it back to the character's start.
    std::string message = "x";
    while (message.size() < 2 * GameEngine::Editor::kLogMaxLineBytes)
        message += "\xC3\xA9";
    Log(sink, Logger::LogLevel::Warning, message);
    LogPollTracker tracker;

    json reply = DescribeLogQuery(sink, json::object(), kClient, tracker, kNow);

    ASSERT_EQ(reply["lines"].size(), 1u);
    const std::string line = reply["lines"][0].get<std::string>();
    EXPECT_LE(json(line).dump().size(), GameEngine::Editor::kLogMaxLineBytes);
    EXPECT_EQ(static_cast<unsigned char>(line.back()), 0xA9u) << "ends on a whole character";
    EXPECT_GT(reply["omitted"]["clippedBytes"].get<std::size_t>(), 0u);
}

} // namespace
