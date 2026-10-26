#pragma once

#include "UI/UIElement.h"

#include <chrono>
#include <optional>
#include <string>

namespace GameEngine
{
class Label;

namespace EditorUI
{

/**
 * @brief A short confirmation that appears over its host and goes away on its own: "Copied 3 lines".
 *
 * It answers "did that work?" for an action whose result the user cannot see, such as a copy to the
 * clipboard. The plate and its text are declared in UI/controls/TransientNotice/TransientNotice.uxml
 * and styled by TransientNotice.css beside it; the sheet places the notice at the bottom center of
 * its parent, which must be a positioned container, and its variables let a host align it to the
 * right end instead.
 *
 * The notice has no timer of its own: the host calls Tick from its per-frame update, and the notice
 * hides kShownDuration after the last Show. Main thread only.
 */
class TransientNotice : public UIElement
{
public:
    /// How long the notice stays after the last Show before it fades out.
    static constexpr std::chrono::milliseconds kShownDuration{1600};

    TransientNotice();

    /// Shows `text` from `now`; a Show while shown replaces the text and restarts the duration.
    /// `top` places the notice's top edge, in its parent's logical pixels, next to what it confirms;
    /// without it the notice sits at the bottom of its parent, centered unless the host aligns it.
    void Show(const std::string& text, std::chrono::steady_clock::time_point now, std::optional<float> top = {});

    /// Hides the notice once its duration has passed. Cheap when hidden.
    void Tick(std::chrono::steady_clock::time_point now);

private:
    // Built in the constructor from the layout; null only when the layout did not load, which is logged.
    Label* m_Text = nullptr;
    std::chrono::steady_clock::time_point m_HideAt;
    bool m_Shown = false;
};

} // namespace EditorUI
} // namespace GameEngine
