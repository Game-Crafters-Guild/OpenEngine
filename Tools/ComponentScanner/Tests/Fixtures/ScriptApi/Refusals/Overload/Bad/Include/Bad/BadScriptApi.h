#pragma once

// A refused declaration. Never compiled: the scanner reads it.

namespace GameEngine::Bad
{

/// Plays a sound.
// @ge-script
void Play(std::int32_t sound);

/// Plays a sound at a volume.
// @ge-script
void Play(std::int32_t sound, float volume);

} // namespace GameEngine::Bad
