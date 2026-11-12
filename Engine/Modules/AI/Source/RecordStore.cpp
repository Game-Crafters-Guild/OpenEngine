#include "AI/RecordStore.h"

namespace GameEngine::AI
{

void RecordStore::Observe(const Sighting& sighting, uint32 tick)
{
    if (const uint32* existing = m_IndexBySubject.Find(sighting.Subject))
    {
        const uint32 firstSeen = m_FirstSeen[*existing];
        Write(*existing, sighting, tick);
        m_FirstSeen[*existing] = firstSeen;
        return;
    }
    if (Capacity == 0)
        return;
    if (GetCount() >= Capacity)
        Remove(OldestRecord());

    const uint32 index = GetCount();
    AppendEmpty();
    Write(index, sighting, tick);
    m_IndexBySubject.InsertOrAssign(sighting.Subject, index);
}

void RecordStore::ClearSeenArea(const Mathematics::Vector3& center, float32 radius, uint32 tick)
{
    if (!ClearOnSight)
        return;
    const float32 radiusSquared = radius * radius;
    for (uint32 index = GetCount(); index-- > 0;)
    {
        if (m_LastSeen[index] < tick && (m_Positions[index] - center).LengthSquared() <= radiusSquared)
            Remove(index);
    }
}

void RecordStore::Restore(std::span<const Record> records)
{
    while (GetCount() > 0)
        Remove(GetCount() - 1);
    for (const Record& record : records)
    {
        if (m_IndexBySubject.Find(record.Last.Subject) != nullptr)
            continue;
        const uint32 index = GetCount();
        AppendEmpty();
        Write(index, record.Last, record.LastSeen);
        m_FirstSeen[index] = record.FirstSeen;
        m_IndexBySubject.InsertOrAssign(record.Last.Subject, index);
    }
}

void RecordStore::Forget(uint32 tick)
{
    for (uint32 index = GetCount(); index-- > 0;)
    {
        if (tick - m_LastSeen[index] > ForgetTicks)
            Remove(index);
    }
}

uint32 RecordStore::Find(uint32 subject) const
{
    const uint32* index = m_IndexBySubject.Find(subject);
    return index != nullptr ? *index : kNoRecord;
}

void RecordStore::Write(uint32 index, const Sighting& sighting, uint32 tick)
{
    m_Subjects[index] = sighting.Subject;
    m_Owners[index] = sighting.Owner;
    m_Kinds[index] = sighting.Kind;
    m_Positions[index] = sighting.Position;
    m_Strengths[index] = sighting.Strength;
    m_Values0[index] = sighting.Value0;
    m_Values1[index] = sighting.Value1;
    m_FirstSeen[index] = tick;
    m_LastSeen[index] = tick;
}

void RecordStore::AppendEmpty()
{
    m_Subjects.push_back(0);
    m_Owners.push_back(0);
    m_Kinds.push_back(0);
    m_Positions.emplace_back();
    m_Strengths.push_back(0.0f);
    m_Values0.push_back(0.0f);
    m_Values1.push_back(0.0f);
    m_FirstSeen.push_back(0);
    m_LastSeen.push_back(0);
}

uint32 RecordStore::OldestRecord() const
{
    uint32 oldest = 0;
    for (uint32 index = 1; index < GetCount(); ++index)
    {
        const bool older = m_LastSeen[index] < m_LastSeen[oldest];
        const bool tieLowerSubject = m_LastSeen[index] == m_LastSeen[oldest] && m_Subjects[index] < m_Subjects[oldest];
        if (older || tieLowerSubject)
            oldest = index;
    }
    return oldest;
}

void RecordStore::Remove(uint32 index)
{
    const uint32 last = GetCount() - 1;
    m_IndexBySubject.Erase(m_Subjects[index]);
    if (index != last)
    {
        m_Subjects[index] = m_Subjects[last];
        m_Owners[index] = m_Owners[last];
        m_Kinds[index] = m_Kinds[last];
        m_Positions[index] = m_Positions[last];
        m_Strengths[index] = m_Strengths[last];
        m_Values0[index] = m_Values0[last];
        m_Values1[index] = m_Values1[last];
        m_FirstSeen[index] = m_FirstSeen[last];
        m_LastSeen[index] = m_LastSeen[last];
        m_IndexBySubject.InsertOrAssign(m_Subjects[index], index);
    }
    m_Subjects.pop_back();
    m_Owners.pop_back();
    m_Kinds.pop_back();
    m_Positions.pop_back();
    m_Strengths.pop_back();
    m_Values0.pop_back();
    m_Values1.pop_back();
    m_FirstSeen.pop_back();
    m_LastSeen.pop_back();
}

} // namespace GameEngine::AI
