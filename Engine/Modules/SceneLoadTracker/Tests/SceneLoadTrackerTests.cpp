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
