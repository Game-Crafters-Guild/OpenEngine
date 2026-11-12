#pragma once

#include "AIECS/AiProgram.h"
#include "AIECS/AiStateTreeProgram.h"
#include "AIECS/AiTaskDefinition.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeAsset.h"
#include "AIECS/Components/AiAgent.h"
#include "ECS/ECS.h"
#include "ECS/ModuleRegistration.h"
#include "Types/FlatMap.h"
#include "Types/FunctionRef.h"
#include "Types/Types.h"

#include <cassert>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace GameEngine::AI
{
class RecordStore;
struct StateTreeCompileError;
}

namespace GameEngine::ECS
{
class SystemScheduleBuilder;
class World;
}

namespace GameEngine::AIECS
{

struct AiCommand;
struct AiOptionSetData;
struct AiStateTreeData;
struct AiStateTreeSource;

/// Fills out[i] with the input's value for agents[i]. The agents are due agents of one program:
/// all of them on the calling thread, or one job's share of them on a job thread (AiInputThreading).
using AiInputFunction = void (*)(const ECS::World& world, std::span<const ECS::EntityHandle> agents,
                                 std::span<float32> out);

/// The threads an input may run on.
enum class AiInputThreading : uint8
{
    /// One call for all due agents of a program, on the thread that runs AiDecideSystem.
    CallingThread,
    /// One call per job, on job threads, at the same time as other calls of the input. The input
    /// only reads; no other system runs while AiDecideSystem runs. Of the AI components (AiAgent,
    /// AiStateTree, the cells, AiPathTrace), it reads only its own agents': other jobs write the rest.
    JobThreads,
};
/// Sets out[i] to 1 when the condition holds for agents[i], else to 0. The agents are one job's share of
/// the due agents of one state-tree program. The calls run on job threads, at the same time as other
/// calls of the condition: a condition only reads the world; no other system runs while AiDecideSystem runs.
/// Of the AI components (AiAgent, AiStateTree, the cells, AiPathTrace), it reads only its own agents':
/// other jobs write those of other agents at the same time.
using AiConditionFunction = void (*)(const ECS::World& world, std::span<const ECS::EntityHandle> agents,
                                     std::span<uint8> out);
/// Writes what one group sees into its records. Runs on the main thread once per group per AI
/// tick, in entity order of the groups, before the decisions.
using AiSensorFunction = void (*)(ECS::World& world, ECS::EntityHandle group, AI::RecordStore& records, uint32 tick);
/// Carries out an order a task issued. Runs on the main thread, in entity order.
using AiCommandFunction = void (*)(ECS::World& world, ECS::EntityHandle agent, const AiCommand& command);

/// The inputs, conditions, tasks, sensors, commands and programs the AI systems use, each named by the id
/// its registration returns. Register on the main thread while no AI system runs; the AI
/// systems read it from job threads without a lock. Each entry records the module generation
/// that registered it (ECS/ModuleRegistration.h), so a module's entries go away with its code.
class AiRegistry
{
public:
    /// The id each Register function returns for a null function. AddProgram and
    /// AiTaskContext::IssueCommand refuse it.
    static constexpr uint16 kNoId = 0xFFFF;

    AiRegistry() = default;
    AiRegistry(const AiRegistry&) = delete;
    AiRegistry& operator=(const AiRegistry&) = delete;

    /// The registry of the engine's schedule. A game registers here.
    static AiRegistry& Get();

    /// Adds AiTickSystem (10), AiKnowledgeSystem (12), AiDecideSystem (14) and AiApplySystem (16), in that
    /// order, on the Simulation cadence in the Early phase. The registry must outlive the schedule.
    /// The world the schedule runs needs RegisterAiWorldHooks.
    void AddSystemsToSchedule(ECS::SystemScheduleBuilder& schedule);

    /// An input that a file can name, with the text the editor shows for it.
    struct NamedInput
    {
        /// The name a file gives in a consideration's "input".
        std::string Name;
        /// The words the editor shows for the input, for example "Threat level". Empty: the editor shows Name.
        std::string Caption;
        /// The unit of the input's value, for example "m". Empty: the value has no unit.
        std::string Unit;
    };

    /// Adds an input. AiProgram::ColumnInputs holds the id. JobThreads runs it beside the scoring.
    /// `name` is the name an option-set file gives in a consideration's "input"; empty, no file can name
    /// it. `caption` and `unit` are the text the editor shows for it (NamedInput); either can be empty.
    /// A newer generation of its module, or any owner of a removed name, replaces it and keeps the id.
    /// @return kNoId when `input` is null or another owner registered `name`; an error names the fix.
    uint16 RegisterInput(AiInputFunction input, AiInputThreading threading, std::string_view name = {},
                         std::string_view caption = {}, std::string_view unit = {});
    /// The inputs that a file can name now, in id order: an input with no name or that its module removed
    /// is left out.
    std::vector<NamedInput> GetNamedInputs() const;
    /// The names of GetNamedInputs, in the same order.
    std::vector<std::string> GetInputNames() const;
    /// Adds a condition. `name` is the name a state-tree program file gives in a "condition" field. The
    /// condition reads no AI component of another agent (AiConditionFunction). A name
    /// is replaced like an input name.
    /// @return kNoId when `condition` is null, `name` is empty or another owner registered `name`; an error
    ///         names the fix.
    uint16 RegisterCondition(AiConditionFunction condition, std::string_view name);
    /// Adds an event type. `name` is the name a state-tree program file gives in an "event" field; AiEvent::Type
    /// holds the id. The module that registered the name owns it like an input name, and a reload that drops the
    /// name removes it. A name that its module removed keeps its id and registers again only in a module load.
    /// @return kNoId when `name` is empty, another owner registered `name`, or a call outside a module load
    ///         names a removed event type; an error names the fix.
    uint16 RegisterEventType(std::string_view name);
    /// The id of the event type `name`, for AiEvent::Type. Registers nothing.
    /// @return kNoId when no loaded code registers `name`.
    uint16 FindEventType(std::string_view name) const;
    /// The names of the conditions that a file can name now, in id order: a condition that its module removed
    /// is left out.
    std::vector<std::string> GetConditionNames() const;
    /// The names of the event types that a file can name now, in id order: an event type that its module
    /// removed is left out.
    std::vector<std::string> GetEventTypeNames() const;
    /// Adds a task. OptionSet::Option::Action holds the id. A task name is replaced like an input name.
    /// @return kNoId when the task has no Tick function or another owner registered its name; an error
    ///         names the fix.
    uint16 RegisterTask(const AiTaskDefinition& task);
    /// Adds a sensor. Every sensor runs for every group on each AI tick.
    /// @return kNoId when `sensor` is null; an error names the fix.
    uint16 RegisterSensor(AiSensorFunction sensor);
    /// Adds a command. AiCommand::Type holds the id.
    /// @return kNoId when `command` is null; an error names the fix.
    uint16 RegisterCommand(AiCommandFunction command);
    /// Interns a program. AiAgent::Program holds the id.
    /// @return AiAgent::kNoProgram when the program names a task, input, consideration, curve or cell
    ///         that does not exist, or has a cooldown with no CooldownCell; an error names the fix.
    uint16 AddProgram(AiProgram program);
    /// Interns the program of an option-set file: each task and input name resolves to its registered id.
    /// @return AiAgent::kNoProgram when the file names a task or input that no module registered (the error
    ///         lists the registered names), or when AddProgram refuses the program.
    uint16 CompileProgram(const AiOptionSetData& data);
    /// Interns the program of a state-tree file: each task, condition, event and input name resolves to its
    /// registered id. AiAgent::Program holds the id, which has AiAgent::kStateTreeProgramBit set.
    /// @return AiAgent::kNoProgram when CheckStateTreeNames refuses the file (the error is its first refusal).
    uint16 CompileStateTree(const AiStateTreeData& data);
    /// Interns the program of a loaded state-tree asset like CompileStateTree, and follows the asset: when the
    /// asset publishes new data, AiTickSystem compiles it under the same id at the next AI tick and moves the
    /// agents to it (ReloadStateTrees).
    /// @return AiAgent::kNoProgram when the asset has no data or CompileStateTree refuses it.
    uint16 LoadStateTree(SharedPtr<const AiStateTreeAsset> asset);
    /// One refusal for each use of a task, condition, event or input name that no module registered, with its
    /// state and item, and one for each state name that more than one state uses, on the second state with the
    /// name; empty when the file has no such name. The order is state by state; in a state: its name, enterIf,
    /// tasks, transitions, modifiers, then option inputs. Adds nothing. A name that no state uses has the item Program.
    std::vector<AI::StateTreeCompileError> CheckStateTreeNames(const AiStateTreeSource& source) const;
    /// Changes at each register, purge and retire call, so that a caller knows when to check its names again.
    uint64 GetVersion() const { return m_Version; }

    /// Module load abort: undoes what the load {moduleId, generation} registered, before its code is
    /// unmapped. An entry it replaced gets the replaced registration back; an entry it added is removed.
    void PurgeModuleRegistrations(std::string_view moduleId, uint64 generation);
    /// Module reload: removes the entries that an older generation of `moduleId` registered and
    /// `currentGeneration` did not register again, before the older code is unmapped. Logs one warning that
    /// names the removed inputs, conditions, events and tasks, when there are any.
    void RetireSupersededModuleRegistrations(std::string_view moduleId, uint64 currentGeneration);

    /// The input with the id, or null when its module removed it. The id must come from RegisterInput.
    AiInputFunction GetInput(uint16 id) const { return m_Inputs[id].Function; }
    /// The threads the input with the id may run on. The id must come from RegisterInput.
    AiInputThreading GetInputThreading(uint16 id) const { return m_Inputs[id].Threading; }
    /// The condition with the id, or null when its module removed it. The id must come from RegisterCondition.
    AiConditionFunction GetCondition(uint16 id) const { return m_Conditions[id]; }
    /// The task with the id; its Tick is null when its module removed it. The id must come from RegisterTask.
    const AiTaskDefinition& GetTask(uint16 id) const { return m_Tasks[id]; }
    /// All sensors, in registration order. A sensor that its module removed is null.
    std::span<const AiSensorFunction> GetSensors() const { return m_Sensors; }
    /// The command with the id, or null when its module removed it. The id must come from RegisterCommand.
    AiCommandFunction GetCommand(uint16 id) const
    {
        assert(id < m_Commands.size() && "AiRegistry::GetCommand: the id is not registered");
        return m_Commands[id];
    }
    /// The number of registered commands. A command id is below it.
    size_t GetCommandCount() const { return m_Commands.size(); }
    /// The utility program, or null when no utility program has the id.
    const AiProgram* FindProgram(uint16 id) const
    {
        return (id & Components::AiAgent::kStateTreeProgramBit) == 0 && id < m_Programs.size() ? &m_Programs[id]
                                                                                              : nullptr;
    }
    /// The state-tree program, or null when no state-tree program has the id.
    const AiStateTreeProgram* FindStateTree(uint16 id) const;
    /// False when the program uses a task, a condition, an event or an input that its module removed. The id must
    /// name a program of either kind.
    bool IsProgramRunnable(uint16 id) const;

private:
    friend class AiTickSystem;
    friend class AiStateTreeReload;

    struct InputEntry
    {
        AiInputFunction Function = nullptr;
        AiInputThreading Threading = AiInputThreading::CallingThread;
        // The entry keeps the caption and the unit, so that a purge restores them with the function.
        std::string Caption;
        std::string Unit;
    };
    /// The module generation that registered an entry, and the one that registered the entry it
    /// replaced. An empty stamp is not module-owned.
    struct EntryOwner
    {
        ECS::ModuleRegistrationStamp Current;
        ECS::ModuleRegistrationStamp Replaced;
    };

    // `replaced` is null for a table whose entries have no names and are never replaced.
    template <typename T>
    static void PurgeTable(std::vector<T>& values, std::vector<EntryOwner>& owners,
                           std::type_identity_t<std::vector<T>>* replaced, std::string_view moduleId,
                           uint64 generation);
    // Returns the id of each entry whose registration it removed.
    template <typename T>
    static std::vector<uint16> RetireTable(std::vector<T>& values, std::vector<EntryOwner>& owners,
                            std::type_identity_t<std::vector<T>>* replaced, std::string_view moduleId,
                            uint64 currentGeneration);
    bool IsRunnable(const AiProgram& program) const;
    bool IsRunnable(const AiStateTreeProgram& program) const;
    // Resolves the names of the file into `out` after CheckStateTreeNames. `source` names the caller in the error.
    bool ResolveStateTree(const char* source, const AiStateTreeData& data, AiStateTreeProgram& out) const;
    // Compiles the new data of each program of LoadStateTree under the same id; a file that does not compile
    // keeps the old program. Then calls `remap` once per id that a reload replaced since the last call for
    // `worldId`, with the program that the world's agents run. AiTickSystem calls it at each AI tick of a world.
    void ReloadStateTrees(uint64 worldId, FunctionRef<void(uint16 id, const AiStateTreeProgram& oldProgram)> remap);
    // Keeps the old program of each changed file in m_RetiredStateTrees under a new reload generation.
    void CompileReloadedStateTrees();
    // Forgets the worlds that do not exist, then frees each retired program that every AI world remapped.
    void FreeRetiredStateTrees();
    void RefreshRunnablePrograms();

    // An entry its module removed holds a null function; its name stays so that the name can register again.
    std::vector<InputEntry> m_Inputs;
    std::vector<std::string> m_InputNames;
    std::vector<EntryOwner> m_InputOwners;
    std::vector<InputEntry> m_ReplacedInputs;
    std::vector<AiConditionFunction> m_Conditions;
    std::vector<std::string> m_ConditionNames;
    std::vector<EntryOwner> m_ConditionOwners;
    std::vector<AiConditionFunction> m_ReplacedConditions;
    // An event type has no function: 1 while registered, 0 when its module removed it.
    std::vector<uint8> m_EventTypes;
    std::vector<std::string> m_EventTypeNames;
    std::vector<EntryOwner> m_EventTypeOwners;
    std::vector<uint8> m_ReplacedEventTypes;
    std::vector<AiTaskDefinition> m_Tasks;
    std::vector<std::string> m_TaskNames;
    std::vector<EntryOwner> m_TaskOwners;
    std::vector<AiTaskDefinition> m_ReplacedTasks;
    uint64 m_Version = 0;
    std::vector<AiSensorFunction> m_Sensors;
    std::vector<EntryOwner> m_SensorOwners;
    std::vector<AiCommandFunction> m_Commands;
    std::vector<EntryOwner> m_CommandOwners;
    std::vector<AiProgram> m_Programs;
    std::vector<uint8> m_ProgramRunnable;
    // State-tree program id & ~AiAgent::kStateTreeProgramBit indexes these.
    std::vector<AiStateTreeProgram> m_StateTrees;
    std::vector<uint8> m_StateTreeRunnable;
    // The asset of a program of LoadStateTree and the data it compiled last; null for CompileStateTree.
    struct StateTreeSource
    {
        SharedPtr<const AiStateTreeAsset> Asset;
        SharedPtr<const AiStateTreeData> Data;
    };
    std::vector<StateTreeSource> m_StateTreeSources;
    // The reload of a program is a generation. The old program stays until each AI world remapped its agents.
    // Only the main thread writes these tables and m_StateTrees, at the AI tick of a world, while no AI job runs.
    struct RetiredStateTree
    {
        uint16 Id = 0;
        uint32 Generation = 0;
        AiStateTreeProgram Program;
    };
    // Ordered by generation.
    std::vector<RetiredStateTree> m_RetiredStateTrees;
    // The generation at the last remap of each AI world, by world id.
    FlatMap<uint64, uint32> m_RemappedGenerations;
    uint32 m_ReloadGeneration = 0;
};

} // namespace GameEngine::AIECS
