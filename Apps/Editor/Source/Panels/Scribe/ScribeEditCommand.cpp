#include "Panels/Scribe/ScribeEditCommand.h"

#include "Panels/Scribe/ScribeDocument.h"

#include <utility>

namespace GameEngine::Editor
{

ScribeEditCommand::ScribeEditCommand(std::weak_ptr<ScribeDocument> document, const char* name,
                                     AIECS::AiStateTreeSource before, AIECS::AiStateTreeSource after)
    : m_Document(std::move(document)), m_Name(name), m_Before(std::move(before)), m_After(std::move(after))
{
}

void ScribeEditCommand::Do()
{
    if (const std::shared_ptr<ScribeDocument> document = m_Document.lock())
        document->ReplaceSource(m_After);
}

void ScribeEditCommand::Undo()
{
    if (const std::shared_ptr<ScribeDocument> document = m_Document.lock())
        document->ReplaceSource(m_Before);
}

} // namespace GameEngine::Editor
