#pragma once

namespace GameEngine::Editor
{
class StatusBarItemRegistry;

/// Registers the editor's own status bar items, in the order the bar shows them.
void RegisterBuiltInStatusBarItems(StatusBarItemRegistry& registry);

} // namespace GameEngine::Editor
