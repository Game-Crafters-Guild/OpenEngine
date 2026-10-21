#pragma once

// The material cook's one shape of concurrency, shared by its two parallel phases: the SPIR-V
// builds of the variant rows and, under --web, the WGSL translations. --jobs bounds both.
//
// Plain threads rather than the engine's job system: a WGSL translation blocks for seconds on
// a child process, and the tool owns every thread it runs, so there is no pool to share.

#include <cstddef>
#include <functional>

namespace GameEngine::Tools
{

// Calls `work(index)` once for every index in [0, count) on at most `jobs` threads (at least
// one) and returns when every call has finished. Each worker claims the next unclaimed index,
// which keeps every worker busy while item costs differ tenfold. With one worker, the calling
// thread runs every index in order. `work` runs on several threads at once: each call must
// write only its own index's output.
void RunOnCookWorkers(size_t count, size_t jobs, const std::function<void(size_t)>& work);

} // namespace GameEngine::Tools
