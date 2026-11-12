#include "Startup/StartupWindowShowGate.h"

namespace GameEngine::Editor::Startup
{
bool StartupWindowShowGate::OnPresent(bool sceneViewShowsImage)
{
    ++m_Presents;
    return sceneViewShowsImage || m_Presents >= kFallbackPresents;
}
} // namespace GameEngine::Editor::Startup
