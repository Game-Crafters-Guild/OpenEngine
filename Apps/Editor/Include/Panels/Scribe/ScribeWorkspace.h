#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace GameEngine::AIECS
{
class AiRegistry;
}

namespace GameEngine::Editor
{
class ScribeDocument;
class UndoRedoService;

/// The program file that the Scribe panel shows. The asset opener opens a file here and the panel reads it,
/// so a file opens whether the panel exists yet or not. Main thread only.
class ScribeWorkspace
{
  public:
    ScribeWorkspace(UndoRedoService* undo, const AIECS::AiRegistry* names);

    /// Shows `path`. When the shown program has unsaved edits, the file waits for ConfirmPendingOpen or
    /// CancelPendingOpen. A file that does not read keeps the shown program and sets the open error.
    void Open(const std::filesystem::path& path);
    /// The file that waits to open; empty when no file waits.
    const std::filesystem::path& GetPendingOpen() const { return m_PendingOpen; }
    /// Opens the file that waits. The unsaved edits of the shown program are lost.
    void ConfirmPendingOpen();
    void CancelPendingOpen() { m_PendingOpen.clear(); }

    /// Null before the first open that read its file.
    const std::shared_ptr<ScribeDocument>& GetDocument() const { return m_Document; }
    /// Why the last open failed, with the fix; empty after an open that read the file.
    const std::string& GetOpenError() const { return m_OpenError; }
    /// The registry whose names the program is checked against; the fields offer its names.
    const AIECS::AiRegistry* GetNames() const { return m_Names; }

  private:
    void Load(const std::filesystem::path& path);

    UndoRedoService* m_Undo = nullptr;
    const AIECS::AiRegistry* m_Names = nullptr;
    std::shared_ptr<ScribeDocument> m_Document;
    std::string m_OpenError;
    std::filesystem::path m_PendingOpen;
};

} // namespace GameEngine::Editor
