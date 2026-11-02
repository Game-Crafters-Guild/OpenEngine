#pragma once

#include "Events/Event.h"
#include "Types/StringId.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class EngineCore;
struct SceneLoadTrackerTestAccess;

/// Collects the work that the first frame of a scene needs, from the owners that do it.
/// It never issues work. Owners register a category, then report work as tickets (a unit
/// of work with a start and an end) or as gates (a condition read from state the owner
/// keeps). Once per frame, Tick takes a snapshot that says if the scene is ready to show.
/// EngineCore::GetSceneLoadTracker() returns the engine's instance.
class SceneLoadTracker
{
  public:
    enum class CostUnit : uint8_t
    {
        Bytes,
        Texels,
        Variants,
        Pipelines,
        Entities,
        Files,
        Bands,
        Items
    };

    /// Frame work holds ready to show. Background work is counted and never holds it.
    enum class Need : uint8_t
    {
        Frame,
        Background
    };

    /// Copied by RegisterCategory: the views need not outlive the call.
    struct CategoryDesc
    {
        std::string_view Id;    // owner-prefixed and stable, for example "shader.variant"
        std::string_view Label; // what a loading screen may show
        CostUnit Unit = CostUnit::Items;
    };

    enum class GateState : uint8_t
    {
        Clear,
        Waiting
    };

    struct GateReport
    {
        GateState State = GateState::Clear;
        // What the gate waits for, while Waiting. The status keeps the view, so it must be
        // fixed text such as a literal; Describe adds the Pending count.
        std::string_view Reason;
        std::size_t Pending = 0; // the items the owner still waits for
    };

    struct CategoryState;
    struct TrackerState;
    class Category;

    /// One unit of work, move-only. Any thread can call it, one call at a time. A ticket
    /// destroyed before Complete stops holding. If its category is not revoked, the ticket is
    /// also logged as dropped and counted as failed.
    class WorkTicket
    {
      public:
        WorkTicket() = default;
        WorkTicket(WorkTicket&& other) noexcept;
        WorkTicket& operator=(WorkTicket&& other) noexcept;
        WorkTicket(const WorkTicket&) = delete;
        WorkTicket& operator=(const WorkTicket&) = delete;
        ~WorkTicket();

        /// Queued to running.
        void Start();
        void Complete();

      private:
        friend class Category;
        WorkTicket(std::shared_ptr<CategoryState> category, uint32_t slot, StringId name, Need need);
        void Finish(bool completed);

        std::shared_ptr<CategoryState> m_Category;
        uint32_t m_Slot = 0;
        StringId m_Name = 0;
        Need m_Need = Need::Frame;
    };

    /// Owns one gate. Destroying it revokes the gate.
    class Gate
    {
      public:
        Gate() = default;
        Gate(Gate&& other) noexcept;
        Gate& operator=(Gate&& other) noexcept;
        Gate(const Gate&) = delete;
        Gate& operator=(const Gate&) = delete;
        ~Gate();

      private:
        friend class Category;
        Gate(std::weak_ptr<TrackerState> tracker, uint64_t id);
        void Revoke();

        std::weak_ptr<TrackerState> m_Tracker;
        uint64_t m_Id = 0;
    };

    /// Owns one registration. Destroying it revokes the category, its gates and its open
    /// tickets: they stop holding, and their end is not counted or logged. An owner that
    /// cancels its work on purpose destroys the category first. A default-constructed
    /// category reports nothing.
    class Category
    {
      public:
        Category() = default;
        Category(Category&& other) noexcept;
        Category& operator=(Category&& other) noexcept;
        Category(const Category&) = delete;
        Category& operator=(const Category&) = delete;
        ~Category();

        /// Any thread. `name` is an id from Intern; the status names the ticket with it.
        WorkTicket BeginWork(StringId name, double units, Need need = Need::Frame);

        /// Any thread. Begins a ticket of one unit named `name` (interned here) and returns
        /// `work` wrapped: it starts the ticket when it runs and completes it when `work`
        /// returns. Dropped unrun or thrown out of, the ticket is abandoned.
        std::function<void()> Track(std::string_view name, Need need, std::function<void()> work);
        /// `evaluate` runs on the main thread in Tick, once per frame. It reads state the
        /// owner keeps and must not register or revoke a gate or a category.
        Gate RegisterGate(std::function<GateReport()> evaluate);

      private:
        friend class SceneLoadTracker;
        Category(std::weak_ptr<TrackerState> tracker, std::shared_ptr<CategoryState> state);
        void Revoke();

        std::weak_ptr<TrackerState> m_Tracker;
        std::shared_ptr<CategoryState> m_State;
    };

    /// One row per category id: registrations that share an id add into one row.
    struct CategoryStatus
    {
        std::string Id;
        std::string Label;
        CostUnit Unit = CostUnit::Items;
        uint32_t Queued = 0;  // Frame tickets not started
        uint32_t Running = 0; // Frame tickets started
        uint32_t Background = 0;
        uint64_t Done = 0;   // in the session
        uint64_t Failed = 0; // in the session; abandoned tickets included
        double RemainingUnits = 0.0;
        uint32_t WaitingGates = 0;
        std::size_t GatePending = 0;
        std::string_view Reason; // the first waiting gate's
        StringId OldestPending = 0; // NameOf gives the name
        double OldestPendingSeconds = 0.0;

        bool HoldsReady() const { return Queued != 0 || Running != 0 || WaitingGates != 0; }
    };

    struct LoadStatus
    {
        bool ReadyToShow = true;
        /// No category moved forward for kStallSeconds while not ready.
        bool Stalled = false;
        double SecondsWithoutProgress = 0.0;
        std::vector<CategoryStatus> Categories;
    };

    static constexpr double kStallSeconds = 10.0;

    SceneLoadTracker();
    ~SceneLoadTracker();
    SceneLoadTracker(const SceneLoadTracker&) = delete;
    SceneLoadTracker& operator=(const SceneLoadTracker&) = delete;

    /// Any thread.
    Category RegisterCategory(const CategoryDesc& desc);
    /// Any thread. Copies the name the first time its id is seen.
    StringId Intern(std::string_view name);

    /// Main thread. True when no Frame ticket is open and every gate is clear at the last
    /// Tick. One frame boundary: a caller that needs a settled scene counts its own frames.
    bool IsReadyToShow() const { return m_Status.ReadyToShow; }
    const LoadStatus& GetStatus() const { return m_Status; }

    /// Main thread. The categories that hold at the last Tick, one clause each: counts, the
    /// oldest pending ticket by name, and the first waiting gate's reason. Built on each call.
    std::string Describe() const;

    /// Any thread. The string interned under `name`, or empty if none was.
    std::string NameOf(StringId name) const;

    /// Raised in Tick each time the snapshot goes from not ready to ready.
    Event<> Ready;

  private:
    // EngineCore::Update calls Tick once per frame: it is the frame boundary of the snapshot.
    // Tests step a tracker through SceneLoadTrackerTestAccess.
    friend class EngineCore;
    friend struct SceneLoadTrackerTestAccess;

    // Main thread, once per frame: evaluates the gates and takes the snapshot that the
    // queries read. Raises Ready when the snapshot becomes ready to show.
    void Tick(std::chrono::steady_clock::time_point now);
    void UpdateRows();
    void UpdateProgress(std::chrono::steady_clock::time_point now);

    std::shared_ptr<TrackerState> m_State;
    LoadStatus m_Status;
    uint64_t m_RowsGeneration = 0;
    std::chrono::steady_clock::time_point m_LastProgress{};
    uint64_t m_LastSettledCount = 0;
    std::size_t m_LastGatePending = 0;
    bool m_HasTicked = false;
};
} // namespace GameEngine
