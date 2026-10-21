#pragma once

#include "Types/Types.h"

#include <memory>
#include <mutex>
#include <vector>

namespace GameEngine::Pathfinding
{

class AStarSolver;
struct SolverScratchStatistics;

// The grid A* solvers of one NavigationWorld. A solve, or one lane of a
// batch that runs its solves one after another, leases an idle solver, or a
// new one when every solver is leased, and the lease returns it, so
// concurrent solves never share scratch and a solver's scratch is reused
// across solves. The mutex is taken once to lease and once to return.
// ReleaseScratchAbove trims idle solvers only: a leased solver's scratch is
// never freed under its solve.
class AStarSolverPool
{
public:
    // One solver, leased from Acquire until the lease is destroyed.
    class Lease
    {
    public:
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) = delete;
        Lease& operator=(Lease&&) = delete;

        AStarSolver& Solver() const { return *m_Solver; }

    private:
        friend class AStarSolverPool;
        Lease(AStarSolverPool& pool, uint32 index, AStarSolver& solver);

        AStarSolverPool& m_Pool;
        uint32 m_Index;
        AStarSolver* m_Solver;
    };

    AStarSolverPool();
    ~AStarSolverPool();

    AStarSolverPool(const AStarSolverPool&) = delete;
    AStarSolverPool& operator=(const AStarSolverPool&) = delete;
    AStarSolverPool(AStarSolverPool&&) = delete;
    AStarSolverPool& operator=(AStarSolverPool&&) = delete;

    Lease Acquire();
    // Frees the scratch of every idle solver whose buffers hold more than
    // `maxCells` cells.
    void ReleaseScratchAbove(uint32 maxCells);
    SolverScratchStatistics Statistics() const;

private:
    // Records the solver's held bytes and the high-water mark, and makes it
    // idle. Never allocates: Acquire reserved room for every solver.
    void Return(uint32 index);

    mutable std::mutex m_Mutex;
    std::vector<std::unique_ptr<AStarSolver>> m_Solvers;
    // Per solver, its bytes as of its last return or release.
    std::vector<uint64> m_HeldBytes;
    // Indices into m_Solvers of the solvers no lease holds.
    std::vector<uint32> m_Idle;
    uint64 m_HighWaterBytes = 0;
};

} // namespace GameEngine::Pathfinding
