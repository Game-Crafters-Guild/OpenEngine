// Test seams of the material system. They stay off the public MaterialSystem.h, which games include.

#pragma once

namespace GameEngine::Engine::Renderer
{

/// True: the next job that MaterialSystem::ParseSurfacesOffThread starts throws before it parses,
/// as a failed allocation would. False: clears a throw that no job took.
void SetNextSurfaceParseJobThrowsForTesting(bool throws);

/// True: each job that MaterialSystem::ParseSurfacesOffThread starts waits before it parses, until a call with
/// false. False: releases the waiting jobs.
void SetSurfaceParseJobsHeldForTesting(bool held);

} // namespace GameEngine::Engine::Renderer
