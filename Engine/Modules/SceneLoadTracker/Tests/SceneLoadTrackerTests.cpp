#include "SceneLoadTracker/SceneLoadTracker.h"

#include "LogCapture.h"
#include "Memory/AllocationCountScope.h"
#include "SceneLoadTrackerTestAccess.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;
using Clock = std::chrono::steady_clock;

namespace
{
const SceneLoadTracker::CategoryStatus* FindRow(const SceneLoadTracker& tracker, const std::string& id)
{
    for (const SceneLoadTracker::CategoryStatus& row : tracker.GetStatus().Categories)
        if (row.Id == id)
            return &row;
    return nullptr;
}

SceneLoadTracker::GateReport WaitingFor(std::size_t pending)
{
    if (pending == 0)
        return {};
    return {.State = SceneLoadTracker::GateState::Waiting, .Reason = "models still loading", .Pending = pending};
}
} // namespace

TEST(SceneLoadTracker, ATrackerWithNoWorkIsReadyToShow)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category category = tracker.RegisterCategory({.Id = "test.work", .Label = "Working"});
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_TRUE(tracker.IsReadyToShow());
    ASSERT_NE(FindRow(tracker, "test.work"), nullptr);
    EXPECT_EQ(FindRow(tracker, "test.work")->Label, "Working");
}

// Each category holds the query on its own; the query is true only when all of them settle.
TEST(SceneLoadTracker, TheQueryIsFalseWhileAnyCategoryHasPendingWork)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category tickets = tracker.RegisterCategory({.Id = "test.tickets"});
    SceneLoadTracker::Category gated = tracker.RegisterCategory({.Id = "test.gated"});
    std::size_t gatePending = 0;
    SceneLoadTracker::Gate gate = gated.RegisterGate([&gatePending] { return WaitingFor(gatePending); });

    SceneLoadTracker::WorkTicket ticket = tickets.BeginWork(tracker.Intern("Materials/Water.material"), 4.0);
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_FALSE(tracker.IsReadyToShow()) << "a queued ticket holds";
    EXPECT_EQ(FindRow(tracker, "test.tickets")->Queued, 1u);
    EXPECT_DOUBLE_EQ(FindRow(tracker, "test.tickets")->RemainingUnits, 4.0);

    ticket.Start();
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_FALSE(tracker.IsReadyToShow()) << "a running ticket holds";
    EXPECT_EQ(FindRow(tracker, "test.tickets")->Running, 1u);

    ticket.Complete();
    gatePending = 3;
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_FALSE(tracker.IsReadyToShow()) << "a waiting gate holds";
    const SceneLoadTracker::CategoryStatus* gatedRow = FindRow(tracker, "test.gated");
    ASSERT_NE(gatedRow, nullptr);
    EXPECT_EQ(gatedRow->WaitingGates, 1u);
    EXPECT_EQ(gatedRow->GatePending, 3u);
    EXPECT_EQ(gatedRow->Reason, "models still loading");
    EXPECT_FALSE(FindRow(tracker, "test.tickets")->HoldsReady());
    EXPECT_EQ(FindRow(tracker, "test.tickets")->Done, 1u);

    gatePending = 0;
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_TRUE(tracker.IsReadyToShow()) << "every category settled";
}

TEST(SceneLoadTracker, BackgroundWorkIsCountedAndNeverHolds)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category category = tracker.RegisterCategory({.Id = "test.warm-up"});
    SceneLoadTracker::WorkTicket ticket =
        category.BeginWork(tracker.Intern("warm-up"), 1.0, SceneLoadTracker::Need::Background);
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_TRUE(tracker.IsReadyToShow());
    EXPECT_EQ(FindRow(tracker, "test.warm-up")->Background, 1u);
    ticket.Complete();
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_EQ(FindRow(tracker, "test.warm-up")->Background, 0u);
    EXPECT_EQ(FindRow(tracker, "test.warm-up")->Done, 1u);
}

TEST(SceneLoadTracker, ATicketDroppedBeforeItCompletesStopsHoldingAndCountsAsFailed)
{
    std::vector<std::string> lines;
    TestLog::ScopedLogCapture capture(&lines);
    SceneLoadTracker tracker;
    SceneLoadTracker::Category category = tracker.RegisterCategory({.Id = "test.work"});
    {
        SceneLoadTracker::WorkTicket ticket = category.BeginWork(tracker.Intern("dropped"), 1.0);
        ticket.Start();
    }
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_TRUE(tracker.IsReadyToShow());
    EXPECT_EQ(FindRow(tracker, "test.work")->Failed, 1u);
    Logger::Log::Flush();
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "'test.work' work 'dropped' was dropped before it completed"), 1u);
}

// An owner that cancels its work on purpose revokes the category first: a ticket that ends
// after that is not logged as dropped.
TEST(SceneLoadTracker, ATicketThatEndsAfterItsCategoryIsRevokedIsNotLogged)
{
    std::vector<std::string> lines;
    TestLog::ScopedLogCapture capture(&lines);
    Logger::Log::Info("SceneLoadTrackerTests: the capture is live");
    SceneLoadTracker tracker;
    std::optional<SceneLoadTracker::Category> category = tracker.RegisterCategory({.Id = "test.canceled"});
    std::optional<SceneLoadTracker::WorkTicket> ticket = category->BeginWork(tracker.Intern("canceled"), 1.0);

    category.reset();
    ticket.reset();
    Logger::Log::Flush();
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "the capture is live"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "was dropped before it completed"), 0u);
}

// Destroying the owning handles revokes the category and its gate, and a ticket that
// outlives its category is safe to complete.
TEST(SceneLoadTracker, RevokingACategoryReleasesItsWorkAndItsGates)
{
    SceneLoadTracker tracker;
    std::optional<SceneLoadTracker::Category> category = tracker.RegisterCategory({.Id = "test.module"});
    SceneLoadTracker::Gate gate = category->RegisterGate([] { return WaitingFor(1); });
    SceneLoadTracker::WorkTicket ticket = category->BeginWork(tracker.Intern("module work"), 1.0);
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_FALSE(tracker.IsReadyToShow());

    category.reset();
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_TRUE(tracker.IsReadyToShow());
    EXPECT_EQ(FindRow(tracker, "test.module"), nullptr);
    ticket.Complete();
}

TEST(SceneLoadTracker, RegistrationsThatShareAnIdAddIntoOneRow)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category first = tracker.RegisterCategory({.Id = "scene.resolve"});
    SceneLoadTracker::Category second = tracker.RegisterCategory({.Id = "scene.resolve"});
    SceneLoadTracker::Gate firstGate = first.RegisterGate([] { return WaitingFor(2); });
    SceneLoadTracker::Gate secondGate = second.RegisterGate([] { return WaitingFor(5); });
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    ASSERT_EQ(tracker.GetStatus().Categories.size(), 1u);
    EXPECT_EQ(FindRow(tracker, "scene.resolve")->WaitingGates, 2u);
    EXPECT_EQ(FindRow(tracker, "scene.resolve")->GatePending, 7u);
}

TEST(SceneLoadTracker, ReadyIsRaisedOnceWhenThePendingWorkSettles)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category category = tracker.RegisterCategory({.Id = "test.work"});
    int raised = 0;
    EventSubscription subscription = tracker.Ready.Subscribe([&raised] { ++raised; });

    SceneLoadTracker::WorkTicket ticket = category.BeginWork(tracker.Intern("work"), 1.0);
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_EQ(raised, 0);
    ticket.Complete();
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_EQ(raised, 1);
}

// A reporter that never completes stalls the query, and the stall detail names it.
TEST(SceneLoadTracker, AReporterThatNeverCompletesIsNamedInTheStallDetail)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category compiles = tracker.RegisterCategory({.Id = "shader.base-compile"});
    SceneLoadTracker::Category textures = tracker.RegisterCategory({.Id = "render.texture-upload"});
    SceneLoadTracker::Gate gate = textures.RegisterGate([] { return WaitingFor(1); });
    SceneLoadTracker::WorkTicket stuck = compiles.BeginWork(tracker.Intern("Materials/Water.material"), 1.0);
    stuck.Start();

    const Clock::time_point start = Clock::now();
    SceneLoadTrackerTestAccess::Tick(tracker,start);
    EXPECT_FALSE(tracker.GetStatus().Stalled);
    SceneLoadTrackerTestAccess::Tick(tracker,start + std::chrono::seconds(5));
    EXPECT_FALSE(tracker.GetStatus().Stalled) << "a stall needs kStallSeconds without progress";
    SceneLoadTrackerTestAccess::Tick(tracker,start + std::chrono::seconds(11));
    ASSERT_TRUE(tracker.GetStatus().Stalled);

    const std::string detail = tracker.Describe();
    EXPECT_NE(detail.find("shader.base-compile: 0 queued, 1 running"), std::string::npos) << detail;
    EXPECT_NE(detail.find("'Materials/Water.material'"), std::string::npos) << detail;
    EXPECT_NE(detail.find("render.texture-upload"), std::string::npos) << detail;
    EXPECT_NE(detail.find("waiting: 1 models still loading"), std::string::npos) << detail;
}

// A tracked task holds under its name until it returns. A task dropped unrun fails and stops holding.
TEST(SceneLoadTracker, ATrackedTaskHoldsUntilItReturns)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category compiles = tracker.RegisterCategory({.Id = "shader.variant"});
    std::function<void()> task = compiles.Track("Materials/Water.material", SceneLoadTracker::Need::Frame, [] {});
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    const SceneLoadTracker::CategoryStatus* row = FindRow(tracker, "shader.variant");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->Queued, 1u);
    EXPECT_EQ(tracker.NameOf(row->OldestPending), "Materials/Water.material");
    task();
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_TRUE(tracker.IsReadyToShow());
    EXPECT_EQ(FindRow(tracker, "shader.variant")->Done, 1u);

    std::optional<std::function<void()>> dropped =
        compiles.Track("Materials/Rock.material", SceneLoadTracker::Need::Frame, [] {});
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_FALSE(tracker.IsReadyToShow());
    dropped.reset();
    SceneLoadTrackerTestAccess::Tick(tracker,Clock::now());
    EXPECT_TRUE(tracker.IsReadyToShow());
    EXPECT_EQ(FindRow(tracker, "shader.variant")->Failed, 1u);
}

// Every test executable carries the allocation counter's hook. This check keeps the zero
// of the next test from being vacuous.
TEST(SceneLoadTracker, TheAllocationCounterSeesAnAllocationOnThisThread)
{
    Memory::AllocationCountScope allocations(Memory::CountWindow::ThisThread);
    void* const memory = ::operator new(64);
    const std::uint64_t observed = allocations.Count();
    ::operator delete(memory);
    EXPECT_GE(observed, 1u);
}

// A Tick with an unchanged set of categories allocates nothing, with queued, running and
// Background tickets and a waiting gate. The texts are longer than any small-string buffer.
TEST(SceneLoadTracker, ASteadyTickAllocatesNothing)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category compiles =
        tracker.RegisterCategory({.Id = "shader.base-compile-of-the-test", .Label = "Compiling the test's shaders"});
    SceneLoadTracker::Category models =
        tracker.RegisterCategory({.Id = "scene.resolve-of-the-test", .Label = "Preparing the test's entities"});
    SceneLoadTracker::Gate gate = models.RegisterGate([] {
        return SceneLoadTracker::GateReport{.State = SceneLoadTracker::GateState::Waiting,
                                            .Reason = "entities wait for their model to load",
                                            .Pending = 3};
    });
    SceneLoadTracker::WorkTicket queued = compiles.BeginWork(tracker.Intern("Materials/AQueuedMaterial.material"), 1.0);
    SceneLoadTracker::WorkTicket running = compiles.BeginWork(tracker.Intern("Materials/ARunningMaterial.material"), 1.0);
    running.Start();
    SceneLoadTracker::WorkTicket background = compiles.BeginWork(
        tracker.Intern("Materials/ABackgroundMaterial.material"), 1.0, SceneLoadTracker::Need::Background);

    const Clock::time_point start = Clock::now();
    SceneLoadTrackerTestAccess::Tick(tracker,start);
    constexpr int kTicks = 32;
    Memory::AllocationCountScope allocations(Memory::CountWindow::ThisThread);
    for (int tick = 1; tick <= kTicks; ++tick)
        SceneLoadTrackerTestAccess::Tick(tracker,start + std::chrono::milliseconds(16 * tick));
    const std::uint64_t count = allocations.Count();
    EXPECT_EQ(count, 0u) << count << " allocations in " << kTicks << " Ticks";
    EXPECT_FALSE(tracker.IsReadyToShow());
}

namespace
{
Clock::time_point At(Clock::time_point start, double seconds)
{
    return start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}
} // namespace

TEST(SceneLoadTracker, TheEstimateIsTheSlowestCategoryAndUnreportedWorkStopsAtNinetyPercent)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category reads =
        tracker.RegisterCategory({.Id = "test.read", .Label = "Reading", .DefaultSecondsPerUnit = 0.5});
    SceneLoadTracker::Category bake =
        tracker.RegisterCategory({.Id = "test.bake", .Label = "Baking", .DefaultSecondsPerUnit = 3.0});
    SceneLoadTracker::Category models = tracker.RegisterCategory({.Id = "test.models", .Label = "Loading models"});
    SceneLoadTracker::WorkTicket read = reads.BeginWork(tracker.Intern("Scenes/A.scene"), 4.0);
    SceneLoadTracker::WorkTicket band = bake.BeginWork(tracker.Intern("Terrain"), 1.0);
    band.Start();
    std::size_t modelsPending = 3;
    SceneLoadTracker::Gate gate = models.RegisterGate([&modelsPending] { return WaitingFor(modelsPending); });

    const Clock::time_point start = Clock::now();
    SceneLoadTrackerTestAccess::Tick(tracker, start);
    EXPECT_DOUBLE_EQ(tracker.GetStatus().SecondsRemaining, 3.0) << "the started band at its seed";
    EXPECT_DOUBLE_EQ(FindRow(tracker, "test.read")->SecondsRemaining, 2.0) << "4 queued units at 0.5 s";

    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 1.0));
    EXPECT_DOUBLE_EQ(FindRow(tracker, "test.bake")->SecondsRemaining, 2.0) << "the band advanced by elapsed";
    EXPECT_DOUBLE_EQ(tracker.GetStatus().SecondsRemaining, 2.0);

    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 10.0));
    EXPECT_NEAR(FindRow(tracker, "test.bake")->SecondsRemaining, 0.3, 1e-9) << "an unreported band stops at 90 %";
    EXPECT_NEAR(FindRow(tracker, "test.models")->SecondsRemaining, 0.1, 1e-9) << "a waiting gate stops at 90 %";
    EXPECT_DOUBLE_EQ(FindRow(tracker, "test.read")->SecondsRemaining, 2.0) << "queued work advances only on Complete";

    modelsPending = 0;
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 11.0));
    modelsPending = 3;
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 12.0));
    EXPECT_DOUBLE_EQ(FindRow(tracker, "test.models")->SecondsRemaining, 1.0) << "a new wait starts its estimate again";
}

TEST(SceneLoadTracker, AWarmLoadAfterAColdOneCountsOnlyItsOwnWork)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category decodes =
        tracker.RegisterCategory({.Id = "test.decode", .Label = "Decoding", .DefaultSecondsPerUnit = 1.0});
    const StringId name = tracker.Intern("Textures/Grass.png");

    // The cold load: 16 units and one that is dropped. They take 0.8 s of frames, so the
    // measured rate, 0.05 s per unit, replaces the seed.
    std::vector<SceneLoadTracker::WorkTicket> cold;
    for (int unit = 0; unit < 16; ++unit)
        cold.push_back(decodes.BeginWork(name, 1.0));
    std::optional<SceneLoadTracker::WorkTicket> dropped = decodes.BeginWork(name, 1.0);
    const Clock::time_point start = Clock::now();
    SceneLoadTrackerTestAccess::Tick(tracker, start);
    EXPECT_DOUBLE_EQ(tracker.GetStatus().SecondsRemaining, 17.0);
    {
        std::vector<std::string> lines;
        TestLog::ScopedLogCapture capture(&lines);
        dropped.reset();
    }
    for (int unit = 0; unit < 16; ++unit)
    {
        cold[unit].Complete();
        SceneLoadTrackerTestAccess::Tick(tracker, At(start, 0.05 * (unit + 1)));
    }
    ASSERT_TRUE(tracker.IsReadyToShow());
    EXPECT_EQ(tracker.GetStatus().SecondsRemaining, 0.0);

    // The menu runs 10 s with no work; then the warm load opens 2 units.
    for (int second = 1; second <= 10; ++second)
        SceneLoadTrackerTestAccess::Tick(tracker, At(start, 0.8 + second));
    SceneLoadTracker::WorkTicket first = decodes.BeginWork(name, 1.0);
    SceneLoadTracker::WorkTicket second = decodes.BeginWork(name, 1.0);
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 11.0));
    EXPECT_NEAR(tracker.GetStatus().SecondsRemaining, 0.1, 1e-9)
        << "only the warm load's 2 units, at the rate the cold load measured";
}

TEST(SceneLoadTracker, TheMeasuredRateCountsTheTimeATicketRunsNotTheTimeItWaitsQueued)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category reads =
        tracker.RegisterCategory({.Id = "test.read", .Label = "Reading", .DefaultSecondsPerUnit = 0.2});
    const StringId name = tracker.Intern("Scenes/A.scene");

    // The first read waits 5 s in the queue, as behind the project's startup scan, then runs 0.6 s.
    SceneLoadTracker::WorkTicket first = reads.BeginWork(name, 1.0);
    const Clock::time_point start = Clock::now();
    for (int second = 0; second < 5; ++second)
        SceneLoadTrackerTestAccess::Tick(tracker, At(start, second));
    first.Start();
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 5.0));
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 5.3));
    first.Complete();
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 5.6));
    ASSERT_TRUE(tracker.IsReadyToShow());

    SceneLoadTracker::WorkTicket next = reads.BeginWork(name, 1.0);
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 6.0));
    EXPECT_NEAR(tracker.GetStatus().SecondsRemaining, 0.6, 1e-6)
        << "the next read at the 0.6 s the first one ran, not the 5.6 s since it was queued";
}

TEST(SceneLoadTracker, ATicketDroppedAfterItRanAddsNoTimeToTheMeasuredRate)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category reads =
        tracker.RegisterCategory({.Id = "test.read", .Label = "Reading", .DefaultSecondsPerUnit = 0.2});
    const StringId name = tracker.Intern("Scenes/A.scene");

    // A read runs 5 s and fails: the loader drops its ticket. The next read runs 0.6 s and completes.
    std::optional<SceneLoadTracker::WorkTicket> failed = reads.BeginWork(name, 1.0);
    failed->Start();
    const Clock::time_point start = Clock::now();
    for (int second = 0; second <= 5; ++second)
        SceneLoadTrackerTestAccess::Tick(tracker, At(start, second));
    {
        std::vector<std::string> lines;
        TestLog::ScopedLogCapture capture(&lines);
        failed.reset();
    }
    SceneLoadTracker::WorkTicket read = reads.BeginWork(name, 1.0);
    read.Start();
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 5.5));
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 5.8));
    read.Complete();
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 6.1));
    ASSERT_TRUE(tracker.IsReadyToShow());

    SceneLoadTracker::WorkTicket next = reads.BeginWork(name, 1.0);
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 6.5));
    EXPECT_NEAR(tracker.GetStatus().SecondsRemaining, 0.6, 1e-6)
        << "the next read at the 0.6 s the completed read ran, not with the 5.5 s of the failed one";
}

TEST(SceneLoadTracker, TicketsThatRunTogetherCountTheirTimeOnce)
{
    SceneLoadTracker tracker;
    SceneLoadTracker::Category decodes =
        tracker.RegisterCategory({.Id = "test.decode", .Label = "Decoding", .DefaultSecondsPerUnit = 0.1});
    const StringId name = tracker.Intern("Textures/Grass.png");

    // Two decodes run together for 2 s: the category does 2 units in 2 s of frames.
    SceneLoadTracker::WorkTicket first = decodes.BeginWork(name, 1.0);
    SceneLoadTracker::WorkTicket second = decodes.BeginWork(name, 1.0);
    first.Start();
    second.Start();
    const Clock::time_point start = Clock::now();
    SceneLoadTrackerTestAccess::Tick(tracker, start);
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 1.0));
    first.Complete();
    second.Complete();
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 2.0));
    ASSERT_TRUE(tracker.IsReadyToShow());

    SceneLoadTracker::WorkTicket next = decodes.BeginWork(name, 1.0);
    SceneLoadTrackerTestAccess::Tick(tracker, At(start, 2.5));
    EXPECT_NEAR(tracker.GetStatus().SecondsRemaining, 1.0, 1e-6)
        << "1 s per unit: the 2 s of frames over the 2 units, not 2 s for each ticket";
}
