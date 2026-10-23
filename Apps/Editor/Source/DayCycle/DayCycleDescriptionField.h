#pragma once

#include "UI/Controls/TextArea.h"

#include <functional>
#include <string>

namespace GameEngine::Editor
{

/// The Day Cycle profile's description: a text area that wraps to the field's width, so a long
/// description is read whole in a narrow inspector. It hands its text to `commit` once per edit, on
/// Enter (Shift+Enter breaks the line) or when the focus leaves it.
class DayCycleDescriptionField : public TextArea
{
  public:
    explicit DayCycleDescriptionField(std::function<void(const std::string&)> commit);

    void OnFocusChanged(bool focused) override;

  private:
    std::function<void(const std::string&)> m_Commit;
};

} // namespace GameEngine::Editor
