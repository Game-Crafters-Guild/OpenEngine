#include "AStarSolverPool.h"

#include "AStarSolver.h"
#include "Pathfinding/SolverScratchStatistics.h"

#include <algorithm>

namespace GameEngine::Pathfinding
{

AStarSolverPool::Lease::Lease(AStarSolverPool& pool, uint32 index, AStarSolver& solver)
    : m_Pool(pool), m_Index(index), m_Solver(&solver)
{
}

AStarSolverPool::Lease::~Lease()
{
    m_Pool.Return(m_Index);
}

AStarSolverPool::AStarSolverPool() = default;
AStarSolverPool::~AStarSolverPool() = default;

AStarSolverPool::Lease AStarSolverPool::Acquire()
{
    const std::lock_guard lock(m_Mutex);
    if (m_Idle.empty())
    {
        // Reserve first, so an allocation that throws leaves the three vectors in step.
        const size_t count = m_Solvers.size() + 1;
        m_Solvers.reserve(count);
        m_HeldBytes.reserve(count);
        m_Idle.reserve(count);
        m_Solvers.push_back(std::make_unique<AStarSolver>());
        m_HeldBytes.push_back(0);
        return Lease(*this, static_cast<uint32>(m_Solvers.size() - 1), *m_Solvers.back());
    }
    const uint32 index = m_Idle.back();
    m_Idle.pop_back();
    return Lease(*this, index, *m_Solvers[index]);
}

void AStarSolverPool::Return(uint32 index)
{
    const std::lock_guard lock(m_Mutex);
    const uint64 held = m_Solvers[index]->HeldBytes();
    m_HeldBytes[index] = held;
    m_HighWaterBytes = std::max(m_HighWaterBytes, held);
    m_Idle.push_back(index);
}

void AStarSolverPool::ReleaseScratchAbove(uint32 maxCells)
{
    const std::lock_guard lock(m_Mutex);
    for (const uint32 index : m_Idle)
    {
        m_Solvers[index]->ReleaseScratchAbove(maxCells);
        m_HeldBytes[index] = m_Solvers[index]->HeldBytes();
    }
}

SolverScratchStatistics AStarSolverPool::Statistics() const
{
    SolverScratchStatistics statistics;
    const std::lock_guard lock(m_Mutex);
    statistics.Solvers = static_cast<uint32>(m_Solvers.size());
    for (const uint64 held : m_HeldBytes)
    {
        if (held != 0)
            ++statistics.SolversHoldingScratch;
        statistics.HeldBytes += held;
    }
    statistics.SolverHighWaterBytes = m_HighWaterBytes;
    return statistics;
}

} // namespace GameEngine::Pathfinding
