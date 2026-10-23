#pragma once

#include "Logger/LogLevel.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace Logger
{
class RingBufferSink;
}

namespace GameEngine::Editor
{

// get_log's defaults: an agent that names nothing gets the warnings and errors, few
// enough to read, never a page of info lines.
inline constexpr Logger::LogLevel kLogDefaultMinLevel = Logger::LogLevel::Warning;
inline constexpr std::size_t kLogDefaultCount = 30;
// The lines one answer may carry, counted as they serialize (quotes and escapes
// included), the newest lines kept: with the hint and the omitted counts beside it
// the whole answer stays under 2 KB.
inline constexpr std::size_t kLogMaxLineBytes = 1536;
// The longest pattern get_log compiles. A pattern is a search for a line, not a program.
inline constexpr std::size_t kLogMaxPatternLength = 256;
// A call without `since` that would repeat the same connection's previous answer within
// this window gets the one-line "unchanged" reply.
inline constexpr std::chrono::seconds kLogUnchangedWindow{30};

// What each debug-server connection's last get_log call without `since` answered, so a
// connection that polls the tail is told "unchanged" instead of receiving the same lines
// again. Connection ids are never reused within a server's life, and entries older than
// kLogUnchangedWindow are dropped, so a closed connection leaves nothing behind for long.
class LogPollTracker
{
  public:
    using Clock = std::chrono::system_clock;

    // True when `clientId` asked `query` within the window and its newest line then was
    // `newestSequence`. Records this call as the connection's latest either way, and
    // passes back the time of the earlier answer in `previousAt`.
    bool IsRepeat(std::uint32_t clientId, const std::string& query, std::uint64_t newestSequence,
                  Clock::time_point now, Clock::time_point& previousAt);

  private:
    struct LastAnswer
    {
        std::string Query;
        std::uint64_t NewestSequence = 0;
        Clock::time_point At;
    };
    std::unordered_map<std::uint32_t, LastAnswer> m_ByClient;
};

// The `get_log` debug response. Parameters: since (a sequence number; only newer lines),
// minLevel (trace, debug, info, warning, error or critical; default warning; any other
// spelling is refused), pattern (ECMAScript regular expression, case-insensitive, at most
// kLogMaxPatternLength characters), count (default kLogDefaultCount), offset (newest
// matches to skip). Lines are "<seq> <LEVEL> <message>", oldest first, at most `count` of
// them and kLogMaxLineBytes as serialized. `nextSince` is the cursor for the next call.
// Without `since` the reply leads with a hint naming that cursor, or, when it would repeat
// this connection's previous answer, is the single `unchanged` line. A `since` ahead of
// this editor's log (a cursor from an earlier session) answers from the start of the log
// and says so in `restarted`. `omitted` counts what the level, the pattern and the cap
// left out and, with `since`, the lines after it the ring overwrote; it names the narrower
// call when the cap cut matching lines. The pattern is matched outside the sink's lock.
nlohmann::json DescribeLogQuery(const Logger::RingBufferSink& sink, const nlohmann::json& params,
                                std::uint32_t clientId, LogPollTracker& tracker,
                                LogPollTracker::Clock::time_point now);

} // namespace GameEngine::Editor
