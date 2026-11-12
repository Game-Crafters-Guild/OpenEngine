#pragma once

#include <string>
#include <string_view>

namespace GameEngine::Editor
{
    /// The version the editor shows in its title bar and info dialog: the engine version from
    /// the root VERSION file and, when the build could read it, the short commit it was built
    /// from ("2026.10.0-alpha.4 (963d147)").
    std::string EditorVersionText();

    /// The short commit id at HEAD when the editor was built. It ends in "-dirty" when tracked
    /// files had uncommitted changes at that build. Empty when the build could not read it.
    std::string_view EditorBuildCommit();
} // namespace GameEngine::Editor
