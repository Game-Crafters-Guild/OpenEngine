#include "Logger/RingBufferSink.h"
#include <algorithm>

namespace Logger {

RingBufferSink::RingBufferSink(size_t capacity)
    : m_Capacity(capacity < 1 ? 1 : capacity)
{
    m_Buffer.resize(m_Capacity);
}

void RingBufferSink::Write(const LogMessage& message)
{
    std::lock_guard lock(m_Mutex);

    auto& slot = m_Buffer[m_Head];
    slot.Level = message.Level;
    slot.Message = message.Message;
    slot.Timestamp = message.Timestamp;
    slot.Sequence = ++m_LastSequence;

    m_Head = (m_Head + 1) % m_Capacity;
    if (m_Count < m_Capacity)
    {
        ++m_Count;
    }
}

RingBufferSink::MessageQueryResult RingBufferSink::GetMessages(const MessageQuery& query) const
{
    std::lock_guard lock(m_Mutex);

    MessageQueryResult result;
    result.LastSequence = m_LastSequence;
    if (m_Count == 0)
        return result;

    const size_t oldest = (m_Head + m_Capacity - m_Count) % m_Capacity;
    const uint64 oldestSequence = m_Buffer[oldest].Sequence;
    if (oldestSequence > query.AfterSequence + 1)
        result.Overwritten = oldestSequence - query.AfterSequence - 1;

    // Sequence numbers rise from the oldest entry to the newest, so the entries
    // after the cursor are one contiguous run at the newest end.
    const uint64 newer = m_LastSequence > query.AfterSequence ? m_LastSequence - query.AfterSequence : 0;
    const size_t first = m_Count - static_cast<size_t>(std::min<uint64>(newer, m_Count));
    result.Messages.reserve(m_Count - first);
    for (size_t i = first; i < m_Count; ++i)
    {
        const auto& entry = m_Buffer[(oldest + i) % m_Capacity];
        if (entry.Level >= query.MinLevel)
            result.Messages.push_back(entry);
        else
            ++result.BelowLevel;
    }
    return result;
}

size_t RingBufferSink::GetCount() const
{
    std::lock_guard lock(m_Mutex);
    return m_Count;
}

void RingBufferSink::Clear()
{
    std::lock_guard lock(m_Mutex);
    m_Head = 0;
    m_Count = 0;
}

} // namespace Logger
