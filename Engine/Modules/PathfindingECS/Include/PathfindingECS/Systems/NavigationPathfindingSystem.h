#pragma once
#include "ECS/Systems.h"
#include "Pathfinding/PathfindingTypes.h"
#include "PathfindingECS/Systems/NavigationPathfindingStatistics.h"
#include <cstddef>
#include <vector>

namespace GameEngine::PathfindingECS
{

class NavigationPathfindingSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "NavigationPathfindingSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;
    // The grid solves read maps without a lock on the world's pool, so no
    // other managed system may run (and write a map) beside this one.
    bool RequiresExclusiveUpdate() const override { return true; }

    // Most path solves one Update starts. Every request joins one queue,
    // whatever raised it: the replan timer, the Failed retry, the movement
    // system's corridor checks or game code. The oldest requests are solved
    // first; the rest keep their place and are solved within
    // ceil(backlog / budget) Updates. A PathDirty request waits as Pending with
    // its old path released; a timer refresh (ReplanRequested) keeps walking
    // its path until the solve lands. A count rather than a time, and not
    // derived from the pool's size, so which requests a frame solves does not
    // depend on the machine.
    static constexpr uint32 kDefaultMaxPathSolvesPerFrame = 64;
    // How long a Failed agent waits before retrying. Occupancy no longer fails a
    // path (A* taxes it), so what is left to clear is a blocked destination or a
    // nav map that was not built yet.
    static constexpr float32 kFailedPathRetrySeconds = 0.2f;
    // The budget in use; 0 is read as 1, so every request is still served.
    uint32 MaxPathSolvesPerFrame = kDefaultMaxPathSolvesPerFrame;

    const NavigationPathfindingStatistics& GetStatistics() const { return m_Statistics; }

private:
    // A queued request as collected this Update. The list lives for one
    // Update; the order across Updates is the agents' PathRequestSequence.
    struct QueuedRequest
    {
        ECS::EntityHandle Entity;
        uint32 Sequence;
        Pathfinding::NavMapHandle Map;
        Pathfinding::PathRequest Request;
    };

    // The first sequence issued by an Update that issued any, so a request's
    // wait can be read back from its sequence. Trimmed to the oldest request
    // still queued.
    struct SequenceIssue
    {
        uint32 FirstSequence;
        uint64 Frame;
        float32 Time;
    };

    bool CollectRequests(ECS::World& world);
    void PrepareBodies(ECS::World& world);
    void SolveOldestRequests(ECS::World& world);
    uint32 IssueSequence();
    std::size_t FindIssue(uint32 sequence) const;
    // How long the request holding `sequence` has waited; 0 when no issue is kept.
    void ReadWait(uint32 sequence, uint32& frames, float32& seconds) const;
    // Sequences grow by one per request, skip 0 (not queued) and wrap, so
    // they are ordered by how long ago they were issued, never by value.
    uint32 SequenceAge(uint32 sequence) const { return m_NextSequence - sequence; }

    std::vector<QueuedRequest> m_Queue;
    // The grid requests among the solved ones, in queue order; kept across
    // Updates so their point vectors keep their capacity.
    std::vector<Pathfinding::GridPathSolve> m_GridSolves;
    std::vector<SequenceIssue> m_SequenceIssues;
    NavigationPathfindingStatistics m_Statistics;
    float32 m_AccumulatedTime = 0.0f;
    uint32 m_NextSequence = 1;
};

} // namespace GameEngine::PathfindingECS
