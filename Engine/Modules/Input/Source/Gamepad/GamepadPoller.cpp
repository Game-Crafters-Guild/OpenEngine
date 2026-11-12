#include "Input/Gamepad/GamepadPoller.h"

#include "Input/Gamepad/GamepadBackend.h"
#include "Input/GamepadFrame.h"
#include "Input/InputScriptApi.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"

#include <cassert>
#include <chrono>
#include <utility>

namespace GameEngine::Input
{
namespace
{

// One gamepad runtime per process: the last start outcome of any poller.
std::atomic<GamepadRuntimeState> s_RuntimeState{GamepadRuntimeState::Starting};

} // namespace

GamepadPoller::GamepadPoller(JobSystem::WorkStealingThreadPool& pool, std::unique_ptr<GamepadBackend> backend)
    : m_Backend(std::move(backend))
    , m_StartChannel(std::make_unique<JobSystem::JobChannel>(
          pool, JobSystem::JobChannelDesc{.Name = "Gamepad backend start", .MaxRunning = 1}))
    , m_FrameThread(std::this_thread::get_id())
{
    assert(m_Backend && "GamepadPoller needs a backend");
    s_RuntimeState.store(GamepadRuntimeState::Starting, std::memory_order_release);
    m_Start = m_StartChannel->Submit([this, inlinePool = pool.IsInlineMode()]() { RunStart(inlinePool); });
}

GamepadPoller::~GamepadPoller()
{
    if (m_Start.IsValid())
        m_Start.Wait();
    m_Backend->Stop();
    m_StartChannel.reset();
}

void GamepadPoller::RunStart(bool inlinePool)
{
    // The rule: device discovery never runs on the thread that reads the frames. An inline pool
    // (no workers, the single-threaded web build) has only that thread.
    assert((inlinePool || std::this_thread::get_id() != m_FrameThread) && "gamepad discovery on the frame thread");
    const auto startTime = std::chrono::steady_clock::now();
    const GamepadRuntimeState state = m_Backend->Start();
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime);
    Logger::Log::Info("Gamepad backend started in {:.1f} ms {} ({})", elapsed.count(),
                      inlinePool ? "on the main thread (inline job pool)" : "on a job",
                      state == GamepadRuntimeState::Available ? "gamepads available" : "gamepads off");
    // Read is ready before a game can see the state: a game that reads Available sees the pad.
    m_Ready.store(state == GamepadRuntimeState::Available, std::memory_order_release);
    s_RuntimeState.store(state, std::memory_order_release);
}

void GamepadPoller::Read(GamepadFrame& frame)
{
    assert(std::this_thread::get_id() == m_FrameThread && "GamepadPoller::Read runs on its constructing thread");
    frame = GamepadFrame{};
    if (!m_Ready.load(std::memory_order_acquire))
        return;
    m_Backend->Read(frame);
}

GamepadRuntimeState GamepadPoller::GetRuntimeState()
{
    return s_RuntimeState.load(std::memory_order_acquire);
}

} // namespace GameEngine::Input
