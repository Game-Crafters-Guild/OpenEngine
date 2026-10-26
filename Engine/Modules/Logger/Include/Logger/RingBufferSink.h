#pragma once

#include "Logger/LogSink.h"
#include <mutex>
#include <vector>

namespace Logger {

// Thread-safe circular buffer sink that retains the last N log messages
// for programmatic access by debug tools (e.g. EditorDebugServer).
class RingBufferSink : public LogSink
{
public:
    struct StoredMessage
    {
        LogLevel Level = LogLevel::Info;
        std::string Message;
        TimePoint Timestamp;
        // 1 for the first message the sink stored, one more for each after it. Clear()
        // keeps counting, so a sequence number names one message for the sink's life.
        std::uint64_t Sequence = 0;
    };

    // What GetMessages selects: the entries newer than AfterSequence at MinLevel or
    // above. Text matching runs in the caller, outside the sink's lock, so a costly
    // pattern never holds up the threads that log.
    struct MessageQuery
    {
        LogLevel MinLevel = LogLevel::Trace;
        // Only entries with a sequence number above this; 0 = every stored entry.
        std::uint64_t AfterSequence = 0;
    };

    struct MessageQueryResult
    {
        // The selected entries, oldest first.
        std::vector<StoredMessage> Messages;
        // Stored entries newer than AfterSequence that MinLevel left out.
        size_t BelowLevel = 0;
        // Entries newer than AfterSequence that the buffer overwrote before this call.
        std::uint64_t Overwritten = 0;
        // The newest sequence number the sink has stored; 0 when nothing was logged.
        std::uint64_t LastSequence = 0;
    };

    explicit RingBufferSink(size_t capacity = 500);
    ~RingBufferSink() override = default;

    void Write(const LogMessage& message) override;
    void Flush() override {}
    bool ShouldLog(LogLevel /*level*/) const override { return true; }
    std::string GetName() const override { return "RingBuffer"; }

    // A copy of the stored messages `query` selects, with the overwrite count.
    MessageQueryResult GetMessages(const MessageQuery& query) const;

    size_t GetCount() const;

    void Clear();

private:
    mutable std::mutex m_Mutex;
    std::vector<StoredMessage> m_Buffer;
    size_t m_Capacity;
    size_t m_Head = 0;
    size_t m_Count = 0;
    std::uint64_t m_LastSequence = 0;
};

} // namespace Logger
