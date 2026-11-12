#include "SceneView/SceneViewport/SceneViewportRegistry.h"

#include <algorithm>
#include <vector>

namespace GameEngine
{
namespace
{
std::vector<SceneViewport*>& LivePanes()
{
    static std::vector<SceneViewport*> s_LivePanes;
    return s_LivePanes;
}
} // namespace

void SceneViewportRegistry::Register(SceneViewport& pane)
{
    std::vector<SceneViewport*>& panes = LivePanes();
    if (std::find(panes.begin(), panes.end(), &pane) == panes.end())
        panes.push_back(&pane);
}

void SceneViewportRegistry::Unregister(SceneViewport& pane)
{
    std::erase(LivePanes(), &pane);
}

std::span<SceneViewport* const> SceneViewportRegistry::GetLive()
{
    return LivePanes();
}

} // namespace GameEngine
