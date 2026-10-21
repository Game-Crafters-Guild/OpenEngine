#pragma once

#include <string>

namespace GameEngine
{
/// Selects and frames in the Scene View the mark-up a chip's `token` names; returns the panel's
/// status line: "Framed <title>", or why not.
std::string FrameMarkupChip(const std::string& token);
} // namespace GameEngine
