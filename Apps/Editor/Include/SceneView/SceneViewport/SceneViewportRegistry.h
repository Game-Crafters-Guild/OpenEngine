#pragma once

#include <span>

namespace GameEngine
{
class SceneViewport;

/// The live Scene View panes of every window. A pane is in the list while it is bound to a controller: it leaves
/// on unbind and on destruction, so the list never holds a destroyed pane. Main thread only.
class SceneViewportRegistry
{
public:
    SceneViewportRegistry() = delete;

    /// Adds `pane` to the live list. A pane in the list stays once.
    static void Register(SceneViewport& pane);
    /// Removes `pane` from the live list. A pane not in the list is ignored.
    static void Unregister(SceneViewport& pane);
    /// The live panes, in the order they were added. Changes when a pane binds, unbinds or is destroyed.
    static std::span<SceneViewport* const> GetLive();
};

} // namespace GameEngine
