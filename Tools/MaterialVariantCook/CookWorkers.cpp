#include "CookWorkers.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace GameEngine::Tools
{
namespace
{
void ClaimUntilDone(size_t count, std::atomic<size_t>& next, const std::function<void(size_t)>& work)
{
    for (size_t index = next.fetch_add(1); index < count; index = next.fetch_add(1))
        work(index);
}
} // namespace

void RunOnCookWorkers(size_t count, size_t jobs, const std::function<void(size_t)>& work)
{
    std::atomic<size_t> next{0};
    const size_t workerCount = std::min(std::max<size_t>(jobs, 1), count);
    if (workerCount <= 1)
    {
        ClaimUntilDone(count, next, work);
        return;
    }
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i)
        workers.emplace_back(ClaimUntilDone, count, std::ref(next), std::cref(work));
    for (std::thread& worker : workers)
        worker.join();
}

} // namespace GameEngine::Tools
