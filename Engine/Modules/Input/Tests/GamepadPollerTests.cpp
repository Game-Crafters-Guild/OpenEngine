// The gamepad poller over a fake backend: discovery runs off the frame thread, the frame read
// never waits for it, and the slot table and snapshot handoff the Windows backend uses keep a
// device in its slot through arrivals, removals and bursts.
#include "Gamepad/GamepadSlotTable.h"
#include "Gamepad/SnapshotTripleBuffer.h"
#include "Input/Gamepad/GamepadBackend.h"
#include "Input/GamepadEdgeTracker.h"
#include "Input/GamepadFrame.h"
#include "Input/Gamepad/GamepadPoller.h"
#include "Input/InputScriptApi.h"
#include "Input/InputSystem.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace GameEngine::Input;

namespace
{

constexpr int kMaxFakeDevices = 8;
constexpr auto kStartTimeout = std::chrono::seconds(10);

struct FakeDevice
{
    bool Buttons[kGamepadButtonCount] = {};
};

struct FakeSnapshot
{
    FakeDevice* Devices[kMaxFakeDevices] = {};
    int Count = 0;
};

// What the fake saw: Start writes StartThread before it releases StartReturned.
struct FakeRecord
{
    std::atomic<bool> StartReturned{false};
    std::thread::id StartThread;
    std::vector<std::thread::id> ReadThreads;
};

// Stands in for a platform backend. The test plays the device callback: it replaces the
// connected set and publishes it, as GameInput's callback thread does. A gate can hold Start open.
class FakeGamepadBackend final : public GamepadBackend
{
  public:
    FakeGamepadBackend(FakeRecord& record, GamepadRuntimeState startState, std::atomic<bool>* startGate)
        : m_Record(record), m_StartState(startState), m_StartGate(startGate)
    {
    }

    GamepadRuntimeState Start() override
    {
        m_Record.StartThread = std::this_thread::get_id();
        while (m_StartGate && !m_StartGate->load(std::memory_order_acquire))
            std::this_thread::yield();
        m_Record.StartReturned.store(true, std::memory_order_release);
        return m_StartState;
    }

    void Read(GamepadFrame& frame) override
    {
        m_Record.ReadThreads.push_back(std::this_thread::get_id());
        if (m_Snapshots.Acquire())
        {
            const FakeSnapshot& snapshot = m_Snapshots.ReadBuffer();
            m_Slots.Reconcile(snapshot.Devices, snapshot.Count, [](FakeDevice*) {}, [](FakeDevice*) {});
        }
        for (int slot = 0; slot < kMaxGamepads; ++slot)
        {
            const FakeDevice* device = m_Slots.At(slot);
            if (!device)
                continue;
            frame.slots[slot].connected = true;
            for (int button = 0; button < kGamepadButtonCount; ++button)
                frame.slots[slot].buttons[button] = device->Buttons[button];
        }
    }

    void Stop() override {}

    void PublishDevices(std::initializer_list<FakeDevice*> devices)
    {
        FakeSnapshot& back = m_Snapshots.WriteBuffer();
        back = FakeSnapshot{};
        for (FakeDevice* device : devices)
            back.Devices[back.Count++] = device;
        m_Snapshots.Publish();
    }

  private:
    FakeRecord& m_Record;
    GamepadRuntimeState m_StartState;
    std::atomic<bool>* m_StartGate;
    SnapshotTripleBuffer<FakeSnapshot> m_Snapshots;
    GamepadSlotTable<FakeDevice> m_Slots;
};

class GamepadPollerTests : public ::testing::Test
{
  protected:
    std::unique_ptr<GamepadPoller> MakePoller(GamepadRuntimeState startState = GamepadRuntimeState::Available,
                                              std::atomic<bool>* startGate = nullptr)
    {
        auto backend = std::make_unique<FakeGamepadBackend>(m_Record, startState, startGate);
        m_Backend = backend.get();
        return std::make_unique<GamepadPoller>(m_Pool, std::move(backend));
    }

    // Waits until the start job has returned and published its outcome.
    void WaitForStart()
    {
        const auto deadline = std::chrono::steady_clock::now() + kStartTimeout;
        while (!m_Record.StartReturned.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(m_Record.StartReturned.load(std::memory_order_acquire)) << "the start job never ran";
        while (GamepadPoller::GetRuntimeState() == GamepadRuntimeState::Starting &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
    }

    JobSystem::WorkStealingThreadPool m_Pool{2, 2};
    FakeRecord m_Record;
    FakeGamepadBackend* m_Backend = nullptr;
};

bool AnySlotConnected(const GamepadFrame& frame)
{
    for (const GamepadFrame::Slot& slot : frame.slots)
    {
        if (slot.connected)
            return true;
    }
    return false;
}

void IgnoreState(int, const float*, int, bool) {}

} // namespace

// The rule: discovery (Start) runs on a job, never on the thread that reads the frames.
TEST_F(GamepadPollerTests, StartRunsOffTheFrameThreadAndReadRunsOnIt)
{
    auto poller = MakePoller();
    WaitForStart();
    GamepadFrame frame;
    poller->Read(frame);
    poller->Read(frame);

    EXPECT_NE(m_Record.StartThread, std::this_thread::get_id()) << "device discovery ran on the frame thread";
    ASSERT_EQ(m_Record.ReadThreads.size(), 2u);
    for (const std::thread::id& readThread : m_Record.ReadThreads)
        EXPECT_EQ(readThread, std::this_thread::get_id());
}

TEST_F(GamepadPollerTests, ReadBeforeStartReturnsReportsNoPadAndDoesNotWait)
{
    std::atomic<bool> startGate{false};
    auto poller = MakePoller(GamepadRuntimeState::Available, &startGate);
    FakeDevice pad;
    m_Backend->PublishDevices({&pad});

    GamepadFrame frame;
    frame.slots[0].connected = true;
    poller->Read(frame);
    EXPECT_FALSE(AnySlotConnected(frame)) << "a read before the start returned reports every slot empty";
    EXPECT_TRUE(m_Record.ReadThreads.empty()) << "the backend is read only after its start returned";
    EXPECT_EQ(GamepadPoller::GetRuntimeState(), GamepadRuntimeState::Starting);

    startGate.store(true, std::memory_order_release);
    WaitForStart();
    poller->Read(frame);
    EXPECT_TRUE(frame.slots[0].connected) << "the pad present at start arrives once the start returned";
}

TEST_F(GamepadPollerTests, MissingRuntimeLeavesGamepadsOffAndNamesTheState)
{
    auto poller = MakePoller(GamepadRuntimeState::RuntimeMissing);
    WaitForStart();
    FakeDevice pad;
    m_Backend->PublishDevices({&pad});

    GamepadFrame frame;
    poller->Read(frame);
    EXPECT_FALSE(AnySlotConnected(frame));
    EXPECT_TRUE(m_Record.ReadThreads.empty()) << "a backend whose start failed is never read";
    EXPECT_EQ(GamepadPoller::GetRuntimeState(), GamepadRuntimeState::RuntimeMissing);

    InputSystem input;
    EXPECT_EQ(input.GetGamepadRuntimeState(), GamepadRuntimeState::RuntimeMissing);
    EXPECT_EQ(Script::GetGamepadRuntimeState(input), GamepadRuntimeState::RuntimeMissing);
}

// After shutdown the state still says that gamepads are off; it does not go back to Starting.
TEST_F(GamepadPollerTests, RuntimeStateKeepsTheStartOutcomeAfterThePollerStops)
{
    auto poller = MakePoller(GamepadRuntimeState::RuntimeMissing);
    WaitForStart();
    poller.reset();
    EXPECT_EQ(GamepadPoller::GetRuntimeState(), GamepadRuntimeState::RuntimeMissing);
}

// A game that reads Available sees the pad on its next read: the poller is ready before the state changes.
TEST_F(GamepadPollerTests, ReadAfterTheStateReportsAvailableSeesThePad)
{
    auto poller = MakePoller();
    FakeDevice pad;
    m_Backend->PublishDevices({&pad});
    const auto deadline = std::chrono::steady_clock::now() + kStartTimeout;
    while (GamepadPoller::GetRuntimeState() != GamepadRuntimeState::Available &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_EQ(GamepadPoller::GetRuntimeState(), GamepadRuntimeState::Available);

    GamepadFrame frame;
    poller->Read(frame);
    EXPECT_TRUE(frame.slots[0].connected) << "the state reported Available before the poller was ready";
}

TEST_F(GamepadPollerTests, ArrivalAppearsOnTheNextReadAndRemovalReleasesHeldButtons)
{
    auto poller = MakePoller();
    WaitForStart();
    GamepadEdgeTracker tracker;
    std::vector<std::pair<GamepadButton, bool>> edges;
    const auto recordEdge = [&edges](int, GamepadButton button, bool down) { edges.emplace_back(button, down); };

    // The tracker adopts the buttons a pad arrives with: the press comes on the read after the arrival.
    FakeDevice pad;
    m_Backend->PublishDevices({&pad});
    GamepadFrame frame;
    poller->Read(frame);
    EXPECT_TRUE(frame.slots[0].connected);
    tracker.Report(frame, &IgnoreState, recordEdge);
    EXPECT_TRUE(edges.empty());

    pad.Buttons[static_cast<int>(GamepadButton::A)] = true;
    poller->Read(frame);
    tracker.Report(frame, &IgnoreState, recordEdge);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_TRUE(edges[0].second);

    m_Backend->PublishDevices({});
    poller->Read(frame);
    EXPECT_FALSE(frame.slots[0].connected);
    tracker.Report(frame, &IgnoreState, recordEdge);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[1].first, GamepadButton::A);
    EXPECT_FALSE(edges[1].second) << "a pad that leaves releases the buttons it held";
}

TEST_F(GamepadPollerTests, DeviceKeepsItsSlotWhenAnotherLeavesAndANewOneTakesTheLowestFreeSlot)
{
    auto poller = MakePoller();
    WaitForStart();
    FakeDevice first;
    FakeDevice second;
    FakeDevice third;
    second.Buttons[static_cast<int>(GamepadButton::B)] = true;

    m_Backend->PublishDevices({&first, &second});
    GamepadFrame frame;
    poller->Read(frame);
    ASSERT_TRUE(frame.slots[0].connected && frame.slots[1].connected);

    m_Backend->PublishDevices({&second});
    poller->Read(frame);
    EXPECT_FALSE(frame.slots[0].connected);
    EXPECT_TRUE(frame.slots[1].connected && frame.slots[1].buttons[static_cast<int>(GamepadButton::B)])
        << "the remaining pad keeps slot 1";

    m_Backend->PublishDevices({&second, &third});
    poller->Read(frame);
    EXPECT_TRUE(frame.slots[0].connected) << "a new pad takes the lowest free slot";
    EXPECT_TRUE(frame.slots[1].buttons[static_cast<int>(GamepadButton::B)]);
}

TEST_F(GamepadPollerTests, BurstBetweenTwoReadsDeliversTheFinalSet)
{
    auto poller = MakePoller();
    WaitForStart();
    FakeDevice first;
    FakeDevice second;
    m_Backend->PublishDevices({&first});
    m_Backend->PublishDevices({&first, &second});
    m_Backend->PublishDevices({&second});

    GamepadFrame frame;
    poller->Read(frame);
    EXPECT_TRUE(frame.slots[0].connected) << "only the final set is seen: the second pad alone, in slot 0";
    EXPECT_FALSE(frame.slots[1].connected);
}

// One writer publishes a rising sequence while one reader acquires: every value the reader sees
// is whole and newer than the last, and the reader ends on the final value.
TEST(SnapshotTripleBufferTests, WriterAndReaderThreadsNeverShareABuffer)
{
    struct Payload
    {
        std::uint64_t Value = 0;
        std::uint64_t Complement = ~std::uint64_t{0};
    };
    constexpr std::uint64_t kPublishes = 200000;
    SnapshotTripleBuffer<Payload> buffer;
    std::thread writer([&buffer]() {
        for (std::uint64_t value = 1; value <= kPublishes; ++value)
        {
            Payload& back = buffer.WriteBuffer();
            back.Value = value;
            back.Complement = ~value;
            buffer.Publish();
        }
    });

    std::uint64_t last = 0;
    bool torn = false;
    bool backwards = false;
    while (last < kPublishes)
    {
        if (!buffer.Acquire())
            continue;
        const Payload& front = buffer.ReadBuffer();
        torn |= front.Complement != ~front.Value;
        backwards |= front.Value <= last;
        last = front.Value;
    }
    writer.join();
    EXPECT_FALSE(torn) << "the reader saw a buffer the writer was filling";
    EXPECT_FALSE(backwards) << "an acquire returned an older value";
    EXPECT_EQ(last, kPublishes);
}
