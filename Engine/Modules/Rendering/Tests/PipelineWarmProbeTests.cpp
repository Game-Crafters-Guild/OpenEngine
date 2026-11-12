// IDevice::TryGetWarmGraphicsPipeline — the non-building cache probe the
// render-graph record path uses so a cold (pipeline, pass format) pair costs a
// skipped draw instead of a backend shader compile on the record thread — and
// IDevice::Request*Pipeline, which hands that build to the device's dispatcher
// and builds each pipeline once.
#include <gtest/gtest.h>

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineCache.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "TestUtils.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
std::unique_ptr<IDevice> CreateHeadlessDevice(DeviceDesc& outDesc)
{
    // Headless: no swapchain render pass, so dynamic rendering is required for
    // graphics pipeline creation (same setup as PipelineLayoutDedupTests).
    outDesc = DeviceDesc{};
    outDesc.preferredAPI = GraphicsAPI::Vulkan;
    outDesc.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(outDesc);
    if (!dev || !dev->Initialize(outDesc))
        return nullptr;
    return dev;
}

// Interned graphics pipeline over the stock triangle shaders, with one UBO at
// set 0 / binding 0.
bool InternTriangle(IDevice& dev, GraphicsPipelineId& outId)
{
    std::vector<uint8_t> vs = Utils::LoadShaderFile("triangle.vert.spv");
    std::vector<uint8_t> fs = Utils::LoadShaderFile("triangle.frag.spv");
    if (vs.empty() || fs.empty())
        return false;

    DescriptorSetLayoutDesc set0{};
    DescriptorBinding b0{};
    b0.binding = 0;
    b0.type = DescriptorType::UniformBuffer;
    b0.count = 1;
    b0.shaderStages = kShaderStageVertex;
    set0.bindings.push_back(b0);

    GraphicsPipelineDesc gd{};
    gd.DebugName = "WarmProbeTriangle";
    gd.VertexShader = std::make_shared<std::vector<uint8_t>>(std::move(vs));
    gd.PixelShader = std::make_shared<std::vector<uint8_t>>(std::move(fs));
    gd.DescriptorSetLayouts = {dev.InternDescriptorSetLayout(set0)};
    gd.ColorBlend.attachments.resize(1);

    outId = dev.InternGraphicsPipeline(gd);
    return outId.IsValid();
}

// Interned compute pipeline over the stock minimal kernel (one storage buffer at
// set 0 / binding 0).
ComputePipelineId InternMinimalCompute(IDevice& dev)
{
    std::vector<uint8_t> cs = Utils::LoadShaderFile("minimal_test.comp.spv");
    if (cs.empty())
        return {};
    DescriptorSetLayoutDesc set0{};
    DescriptorBinding b0{};
    b0.binding = 0;
    b0.type = DescriptorType::StorageBuffer;
    b0.count = 1;
    b0.shaderStages = kShaderStageCompute;
    set0.bindings.push_back(b0);

    ComputePipelineDesc cd{};
    cd.DebugName = "WarmProbeCompute";
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(cs));
    cd.DescriptorSetLayouts = {dev.InternDescriptorSetLayout(set0)};
    return dev.InternComputePipeline(cd);
}

// Stands in for the engine's worker dispatcher: keeps every build job until the
// test runs it, so "not built on the calling thread" is observable.
struct HeldBuilds
{
    std::vector<std::function<void()>> Jobs;

    void Install(IDevice& dev)
    {
        dev.SetPipelineBuildDispatcher(
            {.Dispatch = [this](std::function<void()> job, PipelineBuildPriority) { Jobs.push_back(std::move(job)); }});
    }
    void RunAll()
    {
        std::vector<std::function<void()>> jobs = std::move(Jobs);
        Jobs.clear();
        for (std::function<void()>& job : jobs)
            job();
    }
};

// Stands in for the engine's job workers: one thread runs each Blocking job as
// it comes, and Load jobs are held until the test takes them. Every thread but
// that worker may wait for a build.
class WorkerBuilds
{
  public:
    explicit WorkerBuilds(IDevice& dev) : m_Device(dev), m_Worker([this] { RunUntilStopped(); })
    {
        m_Device.SetPipelineBuildDispatcher(
            {.Dispatch = [this](std::function<void()> job, PipelineBuildPriority priority)
             { Push(std::move(job), priority); },
             .CallerMayWait = [this] { return std::this_thread::get_id() != m_Worker.get_id(); }});
    }
    ~WorkerBuilds()
    {
        m_Device.SetPipelineBuildDispatcher({});
        {
            std::lock_guard lock(m_Mutex);
            m_Stop = true;
        }
        m_Ready.notify_one();
        m_Worker.join();
    }
    uint32_t BlockingJobsRun()
    {
        std::lock_guard lock(m_Mutex);
        return m_BlockingJobsRun;
    }
    std::vector<std::function<void()>> TakeLoadJobs()
    {
        std::lock_guard lock(m_Mutex);
        return std::exchange(m_LoadJobs, {});
    }
    // Runs `work` as a job on the worker thread and waits for it to end.
    void RunOnWorker(std::function<void()> work)
    {
        auto done = std::make_shared<std::promise<void>>();
        std::future<void> ended = done->get_future();
        Push(
            [work = std::move(work), done]
            {
                work();
                done->set_value();
            },
            PipelineBuildPriority::Blocking);
        ended.wait();
    }
    // The next Blocking job runs at once on the thread that dispatches it, and
    // then `after` runs on that thread.
    void RunNextBlockingJobOnTheCaller(std::function<void()> after)
    {
        std::lock_guard lock(m_Mutex);
        m_AfterJobOnTheCaller = std::move(after);
    }

  private:
    void Push(std::function<void()> job, PipelineBuildPriority priority)
    {
        std::unique_lock lock(m_Mutex);
        if (priority == PipelineBuildPriority::Load)
        {
            m_LoadJobs.push_back(std::move(job));
            return;
        }
        if (m_AfterJobOnTheCaller)
        {
            std::function<void()> after = std::exchange(m_AfterJobOnTheCaller, nullptr);
            ++m_BlockingJobsRun;
            lock.unlock();
            job();
            after();
            return;
        }
        m_Queue.push_back(std::move(job));
        m_Ready.notify_one();
    }
    void RunUntilStopped()
    {
        std::unique_lock lock(m_Mutex);
        for (;;)
        {
            m_Ready.wait(lock, [this] { return m_Stop || !m_Queue.empty(); });
            if (m_Queue.empty())
                return;
            std::function<void()> job = std::move(m_Queue.front());
            m_Queue.pop_front();
            ++m_BlockingJobsRun;
            lock.unlock();
            job();
            job = nullptr;
            lock.lock();
        }
    }

    IDevice& m_Device;
    std::mutex m_Mutex;
    std::condition_variable m_Ready;
    std::deque<std::function<void()>> m_Queue;
    std::vector<std::function<void()>> m_LoadJobs;
    std::function<void()> m_AfterJobOnTheCaller;
    uint32_t m_BlockingJobsRun = 0;
    bool m_Stop = false;
    std::thread m_Worker;
};

PipelineFormatKey MakeFormatKey(TextureFormat color, uint8_t samples)
{
    PipelineFormatKey fk{};
    fk.ColorCount = 1;
    fk.ColorFormats[0] = color;
    fk.RasterizationSamples = samples;
    return fk;
}
} // namespace

TEST(PipelineWarmProbe, ColdUntilBuiltThenWarmForThatFormatKeyOnly)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";

    GraphicsPipelineId id{};
    ASSERT_TRUE(InternTriangle(*dev, id));

    const PipelineFormatKey fk = MakeFormatKey(TextureFormat::BGRA8_UNORM, 1);
    const PipelineFormatKey otherFk = MakeFormatKey(TextureFormat::R16G16B16A16_FLOAT, 1);

    // Nothing built yet: the probe must report cold and must NOT build.
    EXPECT_FALSE(dev->TryGetWarmGraphicsPipeline(id, fk).IsValid());
    EXPECT_FALSE(dev->TryGetWarmGraphicsPipeline(id, fk).IsValid())
        << "the probe built a pipeline as a side effect";

    const PipelineHandle built = dev->GetOrCreateGraphicsPipeline(id, fk);
    ASSERT_TRUE(built.IsValid()) << dev->GetLastGraphicsPipelineFailure();

    // Now warm, and the probe returns the very handle the cache holds.
    const PipelineHandle probed = dev->TryGetWarmGraphicsPipeline(id, fk);
    EXPECT_TRUE(probed.IsValid());
    EXPECT_EQ(probed, built);

    // Warmth is per (id, formatKey): a pass with different attachment formats
    // is still cold, which is exactly what makes the record path request a
    // separate warm for it rather than binding the wrong pipeline.
    EXPECT_FALSE(dev->TryGetWarmGraphicsPipeline(id, otherFk).IsValid());
}

TEST(PipelineWarmProbe, PrewarmMakesTheProbeWarm)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";

    GraphicsPipelineId id{};
    ASSERT_TRUE(InternTriangle(*dev, id));

    const PipelineFormatKey fk = MakeFormatKey(TextureFormat::BGRA8_UNORM, 1);
    EXPECT_FALSE(dev->TryGetWarmGraphicsPipeline(id, fk).IsValid());

    // This is the hand-off the record path performs on a cold probe: the build
    // runs on a worker, and the next frame's probe finds it warm.
    const PipelineFormatKey keys[] = {fk};
    EXPECT_EQ(dev->PrewarmGraphicsPipeline(id, keys), 1u);
    EXPECT_TRUE(dev->TryGetWarmGraphicsPipeline(id, fk).IsValid());

    // Already warm: a second prewarm creates nothing.
    EXPECT_EQ(dev->PrewarmGraphicsPipeline(id, keys), 0u);
}

TEST(PipelineWarmProbe, InvalidIdIsCold)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";

    EXPECT_FALSE(dev->TryGetWarmGraphicsPipeline(GraphicsPipelineId{},
                                                 MakeFormatKey(TextureFormat::BGRA8_UNORM, 1))
                     .IsValid());
}

TEST(PipelineBuildRequest, RequestedPipelineIsNotBuiltOnTheCallerAndIsWarmOnceItsBuildRuns)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    EXPECT_FALSE(dev->TryGetWarmComputePipeline(id).IsValid()) << "the request built on the calling thread";
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    EXPECT_EQ(held.Jobs.size(), 1u) << "a second request of a pending pipeline queued a second build";

    held.RunAll();
    EXPECT_TRUE(dev->TryGetWarmComputePipeline(id).IsValid());
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Warm);
    EXPECT_TRUE(held.Jobs.empty());
}

TEST(PipelineBuildRequest, FirstUseOnAThreadThatMayWaitIsBuiltByAWorker)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    const ComputePipelineId computeId = InternMinimalCompute(*dev);
    ASSERT_TRUE(computeId.IsValid()) << "minimal_test.comp.spv missing";
    GraphicsPipelineId graphicsId{};
    ASSERT_TRUE(InternTriangle(*dev, graphicsId)) << "triangle shaders missing";
    const PipelineFormatKey fk = MakeFormatKey(TextureFormat::BGRA8_UNORM, 1);
    WorkerBuilds workers(*dev);

    const PipelineHandle compute = dev->GetOrCreateComputePipeline(computeId);
    const PipelineHandle graphics = dev->GetOrCreateGraphicsPipeline(graphicsId, fk);
    ASSERT_TRUE(compute.IsValid());
    ASSERT_TRUE(graphics.IsValid());
    EXPECT_EQ(workers.BlockingJobsRun(), 2u) << "a first use built its pipeline on the calling thread";
    EXPECT_EQ(dev->TryGetWarmComputePipeline(computeId), compute);
    EXPECT_EQ(dev->TryGetWarmGraphicsPipeline(graphicsId, fk), graphics);
}

TEST(PipelineBuildRequest, FirstUseWaitsForAQueuedBuildAndDoesNotBuildItInline)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";
    WorkerBuilds workers(*dev);

    // The Load job stays queued behind other load work; the first use must not
    // wait for it, nor build the pipeline itself.
    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    const PipelineHandle built = dev->GetOrCreateComputePipeline(id);
    ASSERT_TRUE(built.IsValid());
    EXPECT_EQ(workers.BlockingJobsRun(), 1u) << "the first use built the queued pipeline on the calling thread";

    for (std::function<void()>& job : workers.TakeLoadJobs())
        job();
    EXPECT_EQ(dev->TryGetWarmComputePipeline(id), built) << "the queued Load job built the pipeline a second time";
}

TEST(PipelineBuildRequest, FirstUseWaitsOutASlowWorkerBuildWhileOtherBuildsEnd)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";
    GraphicsPipelineId otherId{};
    ASSERT_TRUE(InternTriangle(*dev, otherId)) << "triangle shaders missing";
    // The worker is slow: it holds each Blocking job until the test releases it. Load jobs of other keys end at
    // once. The six other builds can end before the waiting thread enters WaitForBuildEnd, so a pass checks
    // what the wait returns, not that a wake from another key's build reached the waiting thread.
    std::mutex mutex;
    std::vector<std::function<void()>> heldJobs;
    uint32_t blockingJobs = 0;
    bool released = false;
    std::promise<void> firstBlockingJob;
    std::future<void> blockingJobDispatched = firstBlockingJob.get_future();
    dev->SetPipelineBuildDispatcher({.Dispatch =
                                         [&](std::function<void()> job, PipelineBuildPriority priority)
                                     {
                                         std::unique_lock lock(mutex);
                                         if (priority == PipelineBuildPriority::Blocking && ++blockingJobs == 1)
                                             firstBlockingJob.set_value();
                                         if (priority == PipelineBuildPriority::Blocking && !released)
                                         {
                                             heldJobs.push_back(std::move(job));
                                             return;
                                         }
                                         lock.unlock();
                                         job();
                                     },
                                     .CallerMayWait = [] { return true; }});

    std::future<PipelineHandle> firstUse =
        std::async(std::launch::async, [&] { return dev->GetOrCreateComputePipeline(id); });
    blockingJobDispatched.wait();
    for (TextureFormat color : {TextureFormat::BGRA8_UNORM, TextureFormat::R16G16B16A16_FLOAT})
        for (uint8_t samples : {uint8_t{1}, uint8_t{2}, uint8_t{4}})
            dev->RequestGraphicsPipeline(otherId, MakeFormatKey(color, samples));
    EXPECT_FALSE(dev->TryGetWarmComputePipeline(id).IsValid()) << "the waiting thread built the pipeline itself";

    std::vector<std::function<void()>> jobs;
    {
        std::lock_guard lock(mutex);
        released = true;
        jobs = std::exchange(heldJobs, {});
    }
    for (std::function<void()>& job : jobs)
        job();
    const PipelineHandle built = firstUse.get();
    dev->SetPipelineBuildDispatcher({});
    ASSERT_TRUE(built.IsValid());
    EXPECT_EQ(blockingJobs, 1u) << "the waiting thread queued its build again while the worker was slow";
    EXPECT_EQ(dev->TryGetWarmComputePipeline(id), built);
}

TEST(PipelineBuildRequest, GetOrCreateOnAThreadThatMayNotWaitTakesAQueuedBuildOver)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    const PipelineHandle built = dev->GetOrCreateComputePipeline(id);
    ASSERT_TRUE(built.IsValid());

    // A second build would replace the cached handle, leaking the first (or
    // freeing one a frame has bound).
    held.RunAll();
    EXPECT_EQ(dev->TryGetWarmComputePipeline(id), built) << "the queued job built the pipeline a second time";
}

TEST(PipelineBuildRequest, ALoadBuildQueuedBeforeTheFirstUseWaitsGetsABlockingJob)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";
    WorkerBuilds workers(*dev);

    // Before the first use waits, its build ends, an invalidation drops the
    // pipeline and another caller queues a Load build of the same key.
    workers.RunNextBlockingJobOnTheCaller(
        [&]
        {
            dev->GetMutablePipelineCache().ClearConcreteOnly();
            EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
        });
    const PipelineHandle built = dev->GetOrCreateComputePipeline(id);
    ASSERT_TRUE(built.IsValid());
    EXPECT_EQ(workers.BlockingJobsRun(), 2u) << "the queued Load build got no Blocking job";
    EXPECT_EQ(dev->TryGetWarmComputePipeline(id), built);
}

TEST(PipelineBuildRequest, FirstUseOnAThreadThatRunsTheJobsIsBuiltOnThatThread)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";
    WorkerBuilds workers(*dev);

    // A wait there would hold the worker that its own build needs.
    PipelineHandle built;
    workers.RunOnWorker([&] { built = dev->GetOrCreateComputePipeline(id); });
    ASSERT_TRUE(built.IsValid());
    EXPECT_EQ(workers.BlockingJobsRun(), 1u) << "the worker dispatched a Blocking job for its own first use";
    EXPECT_EQ(dev->TryGetWarmComputePipeline(id), built);
}

TEST(PipelineBuildRequest, FailedBuildIsNotRequestedAgainUntilTheCacheIsInvalidated)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";
    // A cleared cache frees the unpinned desc behind the id: its build cannot succeed.
    dev->GetMutablePipelineCache().Clear();

    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    held.RunAll();
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Failed);
    EXPECT_TRUE(held.Jobs.empty()) << "a failed pipeline was queued again";

    // Hot reload invalidates the cache: what failed may build now, so it is asked for again.
    dev->GetMutablePipelineCache().Clear();
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    EXPECT_EQ(held.Jobs.size(), 1u);
}

TEST(PipelineBuildRequest, FailedBuildIsNotBuiltAgainByTheNextFirstUseOnAThreadThatMayWait)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    GraphicsPipelineId id{};
    ASSERT_TRUE(InternTriangle(*dev, id)) << "triangle shaders missing";
    const PipelineFormatKey fk = MakeFormatKey(TextureFormat::BGRA8_UNORM, 1);
    // A cleared cache frees the unpinned desc behind the id: its build cannot succeed.
    dev->GetMutablePipelineCache().Clear();
    WorkerBuilds workers(*dev);

    ASSERT_FALSE(dev->GetOrCreateGraphicsPipeline(id, fk).IsValid());
    ASSERT_EQ(workers.BlockingJobsRun(), 1u);
    // Every build attempt of the triangle writes its own failure over this one.
    ASSERT_FALSE(dev->GetOrCreateGraphicsPipeline(GraphicsPipelineId{}, fk).IsValid());
    const std::string invalidIdFailure = dev->GetLastGraphicsPipelineFailure();

    EXPECT_FALSE(dev->GetOrCreateGraphicsPipeline(id, fk).IsValid());
    EXPECT_EQ(workers.BlockingJobsRun(), 1u) << "a worker built the failed pipeline again";
    EXPECT_EQ(dev->GetLastGraphicsPipelineFailure(), invalidIdFailure)
        << "the waiting thread built the failed pipeline again";
}

TEST(PipelineBuildRequest, BuildDroppedWithoutRunningIsRequestedAgain)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    held.Jobs.clear(); // the dispatcher destroyed the job unrun, as a shutting-down pool does

    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    ASSERT_EQ(held.Jobs.size(), 1u) << "the dropped build left the pipeline pending with nothing building it";
    held.RunAll();
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Warm);
}

TEST(PipelineBuildRequest, BuildRequestedBeforeAnInvalidationDoesNotPublish)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    // Device-rebuild invalidation: the desc survives, every concrete pipeline is forgotten.
    dev->GetMutablePipelineCache().ClearConcreteOnly();
    held.RunAll();
    EXPECT_FALSE(dev->TryGetWarmComputePipeline(id).IsValid())
        << "a build from before the invalidation published into the cleared cache";

    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    held.RunAll();
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Warm);
}

TEST(PipelineBuildRequest, WorkerBuildOvertakenByAnInvalidationIsBuiltAgainForTheWaitingThread)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";
    // The first job starts after an invalidation, so the cache refuses the pipeline it builds.
    uint32_t jobs = 0;
    dev->SetPipelineBuildDispatcher({.Dispatch =
                                         [&](std::function<void()> job, PipelineBuildPriority)
                                     {
                                         if (jobs++ == 0)
                                             dev->GetMutablePipelineCache().ClearConcreteOnly();
                                         job();
                                     },
                                     .CallerMayWait = [] { return true; }});

    const PipelineHandle built = dev->GetOrCreateComputePipeline(id);
    dev->SetPipelineBuildDispatcher({});
    ASSERT_TRUE(built.IsValid()) << "the waiting thread got no pipeline after the invalidation";
    EXPECT_EQ(jobs, 2u);
    EXPECT_EQ(dev->TryGetWarmComputePipeline(id), built);
}

TEST(PipelineBuildRequest, DeviceShutdownCancelsQueuedBuilds)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    const uint64_t insertsBefore = dev->GetPipelineCacheStats().Inserts;
    dev->Shutdown();
    // A job the pool runs after the device's objects are gone must not reach the backend.
    held.RunAll();
    EXPECT_EQ(dev->GetPipelineCacheStats().Inserts, insertsBefore);
}

TEST(PipelineBuildRequest, QueuedBuildOutlivingTheDeviceTouchesNothingOfIt)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    ASSERT_EQ(held.Jobs.size(), 1u);
    dev->Shutdown();
    dev.reset();
    // A pool whose drain ends before its jobs' captures are destroyed lets the
    // device go first: running the job, then destroying it, must reach nothing
    // the device owned (the ASan preset reports a heap use-after-free otherwise).
    held.RunAll();
    EXPECT_TRUE(held.Jobs.empty());
}

TEST(PipelineBuildRequest, WithoutADispatcherTheBuildRunsInline)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Warm);
    EXPECT_TRUE(dev->TryGetWarmComputePipeline(id).IsValid());
}
