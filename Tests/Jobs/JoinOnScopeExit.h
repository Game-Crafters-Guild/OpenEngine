#pragma once

#include <functional>
#include <span>
#include <thread>
#include <utility>

namespace GameEngine::Tests
{

// Runs `beforeJoin`, then joins each thread that is still joinable, however the test leaves the scope.
// A gtest ASSERT_ returns from the test body, and a joinable std::thread destroyed there terminates the
// process instead of reporting the failure. Declare it after every local that the threads use.
class JoinOnScopeExit
{
  public:
    explicit JoinOnScopeExit(std::span<std::thread> threads, std::function<void()> beforeJoin = {})
        : m_Threads(threads), m_BeforeJoin(std::move(beforeJoin))
    {
    }

    ~JoinOnScopeExit()
    {
        if (m_BeforeJoin)
            m_BeforeJoin();
        for (std::thread& thread : m_Threads)
        {
            if (thread.joinable())
                thread.join();
        }
    }

    JoinOnScopeExit(const JoinOnScopeExit&) = delete;
    JoinOnScopeExit& operator=(const JoinOnScopeExit&) = delete;

  private:
    std::span<std::thread> m_Threads;
    std::function<void()> m_BeforeJoin;
};

} // namespace GameEngine::Tests
