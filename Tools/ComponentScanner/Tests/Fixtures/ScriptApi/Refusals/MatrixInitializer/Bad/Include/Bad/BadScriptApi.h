#pragma once

// A refused declaration. Never compiled: the scanner reads it.

namespace GameEngine::Bad
{

/// A pose.
// @ge-script
struct Pose
{
    Mathematics::Matrix4x4 matrix{Mathematics::Matrix4x4::Identity()};
};

} // namespace GameEngine::Bad
