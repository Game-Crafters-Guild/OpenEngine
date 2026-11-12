#pragma once

#include "Mathematics/Vector3.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::AI
{

/// The memory records of one group: what its sensors saw, one record per subject, column by column.
/// Every tick is an AI tick. A subject is the caller's 32-bit id (an entity handle's id in AIECS).
class RecordStore
{
public:
    /// One sensor report of a subject.
    struct Sighting
    {
        uint32 Subject = 0;
        uint32 Owner = 0;
        uint16 Kind = 0;
        Mathematics::Vector3 Position;
        float32 Strength = 0.0f;
        float32 Value0 = 0.0f;
        float32 Value1 = 0.0f;
    };

    /// One record as Restore takes it: the last sighting of the subject and the ticks of its first
    /// and last sightings.
    struct Record
    {
        Sighting Last;
        uint32 FirstSeen = 0;
        uint32 LastSeen = 0;
    };

    /// The most records the store keeps. When it is full, a new subject replaces the record seen
    /// longest ago; a tie goes to the lower subject id.
    uint32 Capacity = 64;
    /// Forget removes a record when more than this number of ticks passed since its last sighting.
    uint32 ForgetTicks = 200;
    /// When true, ClearSeenArea removes the records in a seen area that no sighting renewed.
    bool ClearOnSight = true;

    /// Writes a sighting: updates the subject's record, or adds one with FirstSeen = tick.
    void Observe(const Sighting& sighting, uint32 tick);
    /// Removes the records with LastSeen before `tick` whose position is within `radius` of
    /// `center`: the sensor sees the place and the subject is not there. Call it after Observe.
    void ClearSeenArea(const Mathematics::Vector3& center, float32 radius, uint32 tick);
    /// Replaces all records with `records`, in their order, and keeps their FirstSeen and LastSeen.
    /// For a load: a scene load calls it with the records a save wrote. A second record of a subject
    /// is ignored. Restore does not apply Capacity: pass at most Capacity records.
    void Restore(std::span<const Record> records);
    /// Removes the records not seen for more than ForgetTicks.
    void Forget(uint32 tick);
    /// The subject's record index, or kNoRecord.
    uint32 Find(uint32 subject) const;

    static constexpr uint32 kNoRecord = 0xFFFFFFFF;

    /// The number of records. Each column below holds one entry per record, at the record's index.
    uint32 GetCount() const { return static_cast<uint32>(m_Subjects.size()); }
    std::span<const uint32> GetSubjects() const { return m_Subjects; }
    std::span<const uint32> GetOwners() const { return m_Owners; }
    std::span<const uint16> GetKinds() const { return m_Kinds; }
    std::span<const Mathematics::Vector3> GetPositions() const { return m_Positions; }
    std::span<const float32> GetStrengths() const { return m_Strengths; }
    std::span<const float32> GetValues0() const { return m_Values0; }
    std::span<const float32> GetValues1() const { return m_Values1; }
    std::span<const uint32> GetFirstSeen() const { return m_FirstSeen; }
    std::span<const uint32> GetLastSeen() const { return m_LastSeen; }

private:
    void Write(uint32 index, const Sighting& sighting, uint32 tick);
    // Adds a record of zeros at the end of every column.
    void AppendEmpty();
    uint32 OldestRecord() const;
    // Moves the last record into `index` and drops the last slot.
    void Remove(uint32 index);

    FlatMap<uint32, uint32> m_IndexBySubject;
    std::vector<uint32> m_Subjects;
    std::vector<uint32> m_Owners;
    std::vector<uint16> m_Kinds;
    std::vector<Mathematics::Vector3> m_Positions;
    std::vector<float32> m_Strengths;
    std::vector<float32> m_Values0;
    std::vector<float32> m_Values1;
    std::vector<uint32> m_FirstSeen;
    std::vector<uint32> m_LastSeen;
};

} // namespace GameEngine::AI
