#pragma once

#include "ECS/ECS.h"
#include "ECS/World.h"
#include "ECS/Query.h"
#include "ECS/Components.h"
#include "ECS/SimulationClock.h"
#include "ECS/SystemWaveTrace.h"
#include "Logger/Logger.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Memory/ScratchScope.h"
#include <algorithm>
#include <concepts>
#include <cstdint>
#include <span>
#include <vector>
#include <memory>
#include <chrono>

namespace GameEngine {
namespace ECS {

// When a system runs. Frame: once per frame with the frame delta. Simulation:
// once per step of the world's SimulationClock with the fixed delta, so a frame
// runs it 0, 1 or more times.
enum class SystemCadence : uint8 {
    Frame,
    Simulation,
};

// The cadence a system type declares with `static constexpr SystemCadence Cadence`;
// Frame when it declares none.
template <typename T>
constexpr SystemCadence CadenceOf() {
    if constexpr (requires { { T::Cadence } -> std::convertible_to<SystemCadence>; })
        return T::Cadence;
    else
        return SystemCadence::Frame;
}

// The delta a Frame system gets. Gameplay: the world clock's frame seconds (the frame delta
// clamped to SimulationClock::kMaxFrameDeltaSeconds, times the time scale), so 0 while paused.
// Real: the frame delta, for systems that run while the game is paused (editor tools, UI).
enum class SystemTime : uint8 {
    Gameplay,
    Real,
};

// The time a system type declares with `static constexpr SystemTime Time`;
// Gameplay when it declares none.
template <typename T>
constexpr SystemTime TimeOf() {
    if constexpr (requires { { T::Time } -> std::convertible_to<SystemTime>; })
        return T::Time;
    else
        return SystemTime::Gameplay;
}

// Base system interface
class ISystem {
public:
    virtual ~ISystem() = default;
    virtual void Update(World& world, float32 deltaTime) = 0;
    virtual const char* GetName() const = 0;
    virtual bool IsEnabled() const { return Enabled; }
    virtual void SetEnabled(bool enable) { Enabled = enable; }
    // Run on the thread calling SystemManager::Update, with no other managed
    // system executing concurrently. Dependencies still establish relative
    // ordering; this requirement also applies to AddParallelSystem.
    virtual bool RequiresExclusiveUpdate() const { return false; }

protected:
    bool Enabled = true;
};

// The systems of one wave can run in parallel; wave N completes before wave N+1 starts.
// A run of adjacent Simulation waves runs once per clock step; a Frame wave runs once per frame.
struct SystemExecutionPlan {
    struct Wave {
        std::vector<size_t> SystemIndices; // indices into SystemManager::systems
        SystemCadence Cadence = SystemCadence::Frame;
    };
    std::vector<Wave> Waves;
    bool IsValid() const { return !Waves.empty(); }

    // Adds a wave that holds only `slot`: for a Frame system after every wave,
    // for a Simulation system after the last Simulation wave.
    void AppendWave(size_t slot, SystemCadence cadence) {
        auto position = Waves.end();
        if (cadence == SystemCadence::Simulation) {
            const auto lastSimulation = std::find_if(Waves.rbegin(), Waves.rend(), [](const Wave& wave) {
                return wave.Cadence == SystemCadence::Simulation;
            });
            if (lastSimulation != Waves.rend())
                position = lastSimulation.base();
        }
        Waves.insert(position, Wave{{slot}, cadence});
    }
};

// System manager for organizing and running systems
class SystemManager {
private:
    std::vector<std::unique_ptr<ISystem>> systems;
    std::vector<std::unique_ptr<ISystem>> parallelSystems;
    JobSystem::WorkStealingThreadPool* jobSystem = nullptr;
    bool m_ParallelWaves = true;

    // Optional per-system update frequency control: every N runs, which are frames
    // for a Frame system and clock steps for a Simulation system.
    std::vector<uint32> seqEveryNFrames;   // 0/1 = every run; >1 = every N runs
    std::vector<uint32> seqFrameCounters;  // incremented each run
    std::vector<uint32> parEveryNFrames;
    std::vector<uint32> parFrameCounters;
    std::vector<SystemCadence> seqCadence; // per sequential slot
    std::vector<SystemTime> seqTime;       // per sequential slot
    std::vector<SystemTime> parTime;       // per parallel system

    // Wave-based execution plan (set by SystemScheduleBuilder::BuildAndRegisterWithWaves)
    SystemExecutionPlan m_ExecutionPlan;

    // Indices of systems added with AddSystem after an execution plan was already
    // installed. Such a system is placed in its own wave at the end of the plan.
    // SetExecutionPlan replaces the whole wave list, so without this record a
    // later plan would drop these systems and they would stop updating with no
    // error. SetExecutionPlan re-appends them. A packaged game hits this path:
    // it installs the ECS plan first and registers ManagedSystemBridge afterwards.
    std::vector<size_t> m_LateAddedSystems;

    // Performance tracking
    struct SystemPerformance {
        std::string Name;
        float32 AverageTime = 0.0f;
        float32 MaxTime = 0.0f;
        float32 LastTime = 0.0f;
        uint32 UpdateCount = 0;
    };

    std::vector<SystemPerformance> performanceData;
    bool trackPerformance = false;

    // Off until a reader enables it (the editor's get_ecs_wave_trace).
    SystemWaveTrace m_WaveTrace;

public:
    explicit SystemManager(JobSystem::WorkStealingThreadPool* js = nullptr) : jobSystem(js) {}

    /// Pin every wave to the thread that calls Update, instead of dispatching
    /// its systems across the job pool.
    ///
    /// Systems in a wave create GPU objects lazily inside their update (feature
    /// initialisation, pipeline warm-up). A backend whose GPU objects belong to
    /// the thread that made them therefore cannot have those systems dispatched
    /// anywhere else, and the owner of the device is what knows that.
    void SetParallelWavesEnabled(bool enable) { m_ParallelWaves = enable; }

    // Set/get execution plan for wave-based parallel updates.
    // An incoming plan replaces Waves wholesale, so systems that were added
    // after a previous plan was built (trailing waves) are re-appended unless
    // the new plan itself schedules them — whatever the caller or timing, a
    // registered system must never silently stop ticking. Deliberate removal
    // goes through Clear() or SetEnabled(false), not plan omission.
    void SetExecutionPlan(SystemExecutionPlan plan) {
        m_ExecutionPlan = std::move(plan);
        if (!m_ExecutionPlan.IsValid())
            return; // invalid plan → flat sequential path runs every system anyway
        for (size_t lateIdx : m_LateAddedSystems) {
            if (lateIdx >= systems.size() || !systems[lateIdx])
                continue; // out of range or retired

            bool covered = false;
            for (const auto& wave : m_ExecutionPlan.Waves) {
                for (size_t idx : wave.SystemIndices) {
                    if (idx == lateIdx) { covered = true; break; }
                }
                if (covered) break;
            }
            if (covered)
                continue;
            m_ExecutionPlan.AppendWave(lateIdx, seqCadence[lateIdx]);
            Logger::Log::Info("[ECS] Execution plan replaced without system '{}' — "
                              "re-appended as a trailing wave",
                              systems[lateIdx] ? systems[lateIdx]->GetName() : "<null>");
        }
    }
    const SystemExecutionPlan& GetExecutionPlan() const { return m_ExecutionPlan; }

    /// The record of how the waves ran (see SystemWaveTrace). Frames name
    /// systems by slot, so every change to the system set clears it. Only the
    /// wave-based path is traced; without an execution plan nothing is recorded.
    SystemWaveTrace& GetWaveTrace() { return m_WaveTrace; }
    size_t GetSequentialSystemCount() const { return systems.size(); }
    // Name of the sequential system a plan wave index refers to (null when out
    // of range) — lets tests assert wave placement without running the plan.
    const char* GetSequentialSystemName(size_t index) const
    {
        return index < systems.size() && systems[index] ? systems[index]->GetName() : nullptr;
    }

    // Sentinel for InstallSystem: append a new slot instead of replacing one.
    static constexpr size_t kAppendSystem = ~size_t{0};

    // Install a sequential system: append a new slot (replaceIndex ==
    // kAppendSystem) or swap the object at an existing slot in place.
    //
    // Replace-in-place is the C12 module-reload seam: the schedule model keys
    // waves by slot index, so swapping the object (instead of appending a
    // duplicate) keeps every wave reference valid while the NEW code takes
    // over ticking. The old object is destroyed here — its destructor runs
    // while its module image is still mapped. Enabled state carries over (a
    // system a user disabled stays disabled across its module's reload);
    // frequency counters are slot-keyed and persist naturally. Returns the
    // slot index.
    size_t InstallSystem(std::unique_ptr<ISystem> system, size_t replaceIndex, SystemCadence cadence,
                         SystemTime time) {
        if (!system)
            return kAppendSystem;

        if (replaceIndex != kAppendSystem) {
            if (replaceIndex >= systems.size()) {
                Logger::Log::Error("[ECS] InstallSystem: replace index {} out of range ({} systems)",
                                   (uint32)replaceIndex, (uint32)systems.size());
                return kAppendSystem;
            }
            const bool wasEnabled = !systems[replaceIndex] || systems[replaceIndex]->IsEnabled();
            const char* name = system->GetName();
            systems[replaceIndex] = std::move(system);
            systems[replaceIndex]->SetEnabled(wasEnabled);
            seqCadence[replaceIndex] = cadence;
            seqTime[replaceIndex] = time;
            m_WaveTrace.Clear();
            if (replaceIndex < performanceData.size())
                performanceData[replaceIndex].Name = name;
            Logger::Log::Info("[ECS] System '{}' replaced in place (slot {}) — new module code takes over",
                              name, (uint32)replaceIndex);
            return replaceIndex;
        }

        ISystem* ptr = system.get();
        systems.push_back(std::move(system));
        m_WaveTrace.Clear();
        // default: every frame
        seqEveryNFrames.push_back(1u);
        seqFrameCounters.push_back(0u);
        seqCadence.push_back(cadence);
        seqTime.push_back(time);

        if (trackPerformance) {
            SystemPerformance perf;
            perf.Name = ptr->GetName();
            performanceData.push_back(perf);
        }

        // A plan runs only its waves: a system added after the plan was built gets
        // a wave of its own after the planned waves of its cadence, so it still runs.
        if (m_ExecutionPlan.IsValid())
        {
            m_ExecutionPlan.AppendWave(systems.size() - 1, cadence);
            m_LateAddedSystems.push_back(systems.size() - 1);
            Logger::Log::Info("[ECS] System '{}' added after the execution plan was built — "
                              "appended as a trailing wave",
                              ptr->GetName());
        }

        Logger::Log::Debug("[ECS] Added system: {}", ptr->GetName());
        return systems.size() - 1;
    }

    // Retire a sequential system slot: destroy the object and null the slot.
    // Wave execution, lookups and the flat fallback all skip null slots, and
    // slot indices held by execution plans stay stable — retirement never
    // shifts other systems. Used when a reloaded module stops declaring a
    // system, and by the load-abort purge. Returns false if out of range or
    // already retired.
    bool RetireSystem(size_t index) {
        if (index >= systems.size() || !systems[index])
            return false;
        Logger::Log::Info("[ECS] System '{}' retired (slot {})", systems[index]->GetName(), (uint32)index);
        systems[index].reset();
        m_WaveTrace.Clear();
        return true;
    }

    // Add a system to run sequentially
    template<typename T, typename... Args>
    T* AddSystem(Args&&... args) {
        auto system = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = system.get();
        InstallSystem(std::move(system), kAppendSystem, CadenceOf<T>(), TimeOf<T>());
        return ptr;
    }

    // Add a system to run in parallel, once per frame.
    template<typename T, typename... Args>
    T* AddParallelSystem(Args&&... args) {
        static_assert(CadenceOf<T>() == SystemCadence::Frame,
                      "AddParallelSystem runs once per frame; add a Simulation system with AddSystem");
        auto system = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = system.get();
        parallelSystems.push_back(std::move(system));
        parEveryNFrames.push_back(1u);
        parFrameCounters.push_back(0u);
        parTime.push_back(TimeOf<T>());

        Logger::Log::Debug("[ECS] Added parallel system: {}", ptr->GetName());
        return ptr;
    }

    // Advances the world's SimulationClock, runs the Simulation systems once per clock step with the
    // fixed delta and the Frame systems once with the delta of their SystemTime. With no plan, every
    // step's Simulation systems run first, then the Frame systems, in slot order.
    void Update(World& world, float32 deltaTime) {
        SimulationClock& clock = world.GetSimulationClock();
        clock.Advance(deltaTime);
        if (m_ExecutionPlan.IsValid())
        {
            UpdateWaveBased(world, deltaTime);
            return;
        }

        const float32 fixedDelta = static_cast<float32>(clock.GetFixedDeltaSeconds());
        while (clock.BeginStep())
            UpdateFlatSequential(world, fixedDelta, SystemCadence::Simulation);
        UpdateFlatSequential(world, deltaTime, SystemCadence::Frame);

        // Legacy parallel systems (separate from wave-based scheduling).
        // Slice 5: joined on a JobCounter instead of vector<TaskHandle>.
        if (!parallelSystems.empty() && jobSystem) {
            JobSystem::JobCounter waveCounter;
            for (size_t i = 0; i < parallelSystems.size(); ++i) {
                auto& system = parallelSystems[i];
                if (!system->IsEnabled()) continue;
                uint32 n = (i < parEveryNFrames.size()) ? parEveryNFrames[i] : 1u;
                if (n > 1u) {
                    uint32 c = (i < parFrameCounters.size()) ? parFrameCounters[i]++ : 0u;
                    if ((c % n) != 0u) continue;
                }
                const float32 systemDelta = FrameDelta(parTime[i], world, deltaTime);
                if (system->RequiresExclusiveUpdate()) {
                    jobSystem->Wait(waveCounter);
                    system->Update(world, systemDelta);
                } else {
                    ISystem* sys = system.get();
                    jobSystem->Run([sys, &world, systemDelta]() {
                        sys->Update(world, systemDelta);
                    }, waveCounter);
                }
            }
            jobSystem->Wait(waveCounter);
        } else {
            for (size_t i = 0; i < parallelSystems.size(); ++i) {
                auto& system = parallelSystems[i];
                if (system && system->IsEnabled())
                    system->Update(world, FrameDelta(parTime[i], world, deltaTime));
            }
        }
    }

private:
    // The delta a Frame system of `time` gets from the frame delta. Read after the clock advanced.
    static float32 FrameDelta(SystemTime time, World& world, float32 frameDelta) {
        if (time == SystemTime::Real)
            return frameDelta;
        return static_cast<float32>(world.GetSimulationClock().m_FrameSeconds);
    }

    // The delta a sequential system gets: the fixed delta for a Simulation system, as passed.
    float32 SystemDelta(size_t idx, World& world, float32 deltaTime) const {
        if (seqCadence[idx] == SystemCadence::Simulation)
            return deltaTime;
        return FrameDelta(seqTime[idx], world, deltaTime);
    }

    // The flat sequential path (no execution plan): the systems of one cadence in slot order.
    void UpdateFlatSequential(World& world, float32 deltaTime, SystemCadence cadence) {
        for (size_t i = 0; i < systems.size(); ++i) {
            auto& system = systems[i];
            if (!system || !system->IsEnabled()) continue; // null = retired slot
            if (seqCadence[i] != cadence) continue;

            const uint32 everyN = (i < seqEveryNFrames.size()) ? seqEveryNFrames[i] : 1u;
            if (everyN > 1u) {
                const uint32 runCount = (i < seqFrameCounters.size()) ? seqFrameCounters[i]++ : 0u;
                if ((runCount % everyN) != 0u) continue;
            }

            auto start = std::chrono::high_resolution_clock::now();
            system->Update(world, SystemDelta(i, world, deltaTime));
            auto end = std::chrono::high_resolution_clock::now();

            if (trackPerformance && i < performanceData.size()) {
                auto duration = std::chrono::duration<float32, std::milli>(end - start).count();
                UpdatePerformanceData(i, duration);
            }
        }
    }

    // Runs one system on the calling thread. With a traced frame it stamps
    // the body into `sample` (written only here, by this thread) and counts
    // the jobs the body publishes.
    void UpdateSequentialSystem(size_t idx, World& world, float32 deltaTime,
                                SystemWaveTrace::Frame* frame = nullptr,
                                uint16 sample = SystemWaveTrace::kNoSample) {
        deltaTime = SystemDelta(idx, world, deltaTime);
        if (frame && sample != SystemWaveTrace::kNoSample) {
            UpdateTracedSystem(idx, world, deltaTime, frame->Systems[sample]);
            return;
        }
        if (!trackPerformance || idx >= performanceData.size()) {
            systems[idx]->Update(world, deltaTime);
            return;
        }
        const auto start = std::chrono::high_resolution_clock::now();
        systems[idx]->Update(world, deltaTime);
        const auto duration = std::chrono::duration<float32, std::milli>(
            std::chrono::high_resolution_clock::now() - start).count();
        UpdatePerformanceData(idx, duration);
    }

    void UpdateTracedSystem(size_t idx, World& world, float32 deltaTime,
                            SystemWaveTrace::SystemSample& sample) {
        const size_t worker = jobSystem ? jobSystem->GetCurrentWorkerId() : SIZE_MAX;
        sample.Worker = worker < SystemWaveTrace::kCallerThread ? static_cast<uint16>(worker)
                                                                : SystemWaveTrace::kCallerThread;
        ISystem& system = *systems[idx];
        SystemWaveTrace::BodyInterval interval(system.GetName());
        JobSystem::WorkStealingThreadPool::PublishCountScope publishes(sample.Publishes);
        sample.BeginNs = SystemWaveTrace::NowNs();
        system.Update(world, deltaTime);
        sample.EndNs = SystemWaveTrace::NowNs();
        if (trackPerformance && idx < performanceData.size()) {
            UpdatePerformanceData(idx, static_cast<float32>(sample.EndNs - sample.BeginNs) * 1.0e-6f);
        }
    }

    // A traced system's sample in `frame`, or kNoSample when untraced.
    uint16 AddTracedSystem(SystemWaveTrace::Frame* frame, SystemWaveTrace::WaveSample* wave, size_t idx) {
        if (!frame || !wave)
            return SystemWaveTrace::kNoSample;
        return m_WaveTrace.AddSystem(*frame, *wave, static_cast<uint32>(idx));
    }

    void UpdateWaveRange(std::span<const size_t> enabled, size_t begin, size_t end,
                         World& world, float32 deltaTime,
                         SystemWaveTrace::Frame* frame, SystemWaveTrace::WaveSample* wave) {
        if (begin == end) return;
        if (end - begin == 1 || !jobSystem || !m_ParallelWaves) {
            for (size_t i = begin; i < end; ++i)
                UpdateSequentialSystem(enabled[i], world, deltaTime, frame,
                                       AddTracedSystem(frame, wave, enabled[i]));
            return;
        }
        if (frame && wave) {
            UpdateTracedFork(enabled, begin, end, world, deltaTime, *frame, *wave);
            return;
        }
        JobSystem::JobCounter waveCounter;
        for (size_t i = begin; i < end; ++i) {
            const size_t idx = enabled[i];
            jobSystem->Run([this, idx, &world, deltaTime]() {
                UpdateSequentialSystem(idx, world, deltaTime);
            }, waveCounter);
        }
        jobSystem->Wait(waveCounter);
    }

    // The forked range of a traced wave: a publish stamp per system, the
    // publish loop's jobs counted, the join timed.
    void UpdateTracedFork(std::span<const size_t> enabled, size_t begin, size_t end,
                          World& world, float32 deltaTime,
                          SystemWaveTrace::Frame& frame, SystemWaveTrace::WaveSample& wave) {
        JobSystem::JobCounter waveCounter;
        {
            JobSystem::WorkStealingThreadPool::PublishCountScope publishes(frame.WaveForkPublishes);
            for (size_t i = begin; i < end; ++i) {
                const size_t idx = enabled[i];
                const uint16 sample = m_WaveTrace.AddSystem(frame, wave, static_cast<uint32>(idx));
                if (sample != SystemWaveTrace::kNoSample)
                    frame.Systems[sample].PublishNs = SystemWaveTrace::NowNs();
                ++wave.Forks;
                SystemWaveTrace::Frame* framePtr = &frame;
                jobSystem->Run([this, idx, &world, deltaTime, framePtr, sample]() {
                    UpdateSequentialSystem(idx, world, deltaTime, framePtr, sample);
                }, waveCounter);
            }
        }
        SystemWaveTrace::JoinInterval interval(wave.PlanWave, static_cast<uint32>(end - begin));
        const uint64 joinBegin = SystemWaveTrace::NowNs();
        jobSystem->Wait(waveCounter);
        wave.JoinWaitNs += SystemWaveTrace::NowNs() - joinBegin;
        ++wave.Joins;
    }

    // Each Frame wave once with the frame delta. Each run of adjacent Simulation waves once per
    // clock step with the fixed delta; every run of the frame steps the same ticks.
    void UpdateWaveBased(World& world, float32 deltaTime) {
        SystemWaveTrace::Frame* frame = nullptr;
        if (m_WaveTrace.IsEnabled())
            frame = &m_WaveTrace.BeginFrame(jobSystem ? jobSystem->GetPublishedJobCount() : 0);
        SimulationClock& clock = world.GetSimulationClock();
        const float32 fixedDelta = static_cast<float32>(clock.GetFixedDeltaSeconds());
        const std::vector<SystemExecutionPlan::Wave>& waves = m_ExecutionPlan.Waves;
        size_t runBegin = 0;
        while (runBegin < waves.size()) {
            const SystemCadence cadence = waves[runBegin].Cadence;
            size_t runEnd = runBegin + 1;
            while (runEnd < waves.size() && waves[runEnd].Cadence == cadence)
                ++runEnd;
            if (cadence == SystemCadence::Frame) {
                UpdateWaves(world, deltaTime, runBegin, runEnd, frame);
            } else {
                clock.RestartSteps();
                while (clock.BeginStep())
                    UpdateWaves(world, fixedDelta, runBegin, runEnd, frame);
            }
            runBegin = runEnd;
        }
        if (frame)
            m_WaveTrace.EndFrame(*frame, jobSystem ? jobSystem->GetPublishedJobCount() : 0);
    }

    // Runs the plan waves in [firstWave, endWave).
    void UpdateWaves(World& world, float32 deltaTime, size_t firstWave, size_t endWave,
                     SystemWaveTrace::Frame* frame) {
        for (size_t planWave = firstWave; planWave < endWave; ++planWave) {
            const auto& wave = m_ExecutionPlan.Waves[planWave];
            // Collect enabled systems in this wave. The list lives in the calling
            // thread's scratch until the wave has joined; forked bodies receive
            // indices, never the list.
            Memory::ScratchScope scratch;
            std::span<size_t> enabledSlots =
                scratch.AllocateArrayForOverwrite<size_t>(wave.SystemIndices.size());
            size_t enabledCount = 0;
            for (size_t idx : wave.SystemIndices) {
                if (idx >= systems.size()) continue;
                auto& sys = systems[idx];
                if (!sys || !sys->IsEnabled()) continue;

                // Frequency control
                uint32 n = (idx < seqEveryNFrames.size()) ? seqEveryNFrames[idx] : 1u;
                if (n > 1u) {
                    uint32 c = (idx < seqFrameCounters.size()) ? seqFrameCounters[idx]++ : 0u;
                    if ((c % n) != 0u) continue;
                }
                enabledSlots[enabledCount++] = idx;
            }

            if (enabledCount == 0) continue;
            const std::span<const size_t> enabled = enabledSlots.first(enabledCount);

            SystemWaveTrace::WaveSample* waveSample =
                frame ? m_WaveTrace.BeginWave(*frame, static_cast<uint32>(planWave)) : nullptr;

            // Each exclusive system splits this wave into joined ranges. It
            // never leaves the caller thread or overlaps a neighboring range.
            // No extra vector or task registry is needed for the split.
            size_t begin = 0;
            for (size_t i = 0; i < enabled.size(); ++i) {
                if (!systems[enabled[i]]->RequiresExclusiveUpdate()) continue;
                UpdateWaveRange(enabled, begin, i, world, deltaTime, frame, waveSample);
                UpdateSequentialSystem(enabled[i], world, deltaTime, frame,
                                       AddTracedSystem(frame, waveSample, enabled[i]));
                begin = i + 1;
            }
            UpdateWaveRange(enabled, begin, enabled.size(), world, deltaTime, frame, waveSample);

            if (waveSample)
                m_WaveTrace.EndWave(*frame, *waveSample);
        }
    }

public:

    // Enable/disable performance tracking
    void SetPerformanceTracking(bool enable) {
        trackPerformance = enable;
        if (enable && performanceData.size() != systems.size()) {
            performanceData.clear();
            for (const auto& system : systems) {
                SystemPerformance perf;
                perf.Name = system ? system->GetName() : "<retired>";
                performanceData.push_back(perf);
            }
        }
    }

    // Get performance data
    const std::vector<SystemPerformance>& GetPerformanceData() const {
        return performanceData;
    }

    // Clear every system's MaxTime, so the next read reports the slowest single
    // update since this call rather than since tracking began.
    void ResetPerformanceMaxima() {
        for (auto& perf : performanceData) {
            perf.MaxTime = 0.0f;
        }
    }

    // Get system by type
    template<typename T>
    T* GetSystem() {
        for (auto& system : systems) {
            if (auto* ptr = dynamic_cast<T*>(system.get())) {
                return ptr;
            }
        }
        for (auto& system : parallelSystems) {
            if (auto* ptr = dynamic_cast<T*>(system.get())) {
                return ptr;
            }
        }
        return nullptr;
    }

    // Enable/disable a system by name. Returns true if a system was matched.
    bool SetSystemEnabledByName(const std::string& systemName, bool enable)
    {
        bool matched = false;
        for (auto& s : systems)
        {
            if (s && s->GetName() && systemName == s->GetName())
            {
                s->SetEnabled(enable);
                matched = true;
            }
        }
        for (auto& s : parallelSystems)
        {
            if (s && s->GetName() && systemName == s->GetName())
            {
                s->SetEnabled(enable);
                matched = true;
            }
        }
        return matched;
    }

    // Configure update frequency (every N frames). Returns true if a system was matched.
    bool SetSystemEveryNFramesByName(const std::string& systemName, uint32 n) {
        // Sequential systems
        for (size_t i = 0; i < systems.size(); ++i) {
            auto& s = systems[i];
            if (s && s->GetName() && systemName == s->GetName()) {
                if (i >= seqEveryNFrames.size()) seqEveryNFrames.resize(i+1, 1u);
                if (i >= seqFrameCounters.size()) seqFrameCounters.resize(i+1, 0u);
                seqEveryNFrames[i] = (n < 1u) ? 1u : n;
                seqFrameCounters[i] = 0u;
                return true;
            }
        }
        // Parallel systems
        for (size_t i = 0; i < parallelSystems.size(); ++i) {
            auto& s = parallelSystems[i];
            if (s && s->GetName() && systemName == s->GetName()) {
                if (i >= parEveryNFrames.size()) parEveryNFrames.resize(i+1, 1u);
                if (i >= parFrameCounters.size()) parFrameCounters.resize(i+1, 0u);
                parEveryNFrames[i] = (n < 1u) ? 1u : n;
                parFrameCounters[i] = 0u;
                return true;
            }
        }
        return false;
    }

    template<typename T>
    bool SetEveryNFrames(uint32 n) {
        // Sequential
        for (size_t i = 0; i < systems.size(); ++i) {
            if (dynamic_cast<T*>(systems[i].get())) {
                if (i >= seqEveryNFrames.size()) seqEveryNFrames.resize(i+1, 1u);
                if (i >= seqFrameCounters.size()) seqFrameCounters.resize(i+1, 0u);
                seqEveryNFrames[i] = (n < 1u) ? 1u : n;
                seqFrameCounters[i] = 0u;
                return true;
            }
        }
        // Parallel
        for (size_t i = 0; i < parallelSystems.size(); ++i) {
            if (dynamic_cast<T*>(parallelSystems[i].get())) {
                if (i >= parEveryNFrames.size()) parEveryNFrames.resize(i+1, 1u);
                if (i >= parFrameCounters.size()) parFrameCounters.resize(i+1, 0u);
                parEveryNFrames[i] = (n < 1u) ? 1u : n;
                parFrameCounters[i] = 0u;
                return true;
            }
        }
        return false;
    }

    // Remove all systems. The execution plan and per-index bookkeeping refer to
    // the cleared vector — they must die with it or stale indices leak into the
    // next registration round.
    void Clear() {
        systems.clear();
        parallelSystems.clear();
        performanceData.clear();
        m_ExecutionPlan = {};
        m_LateAddedSystems.clear();
        m_WaveTrace.Clear();
        seqEveryNFrames.clear();
        seqFrameCounters.clear();
        parEveryNFrames.clear();
        parFrameCounters.clear();
        seqCadence.clear();
        seqTime.clear();
        parTime.clear();
        Logger::Log::Debug("[ECS] All systems cleared");
    }

private:
    void UpdatePerformanceData(size_t index, float32 frameTime) {
        auto& perf = performanceData[index];
        perf.UpdateCount++;
        perf.LastTime = frameTime;

        // Update average (exponential moving average)
        const float32 alpha = 0.1f;
        perf.AverageTime = perf.AverageTime * (1.0f - alpha) + frameTime * alpha;

        // Update max
        if (frameTime > perf.MaxTime) {
            perf.MaxTime = frameTime;
        }
    }
};

// ============================================================================
// EXAMPLE SYSTEMS (MOVED TO TESTS/EXAMPLES)
// ============================================================================
//
// The ECS core no longer contains hardcoded system implementations.
// Systems should be defined in the libraries/applications that use them.
//
// For examples of how to implement systems, see:
// - Tests/ECS/TestSystems.h
// - Engine/Include/Systems/ (for engine-specific systems)
//
// This maintains the modular architecture where ECS core remains minimal
// and doesn't hardcode knowledge of specific component types.
// ============================================================================



} // namespace ECS
} // namespace GameEngine
