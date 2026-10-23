#include "DebugServer/LogQueryReport.h"

#include "DebugServer/DebugServerReply.h"
#include "Logger/RingBufferSink.h"

#include <algorithm>
#include <format>
#include <optional>
#include <regex>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
using json = nlohmann::json;
using StoredMessage = Logger::RingBufferSink::StoredMessage;

// One line as the reply carries it: "<seq> <LEVEL> <message>".
std::string FormatLine(const StoredMessage& entry, std::size_t messageBytes)
{
    std::string line = std::to_string(entry.Sequence);
    line += ' ';
    line += Logger::LogLevelToString(entry.Level);
    line += ' ';
    line.append(entry.Message, 0, messageBytes);
    return line;
}

// What one line adds to the serialized reply: the JSON string with its quotes and
// escapes, and the separator after it.
std::size_t SerializedBytes(const std::string& line)
{
    return json(line).dump(-1, ' ', false, json::error_handler_t::replace).size() + 1;
}

// The longest prefix of `text` within `limit` bytes that does not end inside a UTF-8 sequence.
std::size_t Utf8Prefix(const std::string& text, std::size_t limit)
{
    if (text.size() <= limit)
        return text.size();
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u)
        --end;
    return end;
}

// The newest line clipped so that it alone fits the cap; `clippedBytes` receives what was cut.
std::string ClipToCap(const StoredMessage& entry, std::size_t& clippedBytes)
{
    std::size_t kept = entry.Message.size();
    std::string line = FormatLine(entry, kept);
    while (kept > 0 && SerializedBytes(line) > kLogMaxLineBytes)
    {
        const std::size_t over = SerializedBytes(line) - kLogMaxLineBytes;
        kept = Utf8Prefix(entry.Message, kept > over ? kept - over : 0);
        line = FormatLine(entry, kept);
    }
    clippedBytes = entry.Message.size() - kept;
    return line;
}

// "HH:MM:SSZ" (UTC) for the unchanged reply.
std::string ClockTime(LogPollTracker::Clock::time_point at)
{
    return std::format("{:%H:%M:%S}Z", std::chrono::floor<std::chrono::seconds>(at));
}

// The narrower call when the cap cut matching lines. A higher level is offered only
// while there is one below error to leave.
std::string NarrowerCall(std::size_t overCap, std::size_t count, Logger::LogLevel minLevel)
{
    std::string text = std::to_string(overCap) + " matching line(s) did not fit " + std::to_string(count) +
                       " lines or " + std::to_string(kLogMaxLineBytes) + " bytes: pass ";
    if (minLevel < Logger::LogLevel::Error)
        text += minLevel < Logger::LogLevel::Warning ? "minLevel=warning, " : "minLevel=error, ";
    return text + "a pattern, or a smaller count with offset to page back";
}

} // namespace

bool LogPollTracker::IsRepeat(std::uint32_t clientId, const std::string& query, std::uint64_t newestSequence,
                              Clock::time_point now, Clock::time_point& previousAt)
{
    std::erase_if(m_ByClient, [now](const auto& entry) { return now - entry.second.At > kLogUnchangedWindow; });
    LastAnswer& last = m_ByClient[clientId];
    const bool repeat = last.At != Clock::time_point{} && last.Query == query && last.NewestSequence == newestSequence;
    previousAt = last.At;
    last = LastAnswer{query, newestSequence, now};
    return repeat;
}

json DescribeLogQuery(const Logger::RingBufferSink& sink, const json& params, std::uint32_t clientId,
                      LogPollTracker& tracker, LogPollTracker::Clock::time_point now)
{
    const bool hasSince = params.contains("since");
    const std::uint64_t since = params.value("since", std::uint64_t{0});
    const std::size_t count = params.value("count", kLogDefaultCount);
    const std::size_t offset = params.value("offset", std::size_t{0});

    Logger::LogLevel minLevel = kLogDefaultMinLevel;
    if (const std::string levelName = params.value("minLevel", std::string()); !levelName.empty())
    {
        const std::optional<Logger::LogLevel> parsed = Logger::ParseLogLevel(levelName);
        if (!parsed)
            return RefuseRequest("minLevel '" + levelName +
                                 "' is not a level: use trace, debug, info, warning, error or critical");
        minLevel = *parsed;
    }

    std::optional<std::regex> pattern;
    if (const std::string patternText = params.value("pattern", std::string()); !patternText.empty())
    {
        if (patternText.size() > kLogMaxPatternLength)
            return RefuseRequest("pattern is longer than " + std::to_string(kLogMaxPatternLength) +
                                 " characters: search for a shorter, more specific text");
        try
        {
            pattern.emplace(patternText, std::regex::ECMAScript | std::regex::icase);
        }
        catch (const std::regex_error& error)
        {
            return RefuseRequest(std::string("pattern is not a valid regular expression (") + error.what() +
                                 "): pass an ECMAScript pattern such as \"shader|pipeline\"");
        }
    }

    auto result = sink.GetMessages({.MinLevel = minLevel, .AfterSequence = since});
    // Sequence numbers start at 1 in every editor, so a cursor ahead of this log comes
    // from an earlier session: answer from the start rather than with nothing.
    const bool restarted = hasSince && since > result.LastSequence;
    if (restarted)
        result = sink.GetMessages({.MinLevel = minLevel, .AfterSequence = 0});

    // Match outside the sink's lock, newest first, then skip `offset` and keep `count`.
    std::vector<const StoredMessage*> selected;
    std::size_t notMatching = 0;
    std::size_t matched = 0;
    try
    {
        for (auto entry = result.Messages.rbegin(); entry != result.Messages.rend(); ++entry)
        {
            if (pattern && !std::regex_search(entry->Message, *pattern))
            {
                ++notMatching;
                continue;
            }
            if (matched++ < offset)
                continue;
            if (count == 0 || selected.size() < count)
                selected.push_back(&*entry);
        }
    }
    catch (const std::regex_error&)
    {
        return RefuseRequest("the pattern is too costly to match: use a simpler pattern, such as plain words "
                             "joined by |");
    }

    if (!hasSince)
    {
        const std::uint64_t newest = selected.empty() ? 0 : selected.front()->Sequence;
        LogPollTracker::Clock::time_point previousAt;
        if (tracker.IsRepeat(clientId, params.dump(), newest, now, previousAt))
        {
            const std::string line = "unchanged since " + ClockTime(previousAt) +
                                     "; pass since=" + std::to_string(result.LastSequence);
            return json{{"unchanged", line}, {"nextSince", result.LastSequence}};
        }
    }

    // Keep the newest lines that fit the byte cap; a newest line longer than the cap on
    // its own is clipped rather than dropped, so the answer is never empty for its size.
    std::vector<std::string> lines;
    std::size_t bytes = 0;
    std::size_t clippedBytes = 0;
    for (const StoredMessage* entry : selected)
    {
        std::string line = FormatLine(*entry, entry->Message.size());
        const std::size_t lineBytes = SerializedBytes(line);
        if (bytes + lineBytes > kLogMaxLineBytes)
        {
            if (lines.empty())
                lines.push_back(ClipToCap(*entry, clippedBytes));
            break;
        }
        bytes += lineBytes;
        lines.push_back(std::move(line));
    }
    std::reverse(lines.begin(), lines.end());

    const std::size_t afterOffset = matched - std::min(offset, matched);
    const std::size_t overCap = afterOffset - lines.size();

    json reply = json::object();
    if (!hasSince)
        reply["hint"] = "pass since=" + std::to_string(result.LastSequence) +
                        " on the next call to receive only new lines";
    if (restarted)
        reply["restarted"] = "since=" + std::to_string(since) +
                             " is ahead of this editor's log, which started again: lines from its start";
    reply["nextSince"] = result.LastSequence;
    reply["lines"] = std::move(lines);

    json omitted = json::object();
    if (const std::size_t filtered = result.BelowLevel + notMatching; filtered > 0)
        omitted["filtered"] = filtered;
    if (hasSince && !restarted && result.Overwritten > 0)
        omitted["overwritten"] = result.Overwritten;
    if (clippedBytes > 0)
        omitted["clippedBytes"] = clippedBytes;
    if (overCap > 0)
    {
        omitted["overCap"] = overCap;
        omitted["narrower"] = NarrowerCall(overCap, count, minLevel);
    }
    if (!omitted.empty())
        reply["omitted"] = std::move(omitted);
    return reply;
}

} // namespace GameEngine::Editor
