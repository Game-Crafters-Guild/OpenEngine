#include "Pathfinding/ObstacleAvoidance.h"
#include "Pathfinding/DetourNavMap.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#if GE_PATHFINDING_BACKEND_DETOUR
#include <DetourCommon.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#endif

namespace GameEngine::Pathfinding
{

#if GE_PATHFINDING_BACKEND_DETOUR

struct CrowdManager::Impl
{
    dtCrowd* Crowd = nullptr;
    dtNavMesh* NavMesh = nullptr;
    dtNavMeshQuery* NavQuery = nullptr;
    // The frame of the mesh the crowd steers on: agents live in chart-local coordinates and the
    // public functions take and return world points.
    NavMeshChart Chart;
    // Counts the successful Initialize calls.
    uint32 Generation = 0;
    // Per crowd agent index: whether it arrived (MarkAgentArrived), and the goal it arrived at, in
    // chart-local coordinates.
    struct Arrival
    {
        bool Arrived = false;
        float32 Goal[3] = {};
    };
    std::vector<Arrival> Arrivals;

    void ClearArrival(CrowdAgentHandle agent)
    {
        if (agent >= 0 && static_cast<std::size_t>(agent) < Arrivals.size())
            Arrivals[static_cast<std::size_t>(agent)] = {};
    }

    void ToLocal(float32 x, float32 y, float32 z, float32 out[3]) const { Chart.ToChart(x, y, z, out); }

    void ToWorld(const float32 local[3], float32& outX, float32& outY, float32& outZ) const
    {
        float64 world[3];
        Chart.ToWorld(local, world);
        outX = static_cast<float32>(world[0]);
        outY = static_cast<float32>(world[1]);
        outZ = static_cast<float32>(world[2]);
    }

    ~Impl()
    {
        if (NavQuery)
        {
            dtFreeNavMeshQuery(NavQuery);
            NavQuery = nullptr;
        }
        if (Crowd)
        {
            dtFreeCrowd(Crowd);
            Crowd = nullptr;
        }
    }
};

CrowdManager::CrowdManager()
    : m_Impl(std::make_unique<Impl>())
{
}

CrowdManager::~CrowdManager() = default;
CrowdManager::CrowdManager(CrowdManager&&) noexcept = default;
CrowdManager& CrowdManager::operator=(CrowdManager&&) noexcept = default;

bool CrowdManager::Initialize(DetourNavMap& navMap, uint32 maxAgents)
{
    if (!navMap.IsBuilt())
        return false;

    // Clean up any previous initialization to prevent leaking the old dtCrowd.
    if (m_Impl->Crowd)
        Shutdown();

    m_Impl->NavMesh = static_cast<dtNavMesh*>(navMap.GetDetourNavMesh());
    if (!m_Impl->NavMesh)
        return false;
    m_Impl->Chart = navMap.GetChart();

    m_Impl->NavQuery = dtAllocNavMeshQuery();
    if (!m_Impl->NavQuery)
        return false;

    if (dtStatusFailed(m_Impl->NavQuery->init(m_Impl->NavMesh, 2048)))
    {
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
        return false;
    }

    m_Impl->Crowd = dtAllocCrowd();
    if (!m_Impl->Crowd)
    {
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
        return false;
    }

    static constexpr float32 kDefaultMaxAgentRadius = 2.4f;
    if (!m_Impl->Crowd->init(static_cast<int32>(maxAgents), kDefaultMaxAgentRadius,
                              m_Impl->NavMesh))
    {
        dtFreeCrowd(m_Impl->Crowd);
        m_Impl->Crowd = nullptr;
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
        return false;
    }

    // Configure obstacle avoidance params. All fields must be set — leaving
    // weightDesVel/weightCurVel/gridSize at zero causes the avoidance query
    // to produce zero velocity, making agents unable to move.
    dtObstacleAvoidanceParams avoidanceParams;

    // Low quality
    avoidanceParams.velBias = 0.5f;
    avoidanceParams.weightDesVel = 2.0f;
    avoidanceParams.weightCurVel = 0.75f;
    avoidanceParams.weightSide = 0.75f;
    avoidanceParams.weightToi = 2.5f;
    avoidanceParams.horizTime = 2.5f;
    avoidanceParams.gridSize = 33;
    avoidanceParams.adaptiveDivs = 5;
    avoidanceParams.adaptiveRings = 2;
    avoidanceParams.adaptiveDepth = 1;
    m_Impl->Crowd->setObstacleAvoidanceParams(0, &avoidanceParams);

    // Medium quality
    avoidanceParams.adaptiveDivs = 5;
    avoidanceParams.adaptiveRings = 2;
    avoidanceParams.adaptiveDepth = 2;
    m_Impl->Crowd->setObstacleAvoidanceParams(1, &avoidanceParams);

    // Good quality
    avoidanceParams.adaptiveDivs = 7;
    avoidanceParams.adaptiveRings = 2;
    avoidanceParams.adaptiveDepth = 3;
    m_Impl->Crowd->setObstacleAvoidanceParams(2, &avoidanceParams);

    // High quality
    avoidanceParams.adaptiveDivs = 7;
    avoidanceParams.adaptiveRings = 3;
    avoidanceParams.adaptiveDepth = 3;
    m_Impl->Crowd->setObstacleAvoidanceParams(3, &avoidanceParams);

    m_Impl->Arrivals.assign(maxAgents, {});
    ++m_Impl->Generation;
    return true;
}

void CrowdManager::Shutdown()
{
    if (m_Impl->Crowd)
    {
        dtFreeCrowd(m_Impl->Crowd);
        m_Impl->Crowd = nullptr;
    }
    if (m_Impl->NavQuery)
    {
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
    }
    m_Impl->NavMesh = nullptr;
    m_Impl->Arrivals.clear();
}

bool CrowdManager::IsInitialized() const
{
    return m_Impl && m_Impl->Crowd != nullptr;
}

bool CrowdManager::IsBoundTo(const DetourNavMap& navMap) const
{
    return IsInitialized() && m_Impl->NavMesh == navMap.GetDetourNavMesh();
}

uint32 CrowdManager::GetGeneration() const
{
    return m_Impl ? m_Impl->Generation : 0;
}

CrowdAgentHandle CrowdManager::AddAgent(float32 x, float32 y, float32 z, const CrowdAgentParams& params)
{
    if (!IsInitialized())
        return kInvalidCrowdAgent;

    float32 pos[3];
    m_Impl->ToLocal(x, y, z, pos);

    dtCrowdAgentParams agentParams = {};
    agentParams.radius = params.Radius;
    agentParams.height = params.Height;
    agentParams.maxAcceleration = params.MaxAcceleration;
    agentParams.maxSpeed = params.MaxSpeed;
    agentParams.collisionQueryRange = (params.CollisionQueryRange > 0.0f)
        ? params.CollisionQueryRange : params.Radius * 12.0f;
    agentParams.pathOptimizationRange = (params.PathOptimizationRange > 0.0f)
        ? params.PathOptimizationRange : params.Radius * 30.0f;
    agentParams.separationWeight = params.SeparationWeight;
    agentParams.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS |
                              DT_CROWD_OPTIMIZE_TOPO | DT_CROWD_OBSTACLE_AVOIDANCE |
                              DT_CROWD_SEPARATION;
    agentParams.obstacleAvoidanceType = static_cast<uint8>(params.AvoidanceQuality);
    agentParams.queryFilterType = 0;

    const auto handle = static_cast<CrowdAgentHandle>(m_Impl->Crowd->addAgent(pos, &agentParams));
    m_Impl->ClearArrival(handle);
    return handle;
}

void CrowdManager::RemoveAgent(CrowdAgentHandle agent)
{
    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;
    m_Impl->Crowd->removeAgent(static_cast<int32>(agent));
    m_Impl->ClearArrival(agent);
}

void CrowdManager::SetAgentTarget(CrowdAgentHandle agent, float32 x, float32 y, float32 z)
{
    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;

    float32 targetPos[3];
    m_Impl->ToLocal(x, y, z, targetPos);

    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    const float32 agentRadius = (crowdAgent && crowdAgent->active) ? crowdAgent->params.radius : 0.6f;
    const float32 agentHeight = (crowdAgent && crowdAgent->active) ? crowdAgent->params.height : 2.0f;
    const float32 extent[3] = {
        std::max(2.0f, agentRadius * 4.0f),
        std::max(4.0f, agentHeight * 2.0f),
        std::max(2.0f, agentRadius * 4.0f)
    };

    dtPolyRef targetRef = 0;
    float32 nearestPos[3] = {targetPos[0], targetPos[1], targetPos[2]};
    dtStatus status = m_Impl->NavQuery->findNearestPoly(targetPos, extent,
                                                         m_Impl->Crowd->getFilter(0),
                                                         &targetRef, nearestPos);
    if (dtStatusFailed(status) || targetRef == 0)
    {
        Logger::Log::Warning("[CrowdManager] SetAgentTarget: findNearestPoly failed for target ({}, {}, {})",
                             x, y, z);
        return;
    }

    m_Impl->ClearArrival(agent);
    if (!m_Impl->Crowd->requestMoveTarget(static_cast<int32>(agent), targetRef, nearestPos))
    {
        Logger::Log::Warning("[CrowdManager] SetAgentTarget: requestMoveTarget failed for agent {} "
                             "to ({}, {}, {})", agent, x, y, z);
    }
}

void CrowdManager::MarkAgentArrived(CrowdAgentHandle agent)
{
    if (!IsInitialized() || agent == kInvalidCrowdAgent ||
        static_cast<std::size_t>(agent) >= m_Impl->Arrivals.size())
        return;
    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    if (!crowdAgent || !crowdAgent->active)
        return;
    Impl::Arrival& arrival = m_Impl->Arrivals[static_cast<std::size_t>(agent)];
    arrival.Arrived = true;
    dtVcopy(arrival.Goal, crowdAgent->targetPos);
    m_Impl->Crowd->resetMoveTarget(static_cast<int32>(agent));
}

void CrowdManager::SetAgentParams(CrowdAgentHandle agent, const CrowdAgentParams& params)
{
    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;

    const dtCrowdAgent* existingAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    if (!existingAgent || !existingAgent->active)
        return;

    dtCrowdAgentParams agentParams = existingAgent->params;
    agentParams.radius = params.Radius;
    agentParams.height = params.Height;
    agentParams.maxAcceleration = params.MaxAcceleration;
    agentParams.maxSpeed = params.MaxSpeed;
    agentParams.separationWeight = params.SeparationWeight;
    agentParams.obstacleAvoidanceType = static_cast<uint8>(params.AvoidanceQuality);
    // Recompute derived ranges from the new radius to keep them consistent.
    agentParams.collisionQueryRange = (params.CollisionQueryRange > 0.0f)
        ? params.CollisionQueryRange : params.Radius * 12.0f;
    agentParams.pathOptimizationRange = (params.PathOptimizationRange > 0.0f)
        ? params.PathOptimizationRange : params.Radius * 30.0f;

    m_Impl->Crowd->updateAgentParameters(static_cast<int32>(agent), &agentParams);
}

void CrowdManager::GetAgentPosition(CrowdAgentHandle agent, float32& outX, float32& outY, float32& outZ) const
{
    outX = outY = outZ = 0.0f;

    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;

    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    if (!crowdAgent || !crowdAgent->active)
        return;

    m_Impl->ToWorld(crowdAgent->npos, outX, outY, outZ);
}

void CrowdManager::GetAgentVelocity(CrowdAgentHandle agent, float32& outVx, float32& outVy, float32& outVz) const
{
    outVx = outVy = outVz = 0.0f;

    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;

    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    if (!crowdAgent || !crowdAgent->active)
        return;

    outVx = crowdAgent->vel[0];
    outVy = crowdAgent->vel[1];
    outVz = crowdAgent->vel[2];
}

void CrowdManager::GetAgentTarget(CrowdAgentHandle agent, float32& outX, float32& outY, float32& outZ) const
{
    outX = outY = outZ = 0.0f;

    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;

    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    if (!crowdAgent || !crowdAgent->active)
        return;

    m_Impl->ToWorld(crowdAgent->targetPos, outX, outY, outZ);
}

bool CrowdManager::IsAgentActive(CrowdAgentHandle agent) const
{
    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return false;

    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    return crowdAgent && crowdAgent->active;
}

bool CrowdManager::IsAgentHeldOffGoal(CrowdAgentHandle agent) const
{
    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return false;

    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    if (!crowdAgent || !crowdAgent->active || crowdAgent->targetState != DT_CROWDAGENT_TARGET_VALID ||
        crowdAgent->ncorners == 0 || (crowdAgent->cornerFlags[crowdAgent->ncorners - 1] & DT_STRAIGHTPATH_END) == 0)
        return false;

    // Two agents touch within their radii and this share of the agent's radius: the separation
    // keeps neighbors about that far apart.
    constexpr float32 kTouchMarginRadii = 0.5f;
    // Two goals this close, in meters, are the same goal: both orders named one point.
    constexpr float32 kSameGoal = 0.05f;
    const float32* goal = &crowdAgent->cornerVerts[(crowdAgent->ncorners - 1) * 3];
    const float32 ownDistance = dtVdist2D(crowdAgent->npos, goal);
    for (int32 i = 0; i < crowdAgent->nneis; ++i)
    {
        const dtCrowdAgent* neighbor = m_Impl->Crowd->getAgent(crowdAgent->neis[i].idx);
        if (!neighbor || !neighbor->active)
            continue;
        const float32 touch =
            crowdAgent->params.radius * (1.0f + kTouchMarginRadii) + neighbor->params.radius;
        if (dtVdist2D(neighbor->npos, crowdAgent->npos) > touch)
            continue;
        const Impl::Arrival& arrival = m_Impl->Arrivals[static_cast<std::size_t>(crowdAgent->neis[i].idx)];
        if (dtVdist2D(neighbor->npos, goal) < ownDistance ||
            (arrival.Arrived && dtVdist2D(arrival.Goal, crowdAgent->targetPos) <= kSameGoal))
            return true;
    }
    return false;
}

void CrowdManager::Update(float32 deltaTime)
{
    if (!IsInitialized())
        return;
    m_Impl->Crowd->update(deltaTime, nullptr);
}

#else // !GE_PATHFINDING_BACKEND_DETOUR

struct CrowdManager::Impl
{
};

CrowdManager::CrowdManager()
    : m_Impl(std::make_unique<Impl>())
{
}

CrowdManager::~CrowdManager() = default;
CrowdManager::CrowdManager(CrowdManager&&) noexcept = default;
CrowdManager& CrowdManager::operator=(CrowdManager&&) noexcept = default;

bool CrowdManager::Initialize(DetourNavMap& /*navMap*/, uint32 /*maxAgents*/)
{
    return false;
}

void CrowdManager::Shutdown()
{
}

bool CrowdManager::IsInitialized() const
{
    return false;
}

bool CrowdManager::IsBoundTo(const DetourNavMap& /*navMap*/) const
{
    return false;
}

uint32 CrowdManager::GetGeneration() const
{
    return 0;
}

CrowdAgentHandle CrowdManager::AddAgent(float32 /*x*/, float32 /*y*/, float32 /*z*/, const CrowdAgentParams& /*params*/)
{
    return kInvalidCrowdAgent;
}

void CrowdManager::RemoveAgent(CrowdAgentHandle /*agent*/)
{
}

void CrowdManager::SetAgentTarget(CrowdAgentHandle /*agent*/, float32 /*x*/, float32 /*y*/, float32 /*z*/)
{
}

void CrowdManager::MarkAgentArrived(CrowdAgentHandle /*agent*/)
{
}

void CrowdManager::SetAgentParams(CrowdAgentHandle /*agent*/, const CrowdAgentParams& /*params*/)
{
}

void CrowdManager::GetAgentPosition(CrowdAgentHandle /*agent*/, float32& outX, float32& outY, float32& outZ) const
{
    outX = outY = outZ = 0.0f;
}

void CrowdManager::GetAgentVelocity(CrowdAgentHandle /*agent*/, float32& outVx, float32& outVy, float32& outVz) const
{
    outVx = outVy = outVz = 0.0f;
}

void CrowdManager::GetAgentTarget(CrowdAgentHandle /*agent*/, float32& outX, float32& outY, float32& outZ) const
{
    outX = outY = outZ = 0.0f;
}

bool CrowdManager::IsAgentActive(CrowdAgentHandle /*agent*/) const
{
    return false;
}

bool CrowdManager::IsAgentHeldOffGoal(CrowdAgentHandle /*agent*/) const
{
    return false;
}

void CrowdManager::Update(float32 /*deltaTime*/)
{
}

#endif // GE_PATHFINDING_BACKEND_DETOUR

} // namespace GameEngine::Pathfinding
