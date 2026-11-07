#pragma once

namespace GameEngine::Editor
{
class UndoRedoService;

/// Identifies the history service that owns a synchronous document mutation.
class HistoryMutationScope
{
public:
    enum class Phase
    {
        Edit,
        Restore
    };

    explicit HistoryMutationScope(const UndoRedoService* history, Phase phase = Phase::Edit)
        : m_Previous(s_Active), m_PreviousPhase(s_Phase)
    {
        s_Active = history;
        s_Phase = phase;
    }
    ~HistoryMutationScope()
    {
        s_Active = m_Previous;
        s_Phase = m_PreviousPhase;
    }
    HistoryMutationScope(const HistoryMutationScope&) = delete;
    HistoryMutationScope& operator=(const HistoryMutationScope&) = delete;

    /// Whether this service owns the current mutation, including nested callbacks.
    static bool IsActiveFor(const UndoRedoService* history) { return history && s_Active == history; }
    /// Whether this service is restoring a revision or cancelling an edit.
    static bool IsRestoringFor(const UndoRedoService* history)
    {
        return IsActiveFor(history) && s_Phase == Phase::Restore;
    }

private:
    const UndoRedoService* m_Previous;
    Phase m_PreviousPhase;
    inline static thread_local const UndoRedoService* s_Active = nullptr;
    inline static thread_local Phase s_Phase = Phase::Edit;
};
} // namespace GameEngine::Editor
