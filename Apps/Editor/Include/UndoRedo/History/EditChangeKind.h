#pragma once

#include <cstdint>

namespace GameEngine::Editor
{
enum class EditChangeKind : std::uint8_t
{
    Preview,
    Commit,
    UndoRedo,
    InspectorRebuild
};
} // namespace GameEngine::Editor
