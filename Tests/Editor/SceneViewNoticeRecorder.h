#pragma once

#include <functional>
#include <string>

namespace GameEngine::Editor::TestStubs
{

/// The text of the last ShowSceneViewNotice call in EditorTests, where SceneViewLeafStubs.cpp records
/// the notice in place of the Scene View overlay. A test clears it before the action it checks.
std::string& LastSceneViewNotice();

/// The button of the last notice: its label, its tooltip and what a click does. All are empty after a
/// notice with no button.
struct RecordedNoticeAction
{
    std::string Label;
    std::string Tooltip;
    std::function<void()> OnInvoke;
};
RecordedNoticeAction& LastSceneViewNoticeAction();

} // namespace GameEngine::Editor::TestStubs
