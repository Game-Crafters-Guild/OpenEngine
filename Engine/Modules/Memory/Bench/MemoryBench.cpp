// MemoryBench: the Memory module's micro-benches, run in Release. Not built by default
// (EXCLUDE_FROM_ALL): build the MemoryBench target and run it.
//
//     MemoryBench ArenaBump [samples]
//     MemoryBench ObjectPoolChurn <pool|heap> <small|large> [samples]
//     MemoryBench Scratch AB|BA [samples]
//     MemoryBench RangeChurn range <1k|10k> [samples]
//
// ArenaBump: one sample is a frame of 4096 bump allocations of 16, 64 and 256 bytes in a
// fixed shuffled order, each written once, then a Memory::Arena Reset; it reports
// nanoseconds per allocation. A comparison builds this bench at both commits and runs the
// two executables in paired visits.
//
// ObjectPoolChurn: a live set of objects (small: 1,024 of 64 bytes; large: 65,536 of 4 KiB,
// 70 pool blocks) churned by 4096 Delete-then-New pairs per sample, each new object written
// once; it reports nanoseconds per pair. The live-set indices come from one seeded generator
// that continues across samples, so no sample replays another's sequence: an allocator can lay
// out a replayed sequence's objects in visiting order, and the bench would then measure that
// layout instead of the allocator. `pool` uses a Memory::ObjectPool<T>, `heap` the global new
// and delete of the same type, so both arms are this one executable, run alternately in
// paired visits. `pool` also prints the pool's block count from the stats snapshot.
//
// Scratch: one call is a scope opened, allocations of 16, 64 and 256 bytes each written once,
// and the scope closed: (a) as the thread's outermost scope, (b) nested inside an open one.
// Arm A, compiled beside, is the shape ScratchScope replaces: three retained thread_local
// vectors cleared and resized to the same sizes. One sample is 1000 calls; it reports
// nanoseconds per call. Both arms run in one process, in the order given.
//
// RangeChurn: a mesh stream's ranges in a Memory::RangeAllocator, counted in 32-byte vertices:
// a live set of 1,000 or 10,000 ranges with vertex counts log-uniform from 24 to 65,536, in a
// space 1.25 times the live set's expected size. One sample is a frame: 256 pairs, each a range
// of the live set queued with FreeAfter (retired four frames later) and a new range allocated in
// its place, then ReclaimThrough the frame; it reports nanoseconds per pair. Sizes and live-set
// indices come from one seeded generator that continues across samples. A request that does not
// fit leaves its live-set entry empty and is counted. After the last sample everything queued is
// reclaimed and it prints the fragmentation ((free - largest free) / free), used bytes over the
// live set's requested bytes, the refused requests and the free ranges.
//
// Every case takes 100 warm-up samples first, then prints the median and p99 over the
// remaining samples.

#include "Memory/Arena.h"
#include "Memory/ObjectPool.h"
#include "Memory/RangeAllocator.h"
#include "Memory/ScratchScope.h"
#include "Memory/StatsSnapshot.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace
{
using GameEngine::Memory::Arena;

constexpr std::size_t kAllocationsPerSample = 4096;
constexpr std::size_t kWarmupSamples = 100;
constexpr std::size_t kDefaultSamples = 2000;

/// The request sizes of one sample, 16, 64 and 256 bytes in equal shares, in an order fixed by
/// a seeded generator so every run sees the same sequence.
std::vector<std::size_t> MakeSizes()
{
    constexpr std::array<std::size_t, 3> kSizes = {16, 64, 256};
    std::vector<std::size_t> sizes(kAllocationsPerSample);
    std::uint32_t state = 0x9E3779B9u;
    for (std::size_t index = 0; index < sizes.size(); ++index)
    {
        state = state * 1664525u + 1013904223u;
        sizes[index] = kSizes[(state >> 16) % kSizes.size()];
    }
    return sizes;
}

struct Summary
{
    double MedianNanoseconds;
    double P99Nanoseconds;
};

/// The nearest-rank quantile of sorted samples.
double Quantile(const std::vector<double>& sorted, double quantile)
{
    return sorted[static_cast<std::size_t>(quantile * static_cast<double>(sorted.size() - 1))];
}

Summary Summarize(std::vector<double>& samples)
{
    std::sort(samples.begin(), samples.end());
    return {Quantile(samples, 0.5), Quantile(samples, 0.99)};
}

/// Written with every allocation's address, so the bumps cannot be optimized away.
volatile std::uintptr_t g_Sink = 0;

/// Times `samples` frames of `allocate` over `sizes`, each followed by `reset`, after the
/// warm-up; returns nanoseconds per allocation per frame.
template <class Allocate, class Reset>
std::vector<double> MeasureFrames(const std::vector<std::size_t>& sizes, std::size_t samples, Allocate allocate,
                                  Reset reset)
{
    std::vector<double> perAllocation;
    perAllocation.reserve(samples);
    std::uintptr_t sink = 0;
    for (std::size_t sample = 0; sample < kWarmupSamples + samples; ++sample)
    {
        const auto start = std::chrono::steady_clock::now();
        for (const std::size_t bytes : sizes)
        {
            auto* memory = static_cast<unsigned char*>(allocate(bytes));
            *memory = static_cast<unsigned char>(bytes);
            sink ^= reinterpret_cast<std::uintptr_t>(memory);
        }
        const auto end = std::chrono::steady_clock::now();
        reset();
        if (sample >= kWarmupSamples)
            perAllocation.push_back(std::chrono::duration<double, std::nano>(end - start).count() /
                                    static_cast<double>(sizes.size()));
    }
    g_Sink = sink;
    return perAllocation;
}

Summary RunArena(const std::vector<std::size_t>& sizes, std::size_t samples)
{
    Arena arena("MemoryBench.ArenaBump");
    std::vector<double> perAllocation = MeasureFrames(
        sizes, samples, [&arena](std::size_t bytes) { return arena.Allocate(bytes); }, [&arena] { arena.Reset(); });
    return Summarize(perAllocation);
}

void Print(const Summary& summary, std::size_t samples)
{
    std::printf("ArenaBump samples=%zu median_ns_per_alloc=%.4f p99_ns_per_alloc=%.4f\n", samples,
                summary.MedianNanoseconds, summary.P99Nanoseconds);
}

constexpr std::size_t kPairsPerSample = 4096;
constexpr std::size_t kSmallLiveObjects = 1024;
constexpr std::size_t kLargeLiveObjects = 65536;
constexpr std::string_view kPoolStatsName = "MemoryBench.ObjectPoolChurn";

struct SmallObject
{
    std::array<std::uint64_t, 8> Words{};
};
static_assert(sizeof(SmallObject) == 64);

struct LargeObject
{
    std::array<std::uint64_t, 512> Words{};
};
static_assert(sizeof(LargeObject) == 4096);

/// Fills a live set of `liveObjects` through `create`, then times samples of Delete-then-New
/// pairs through `destroy` and `create` after the warm-up; returns nanoseconds per pair per
/// sample. The live set is destroyed at the end.
template <class T, class Create, class Destroy>
std::vector<double> MeasureChurn(std::size_t liveObjects, std::size_t samples, Create create, Destroy destroy)
{
    std::vector<T*> live(liveObjects);
    for (T*& object : live)
        object = create();
    std::vector<double> perPair;
    perPair.reserve(samples);
    std::uintptr_t sink = 0;
    std::uint32_t state = 0x2545F491u;
    for (std::size_t sample = 0; sample < kWarmupSamples + samples; ++sample)
    {
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t pair = 0; pair < kPairsPerSample; ++pair)
        {
            state = state * 1664525u + 1013904223u;
            const std::size_t index = (state >> 8) % liveObjects;
            destroy(live[index]);
            T* object = create();
            object->Words[0] = index;
            live[index] = object;
            sink ^= reinterpret_cast<std::uintptr_t>(object);
        }
        const auto end = std::chrono::steady_clock::now();
        if (sample >= kWarmupSamples)
            perPair.push_back(std::chrono::duration<double, std::nano>(end - start).count() /
                              static_cast<double>(kPairsPerSample));
    }
    for (T* object : live)
        destroy(object);
    g_Sink = sink;
    return perPair;
}

std::uint64_t PoolBlockCount()
{
    for (const GameEngine::Memory::StatsEntry& entry : GameEngine::Memory::SnapshotStats())
    {
        if (entry.Name == kPoolStatsName)
            return entry.Blocks;
    }
    return 0;
}

/// Runs one arm of ObjectPoolChurn for `T` and prints its line.
template <class T>
void RunObjectPoolChurn(bool usePool, std::string_view caseName, std::size_t liveObjects, std::size_t samples)
{
    std::vector<double> perPair;
    std::uint64_t blocks = 0;
    if (usePool)
    {
        GameEngine::Memory::ObjectPool<T> pool(kPoolStatsName);
        perPair = MeasureChurn<T>(
            liveObjects, samples, [&pool] { return pool.New(); }, [&pool](T* object) { pool.Delete(object); });
        blocks = PoolBlockCount();
    }
    else
    {
        perPair = MeasureChurn<T>(
            liveObjects, samples, [] { return new T(); }, [](T* object) { delete object; });
    }
    const Summary summary = Summarize(perPair);
    std::printf("ObjectPoolChurn arm=%s case=%.*s live=%zu samples=%zu blocks=%llu median_ns_per_pair=%.4f "
                "p99_ns_per_pair=%.4f\n",
                usePool ? "pool" : "heap", static_cast<int>(caseName.size()), caseName.data(), liveObjects, samples,
                static_cast<unsigned long long>(blocks), summary.MedianNanoseconds, summary.P99Nanoseconds);
}

constexpr std::size_t kScratchCallsPerSample = 1000;
constexpr std::array<std::size_t, 3> kScratchSizes = {16, 64, 256};

/// Times `samples` samples of `kScratchCallsPerSample` calls of `call` after the warm-up;
/// returns nanoseconds per call per sample.
template <class Call>
std::vector<double> MeasureCalls(std::size_t samples, Call call)
{
    std::vector<double> perCall;
    perCall.reserve(samples);
    std::uintptr_t sink = 0;
    for (std::size_t sample = 0; sample < kWarmupSamples + samples; ++sample)
    {
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < kScratchCallsPerSample; ++index)
            sink ^= call();
        const auto end = std::chrono::steady_clock::now();
        if (sample >= kWarmupSamples)
            perCall.push_back(std::chrono::duration<double, std::nano>(end - start).count() /
                              static_cast<double>(kScratchCallsPerSample));
    }
    g_Sink = sink;
    return perCall;
}

/// One scope: the three allocations, each written once.
std::uintptr_t ScratchCall()
{
    GameEngine::Memory::ScratchScope scratch;
    std::uintptr_t sink = 0;
    for (const std::size_t bytes : kScratchSizes)
    {
        auto* memory = static_cast<unsigned char*>(scratch.Allocate(bytes));
        *memory = static_cast<unsigned char>(bytes);
        sink ^= reinterpret_cast<std::uintptr_t>(memory);
    }
    return sink;
}

/// Arm A: the retained thread_local vectors a scratch scope replaces.
std::uintptr_t VectorCall()
{
    thread_local std::array<std::vector<unsigned char>, 3> t_Vectors;
    std::uintptr_t sink = 0;
    for (std::size_t index = 0; index < kScratchSizes.size(); ++index)
    {
        std::vector<unsigned char>& vector = t_Vectors[index];
        vector.clear();
        vector.resize(kScratchSizes[index]);
        vector[0] = static_cast<unsigned char>(kScratchSizes[index]);
        sink ^= reinterpret_cast<std::uintptr_t>(vector.data());
    }
    return sink;
}

void PrintCase(const char* arm, const char* scratchCase, std::size_t samples, std::vector<double> perCall)
{
    const Summary summary = Summarize(perCall);
    std::printf("Scratch arm=%s case=%s samples=%zu median_ns_per_call=%.4f p99_ns_per_call=%.4f\n", arm,
                scratchCase, samples, summary.MedianNanoseconds, summary.P99Nanoseconds);
}

void RunScratchArm(char arm, std::size_t samples)
{
    if (arm == 'A')
    {
        PrintCase("A", "outermost", samples, MeasureCalls(samples, VectorCall));
        PrintCase("A", "nested", samples, MeasureCalls(samples, VectorCall));
        return;
    }
    PrintCase("B", "outermost", samples, MeasureCalls(samples, ScratchCall));
    GameEngine::Memory::ScratchScope outer;
    static_cast<unsigned char*>(outer.Allocate(64))[0] = 1;
    PrintCase("B", "nested", samples, MeasureCalls(samples, ScratchCall));
}

constexpr std::size_t kRangePairsPerSample = 256;
constexpr std::uint64_t kRangeRetireFrames = 4;
constexpr std::uint64_t kVertexBytes = 32;
constexpr double kMinVertices = 24.0;
constexpr double kMaxVertices = 65536.0;
constexpr double kRangeSpaceHeadroom = 1.25;
constexpr std::size_t kRangeSizeTableLength = 1 << 16;

/// Vertex counts log-uniform over [kMinVertices, kMaxVertices], drawn once so the timed loop
/// only indexes the table.
std::vector<std::uint64_t> MakeVertexCounts()
{
    std::vector<std::uint64_t> counts(kRangeSizeTableLength);
    std::uint32_t state = 0x6A09E667u;
    for (std::uint64_t& count : counts)
    {
        state = state * 1664525u + 1013904223u;
        const double unit = static_cast<double>(state >> 8) / static_cast<double>(1u << 24);
        count = static_cast<std::uint64_t>(kMinVertices * std::pow(kMaxVertices / kMinVertices, unit));
    }
    return counts;
}

/// The expected vertex count of one range: the mean of the log-uniform distribution.
double ExpectedVertices()
{
    return (kMaxVertices - kMinVertices) / std::log(kMaxVertices / kMinVertices);
}

/// The RangeAllocator arm: one unit is one vertex.
class RangeArm
{
  public:
    using Handle = GameEngine::Memory::RangeAllocator::Allocation;

    explicit RangeArm(std::uint64_t capacityBytes)
        : m_Ranges("MemoryBench.RangeChurn", GameEngine::Memory::Residency::Gpu, kVertexBytes,
                   capacityBytes / kVertexBytes, 1024)
    {
    }
    Handle Allocate(std::uint64_t bytes) { return m_Ranges.Allocate((bytes + kVertexBytes - 1) / kVertexBytes); }
    void FreeAfter(Handle handle, std::uint64_t stamp) { m_Ranges.FreeAfter(handle, stamp); }
    void Reclaim(std::uint64_t stamp) { m_Ranges.ReclaimThrough(stamp); }
    double Fragmentation() const { return m_Ranges.GetStats().Fragmentation; }
    std::uint64_t UsedBytes() const { return m_Ranges.Used() * kVertexBytes; }
    std::uint32_t FreeRanges() const { return m_Ranges.GetStats().FreeRanges; }
    static const char* Name() { return "range"; }

  private:
    GameEngine::Memory::RangeAllocator m_Ranges;
};

/// Runs RangeChurn with `liveRanges` live ranges and prints its line.
void RunRangeChurn(std::string_view caseName, std::size_t liveRanges, std::size_t samples)
{
    const std::vector<std::uint64_t> vertexCounts = MakeVertexCounts();
    const auto capacityBytes = static_cast<std::uint64_t>(static_cast<double>(liveRanges) * ExpectedVertices() *
                                                          kRangeSpaceHeadroom) *
                               kVertexBytes;
    RangeArm arm(capacityBytes);
    std::vector<RangeArm::Handle> live(liveRanges);
    std::vector<std::uint64_t> requested(liveRanges, 0);
    std::uint64_t refused = 0;
    std::uint32_t state = 0xBB67AE85u;
    for (std::size_t index = 0; index < liveRanges; ++index)
    {
        state = state * 1664525u + 1013904223u;
        const std::uint64_t bytes = vertexCounts[(state >> 8) % vertexCounts.size()] * kVertexBytes;
        live[index] = arm.Allocate(bytes);
        requested[index] = live[index].IsValid() ? bytes : 0;
        refused += live[index].IsValid() ? 0 : 1;
    }

    std::vector<double> perPair;
    perPair.reserve(samples);
    for (std::size_t sample = 0; sample < kWarmupSamples + samples; ++sample)
    {
        const std::uint64_t frame = sample + 1;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t pair = 0; pair < kRangePairsPerSample; ++pair)
        {
            state = state * 1664525u + 1013904223u;
            const std::size_t index = (state >> 8) % liveRanges;
            state = state * 1664525u + 1013904223u;
            const std::uint64_t bytes = vertexCounts[(state >> 8) % vertexCounts.size()] * kVertexBytes;
            arm.FreeAfter(live[index], frame + kRangeRetireFrames);
            live[index] = arm.Allocate(bytes);
            requested[index] = live[index].IsValid() ? bytes : 0;
            refused += live[index].IsValid() ? 0 : 1;
        }
        arm.Reclaim(frame);
        const auto end = std::chrono::steady_clock::now();
        if (sample >= kWarmupSamples)
            perPair.push_back(std::chrono::duration<double, std::nano>(end - start).count() /
                              static_cast<double>(kRangePairsPerSample));
    }

    arm.Reclaim(UINT64_MAX);
    std::uint64_t requestedBytes = 0;
    for (const std::uint64_t bytes : requested)
        requestedBytes += bytes;
    const Summary summary = Summarize(perPair);
    std::printf("RangeChurn arm=%s case=%.*s live=%zu samples=%zu median_ns_per_pair=%.4f p99_ns_per_pair=%.4f "
                "fragmentation=%.4f used_over_requested=%.6f refused=%llu free_ranges=%u\n",
                RangeArm::Name(), static_cast<int>(caseName.size()), caseName.data(), liveRanges, samples,
                summary.MedianNanoseconds, summary.P99Nanoseconds, arm.Fragmentation(),
                static_cast<double>(arm.UsedBytes()) / static_cast<double>(requestedBytes),
                static_cast<unsigned long long>(refused), arm.FreeRanges());
}

int Usage()
{
    std::fprintf(stderr, "usage: MemoryBench ArenaBump [samples]\n"
                         "       MemoryBench ObjectPoolChurn <pool|heap> <small|large> [samples]\n"
                         "       MemoryBench Scratch AB|BA [samples]\n"
                         "       MemoryBench RangeChurn range <1k|10k> [samples]\n");
    return 2;
}

/// The positive sample count at `argv[index]`, the default when absent, or 0 when invalid.
std::size_t ParseSamples(int argc, char** argv, int index)
{
    return argc > index ? std::strtoull(argv[index], nullptr, 10) : kDefaultSamples;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        return Usage();
    const std::string_view benchName = argv[1];
    if (benchName == "ArenaBump")
    {
        const std::size_t samples = ParseSamples(argc, argv, 2);
        if (samples == 0)
            return Usage();
        Print(RunArena(MakeSizes(), samples), samples);
        return 0;
    }
    if (benchName == "ObjectPoolChurn" && argc >= 4)
    {
        const std::string_view arm = argv[2];
        const std::string_view caseName = argv[3];
        const std::size_t samples = ParseSamples(argc, argv, 4);
        if ((arm != "pool" && arm != "heap") || (caseName != "small" && caseName != "large") || samples == 0)
            return Usage();
        if (caseName == "small")
            RunObjectPoolChurn<SmallObject>(arm == "pool", caseName, kSmallLiveObjects, samples);
        else
            RunObjectPoolChurn<LargeObject>(arm == "pool", caseName, kLargeLiveObjects, samples);
        return 0;
    }
    if (benchName == "Scratch" && argc > 2)
    {
        const std::string_view order = argv[2];
        const std::size_t samples = ParseSamples(argc, argv, 3);
        if ((order != "AB" && order != "BA") || samples == 0)
            return Usage();
        for (const char arm : order)
            RunScratchArm(arm, samples);
        return 0;
    }
    if (benchName == "RangeChurn" && argc >= 4)
    {
        const std::string_view arm = argv[2];
        const std::string_view caseName = argv[3];
        const std::size_t samples = ParseSamples(argc, argv, 4);
        if (arm != "range" || (caseName != "1k" && caseName != "10k") || samples == 0)
            return Usage();
        RunRangeChurn(caseName, caseName == "1k" ? 1000 : 10000, samples);
        return 0;
    }
    return Usage();
}
