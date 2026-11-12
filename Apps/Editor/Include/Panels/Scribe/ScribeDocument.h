#pragma once

#include "AI/StateTree/StateTreeCompileError.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeSource.h"
#include "Types/Types.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::AIECS
{
class AiRegistry;
}

namespace GameEngine::Editor
{
class UndoRedoService;

/// One row of the Scribe outline. The states come first, depth first, each followed by its enter
/// condition, its considerations, its tasks and its transitions; then the program's events and inputs as two groups.
struct ScribeOutlineRow
{
    enum class Kind : uint8
    {
        /// The program's refusal when no row of a state holds it.
        Program,
        State,
        EnterCondition,
        /// A consideration of the option that scores the state in its Utility parent.
        Consideration,
        Task,
        Transition,
        /// The heading of the events or the inputs.
        Group,
        /// An event or an input name.
        Name,
    };
    static constexpr uint16 kNoState = 0xFFFF;

    Kind RowKind = Kind::State;
    /// The index of the state in the definition; kNoState for a program, group or name row.
    uint16 State = kNoState;
    /// The index of the task or the transition in its state's list, or of the consideration in its option.
    uint16 Item = 0;
    uint8 Depth = 0;
    std::string Text;
    std::string Detail;
    /// The refusal that this row holds, with its fix; empty when the row holds none.
    std::string Refusal;
    /// The kind ("input", "condition", "event" or "task") and the name that no module registers, when that is the
    /// cause of the refusal; null and empty for another refusal.
    const char* UnregisteredKind = nullptr;
    std::string UnregisteredName;
    /// True for the row of a state that is an option of its Utility parent.
    bool IsOption = false;
};

/// A state-tree program file (.scribe) open in the Scribe panel: the authored form, the refusal of its
/// compile, and its save. Each edit is one undo step that holds the source before and after the edit.
/// Main thread only.
class ScribeDocument : public std::enable_shared_from_this<ScribeDocument>
{
    // Only Open can name this type, so only Open constructs a document.
    struct ConstructionKey
    {
        explicit ConstructionKey() = default;
    };

  public:
    /// How an edit applies. A Preview shows the edit with no undo step; the Commit after it takes one undo step
    /// from the source before the first Preview, so that a drag is one undo step.
    enum class Phase : uint8
    {
        Preview,
        Commit,
    };

    /// Reads `path`. Null with the reason in `outError` when the file does not read or parse.
    /// `undo` is null for edits that take no undo step; `names` checks the task, condition, event and input
    /// names, and is null for no name check.
    static std::shared_ptr<ScribeDocument> Open(const std::filesystem::path& path, UndoRedoService* undo,
                                                const AIECS::AiRegistry* names, std::string& outError);

    const std::filesystem::path& GetPath() const { return m_Path; }
    const AIECS::AiStateTreeSource& GetSource() const { return m_Source; }
    /// The compile refusal first when the compile refuses, then one refusal per unregistered name and per
    /// repeated state name, in state order. Empty when the program compiles and every name is registered and unique.
    const std::vector<AI::StateTreeCompileError>& GetRefusals() const { return m_Refusals; }
    /// Changes at each change of the source and at each save.
    uint64 GetVersion() const { return m_Version; }
    /// True when the source differs from the text of the last save or open.
    bool IsDirty() const { return m_Dirty; }
    /// True from the first Preview of an edit to its Commit.
    bool IsPreviewing() const { return m_PreviewStart.has_value(); }

    std::vector<ScribeOutlineRow> BuildOutline() const;
    /// The row as a path from the state that holds it ("Attack > Consider Morale", "Commander > Option Attack").
    std::string GetRowPath(const ScribeOutlineRow& row) const;
    /// The word for a curve preset in the outline and the fields, in lower case ("linear").
    static const char* CurveWord(AI::CurvePreset preset);
    /// The muted word before the row's name ("Option", "Consider"); empty for a row whose name is its kind.
    static const char* KindWord(const ScribeOutlineRow& row);
    /// The option that scores `state` in its Utility parent, with the parent in `outUtility`; null when the state
    /// has no option.
    const AI::OptionSet::Option* FindOption(uint16 state, uint16& outUtility) const;

    /// An empty name removes the name.
    void RenameState(uint16 state, std::string name);
    /// An empty name removes the enter condition.
    void SetEnterCondition(uint16 state, std::string_view conditionName);
    /// Applies to an event transition only.
    void SetTransitionEvent(uint16 state, uint16 transition, std::string_view eventName);

    /// The option edits take the state that the option scores in its Utility parent; they change nothing for a
    /// state with no option.
    void SetOptionWeight(uint16 state, float32 weight, Phase phase);
    void SetOptionRank(uint16 state, uint8 rank);
    /// `input` is an input name; an empty name changes nothing.
    void AddConsideration(uint16 state, std::string_view input);
    void RemoveConsideration(uint16 state, uint16 consideration);
    void SetConsiderationInput(uint16 state, uint16 consideration, std::string_view input);
    void SetConsiderationRange(uint16 state, uint16 consideration, float32 min, float32 max, Phase phase);

    /// The Utility edits take a Utility state; they change nothing for a state of another policy.
    void SetCommitmentMargin(uint16 state, float32 margin, Phase phase);
    void SetMinimumTicks(uint16 state, uint32 ticks);
    void SetNoiseMargin(uint16 state, float32 margin, Phase phase);

    /// Writes the source to the file through WriteAiStateTree, and reports the write to the asset manager so
    /// that the asset reloads. False with the reason in `outError`.
    bool Save(std::string& outError);

    /// Shows `source`, ends a preview and checks the program again. The edit command calls it for Do and Undo.
    void ReplaceSource(AIECS::AiStateTreeSource source);
    /// Checks the program again and changes the version. The panel calls it when the registry's names change.
    void CheckNames();

    ScribeDocument(ConstructionKey key, std::filesystem::path path, UndoRedoService* undo,
                   const AIECS::AiRegistry* names);

  private:
    void Commit(const char* name, AIECS::AiStateTreeSource edited);
    void Apply(const char* name, AIECS::AiStateTreeSource edited, Phase phase);
    void Show(AIECS::AiStateTreeSource source);
    // The source that an edit starts from: the source before the preview while a preview shows.
    const AIECS::AiStateTreeSource& EditBase() const;

    std::filesystem::path m_Path;
    UndoRedoService* m_Undo = nullptr;
    const AIECS::AiRegistry* m_Names = nullptr;
    AIECS::AiStateTreeSource m_Source;
    // The source before the first preview; empty when no preview shows.
    std::optional<AIECS::AiStateTreeSource> m_PreviewStart;
    std::string m_SavedText;
    std::vector<AI::StateTreeCompileError> m_Refusals;
    // True when m_Refusals[0] is the compile refusal; the other refusals are the registry's name refusals.
    bool m_CompileRefused = false;
    uint64 m_Version = 0;
    bool m_Dirty = false;
};

} // namespace GameEngine::Editor
