#pragma once

#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace GameEngine::Tests
{

// Yields until `pred` holds or `timeout` passes; false on the timeout.
inline bool WaitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

// Holds every worker of the pool inside one task each until Release(), which
// returns once every holding task has stopped touching this object, so it may
// be destroyed before the pool.
class HeldWorkers
{
  public:
    explicit HeldWorkers(JobSystem::WorkStealingThreadPool& pool)
    {
        const size_t workers = pool.GetWorkerCount();
        for (size_t i = 0; i < workers; ++i)
        {
            pool.EnqueueWork([this] {
                m_Holding.fetch_add(1);
                while (!m_Released.load())
                    std::this_thread::yield();
                m_Holding.fetch_sub(1);
            });
        }
        EXPECT_TRUE(WaitUntil([this, workers] { return m_Holding.load() == workers; }, std::chrono::seconds(10)));
    }
    void Release()
    {
        m_Released.store(true);
        EXPECT_TRUE(WaitUntil([this] { return m_Holding.load() == 0; }, std::chrono::seconds(10)));
    }

  private:
    std::atomic<size_t> m_Holding{0};
    std::atomic<bool> m_Released{false};
};

} // namespace GameEngine::Tests
