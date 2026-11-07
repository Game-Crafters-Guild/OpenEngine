#pragma once

#include <cstdint>
#include <string>

namespace GameEngine::Editor
{
// Editor-side command interface used by UndoRedoService.
// Commands are expected to be deterministic and reversible.
class IEditorCommand
{
public:
    virtual ~IEditorCommand() = default;

    // Human-readable label for debugging/UI (ownership stays with command).
    virtual const char* GetName() const = 0;
    /// Stored before/after values for the history row, independent of the current document state.
    virtual std::string GetDescription() const { return {}; }

    // Stable type identifier for tooling/inspection (e.g. undo-stack dumps).
    // Defaults to a generic label; commands whose identity matters for debugging
    // (DeleteEntitiesCommand vs WorldSnapshotCommand, etc.) override it.
    virtual const char* GetTypeName() const { return "Command"; }

    // Apply the command.
    virtual void Do() = 0;

    // Revert the command.
    virtual void Undo() = 0;

    // Optional explicit redo hook (default re-applies Do()).
    virtual void Redo() { Do(); }

    // A failed replay leaves the command's targets unchanged.
    virtual bool TryUndo() { Undo(); return true; }
    virtual bool TryRedo() { Redo(); return true; }
    virtual void NotifyReplay() {}

    virtual bool IsContinuous() const { return false; }
    virtual bool IsNoOp() const { return false; }

    // Optional merge/coalescing support (not required for interactive edits that
    // already commit a single command).
    virtual bool CanMergeWith(const IEditorCommand& /*other*/) const { return false; }
    virtual bool MergeWith(const IEditorCommand& /*other*/) { return false; }
};

} // namespace GameEngine::Editor


