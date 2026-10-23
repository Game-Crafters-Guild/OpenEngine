#pragma once

#include "Types/Types.h"

namespace GameEngine::Particles
{
/// Grows each time an id becomes findable that was not: a first registration, or an id an aborted
/// module load dropped and a later load registers again. Never 0: the built-ins count. A stack that
/// failed to load because it named an id nobody had registered may load once this has grown.
uint64 ParticleProcessorRevision();
} // namespace GameEngine::Particles
