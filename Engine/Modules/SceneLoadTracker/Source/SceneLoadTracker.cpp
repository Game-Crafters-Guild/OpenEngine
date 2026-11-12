#include "SceneLoadTracker/SceneLoadTracker.h"

#include "Logger/Logger.h"
#include "Types/FlatMap.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <format>
#include <limits>
#include <mutex>
#include <shared_mutex>

namespace GameEngine
{
namespace
{
constexpr uint32_t kNoSlot = std::numeric_limits<uint32_t>::max();
// The seed of a category that declares none.
constexpr double kDefaultSecondsPerUnit = 0.1;
// The estimate of a waiting gate. A gate reports no cost of its own.
constexpr double kDefaultGateSeconds = 1.0;
// The measured rate replaces the seed once a category was active this long or completed this many units.
constexpr double kMeasuredRateSeconds = 0.5;
constexpr double kMeasuredRateUnits = 16.0;
// Started work with no report and a waiting gate advance by elapsed / estimate up to this share.
constexpr double kUnreportedShare = 0.9;

double SecondsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
{
    return std::chrono::duration<double>(to - from).count();
}

// The seconds that work estimated at `estimate` still needs after `elapsed` seconds with no report.
double UnreportedRemaining(double estimate, double elapsed)
{
    return estimate - std::min(kUnreportedShare * estimate, elapsed);
}
} // namespace

struct SceneLoadTracker::TrackerState
{
    struct GateEntry
    {
        uint64_t Id = 0;
        std::shared_ptr<CategoryState> Category;
        std::function<GateReport()> Evaluate;
        bool Waiting = false;
        std::chrono::steady_clock::time_point WaitingSince{}; // the first Tick of this wait
    };

    std::string NameOf(StringId id)
    {
        std::shared_lock lock(NamesMutex);
        const std::string* name = Names.Find(id);
        return name ? *name : std::string{};
    }

    StringId Intern(std::string_view name)
    {
        const StringId id = HashStringId(name);
        {
            std::shared_lock lock(NamesMutex);
            if (const std::string* known = Names.Find(id))
            {
                if (*known != name)
                    Logger::Log::Error("SceneLoadTracker: '{}' and '{}' have the same id; rename one", *known, name);
                return id;
            }
        }
        std::unique_lock lock(NamesMutex);
        std::string& stored = Names.GetOrInsert(id);
        if (stored.empty())
            stored.assign(name);
        return id;
    }

    // Lock order: Mutex, then a category's Mutex, then NamesMutex.
    std::mutex Mutex;
    std::vector<std::shared_ptr<CategoryState>> Categories;
    // Changes when a category is registered or revoked, so Tick rebuilds its rows.
    uint64_t Generation = 1;
    std::vector<GateEntry> Gates;
    uint64_t NextGateId = 1;

    std::shared_mutex NamesMutex;
    FlatMap<StringId, std::string> Names;
};

struct SceneLoadTracker::CategoryState
{
    // One open Frame ticket. Open slots form a list in begin order, so Head is the oldest.
    struct Slot
    {
        StringId Name = 0;
        std::chrono::steady_clock::time_point Begin{};
        double Units = 0.0;
        uint32_t Prev = kNoSlot;
        uint32_t Next = kNoSlot;
        bool Running = false;
        bool SeenRunning = false;
        std::chrono::steady_clock::time_point RunningSince{}; // the first Tick that saw it running
        double ActiveSeconds = 0.0; // its shares of the Tick intervals it ran in
    };

    uint32_t Open(StringId name, double units)
    {
        uint32_t index = 0;
        if (FreeSlots.empty())
        {
            index = static_cast<uint32_t>(Slots.size());
            Slots.emplace_back();
        }
        else
        {
            index = FreeSlots.back();
            FreeSlots.pop_back();
        }
        Slots[index] = {.Name = name, .Begin = std::chrono::steady_clock::now(), .Units = units, .Prev = Tail};
        if (Tail != kNoSlot)
            Slots[Tail].Next = index;
        else
            Head = index;
        Tail = index;
        ++Queued;
        RemainingUnits += units;
        return index;
    }

    // A completed ticket gives its units and its shares to the measured rate at the next Tick.
    // A dropped ticket gives none.
    void Close(uint32_t index, bool completed)
    {
        const Slot& slot = Slots[index];
        if (completed)
        {
            UnitsSinceTick += slot.Units;
            SecondsSinceTick += slot.ActiveSeconds;
            ++CompletedSinceTick;
            if (!slot.SeenRunning)
                ++CompletedUnseenSinceTick;
        }
        (slot.Running ? Running : Queued) -= 1;
        RemainingUnits -= slot.Units;
        if (slot.Prev != kNoSlot)
            Slots[slot.Prev].Next = slot.Next;
        else
            Head = slot.Next;
        if (slot.Next != kNoSlot)
            Slots[slot.Next].Prev = slot.Prev;
        else
            Tail = slot.Prev;
        FreeSlots.push_back(index);
    }

    // The wall seconds per unit: the measured rate once there is enough of it, else the seed.
    double SecondsPerUnit() const
    {
        const bool measured = ActiveUnits > 0.0 &&
                              (ActiveSeconds >= kMeasuredRateSeconds || ActiveUnits >= kMeasuredRateUnits);
        return measured ? ActiveSeconds / ActiveUnits : SecondsPerUnitSeed;
    }

    // Main thread, in Tick. Moves the tickets that completed since the last Tick into the measured rate
    // and returns the seconds the open Frame tickets still need. Only Tick's `now` is read, so a test
    // can give any timeline.
    double UpdateEstimate(std::chrono::steady_clock::time_point now, double interval)
    {
        // The tickets that ran at the last Tick and the tickets that completed before a Tick saw them run share
        // the interval. Time in the queue is not work.
        const uint32_t sharing = RunningAtLastTick + CompletedUnseenSinceTick;
        const double share = sharing > 0 ? interval / sharing : 0.0;
        ActiveSeconds += SecondsSinceTick + share * CompletedSinceTick;
        ActiveUnits += UnitsSinceTick;
        UnitsSinceTick = 0.0;
        SecondsSinceTick = 0.0;
        CompletedSinceTick = 0;
        CompletedUnseenSinceTick = 0;
        RunningAtLastTick = Running;
        const double secondsPerUnit = SecondsPerUnit();
        double seconds = 0.0;
        for (uint32_t index = Head; index != kNoSlot; index = Slots[index].Next)
        {
            Slot& slot = Slots[index];
            if (slot.SeenRunning)
                slot.ActiveSeconds += share;
            else if (slot.Running)
            {
                slot.SeenRunning = true;
                slot.RunningSince = now;
            }
            const double estimate = slot.Units * secondsPerUnit;
            seconds += slot.SeenRunning ? UnreportedRemaining(estimate, SecondsBetween(slot.RunningSince, now)) : estimate;
        }
        return seconds;
    }

    std::string Id;
    std::string Label;
    CostUnit Unit = CostUnit::Items;
    std::weak_ptr<TrackerState> Tracker;

    std::mutex Mutex;
    std::vector<Slot> Slots;
    std::vector<uint32_t> FreeSlots;
    uint32_t Head = kNoSlot;
    uint32_t Tail = kNoSlot;
    uint32_t Queued = 0;
    uint32_t Running = 0;
    uint32_t Background = 0;
    uint64_t Done = 0;
    uint64_t Failed = 0;
    double RemainingUnits = 0.0;
    double SecondsPerUnitSeed = kDefaultSecondsPerUnit;
    // The measured rate: the shares of the Tick intervals that completed Frame tickets ran in, and
    // their units. A dropped ticket adds neither.
    double ActiveSeconds = 0.0;
    double ActiveUnits = 0.0;
    // The tickets that completed since the last Tick: their units, their shares and their number.
    double UnitsSinceTick = 0.0;
    double SecondsSinceTick = 0.0;
    uint32_t CompletedSinceTick = 0;
    uint32_t CompletedUnseenSinceTick = 0; // no Tick saw them running
    uint32_t RunningAtLastTick = 0;
    // Set by Category::Revoke. A ticket that ends after it is not counted or logged.
    bool Revoked = false;
};

// --- WorkTicket ---

SceneLoadTracker::WorkTicket::WorkTicket(std::shared_ptr<CategoryState> category, uint32_t slot, StringId name,
                                         Need need)
    : m_Category(std::move(category)), m_Slot(slot), m_Name(name), m_Need(need)
{
}

SceneLoadTracker::WorkTicket::WorkTicket(WorkTicket&& other) noexcept
    : m_Category(std::move(other.m_Category)), m_Slot(other.m_Slot), m_Name(other.m_Name), m_Need(other.m_Need)
{
}

SceneLoadTracker::WorkTicket& SceneLoadTracker::WorkTicket::operator=(WorkTicket&& other) noexcept
{
    if (this == &other)
        return *this;
    if (m_Category)
        Finish(false);
    m_Category = std::move(other.m_Category);
    m_Slot = other.m_Slot;
    m_Name = other.m_Name;
    m_Need = other.m_Need;
    return *this;
}

SceneLoadTracker::WorkTicket::~WorkTicket()
{
    if (m_Category)
        Finish(false);
}

void SceneLoadTracker::WorkTicket::Start()
{
    if (!m_Category || m_Need == Need::Background)
        return;
    std::lock_guard lock(m_Category->Mutex);
    CategoryState::Slot& slot = m_Category->Slots[m_Slot];
    if (slot.Running)
        return;
    slot.Running = true;
    --m_Category->Queued;
    ++m_Category->Running;
}

void SceneLoadTracker::WorkTicket::Complete()
{
    if (m_Category)
        Finish(true);
}

void SceneLoadTracker::WorkTicket::Finish(bool completed)
{
    const std::shared_ptr<CategoryState> category = std::move(m_Category);
    {
        std::lock_guard lock(category->Mutex);
        if (m_Need == Need::Background)
            --category->Background;
        else
            category->Close(m_Slot, completed);
        // The owner revoked the category first: it canceled this work on purpose.
        if (category->Revoked)
            return;
        (completed ? category->Done : category->Failed) += 1;
    }
    if (completed)
        return;
    const std::shared_ptr<TrackerState> tracker = category->Tracker.lock();
    Logger::Log::Warning("SceneLoadTracker: '{}' work '{}' was dropped before it completed", category->Id,
                         tracker ? tracker->NameOf(m_Name) : std::string{});
}

// --- Gate ---

SceneLoadTracker::Gate::Gate(std::weak_ptr<TrackerState> tracker, uint64_t id) : m_Tracker(std::move(tracker)), m_Id(id)
{
}

SceneLoadTracker::Gate::Gate(Gate&& other) noexcept : m_Tracker(std::move(other.m_Tracker)), m_Id(other.m_Id)
{
    other.m_Id = 0;
}

SceneLoadTracker::Gate& SceneLoadTracker::Gate::operator=(Gate&& other) noexcept
{
    if (this == &other)
        return *this;
    Revoke();
    m_Tracker = std::move(other.m_Tracker);
    m_Id = other.m_Id;
    other.m_Id = 0;
    return *this;
}

SceneLoadTracker::Gate::~Gate()
{
    Revoke();
}

void SceneLoadTracker::Gate::Revoke()
{
    const std::shared_ptr<TrackerState> tracker = m_Tracker.lock();
    if (!tracker || m_Id == 0)
        return;
    std::lock_guard lock(tracker->Mutex);
    std::erase_if(tracker->Gates, [id = m_Id](const TrackerState::GateEntry& gate) { return gate.Id == id; });
    m_Id = 0;
}

// --- Category ---

SceneLoadTracker::Category::Category(std::weak_ptr<TrackerState> tracker, std::shared_ptr<CategoryState> state)
    : m_Tracker(std::move(tracker)), m_State(std::move(state))
{
}

SceneLoadTracker::Category::Category(Category&& other) noexcept
    : m_Tracker(std::move(other.m_Tracker)), m_State(std::move(other.m_State))
{
}

SceneLoadTracker::Category& SceneLoadTracker::Category::operator=(Category&& other) noexcept
{
    if (this == &other)
        return *this;
    Revoke();
    m_Tracker = std::move(other.m_Tracker);
    m_State = std::move(other.m_State);
    return *this;
}

SceneLoadTracker::Category::~Category()
{
    Revoke();
}

SceneLoadTracker::WorkTicket SceneLoadTracker::Category::BeginWork(StringId name, double units, Need need)
{
    if (!m_State)
        return {};
    std::lock_guard lock(m_State->Mutex);
    if (need == Need::Background)
    {
        ++m_State->Background;
        return WorkTicket(m_State, kNoSlot, name, need);
    }
    return WorkTicket(m_State, m_State->Open(name, units), name, need);
}

std::function<void()> SceneLoadTracker::Category::Track(std::string_view name, Need need, std::function<void()> work)
{
    const std::shared_ptr<TrackerState> tracker = m_Tracker.lock();
    if (!tracker || !m_State)
        return work;
    auto ticket = std::make_shared<WorkTicket>(BeginWork(tracker->Intern(name), 1.0, need));
    return [ticket = std::move(ticket), work = std::move(work)]
    {
        ticket->Start();
        work();
        ticket->Complete();
    };
}

SceneLoadTracker::Gate SceneLoadTracker::Category::RegisterGate(std::function<GateReport()> evaluate)
{
    const std::shared_ptr<TrackerState> tracker = m_Tracker.lock();
    if (!tracker || !m_State || !evaluate)
        return {};
    std::lock_guard lock(tracker->Mutex);
    const uint64_t id = tracker->NextGateId++;
    tracker->Gates.push_back({.Id = id, .Category = m_State, .Evaluate = std::move(evaluate)});
    return Gate(m_Tracker, id);
}

void SceneLoadTracker::Category::Revoke()
{
    const std::shared_ptr<CategoryState> state = std::move(m_State);
    if (!state)
        return;
    {
        std::lock_guard lock(state->Mutex);
        state->Revoked = true;
    }
    const std::shared_ptr<TrackerState> tracker = m_Tracker.lock();
    if (!tracker)
        return;
    std::lock_guard lock(tracker->Mutex);
    std::erase(tracker->Categories, state);
    ++tracker->Generation;
    std::erase_if(tracker->Gates, [&state](const TrackerState::GateEntry& gate) { return gate.Category == state; });
}

// --- SceneLoadTracker ---

namespace
{
SceneLoadTracker::CategoryStatus& RowFor(std::vector<SceneLoadTracker::CategoryStatus>& rows, const std::string& id)
{
    const auto it = std::find_if(rows.begin(), rows.end(),
                                 [&id](const SceneLoadTracker::CategoryStatus& row) { return row.Id == id; });
    if (it != rows.end())
        return *it;
    SceneLoadTracker::CategoryStatus& row = rows.emplace_back();
    row.Id = id;
    return row;
}
} // namespace

SceneLoadTracker::SceneLoadTracker() : m_State(std::make_shared<TrackerState>()) {}

SceneLoadTracker::~SceneLoadTracker() = default;

SceneLoadTracker::Category SceneLoadTracker::RegisterCategory(const CategoryDesc& desc)
{
    auto state = std::make_shared<CategoryState>();
    state->Id.assign(desc.Id);
    state->Label.assign(desc.Label);
    state->Unit = desc.Unit;
    if (desc.DefaultSecondsPerUnit > 0.0)
        state->SecondsPerUnitSeed = desc.DefaultSecondsPerUnit;
    state->Tracker = m_State;
    std::lock_guard lock(m_State->Mutex);
    m_State->Categories.push_back(state);
    ++m_State->Generation;
    return Category(m_State, std::move(state));
}

StringId SceneLoadTracker::Intern(std::string_view name)
{
    return m_State->Intern(name);
}

std::string SceneLoadTracker::NameOf(StringId name) const
{
    return m_State->NameOf(name);
}

std::string SceneLoadTracker::Describe() const
{
    std::vector<std::string> clauses;
    for (const CategoryStatus& row : m_Status.Categories)
    {
        if (!row.HoldsReady())
            continue;
        std::string& clause =
            clauses.emplace_back(std::format("{}: {} queued, {} running", row.Id, row.Queued, row.Running));
        if (row.OldestPending != 0)
            clause += std::format(", oldest '{}' for {:.1f} s", NameOf(row.OldestPending), row.OldestPendingSeconds);
        if (row.WaitingGates != 0)
            clause += std::format(", waiting: {} {}", row.GatePending, row.Reason);
    }
    return Join(clauses, "; ");
}

void SceneLoadTracker::UpdateRows()
{
    // Under m_State->Mutex. The rows keep their strings while the category set is unchanged,
    // so a steady Tick allocates nothing.
    std::vector<CategoryStatus>& rows = m_Status.Categories;
    if (m_RowsGeneration != m_State->Generation)
    {
        rows.clear();
        for (const std::shared_ptr<CategoryState>& category : m_State->Categories)
        {
            CategoryStatus& row = RowFor(rows, category->Id);
            row.Label = category->Label;
            row.Unit = category->Unit;
        }
        m_RowsGeneration = m_State->Generation;
        return;
    }
    for (CategoryStatus& row : rows)
        row = {.Id = std::move(row.Id), .Label = std::move(row.Label), .Unit = row.Unit};
}

void SceneLoadTracker::Tick(std::chrono::steady_clock::time_point now)
{
    std::vector<CategoryStatus>& rows = m_Status.Categories;
    const double interval = m_HasTicked ? SecondsBetween(m_LastTick, now) : 0.0;
    m_LastTick = now;
    {
        std::lock_guard lock(m_State->Mutex);
        UpdateRows();
        for (const std::shared_ptr<CategoryState>& category : m_State->Categories)
        {
            CategoryStatus& row = RowFor(rows, category->Id);
            std::lock_guard categoryLock(category->Mutex);
            row.SecondsRemaining = std::max(row.SecondsRemaining, category->UpdateEstimate(now, interval));
            row.Queued += category->Queued;
            row.Running += category->Running;
            row.Background += category->Background;
            row.Done += category->Done;
            row.Failed += category->Failed;
            row.RemainingUnits += category->RemainingUnits;
            if (category->Head == kNoSlot)
                continue;
            const CategoryState::Slot& oldest = category->Slots[category->Head];
            const double age = SecondsBetween(oldest.Begin, now);
            if (row.OldestPending == 0 || age > row.OldestPendingSeconds)
            {
                row.OldestPending = oldest.Name;
                row.OldestPendingSeconds = age;
            }
        }
        for (TrackerState::GateEntry& gate : m_State->Gates)
        {
            const GateReport report = gate.Evaluate();
            if (report.State != GateState::Waiting)
            {
                gate.Waiting = false;
                continue;
            }
            if (!gate.Waiting)
            {
                gate.Waiting = true;
                gate.WaitingSince = now;
            }
            CategoryStatus& row = RowFor(rows, gate.Category->Id);
            if (row.WaitingGates++ == 0)
                row.Reason = report.Reason;
            row.GatePending += report.Pending;
            row.SecondsRemaining = std::max(
                row.SecondsRemaining, UnreportedRemaining(kDefaultGateSeconds, SecondsBetween(gate.WaitingSince, now)));
        }
    }

    const bool wasReady = m_Status.ReadyToShow;
    m_Status.ReadyToShow = std::none_of(rows.begin(), rows.end(), [](const CategoryStatus& row) { return row.HoldsReady(); });
    m_Status.SecondsRemaining = 0.0;
    for (const CategoryStatus& row : rows)
        m_Status.SecondsRemaining = std::max(m_Status.SecondsRemaining, row.SecondsRemaining);
    UpdateProgress(now);
    if (m_Status.ReadyToShow && !wasReady)
        Ready.Invoke();
}

void SceneLoadTracker::UpdateProgress(std::chrono::steady_clock::time_point now)
{
    uint64_t settled = 0;
    std::size_t gatePending = 0;
    for (const CategoryStatus& row : m_Status.Categories)
    {
        settled += row.Done + row.Failed;
        gatePending += row.GatePending;
    }
    const bool moved = !m_HasTicked || m_Status.ReadyToShow || settled != m_LastSettledCount ||
                       gatePending < m_LastGatePending;
    if (moved)
        m_LastProgress = now;
    m_LastSettledCount = settled;
    m_LastGatePending = gatePending;
    m_HasTicked = true;

    const bool wasStalled = m_Status.Stalled;
    m_Status.SecondsWithoutProgress = SecondsBetween(m_LastProgress, now);
    m_Status.Stalled = !m_Status.ReadyToShow && m_Status.SecondsWithoutProgress > kStallSeconds;
    if (m_Status.Stalled && !wasStalled)
        Logger::Log::Error("SceneLoadTracker: no progress for {:.1f} s; pending: {}", m_Status.SecondsWithoutProgress,
                           Describe());
}
} // namespace GameEngine
