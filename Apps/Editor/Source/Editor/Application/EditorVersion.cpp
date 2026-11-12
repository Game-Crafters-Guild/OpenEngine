#include "Editor/Application/EditorVersion.h"

#include "BuildInfo/BuildCommit.h"
#include "Core/EngineVersion.h"

namespace GameEngine::Editor
{
std::string EditorVersionText()
{
    std::string text = kEngineVersion;
    const std::string_view commit = EditorBuildCommit();
    if (!commit.empty())
    {
        text += " (";
        text += commit;
        text += ')';
    }
    return text;
}

std::string_view EditorBuildCommit()
{
    return BuildInfo::kBuildCommit;
}
} // namespace GameEngine::Editor
