#pragma once

#include "JobSystem/TaskHandle.h"
#include "TerrainECS/TerrainHeightFunction/TerrainHeightFunctionKey.h"
#include "TerrainHeightFunction/TerrainHeightFunctionRegistry.h"
#include "Types/Types.h"

#include <memory>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::TerrainECS
{
class ITerrainHeightFunction;
class RevocableTerrainHeightFunction;

/// The built height functions of the terrains in use, one per name, registration generation, seed
/// and footprint. A build runs the function's constructor on the job system; until it ends, a request
/// answers null and the caller keeps the base it has. Main thread only. TerrainService owns one.
class TerrainHeightFunctionInstances
{
public:
    /// Builds on `jobs`; null builds inline, on the calling thread.
    explicit TerrainHeightFunctionInstances(JobSystem::WorkStealingThreadPool* jobs);
    /// Waits for the builds in flight.
    ~TerrainHeightFunctionInstances();

    TerrainHeightFunctionInstances(const TerrainHeightFunctionInstances&) = delete;
    TerrainHeightFunctionInstances& operator=(const TerrainHeightFunctionInstances&) = delete;

    /// The instance `registration` built for `key`, or null while it builds. The first request starts
    /// the build.
    std::shared_ptr<const RevocableTerrainHeightFunction> Request(const TerrainHeightFunctionKey& key,
                                                                  const TerrainHeightFunctionRegistration& registration);
    /// Drops each built instance that no caller holds and that no one requested since the last call,
    /// including one whose build ended after the last request.
    void ReleaseUnused();
    /// Revokes every instance that a load of `moduleId` with a generation in [firstGeneration,
    /// endGeneration) registered, after its build ends, and forgets it. A caller that still holds one
    /// reads it as revoked. Returns the count.
    std::size_t RevokeModuleGenerations(std::string_view moduleId, uint64 firstGeneration, uint64 endGeneration);

private:
    // A build's output, written by the build job and read on the main thread after the job ends.
    struct BuildResult
    {
        std::unique_ptr<ITerrainHeightFunction> Function;
    };

    struct Slot
    {
        TerrainHeightFunctionKey BuiltFor;
        TerrainHeightFunctionRegistration Registration;
        JobSystem::TaskHandle Build;
        std::shared_ptr<BuildResult> Result;
        std::shared_ptr<RevocableTerrainHeightFunction> Instance;
        bool Requested = true;
    };

    void FinishBuild(Slot& slot);

    JobSystem::WorkStealingThreadPool* m_Jobs = nullptr;
    std::vector<Slot> m_Slots;
};

} // namespace GameEngine::TerrainECS
