#pragma once

#include <cmath>
#include <optional>

namespace GameEngine::Editor
{

// What the Scene View popover's Reading line shows of a live EV100. The line has one decimal; a value
// that hovers across a rounding boundary (the Picture meter re-metering every frame) would flip the
// label each frame, so the shown value moves only once the live one is a whole display step away.
class ExposureReadingFilter
{
  public:
    // The display step of the Reading line, in EV100.
    static constexpr float kStepEv = 0.1f;

    // Feeds the latest value; true when the shown value changed (the label needs a refresh).
    bool Accept(std::optional<float> ev)
    {
        if (!ev)
        {
            const bool hadValue = m_Shown.has_value();
            m_Shown.reset();
            return hadValue;
        }
        if (m_Shown && std::fabs(*ev - *m_Shown) < kStepEv)
            return false;
        m_Shown = *ev;
        return true;
    }

    // Forgets the shown value, so the next Accept shows its value at once (a meter switch).
    void Reset() { m_Shown.reset(); }

    std::optional<float> Shown() const { return m_Shown; }

  private:
    std::optional<float> m_Shown;
};

} // namespace GameEngine::Editor
