#include "TerrainHeightFunction/TerrainHeightFunctionInstances.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "TerrainECS/TerrainHeightFunction/ITerrainHeightFunction.h"
#include "TerrainECS/TerrainHeightFunction/RevocableTerrainHeightFunction.h"

#include <algorithm>

namespace GameEngine::TerrainECS
{
TerrainHeightFunctionInstances::TerrainHeightFunctionInstances(JobSystem::WorkStealingThreadPool* jobs)
    : m_Jobs(jobs)
{
}

TerrainHeightFunctionInstances::~TerrainHeightFunctionInstances()
{
    for (Slot& slot : m_Slots)
    {
        if (slot.Build.IsValid())
            slot.Build.Wait();
    }
}

void TerrainHeightFunctionInstances::FinishBuild(Slot& slot)
{
    if (slot.Instance || (slot.Build.IsValid() && !slot.Build.IsDone()))
        return;
    slot.Build = {};
    slot.Instance = std::make_shared<RevocableTerrainHeightFunction>(std::move(slot.Result->Function));
    slot.Result.reset();
}

std::shared_ptr<const RevocableTerrainHeightFunction> TerrainHeightFunctionInstances::Request(
    const TerrainHeightFunctionKey& key, const TerrainHeightFunctionRegistration& registration)
{
    auto slot = std::ranges::find(m_Slots, key, &Slot::BuiltFor);
    if (slot == m_Slots.end())
    {
        Slot& added = m_Slots.emplace_back();
        added.BuiltFor = key;
        added.Registration = registration;
        added.Result = std::make_shared<BuildResult>();
        const auto build = [result = added.Result, factory = registration.Factory, context = key.Context] {
            result->Function = factory(context);
        };
        if (m_Jobs != nullptr)
            added.Build = m_Jobs->Submit(build);
        else
            build();
        slot = std::prev(m_Slots.end());
    }
    slot->Requested = true;
    FinishBuild(*slot);
    return slot->Instance;
}

void TerrainHeightFunctionInstances::ReleaseUnused()
{
    // A build that ended after its key was abandoned gets its instance here, so the erase drops it.
    for (Slot& slot : m_Slots)
        FinishBuild(slot);
    std::erase_if(m_Slots, [](const Slot& slot) {
        return !slot.Requested && slot.Instance && slot.Instance.use_count() == 1;
    });
    for (Slot& slot : m_Slots)
        slot.Requested = false;
}

std::size_t TerrainHeightFunctionInstances::RevokeModuleGenerations(std::string_view moduleId, uint64 firstGeneration,
                                                                    uint64 endGeneration)
{
    const auto owned = [&](const Slot& slot) {
        return TerrainHeightFunctionRegistry::IsOwnedByModuleGenerations(slot.Registration, moduleId, firstGeneration,
                                                                         endGeneration);
    };
    for (Slot& slot : m_Slots)
    {
        if (!owned(slot))
            continue;
        if (slot.Build.IsValid())
            slot.Build.Wait();
        FinishBuild(slot);
        slot.Instance->Revoke();
    }
    return std::erase_if(m_Slots, owned);
}

} // namespace GameEngine::TerrainECS
