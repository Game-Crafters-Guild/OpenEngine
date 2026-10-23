#include "DayCycle/DayCycleDescriptionField.h"

#include <utility>

namespace GameEngine::Editor
{

DayCycleDescriptionField::DayCycleDescriptionField(std::function<void(const std::string&)> commit)
    : m_Commit(std::move(commit))
{
    AddClass("inspector-text-area");
    // The commit may rebuild the inspector and destroy this field: it runs on copies, last.
    SetOnSubmit([this]() {
        const std::function<void(const std::string&)> commit = m_Commit;
        commit(GetValue());
    });
}

void DayCycleDescriptionField::OnFocusChanged(bool focused)
{
    TextArea::OnFocusChanged(focused);
    if (focused)
        return;
    const std::function<void(const std::string&)> commit = m_Commit;
    commit(GetValue());
}

} // namespace GameEngine::Editor
