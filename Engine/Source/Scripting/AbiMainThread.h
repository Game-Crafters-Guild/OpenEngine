#pragma once

// AbiMainThread.h - The scripting ABI's two thread predicates: which threads an export that reads
// frame-thread state admits.

#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"

namespace GameEngine::ScriptingAbi
{

/**
 * @brief True when the engine has no frame loop and the caller is the only thread that drives it: no
 * engine was ever initialized in this process (a gtest host that drives exports directly), or the
 * scripting ABI's bootstrap initialized it for a host with no frame loop (dotnet test). Nothing else
 * mutates frame-thread state there, so refusing would protect nothing.
 */
inline bool NoFrameLoopFor(const EngineCore& engine)
{
    return !engine.HasEverInitialized() || engine.IsStandaloneHosted();
}

/**
 * @brief Is this thread inside the engine's scheduling order: the main thread, or a job body on any
 * thread (a worker, or the main thread running a ParallelFor unit or a tagged job while it waits).
 *
 * A thread the engine does not schedule (a script's own Task.Run or timer) answers false: it can run
 * while a wave writes what the export reads. Read by GE_Animator_PollEventsOnEntity and by the
 * generated script API thunks, which look the engine up once per call. Reads no ScriptManager.
 */
inline bool OnEngineThreadFor(const EngineCore& engine)
{
    return NoFrameLoopFor(engine) || engine.IsMainThread() || JobSystem::WorkStealingThreadPool::IsExecutingJob();
}

/**
 * @brief May this thread mutate frame-thread state now: the main thread outside any job body.
 *
 * A job body answers false whichever thread runs it, so the answer is the same on a worker and on the
 * main thread running a unit or a tagged job while it waits. Read by the UI registry's unload sweep
 * and by the generated script API thunks. Reads no ScriptManager.
 */
inline bool OnEngineMainThreadFor(const EngineCore& engine)
{
    return NoFrameLoopFor(engine) || (engine.IsMainThread() && !JobSystem::WorkStealingThreadPool::IsExecutingJob());
}

/// OnEngineMainThread for the engine instance this process binds; a function, so it can be a predicate pointer
/// (UIElementTypeABI.cpp).
inline bool OnEngineMainThread()
{
    return OnEngineMainThreadFor(EngineCore::GetInstance());
}

} // namespace GameEngine::ScriptingAbi
