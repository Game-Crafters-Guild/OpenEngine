#pragma once

#include "AIECS/Assets/AiStateTree/AiStateTreeSource.h"
#include "UndoRedo/IEditorCommand.h"

#include <memory>

namespace GameEngine::Editor
{
class ScribeDocument;

/// One edit of a program in the Scribe panel: the whole source before and after it (a program is small).
/// A document that is closed takes no change.
class ScribeEditCommand final : public IEditorCommand
{
  public:
    ScribeEditCommand(std::weak_ptr<ScribeDocument> document, const char* name, AIECS::AiStateTreeSource before,
                      AIECS::AiStateTreeSource after);

    const char* GetName() const override { return m_Name; }
    const char* GetTypeName() const override { return "ScribeEditCommand"; }
    void Do() override;
    void Undo() override;

  private:
    std::weak_ptr<ScribeDocument> m_Document;
    const char* m_Name = nullptr;
    AIECS::AiStateTreeSource m_Before;
    AIECS::AiStateTreeSource m_After;
};

} // namespace GameEngine::Editor
