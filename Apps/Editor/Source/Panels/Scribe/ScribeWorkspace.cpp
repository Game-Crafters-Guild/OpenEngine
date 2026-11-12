#include "Panels/Scribe/ScribeWorkspace.h"

#include "Panels/Scribe/ScribeDocument.h"

namespace GameEngine::Editor
{

ScribeWorkspace::ScribeWorkspace(UndoRedoService* undo, const AIECS::AiRegistry* names) : m_Undo(undo), m_Names(names)
{
}

void ScribeWorkspace::Open(const std::filesystem::path& path)
{
    if (m_Document && m_Document->IsDirty())
    {
        m_PendingOpen = path;
        return;
    }
    Load(path);
}

void ScribeWorkspace::ConfirmPendingOpen()
{
    if (m_PendingOpen.empty())
        return;
    const std::filesystem::path path = m_PendingOpen;
    Load(path);
}

void ScribeWorkspace::Load(const std::filesystem::path& path)
{
    m_PendingOpen.clear();
    m_OpenError.clear();
    if (std::shared_ptr<ScribeDocument> document = ScribeDocument::Open(path, m_Undo, m_Names, m_OpenError))
        m_Document = std::move(document);
}

} // namespace GameEngine::Editor
