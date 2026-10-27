#pragma once

#include <string>

namespace GameEngine::Editor::TestStubs
{

/// The text of the last ShowSceneViewNotice call in EditorTests, where SceneViewLeafStubs.cpp records
/// the notice in place of the Scene View overlay. A test clears it before the action it checks.
std::string& LastSceneViewNotice();

} // namespace GameEngine::Editor::TestStubs
