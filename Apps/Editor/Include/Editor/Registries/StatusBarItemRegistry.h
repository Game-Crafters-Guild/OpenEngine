#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::Editor
{

/// What one status bar item shows now.
struct StatusBarItemDisplay
{
    /// The item's text; empty hides the item.
    std::string Text;
    /// Shown when the pointer rests on the item: what the text means and when it goes away.
    std::string Tooltip;
};

/// One entry of the editor's status bar: a short text its owner computes, shown while non-empty.
struct StatusBarItem
{
    /// Unique among the items; names the item's label, `StatusBarItem.<Id>`, for UI tree readers.
    std::string Id;
    /// The item's display now. Polled on the main thread, a few times a second, so it reads
    /// counters rather than doing work.
    std::function<StatusBarItemDisplay()> Display;
};

/**
 * The status bar's registered items: the bar shows each in its own label inside the
 * `StatusBarItems` element of the editor layout, in registration order, while its text is
 * non-empty.
 *
 * The application owns one registry and calls Update every frame; it never learns what the
 * items are. An item's owner registers it once at startup (RegisterBuiltInStatusBarItems).
 * Main thread only.
 */
class StatusBarItemRegistry
{
  public:
    /// How often the items' texts are polled: the bar changes at most this often.
    static constexpr std::chrono::milliseconds kPollInterval{250};

    /// Adds an item. An id registered before is refused: it logs an error naming the id and
    /// asserts.
    void Register(StatusBarItem item);

    /// Once kPollInterval has passed since the last poll, polls every item's display and writes
    /// it into the item's label under `root`'s `StatusBarItems` element, creating the label on
    /// first use and after a layout rebuild. The first call polls. A null root, or a root
    /// without the element, skips the write.
    void Update(std::chrono::duration<double> elapsed, UIElement* root);

  private:
    std::vector<StatusBarItem> m_Items;
    std::chrono::duration<double> m_SinceLastPoll{kPollInterval};
};

} // namespace GameEngine::Editor
